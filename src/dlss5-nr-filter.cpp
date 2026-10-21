// obs-dlss5-nr — DLSS 5 Neural Rendering filter for OBS Studio
// Copyright (C) 2026 Saganaki22
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// Runs video frames through NVIDIA DLSS NR (NGX feature 18) on a private
// D3D12 device. The default path stages pixels through the CPU, with NR on
// a worker thread. The experimental shared-texture path is opt-in and only
// used in Low latency mode. Frames can be resized on the GPU before staging.

#include "dlss5-nr-filter.h"

#include <d3d11.h>
#include <dxgi.h>
#include <obs-module.h>
#include <util/base.h>
#include <util/platform.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bridge/nr_bridge.h"
#include "frame-policy.h"

// DLSSNR.Style values, as consumed by nvngx_dlssnr (NGX feature 18).
enum DlssNrStyle {
	DLSSNR_STYLE_DEFAULT = 0,
	DLSSNR_STYLE_NATURAL = 1,
	DLSSNR_STYLE_CINEMATIC = 2,
};

enum DlssNrChannelOrder {
	DLSSNR_CHANNEL_AUTO = 0,
	DLSSNR_CHANNEL_RGBA = 1,
	DLSSNR_CHANNEL_BGRA = 2,
};

static constexpr uint64_t LOG_THROTTLE_NS = 5000000000ULL; // 5 s
static constexpr uint64_t INIT_RETRY_NS = 5000000000ULL;   // 5 s
static constexpr DWORD KM_TIMEOUT_MS = 100;

struct dlss5nr_filter {
	obs_source_t *context;

	// Mirrored settings (consumed by the bridge).
	int style = DLSSNR_STYLE_NATURAL;
	int preset = 3;
	float intensity = 1.0f;
	float tone = 1.0f;
	float structure = 1.0f;
	float skin = -1.0f;
	bool automask = false;
	bool temporal = false; // NVIDIA Optical Flow motion vectors for NR
	int ui_correction = 0;
	int gpu_index = 0;
	int channel_order = DLSSNR_CHANNEL_AUTO;
	bool reset_pending = false;
	bool initialized = false;

	// Bridge lifecycle / status.
	bool init_attempted = false;
	uint64_t last_init_attempt = 0;
	std::atomic<uint64_t> last_error_log{0};
	std::atomic<bool> processing_failed{false};
	bool diagnostics = false;
	std::mutex status_mutex;
	std::string status_text;

	// Zero-copy GPU path surfaces.
	gs_texture_t *shared_in = nullptr;  // RGBA16F, KM shared, render target
	gs_texture_t *shared_out = nullptr; // RGBA16F, opened from bridge handle
	IDXGIKeyedMutex *shared_in_km = nullptr;
	uint32_t shared_w = 0, shared_h = 0;
	bool using_gpu = false;
	bool gpu_broken = false;    // set when shared-texture setup fails; avoid per-frame retry
	bool gpu_zero_copy = false; // user opt-in; experimental
	bool async_mode = true;     // "Smooth": NR on worker thread, variable latency
	uint32_t nr_fps = 0;        // 0 = process every frame; else throttle NR to N fps
	frame_policy::RateLimiter limiter;
	uint32_t processing_height = 1080;
	uint32_t source_w = 0, source_h = 0;
	uint64_t last_render_ns = 0;
	bool cached_frame = false;
	uint64_t perf_start_ns = 0;
	uint32_t presented_frames = 0;
	std::atomic<double> process_ms{0.0};

	// Async machinery (Smooth mode). The graphics thread only snapshots
	// frames into jobs and draws the newest completed result; a worker
	// thread runs the bridge.
	struct AsyncJob {
		std::vector<uint8_t> pixels;
		uint32_t pitch = 0;
		uint32_t w = 0, h = 0;
		uint64_t seq = 0;
		NrBridgeParams params{};
	};
	bool worker_started = false;
	std::thread worker;
	std::mutex job_mutex;
	std::condition_variable job_cv;
	std::deque<AsyncJob> jobs; // bounded; newest wins
	std::mutex result_mutex;
	AsyncJob result;
	bool result_valid = false;
	std::atomic<bool> stop_flag{false};
	uint64_t submit_seq = 0;
	uint64_t drawn_seq = 0;
	std::vector<uint8_t> draw_buf;

	// CPU staging fallback surfaces.
	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stagesurface = nullptr;
	gs_texture_t *out_tex = nullptr;
	std::vector<uint8_t> out_buf;

