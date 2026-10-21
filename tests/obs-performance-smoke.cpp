// Run against an installed OBS runtime, using a synthetic animated 4K source.
// No user's scene collection or capture device is opened or changed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <obs.h>
#include <graphics/vec4.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static std::atomic<uint64_t> received{0}, changed{0}, nonblack{0};
static uint64_t previous_hash = 0;

static const char *source_name(void *)
{
	return "Performance test pattern";
}
static void *source_create(obs_data_t *, obs_source_t *)
{
	return new int(0);
}
static void source_destroy(void *p)
{
	delete static_cast<int *>(p);
}
static uint32_t source_width(void *)
{
	return 3840;
}
static uint32_t source_height(void *)
{
	return 2160;
}
static void source_render(void *, gs_effect_t *)
{
	const uint64_t tick = obs_get_video_frame_time() / 33333333;
	vec4 color;
	vec4_set(&color, 0.15f + float(tick % 40) / 80.0f, 0.35f, 0.65f, 1.0f);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(nullptr, 0, 3840, 2160);
	vec4_set(&color, 0.75f, 0.2f + float(tick % 30) / 60.0f, 0.2f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(nullptr, 0, 1920, 1080);
}

static void receive_frame(void *, video_data *frame)
{
	uint64_t hash = 1469598103934665603ULL;
	uint64_t light = 0;
	for (uint32_t y = 0; y < 720; y += 47) {
		const uint8_t *row = frame->data[0] + y * frame->linesize[0];
		for (uint32_t x = 0; x < 1280; x += 43) {
			for (uint32_t c = 0; c < 3; ++c) {
				hash = (hash ^ row[x * 4 + c]) * 1099511628211ULL;
				light += row[x * 4 + c];
			}
		}
	}
	++received;
	if (light > 0)
		++nonblack;
	if (hash != previous_hash)
		++changed;
	previous_hash = hash;
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		std::fprintf(stderr, "usage: smoke plugin.dll data-directory obs-module-config-directory scenario\n");
		return 2;
	}
	if (!obs_startup("en-US", argv[3], nullptr))
		return 3;
	obs_add_data_path("C:/Program Files/obs-studio/data/libobs");
	obs_video_info video{};
	video.graphics_module = "C:/Program Files/obs-studio/bin/64bit/libobs-d3d11.dll";
	video.fps_num = 30;
	video.fps_den = 1;
	video.base_width = 3840;
	video.base_height = 2160;
	video.output_width = 1280;
	video.output_height = 720;
	video.output_format = VIDEO_FORMAT_BGRA;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_FULL;
	video.scale_type = OBS_SCALE_BILINEAR;
	if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS)
		return 4;
	obs_module_t *module = nullptr;
	std::string plugin_path = argv[1];
	std::replace(plugin_path.begin(), plugin_path.end(), '\\', '/');
	if (obs_open_module(&module, plugin_path.c_str(), argv[2]) != MODULE_SUCCESS || !obs_init_module(module))
		return 5;
	obs_source_info source{};
	source.id = "nr-performance-pattern";
	source.type = OBS_SOURCE_TYPE_INPUT;
	source.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	source.get_name = source_name;
	source.create = source_create;
	source.destroy = source_destroy;
	source.get_width = source_width;
	source.get_height = source_height;
	source.video_render = source_render;
	obs_register_source(&source);
	obs_source_t *pattern = obs_source_create_private(source.id, "4K pattern", nullptr);
	obs_data_t *settings = obs_data_create();
	const int scenario = std::atoi(argv[4]);
	obs_data_set_bool(settings, "async_mode", scenario != 0 && scenario != 3 && scenario != 7);
	obs_data_set_bool(settings, "temporal", scenario == 0 || scenario == 2 || scenario == 5);
	obs_data_set_int(settings, "processing_height", scenario == 0 ? 0 : scenario == 5 ? 720 : 1080);
	obs_data_set_int(settings, "nr_fps", 30);
	obs_data_set_bool(settings, "diagnostics", scenario >= 6);
	obs_source_t *filter = obs_source_create_private("obs-dlss5-nr-filter", "NR smoke", settings);
	if (!filter || !pattern)
		return 6;
	obs_source_filter_add(pattern, filter);
	obs_set_output_source(0, pattern);
	obs_add_raw_video_callback(nullptr, receive_frame, nullptr);
	// Model construction is excluded from the steady-state window.
	std::this_thread::sleep_for(std::chrono::seconds(5));
	const auto before_frames = received.load(), before_changed = changed.load(), before_black = nonblack.load();
	const auto before_total = obs_get_total_frames(), before_lagged = obs_get_lagged_frames();
	std::this_thread::sleep_for(std::chrono::seconds(10));
	const auto frames = received.load() - before_frames;
	const auto changes = changed.load() - before_changed;
	const auto light = nonblack.load() - before_black;
	std::printf("RESULT scenario=%d frames=%llu changed=%llu nonblack=%llu render_frames=%u lagged=%u\n", scenario,
		    (unsigned long long)frames, (unsigned long long)changes, (unsigned long long)light,
		    obs_get_total_frames() - before_total, obs_get_lagged_frames() - before_lagged);
	obs_properties_t *props = obs_source_properties(filter);
	const auto *resolution = obs_properties_get(props, "processing_height");
	const bool dropdown_ok =
		scenario == 0 ||
		(resolution && obs_property_list_item_count(const_cast<obs_property_t *>(resolution)) == 5);
	obs_properties_destroy(props);
	obs_data_t *current = obs_source_get_settings(filter);
	const std::string status = obs_data_get_string(current, "status");
	std::printf("STATUS %s\n", status.c_str());
	const bool enhanced = status.find("enhanced") != std::string::npos;
	obs_data_release(current);
	bool switches_ok = true;
	if (scenario == 6 || scenario == 7) {
		// This run uses fake-nr-runtime.dll copied under an isolated config root.
		// The plugin must report unsupported and keep the animated input live.
		switches_ok = status.find("FeatureNotSupported") != std::string::npos;
		auto count = reinterpret_cast<unsigned(__cdecl *)()>(
			GetProcAddress(GetModuleHandleW(L"nvngx_dlssnr.dll"), "NRTest_GetCreateCount"));
		const unsigned initial_count = count ? count() : 0;
		switches_ok &= initial_count >= 3 && initial_count <= 4;
		props = obs_source_properties(filter);
		auto *retry = obs_properties_get(props, "retry_feature");
		switches_ok &= retry && obs_property_button_clicked(retry, filter);
		obs_properties_destroy(props);
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const unsigned manual_count = count ? count() : 0;
		switches_ok &= manual_count > initial_count && manual_count <= initial_count + 2;
		obs_data_set_int(settings, "style", 2);
		obs_source_update(filter, settings);
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const unsigned style_count = count ? count() : 0;
		switches_ok &= style_count == manual_count + 1;
		std::printf("CREATION-COUNTS initial=%u manual=%u style=%u\n", initial_count, manual_count,
			    style_count);
		std::printf("FAILURE-PATH unsupported=%d pass-through-changes=%llu\n", switches_ok,
			    (unsigned long long)changes);
	}
	if (scenario == 4) {
		// Repeated resize/mode changes must not publish stale or empty textures.
		for (int height : {720, 1440, 2160, 0, 1080}) {
			obs_data_set_int(settings, "processing_height", height);
			obs_data_set_bool(settings, "async_mode", height != 1440);
			obs_source_update(filter, settings);
			std::this_thread::sleep_for(std::chrono::seconds(6));
			props = obs_source_properties(filter);
			obs_properties_destroy(props);
			current = obs_source_get_settings(filter);
			const std::string resize_status = obs_data_get_string(current, "status");
			std::printf("SWITCH height=%d status=%s\n", height, resize_status.c_str());
			std::fflush(stdout);
			const int effective_height = height ? height : 2160;
			const std::string expected = "NR " + std::to_string(effective_height * 16 / 9) + "x" +
						     std::to_string(effective_height);
			switches_ok &= resize_status.find(expected) != std::string::npos &&
				       resize_status.find("enhanced") != std::string::npos;
			obs_data_release(current);
			switches_ok &= obs_source_get_width(filter) == 3840 && obs_source_get_height(filter) == 2160;
		}
	}
	obs_remove_raw_video_callback(receive_frame, nullptr);
	obs_set_output_source(0, nullptr);
	obs_source_filter_remove(pattern, filter);
	obs_source_release(filter);
	obs_source_release(pattern);
	obs_data_release(settings);
	obs_shutdown();
	const bool failure_test = scenario == 6 || scenario == 7;
	const bool rate_ok = (scenario == 1 || scenario == 5 || failure_test) ? changes >= 270 : changes >= 20;
	const int exit_code = frames >= 100 && light == frames && rate_ok && dropdown_ok && switches_ok &&
					      (enhanced || scenario == 0 || failure_test)
				      ? 0
				      : 7;
	std::printf("SMOKE-EXIT %d\n", exit_code);
	std::fflush(stdout);
	return exit_code;
}