	void set_status(const char *fmt, ...)
	{
		char buf[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		std::lock_guard<std::mutex> lock(status_mutex);
		status_text = buf;
	}
};

static std::atomic<int> g_bridge_users{0};

static void log_error_throttled(dlss5nr_filter *f, const char *err);

// ------------------------------------------------------------ async worker

static void async_worker(dlss5nr_filter *f)
{
	while (!f->stop_flag.load()) {
		dlss5nr_filter::AsyncJob job;
		{
			std::unique_lock<std::mutex> lock(f->job_mutex);
			f->job_cv.wait_for(lock, std::chrono::milliseconds(100),
					   [&] { return f->stop_flag.load() || !f->jobs.empty(); });
			if (f->stop_flag.load())
				break;
			if (f->jobs.empty())
				continue;
			job = std::move(f->jobs.back());
			f->jobs.clear(); // stale frames dropped — newest wins
		}

		std::vector<uint8_t> out((size_t)job.w * job.h * 4);
		const uint64_t begin_ns = os_gettime_ns();
		const bool ok = nrbridge::process(job.pixels.data(), (int)job.pitch, out.data(), (int)job.w * 4,
						  (int)job.w, (int)job.h, job.params);

		f->process_ms.store((os_gettime_ns() - begin_ns) / 1000000.0);
		f->processing_failed.store(!ok);
		if (!ok) {
			const std::string error = nrbridge::last_error();
			f->set_status("NR failed: %s", error.c_str());
			log_error_throttled(f, error.c_str());
		}
		std::lock_guard<std::mutex> lock(f->result_mutex);
		if (ok) {
			f->result.pixels = std::move(out);
			f->result.w = job.w;
			f->result.h = job.h;
			f->result.seq = job.seq;
			f->result_valid = true;
		}
	}
}

static void start_worker(dlss5nr_filter *f)
{
	if (f->worker_started)
		return;
	f->stop_flag.store(false);
	f->worker = std::thread(async_worker, f);
	f->worker_started = true;
}

static void stop_worker(dlss5nr_filter *f)
{
	if (!f->worker_started)
		return;
	f->stop_flag.store(true);
	f->job_cv.notify_all();
	if (f->worker.joinable())
		f->worker.join();
	f->worker_started = false;
}

static void clear_completed_frames(dlss5nr_filter *f)
{
	// Caller stops the worker before invalidating results or processing dimensions.
	{
		std::lock_guard<std::mutex> lock(f->job_mutex);
		f->jobs.clear();
	}
	{
		std::lock_guard<std::mutex> lock(f->result_mutex);
		f->result_valid = false;
		f->result.pixels.clear();
	}
	f->drawn_seq = 0;
	f->processing_failed.store(false);
	f->cached_frame = false;
	f->last_render_ns = 0;
	f->limiter.reset();
	f->perf_start_ns = 0;
	f->presented_frames = 0;
}

// ---------------------------------------------------------------- lifecycle

static const char *dlss5nr_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("FilterName");
}

static void *dlss5nr_create(obs_data_t *settings, obs_source_t *context)
{
	auto *f = new dlss5nr_filter{};
	f->context = context;

	obs_enter_graphics();
	f->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	obs_leave_graphics();

	f->set_status("%s", obs_module_text("Status.Initializing"));
	g_bridge_users.fetch_add(1, std::memory_order_relaxed);

	obs_source_update(context, settings);
	return f;
}

static void dlss5nr_destroy(void *data)
{
	auto *f = static_cast<dlss5nr_filter *>(data);
	if (!f)
		return;

	stop_worker(f);

	obs_enter_graphics();
	if (f->shared_in_km) {
		f->shared_in_km->Release();
		f->shared_in_km = nullptr;
	}
	if (f->shared_in)
		gs_texture_destroy(f->shared_in);
	if (f->shared_out)
		gs_texture_destroy(f->shared_out);
	if (f->texrender)
		gs_texrender_destroy(f->texrender);
	if (f->stagesurface)
		gs_stagesurface_destroy(f->stagesurface);
	if (f->out_tex)
		gs_texture_destroy(f->out_tex);
	obs_leave_graphics();

	if (g_bridge_users.fetch_sub(1, std::memory_order_relaxed) == 1)
		nrbridge::shutdown();

	delete f;
}

static uint32_t dlss5nr_width(void *data)
{
	auto *f = static_cast<dlss5nr_filter *>(data);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	return parent ? obs_source_get_base_width(parent) : 0;
}

static uint32_t dlss5nr_height(void *data)
{
	auto *f = static_cast<dlss5nr_filter *>(data);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	return parent ? obs_source_get_base_height(parent) : 0;
}

// ------------------------------------------------------------- bridge setup

static wchar_t *utf8_to_wide(const char *s)
{
	wchar_t *out = nullptr;
	if (s && os_utf8_to_wcs_ptr(s, 0, &out) > 0)
		return out;
	return out;
}

static std::string dir_of(const std::string &path)
{
	size_t p = path.find_last_of("/\\");
	return p == std::string::npos ? std::string() : path.substr(0, p);
}

static bool find_runtime_dll(std::string *dll_path_out, std::string *expected_path_out)
{
	char *cfg = obs_module_config_path("runtime\\nvngx_dlssnr.dll");
	std::string cfg_path = cfg ? cfg : "";
	bfree(cfg);

	if (os_file_exists(cfg_path.c_str())) {
		*dll_path_out = cfg_path;
		*expected_path_out = cfg_path;
		return true;
	}

	char *data = obs_module_file("runtime\\nvngx_dlssnr.dll");
	std::string data_path = data ? data : "";
	bfree(data);

	*expected_path_out = cfg_path;
	if (!data_path.empty() && os_file_exists(data_path.c_str())) {
		*dll_path_out = data_path;
		return true;
	}
	return false;
}

static bool ensure_bridge_ready(dlss5nr_filter *f)
{
	if (nrbridge::ready())
		return true;

	const uint64_t now = os_gettime_ns();
	if (f->init_attempted && now - f->last_init_attempt < INIT_RETRY_NS)
		return false;

	f->init_attempted = true;
	f->last_init_attempt = now;

	std::string dll_path, expected;
	if (!find_runtime_dll(&dll_path, &expected)) {
		f->set_status("nvngx_dlssnr.dll not found — place it at:\n%s", expected.c_str());
		return false;
	}

	// Shim is installed next to the plugin binary.
	const char *bin = obs_get_module_binary_path(obs_current_module());
	std::string shim_dir = bin ? dir_of(bin) : std::string();

	const int gpu_index = f->gpu_index;
	wchar_t *runtime_dir_w = utf8_to_wide(dir_of(dll_path).c_str());
	wchar_t *shim_dir_w = utf8_to_wide(shim_dir.c_str());
	const bool ok = nrbridge::init(gpu_index, runtime_dir_w, shim_dir_w);
	bfree(runtime_dir_w);
	bfree(shim_dir_w);

	if (!ok) {
		f->set_status("DLSS NR init failed: %s", nrbridge::last_error());
		return false;
	}

	f->set_status("Ready — %s\n%s", nrbridge::gpu_name(), dll_path.c_str());
	blog(LOG_INFO, "[obs-dlss5-nr] bridge initialized: %s (runtime: %s)", nrbridge::gpu_name(), dll_path.c_str());
	return true;
}

static void log_error_throttled(dlss5nr_filter *f, const char *err)
{
	const uint64_t now = os_gettime_ns();
	uint64_t previous = f->last_error_log.load(std::memory_order_relaxed);
	if (previous && now - previous < LOG_THROTTLE_NS)
		return;
	if (!f->last_error_log.compare_exchange_strong(previous, now, std::memory_order_relaxed))
		return;
	blog(LOG_WARNING, "[obs-dlss5-nr] NR processing failed, passing through: %s", err);
}

static void log_state_throttled(dlss5nr_filter *f, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	f->set_status("%s", buf);
	log_error_throttled(f, buf);
}

// ---------------------------------------------------------------- rendering

static void destroy_cpu_surfaces(dlss5nr_filter *f)
{
	if (f->stagesurface) {
		gs_stagesurface_destroy(f->stagesurface);
		f->stagesurface = nullptr;
	}
	if (f->out_tex) {
		gs_texture_destroy(f->out_tex);
		f->out_tex = nullptr;
	}
	f->out_buf.clear();
}

static void destroy_gpu_surfaces(dlss5nr_filter *f)
{
	if (f->shared_in_km) {
		f->shared_in_km->Release();
		f->shared_in_km = nullptr;
	}
	if (f->shared_in) {
		gs_texture_destroy(f->shared_in);
		f->shared_in = nullptr;
	}
	if (f->shared_out) {
		gs_texture_destroy(f->shared_out);
		f->shared_out = nullptr;
	}
	nrbridge::detach_shared();
	f->using_gpu = false;
}

static bool ensure_gpu_surfaces(dlss5nr_filter *f, uint32_t cx, uint32_t cy)
{
	if (f->gpu_broken)
		return false;
	if (f->using_gpu && f->shared_in && f->shared_out && f->shared_w == cx && f->shared_h == cy)
		return true;

	destroy_gpu_surfaces(f);

	obs_enter_graphics();
	f->shared_in = gs_texture_create(cx, cy, GS_RGBA16F, 1, nullptr, GS_RENDER_TARGET | GS_SHARED_KM_TEX);
	if (f->shared_in) {
		if (auto *obj = gs_texture_get_obj(f->shared_in)) {
			static_cast<ID3D11Texture2D *>(obj)->QueryInterface(IID_PPV_ARGS(&f->shared_in_km));
		}
		// A keyed mutex starts out owned by its creating device; hand
		// ownership over so the bridge side can acquire it.
		if (f->shared_in_km)
			f->shared_in_km->ReleaseSync(0);
	}
	obs_leave_graphics();

	if (!f->shared_in) {
		f->gpu_broken = true;
		return false;
	}

	const uint32_t in_handle = gs_texture_get_shared_handle(f->shared_in);
	uint32_t out_handle = 0;
	if (!nrbridge::attach_shared(in_handle, cx, cy, &out_handle) || out_handle == 0) {
		log_error_throttled(f, nrbridge::last_error());
		destroy_gpu_surfaces(f);
		f->gpu_broken = true;
		return false;
	}

	obs_enter_graphics();
	f->shared_out = gs_texture_open_shared(out_handle);
	obs_leave_graphics();

	if (!f->shared_out) {
		destroy_gpu_surfaces(f);
		f->gpu_broken = true;
		return false;
	}

	f->shared_w = cx;
	f->shared_h = cy;
	f->using_gpu = true;
	blog(LOG_INFO, "[obs-dlss5-nr] GPU surfaces attached at %ux%u (shared_in=%p, out_handle=%u)", (int)cx, (int)cy,
	     (void *)f->shared_in, out_handle);
	return true;
}

static bool ensure_cpu_surfaces(dlss5nr_filter *f, uint32_t cx, uint32_t cy)
{
	if (f->stagesurface && f->out_tex && f->shared_w == cx && f->shared_h == cy)
		return true;

	stop_worker(f);
	clear_completed_frames(f);
	obs_enter_graphics();
	destroy_cpu_surfaces(f);
	f->stagesurface = gs_stagesurface_create(cx, cy, GS_BGRA);
	f->out_tex = gs_texture_create(cx, cy, GS_BGRA, 1, nullptr, GS_DYNAMIC);
	obs_leave_graphics();

	f->shared_w = cx;
	f->shared_h = cy;
	f->out_buf.resize((size_t)cx * cy * 4);

	if (!f->stagesurface || !f->out_tex) {
		f->set_status("Failed to allocate OBS staging surfaces");
		return false;
	}
	return true;
}

static void draw_texture(gs_texture_t *tex, uint32_t cx, uint32_t cy)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, cx, cy);
}

// Renders the parent source into tex with an opaque blend, like the standard
// filter copy pattern.
static void render_parent_into(dlss5nr_filter *f, obs_source_t *parent, uint32_t cx, uint32_t cy)
{
	gs_viewport_push();
	gs_projection_push();

	gs_texture_t *previous_target = gs_get_render_target();
	gs_zstencil_t *previous_zs = gs_get_zstencil_target();
	gs_set_render_target(f->shared_in, nullptr);
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)f->source_w, 0.0f, (float)f->source_h, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_video_render(parent);
	gs_blend_state_pop();

	gs_set_render_target(previous_target, previous_zs);

	gs_projection_pop();
	gs_viewport_pop();
}

// Outcome of a GPU-path attempt.
enum class GpuResult {
	Ok,     // frame processed and drawn
	Skip,   // transient failure — pass through this frame, stay on GPU
	Broken, // setup failed — detach and use CPU staging from now on
};

static GpuResult process_gpu_path(dlss5nr_filter *f, obs_source_t *parent, obs_source_t *context, uint32_t cx,
				  uint32_t cy)
{
	UNUSED_PARAMETER(context);
	if (!ensure_gpu_surfaces(f, cx, cy))
		return GpuResult::Broken;

	if (f->shared_in_km) {
		if (FAILED(f->shared_in_km->AcquireSync(0, KM_TIMEOUT_MS))) {
			log_state_throttled(f, "OBS-side AcquireSync timed out (%lu) — passing through this frame",
					    (unsigned long)GetLastError());
			return GpuResult::Skip;
		}
	}

	render_parent_into(f, parent, cx, cy);

	if (f->shared_in_km)
		f->shared_in_km->ReleaseSync(0);
	gs_flush();

	NrBridgeParams p{};
	p.style = f->style;
	p.preset = f->preset;
	p.intensity = f->intensity;
	p.tone = f->tone;
	p.structure = f->structure;
	p.skin = f->skin;
	p.automask = f->automask ? 1 : 0;
	p.temporal = f->temporal ? 1 : 0;
	p.ui_correction = f->ui_correction;
	p.reset = f->reset_pending ? 1 : 0;
	f->reset_pending = false;

	if (!nrbridge::process_gpu((int)cx, (int)cy, p)) {
		f->set_status("NR failed — passing through. %s", nrbridge::last_error());
		log_error_throttled(f, nrbridge::last_error());
		// Transient errors keep GPU mode; the bridge refuses CPU staging
		// while attached, so falling through to CPU here would crash.
		return GpuResult::Skip;
	}

	// The bridge refreshes this every 300 frames; log it when it changes.
	const char *tt = nrbridge::timing_text();
	if (*tt) {
		static std::string s_last_timing;
		if (s_last_timing != tt) {
			s_last_timing = tt;
			blog(LOG_INFO, "[obs-dlss5-nr] %s", tt);
		}
	}

	draw_texture(f->shared_out, f->source_w, f->source_h);
	return GpuResult::Ok;
}

static bool process_cpu_path(dlss5nr_filter *f, obs_source_t *parent, obs_source_t *context, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(context);
	if (!f->texrender)
		return false;
	if (!ensure_cpu_surfaces(f, cx, cy))
		return false;

	// Grab the parent's rendered frame.
	gs_texrender_reset(f->texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool grabbed = false;
	if (gs_texrender_begin(f->texrender, cx, cy)) {
		gs_ortho(0.0f, (float)f->source_w, 0.0f, (float)f->source_h, -100.0f, 100.0f);
		obs_source_video_render(parent);
		gs_texrender_end(f->texrender);
		grabbed = true;
	}
	gs_blend_state_pop();
	if (!grabbed)
		return false;

	gs_texture_t *src_tex = gs_texrender_get_texture(f->texrender);
	if (!src_tex)
		return false;

	gs_stage_texture(f->stagesurface, src_tex);
	uint8_t *mapped = nullptr;
	uint32_t pitch = 0;
	if (!gs_stagesurface_map(f->stagesurface, &mapped, &pitch))
		return false;

	NrBridgeParams p{};
	p.style = f->style;
	p.preset = f->preset;
	p.intensity = f->intensity;
	p.tone = f->tone;
	p.structure = f->structure;
	p.skin = f->skin;
	p.automask = f->automask ? 1 : 0;
	p.temporal = f->temporal ? 1 : 0;
	p.ui_correction = f->ui_correction;
	p.reset = f->reset_pending ? 1 : 0;
	f->reset_pending = false;

	const bool ok = nrbridge::process(mapped, (int)pitch, f->out_buf.data(), (int)cx * 4, (int)cx, (int)cy, p);
	gs_stagesurface_unmap(f->stagesurface);

	if (!ok) {
		f->set_status("NR failed — passing through. %s", nrbridge::last_error());
		log_error_throttled(f, nrbridge::last_error());
		return false;
	}

	gs_texture_set_image(f->out_tex, f->out_buf.data(), cx * 4, false);
	draw_texture(f->out_tex, f->source_w, f->source_h);
	return true;
}
// Smooth mode: snapshot this frame into a job, then draw the newest
// completed result (or pass through until the first result exists).
// nr_fps throttles how often frames are submitted for processing; between
// submissions the last NR'd frame keeps displaying.
static bool process_async_path(dlss5nr_filter *f, obs_source_t *parent, obs_source_t *context, uint32_t cx, uint32_t cy,
			       bool submit)
{
	UNUSED_PARAMETER(context);
	if (!f->texrender)
		return false;
	if (!ensure_cpu_surfaces(f, cx, cy))
		return false;

	start_worker(f);

	bool grabbed = false;
	if (submit) {
		// Grab the parent's rendered frame.
		gs_texrender_reset(f->texrender);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		if (gs_texrender_begin(f->texrender, cx, cy)) {
			gs_ortho(0.0f, (float)f->source_w, 0.0f, (float)f->source_h, -100.0f, 100.0f);
			obs_source_video_render(parent);
			gs_texrender_end(f->texrender);
			grabbed = true;
		}
		gs_blend_state_pop();
	}

	if (grabbed) {
		gs_texture_t *src_tex = gs_texrender_get_texture(f->texrender);
		if (src_tex) {
			gs_stage_texture(f->stagesurface, src_tex);
			uint8_t *mapped = nullptr;
			uint32_t pitch = 0;
			if (gs_stagesurface_map(f->stagesurface, &mapped, &pitch)) {
				// Snapshot into a job; drop the oldest if the queue is full.
				dlss5nr_filter::AsyncJob job;
				job.pitch = pitch;
				job.w = cx;
				job.h = cy;
				job.seq = ++f->submit_seq;
				job.params.style = f->style;
				job.params.preset = f->preset;
				job.params.intensity = f->intensity;
				job.params.tone = f->tone;
				job.params.structure = f->structure;
				job.params.skin = f->skin;
				job.params.automask = f->automask ? 1 : 0;
				job.params.temporal = f->temporal ? 1 : 0;
				job.params.ui_correction = f->ui_correction;
				job.params.reset = f->reset_pending ? 1 : 0;
				f->reset_pending = false;

				job.pixels.resize((size_t)pitch * cy);
				memcpy(job.pixels.data(), mapped, (size_t)pitch * cy);
				gs_stagesurface_unmap(f->stagesurface);

				{
					std::lock_guard<std::mutex> lock(f->job_mutex);
					if (!f->jobs.empty() && f->jobs.back().params.reset)
						job.params.reset =
							1; // Don't drop a pending history reset with a stale frame.
					f->jobs.clear();   // Keep only the newest pending frame.
					f->jobs.push_back(std::move(job));
				}
				f->job_cv.notify_one();
			}
		}
	}

	if (f->processing_failed.load())
		return false; // Failed processing always falls back to live pass-through.

	// Draw the newest completed result; keep drawing it while a newer one
	// is in flight (and between throttled submissions) so the output never
	// flickers between NR and raw.
	bool new_result = false;
	{
		std::lock_guard<std::mutex> lock(f->result_mutex);
		if (f->result_valid && f->result.seq != f->drawn_seq && f->result.w == cx && f->result.h == cy) {
			std::swap(f->draw_buf, f->result.pixels);
			f->drawn_seq = f->result.seq;
			new_result = true;
		}
	}

	if (f->drawn_seq == 0)
		return false; // nothing processed yet — brief pass-through at start

	if (new_result) {
		gs_texture_set_image(f->out_tex, f->draw_buf.data(), cx * 4, false);
		++f->presented_frames;
	}
	draw_texture(f->out_tex, f->source_w, f->source_h);
	return true;
}

static void dlss5nr_video_render(void *data, gs_effect_t *filter_effect)
{
	UNUSED_PARAMETER(filter_effect);
	auto *f = static_cast<dlss5nr_filter *>(data);
	obs_source_t *context = f->context;
	obs_source_t *parent = obs_filter_get_parent(context);
	f->source_w = parent ? obs_source_get_base_width(parent) : 0;
	f->source_h = parent ? obs_source_get_base_height(parent) : 0;
	if (!f->source_w || !f->source_h) {
		obs_source_skip_video_filter(context);
		return;
	}
	const auto size = frame_policy::processing_size(f->source_w, f->source_h, f->processing_height);
	const uint32_t cx = size.width, cy = size.height;
	const uint64_t now = obs_get_video_frame_time();
	const bool duplicate = now == f->last_render_ns;
	const bool resized = cx != f->shared_w || cy != f->shared_h;
	if (resized) {
		f->cached_frame = false;
		f->limiter.reset();
	}
	if (duplicate && !resized) {
		if (f->cached_frame)
			draw_texture(f->using_gpu ? f->shared_out : f->out_tex, f->source_w, f->source_h);
		else
			obs_source_skip_video_filter(context);
		return;
	}

	bool processed = false;
	if (ensure_bridge_ready(f)) {
		const bool submit = f->limiter.due(now, f->nr_fps);
		if (f->async_mode) {
			processed = process_async_path(f, parent, context, cx, cy, submit);
		} else if (!submit && f->cached_frame) {
			draw_texture(f->using_gpu ? f->shared_out : f->out_tex, f->source_w, f->source_h);
			processed = true;
		} else {
			const uint64_t begin_ns = os_gettime_ns();
			if (f->gpu_zero_copy && gs_shared_texture_available()) {
				switch (process_gpu_path(f, parent, context, cx, cy)) {
				case GpuResult::Ok:
					processed = true;
					break;
				case GpuResult::Skip:
					break;
				case GpuResult::Broken:
					processed = process_cpu_path(f, parent, context, cx, cy);
					break;
				}
			} else {
				processed = process_cpu_path(f, parent, context, cx, cy);
			}
			f->process_ms.store((os_gettime_ns() - begin_ns) / 1000000.0);
			f->processing_failed.store(!processed);
			if (processed)
				++f->presented_frames;
		}
	}
	f->last_render_ns = now;
	f->cached_frame = processed;
	if (!processed)
		obs_source_skip_video_filter(context);

	if (!f->perf_start_ns)
		f->perf_start_ns = now;
	if (now - f->perf_start_ns >= LOG_THROTTLE_NS) {
		const double fps = f->presented_frames * 1000000000.0 / (now - f->perf_start_ns);
		if (f->presented_frames && !f->processing_failed.load()) {
			char perf[256];
			snprintf(perf, sizeof(perf), "%s | %s | NR %ux%u | enhanced %.1f FPS | last processing %.1f ms",
				 f->using_gpu ? "GPU sharing" : "CPU staging", f->async_mode ? "Smooth" : "Low latency",
				 cx, cy, fps, f->process_ms.load());
			f->set_status("%s", perf);
			blog(LOG_INFO, "[obs-dlss5-nr] %s", perf);
		}
		f->presented_frames = 0;
		f->perf_start_ns = now;
	}
}

// ----------------------------------------------------------------- settings

static void dlss5nr_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<dlss5nr_filter *>(data);

	const long long new_style = obs_data_get_int(settings, "style");
	if (f->initialized && new_style != f->style)
		blog(LOG_INFO, "[obs-dlss5-nr] style changed to %lld — NR feature will be rebuilt", new_style);
	f->style = (int)new_style;
	f->preset = (int)obs_data_get_int(settings, "preset");
	f->intensity = (float)obs_data_get_double(settings, "intensity");
	f->tone = (float)obs_data_get_double(settings, "tone");
	f->structure = (float)obs_data_get_double(settings, "structure");
	f->skin = (float)obs_data_get_double(settings, "skin");
	f->automask = obs_data_get_bool(settings, "automask");
	f->temporal = obs_data_get_bool(settings, "temporal");
	// DLSSNR.UICorrection is left disabled (0): the runtime renders black
	// with correction enabled unless UI-detection inputs are provided, and
	// the reference integrations hardcode it off.
	f->ui_correction = 0;
	f->gpu_index = (int)obs_data_get_int(settings, "gpu_index");
	f->channel_order = (int)obs_data_get_int(settings, "channel_order");
	const bool diagnostics = obs_data_get_bool(settings, "diagnostics");
	if (diagnostics != f->diagnostics)
		blog(LOG_INFO, "[obs-dlss5-nr] diagnostic logging %s; restart OBS to change NGX verbosity",
		     diagnostics ? "enabled" : "disabled");
	f->diagnostics = diagnostics;
	nrbridge::set_diagnostics(diagnostics);
	const bool new_async = obs_data_get_bool(settings, "async_mode");
	const bool new_gpu = obs_data_get_bool(settings, "gpu_zero_copy");
	uint32_t new_height = (uint32_t)obs_data_get_int(settings, "processing_height");
	if (new_height != 0 && new_height != 720 && new_height != 1080 && new_height != 1440 && new_height != 2160)
		new_height = 1080;
	const bool path_changed = f->async_mode != new_async || f->gpu_zero_copy != new_gpu ||
				  f->processing_height != new_height;
	if (path_changed) {
		stop_worker(f);
		clear_completed_frames(f);
		obs_enter_graphics();
		if (f->using_gpu)
			destroy_gpu_surfaces(f);
		// Dimensions are shared between CPU/GPU paths, so discard both on a switch.
		destroy_cpu_surfaces(f);
		f->shared_w = f->shared_h = 0;
		obs_leave_graphics();
		f->gpu_broken = false;
	}
	f->async_mode = new_async;
	f->gpu_zero_copy = new_gpu;
	f->processing_height = new_height;
	f->nr_fps = (uint32_t)obs_data_get_int(settings, "nr_fps");
	if (f->nr_fps != 0 && f->nr_fps != 15 && f->nr_fps != 24 && f->nr_fps != 30 && f->nr_fps != 60)
		f->nr_fps = 0;
	f->initialized = true;
}

static bool dlss5nr_reset_history_cb(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	auto *f = static_cast<dlss5nr_filter *>(data);
	f->reset_pending = true;
	blog(LOG_INFO, "[obs-dlss5-nr] history reset requested");
	return false;
}

static bool dlss5nr_retry_cb(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<dlss5nr_filter *>(data);
	nrbridge::request_retry();
	f->last_error_log.store(0);
	f->set_status("Retry requested — waiting for the next processing frame");
	blog(LOG_INFO, "[obs-dlss5-nr] manual feature retry requested");
	return true;
}

static obs_properties_t *dlss5nr_properties(void *data)
{
	auto *f = static_cast<dlss5nr_filter *>(data);

	// Push the current status into the settings so the info text shows it.
	{
		std::lock_guard<std::mutex> lock(f->status_mutex);
		obs_data_t *s = obs_source_get_settings(f->context);
		obs_data_set_string(s, "status", f->status_text.c_str());
		obs_data_release(s);
	}

	obs_properties_t *props = obs_properties_create();

	obs_property_t *status = obs_properties_add_text(props, "status", obs_module_text("Status"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(status, OBS_TEXT_INFO_NORMAL);

	obs_property_t *style = obs_properties_add_list(props, "style", obs_module_text("Style"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("Style.Default"), DLSSNR_STYLE_DEFAULT);
	obs_property_list_add_int(style, obs_module_text("Style.Natural"), DLSSNR_STYLE_NATURAL);
	obs_property_list_add_int(style, obs_module_text("Style.Cinematic"), DLSSNR_STYLE_CINEMATIC);

	obs_property_t *preset = obs_properties_add_list(props, "preset", obs_module_text("Preset"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(preset, "0", 0);
	obs_property_list_add_int(preset, "1", 1);
	obs_property_list_add_int(preset, "2", 2);
	obs_property_list_add_int(preset, "3", 3);

	obs_properties_add_float_slider(props, "intensity", obs_module_text("Intensity"), 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(props, "tone", obs_module_text("LocalTone"), 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(props, "structure", obs_module_text("LocalStructure"), 0.0, 2.0, 0.05);
	obs_properties_add_float_slider(props, "skin", obs_module_text("Skin"), -1.0, 2.0, 0.05);
	obs_properties_add_bool(props, "automask", obs_module_text("AutoMask"));

	obs_property_t *temporal = obs_properties_add_list(props, "temporal", obs_module_text("Temporal"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_BOOL);
	obs_property_list_add_bool(temporal, obs_module_text("Temporal.Off"), false);
	obs_property_list_add_bool(temporal, obs_module_text("Temporal.On"), true);
	obs_property_set_long_description(temporal, obs_module_text("Temporal.Tip"));

	obs_properties_t *advanced = obs_properties_create();

	obs_property_t *diagnostics = obs_properties_add_bool(advanced, "diagnostics", obs_module_text("Diagnostics"));
	obs_property_set_long_description(diagnostics, obs_module_text("Diagnostics.Tip"));
	obs_properties_add_button(advanced, "retry_feature", obs_module_text("RetryFeature"), dlss5nr_retry_cb);

	obs_properties_add_int(advanced, "gpu_index", obs_module_text("GPUIndex"), 0, 15, 1);

	obs_properties_add_bool(advanced, "gpu_zero_copy", obs_module_text("GPUZeroCopy"));

	obs_property_t *chan = obs_properties_add_list(advanced, "channel_order", obs_module_text("ChannelOrder"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(chan, obs_module_text("ChannelOrder.Auto"), DLSSNR_CHANNEL_AUTO);
	obs_property_list_add_int(chan, obs_module_text("ChannelOrder.RGBA"), DLSSNR_CHANNEL_RGBA);
	obs_property_list_add_int(chan, obs_module_text("ChannelOrder.BGRA"), DLSSNR_CHANNEL_BGRA);

	obs_properties_add_group(props, "advanced", obs_module_text("Advanced"), OBS_GROUP_NORMAL, advanced);

	obs_property_t *mode = obs_properties_add_list(props, "async_mode", obs_module_text("ProcessingMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_BOOL);
	obs_property_list_add_bool(mode, obs_module_text("ProcessingMode.Smooth"), true);
	obs_property_list_add_bool(mode, obs_module_text("ProcessingMode.Realtime"), false);
	obs_property_set_long_description(mode, obs_module_text("ProcessingMode.Tip"));

	obs_property_t *resolution = obs_properties_add_list(props, "processing_height",
							     obs_module_text("ProcessingResolution"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(resolution, obs_module_text("ProcessingResolution.Source"), 0);
	obs_property_list_add_int(resolution, "2160p (up to 3840 x 2160)", 2160);
	obs_property_list_add_int(resolution, "1440p (up to 2560 x 1440)", 1440);
	obs_property_list_add_int(resolution, "1080p (up to 1920 x 1080)", 1080);
	obs_property_list_add_int(resolution, "720p (up to 1280 x 720)", 720);
	obs_property_set_long_description(resolution, obs_module_text("ProcessingResolution.Tip"));

	obs_property_t *nr_fps = obs_properties_add_list(props, "nr_fps", obs_module_text("NRFps"), OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(nr_fps, obs_module_text("NRFps.Source"), 0);
	obs_property_list_add_int(nr_fps, "60", 60);
	obs_property_list_add_int(nr_fps, "30", 30);
	obs_property_list_add_int(nr_fps, "24", 24);
	obs_property_list_add_int(nr_fps, "15", 15);
	obs_property_set_long_description(nr_fps, obs_module_text("NRFps.Tip"));

	obs_properties_add_button(props, "reset_history", obs_module_text("ResetHistory.Button"),
				  dlss5nr_reset_history_cb);

	return props;
}

static void dlss5nr_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "style", DLSSNR_STYLE_NATURAL);
	obs_data_set_default_int(settings, "preset", 3);
	obs_data_set_default_double(settings, "intensity", 1.0);
	obs_data_set_default_double(settings, "tone", 1.0);
	obs_data_set_default_double(settings, "structure", 1.0);
	obs_data_set_default_double(settings, "skin", -1.0);
	obs_data_set_default_bool(settings, "automask", false);
	obs_data_set_default_bool(settings, "temporal", false);
	obs_data_set_default_int(settings, "gpu_index", 0);
	obs_data_set_default_bool(settings, "gpu_zero_copy", false);
	obs_data_set_default_bool(settings, "diagnostics", false);
	obs_data_set_default_bool(settings, "async_mode", true);
	obs_data_set_default_int(settings, "nr_fps", 0);
	obs_data_set_default_int(settings, "processing_height", 1080);
	obs_data_set_default_int(settings, "channel_order", DLSSNR_CHANNEL_AUTO);
	obs_data_set_default_string(settings, "status", "");
}

static obs_source_info dlss5nr_filter_info = {
	.id = "obs-dlss5-nr-filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = dlss5nr_get_name,
	.create = dlss5nr_create,
	.destroy = dlss5nr_destroy,
	.get_width = dlss5nr_width,
	.get_height = dlss5nr_height,
	.get_defaults = dlss5nr_defaults,
	.get_properties = dlss5nr_properties,
	.update = dlss5nr_update,
	.video_render = dlss5nr_video_render,
	.icon_type = OBS_ICON_TYPE_UNKNOWN,
};

void register_dlss5nr_filter(void)
{
	obs_register_source(&dlss5nr_filter_info);
	blog(LOG_INFO, "[obs-dlss5-nr] filter registered");
}
