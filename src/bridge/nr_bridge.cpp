// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors
// Adapted for obs-dlss5-nr (GPL-2.0 project) by Saganaki22, 2026.
//
// D3D12 + NVIDIA NGX host for DLSS Neural Rendering (feature 18). The public
// NGX SDK does not document this feature; the parameter keys and call flow
// follow NVIDIA's NGX/DLSS SDK conventions, with the snippet-specific ABI
// handling recovered from shipping runtime behavior.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nr_bridge.h"
#include "nvof_flow.h"
#include "ngx-diagnostics.h"
#include <util/base.h>

using Microsoft::WRL::ComPtr;
using NGXResult = int;
static constexpr NGXResult NGX_SUCCESS = 1;
static constexpr int NR_FEATURE_ID = 18;
static constexpr unsigned long long APP_ID = 141959980ULL;
static constexpr const char *PROJECT_ID = "53f803cc-a12f-4d69-90d5-19b7599cad19";
static constexpr const wchar_t *SHIM_NAME = L"nvngx.dll_obs.dll";
// A blocked graphics thread is a stopped stream: fail open quickly instead of
// stalling for seconds.
static constexpr DWORD FENCE_TIMEOUT_MS = 200;

struct NGXHandle {
	unsigned int Id;
};

// Minimal ABI-compatible interface used by the NVIDIA NGX parameter object.
struct NGXParameter {
	virtual void Set(const char *, unsigned long long) = 0;
	virtual void Set(const char *, float) = 0;
	virtual void Set(const char *, double) = 0;
	virtual void Set(const char *, unsigned int) = 0;
	virtual void Set(const char *, int) = 0;
	virtual void Set(const char *, ID3D11Resource *) = 0;
	virtual void Set(const char *, ID3D12Resource *) = 0;
	virtual void Set(const char *, void *) = 0;
	virtual NGXResult Get(const char *, unsigned long long *) const = 0;
	virtual NGXResult Get(const char *, float *) const = 0;
	virtual NGXResult Get(const char *, double *) const = 0;
	virtual NGXResult Get(const char *, unsigned int *) const = 0;
	virtual NGXResult Get(const char *, int *) const = 0;
	virtual NGXResult Get(const char *, ID3D11Resource **) const = 0;
	virtual NGXResult Get(const char *, ID3D12Resource **) const = 0;
	virtual NGXResult Get(const char *, void **) const = 0;
	virtual void Reset() = 0;
};

using InitExtFn = NGXResult(__cdecl *)(unsigned long long, const wchar_t *, ID3D12Device *, int, const void *);
using SnippetInitFn = NGXResult(__cdecl *)(unsigned long long, const wchar_t *, ID3D12Device *, const void *, int);
using InitProjectIdFn = NGXResult(__cdecl *)(const char *, int, const char *, const wchar_t *, ID3D12Device *, int,
					     const void *);
using AllocParamsFn = NGXResult(__cdecl *)(NGXParameter **);
using CreateFeatureFn = NGXResult(__cdecl *)(ID3D12GraphicsCommandList *, int, NGXParameter *, NGXHandle **);
using EvaluateFeatureFn = NGXResult(__cdecl *)(ID3D12GraphicsCommandList *, const NGXHandle *, const NGXParameter *,
					       void *);
using ReleaseFeatureFn = NGXResult(__cdecl *)(NGXHandle *);
using ShutdownFn = NGXResult(__cdecl *)();

using ShimInitFn = NGXResult(__cdecl *)(void *, unsigned long long, const wchar_t *, ID3D12Device *, int, const void *);
using ShimCreateFn = NGXResult(__cdecl *)(void *, ID3D12GraphicsCommandList *, int, NGXParameter *, NGXHandle **);
using ShimEvaluateFn = NGXResult(__cdecl *)(void *, ID3D12GraphicsCommandList *, const NGXHandle *,
					    const NGXParameter *, void *);
using ShimReleaseFn = NGXResult(__cdecl *)(void *, NGXHandle *);

static std::mutex g_mutex;
static std::string g_last_error;
static std::string g_gpu_name = "unknown";
static std::wstring g_runtime_dir;
static int g_gpu_index = 0;
static std::atomic<bool> g_initialized{false};
static std::atomic<bool> g_diagnostics{false};
static std::atomic<uint64_t> g_retry_generation{0};
static ngx_diagnostics::CreateRetry g_create_retry;
static std::string g_create_error;

static HMODULE g_core_mod = nullptr;
static HMODULE g_nr_mod = nullptr;
static HMODULE g_shim_mod = nullptr;
static InitExtFn g_core_init_ext = nullptr;
static InitProjectIdFn g_core_init_project = nullptr;
static AllocParamsFn g_alloc_params = nullptr;
static CreateFeatureFn g_core_create = nullptr;
static EvaluateFeatureFn g_core_eval = nullptr;
static ReleaseFeatureFn g_core_release = nullptr;
static ShutdownFn g_core_shutdown = nullptr;
static SnippetInitFn g_nr_init = nullptr;
static CreateFeatureFn g_nr_create = nullptr;
static EvaluateFeatureFn g_nr_eval = nullptr;
static ReleaseFeatureFn g_nr_release = nullptr;
using SetLogCallbackFn = void(__cdecl *)(NGXLogCallback);
static SetLogCallbackFn g_set_log_callback = nullptr;
static NGXLogCallback g_log_proxy = nullptr;
static ShimInitFn g_shim_init = nullptr;
static ShimCreateFn g_shim_create = nullptr;
static ShimEvaluateFn g_shim_eval = nullptr;
static ShimReleaseFn g_shim_release = nullptr;

static ComPtr<ID3D12Device> g_device;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<ID3D12CommandAllocator> g_cmd_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_cmd;
static ComPtr<ID3D12Fence> g_fence;
static UINT64 g_fence_value = 0;

static NGXParameter *g_params = nullptr;
static NGXHandle *g_feature = nullptr;
static ComPtr<ID3D12Resource> g_color;
static ComPtr<ID3D12Resource> g_output;
static ComPtr<ID3D12Resource> g_upload;
static ComPtr<ID3D12Resource> g_readback;
// Temporal mode: full-resolution R16G16_FLOAT UV motion vectors estimated by
// NVIDIA Optical Flow from the raw input frames (CPU path only).
static ComPtr<ID3D12Resource> g_mvec;
static ComPtr<ID3D12Resource> g_mvec_upload;
static UINT g_mvec_row_pitch = 0;
static UINT64 g_mvec_total_bytes = 0;
static ComPtr<IDXGIAdapter1> g_adapter;
static UINT g_width = 0, g_height = 0, g_row_pitch = 0;
static UINT64 g_total_bytes = 0;
static int g_feature_style = -999;
static int g_feature_preset = -999;
static int g_feature_ui_correction = -999;
// Temporal setting latched at feature creation: -1 unknown, 0 off, 1 on.
static int g_feature_motion = -1;
// Channel-order verdict is stable per feature build: -1 unknown, 0 RGBA, 1 BGRA.
static int g_slots_are_bgra_cache = -1;

// ---- zero-copy interop state -------------------------------------------
// OBS side owns two legacy-shared D3D11 textures it cannot open in D3D12
// itself (libobs uses legacy DXGI sharing). A helper D3D11 device on the same
// GPU relays pixels between the OBS-visible textures and NTHANDLE-shared
// textures that D3D12/NGX can consume directly. No pixel ever crosses the CPU.
static bool g_gpu_mode = false;
static UINT g_shared_w = 0, g_shared_h = 0;
// Per-stage timing, logged as running averages every N frames.
static double g_ms_relay = 0.0, g_ms_eval = 0.0, g_ms_publish = 0.0;
static uint32_t g_ms_frames = 0;
static LARGE_INTEGER g_qpc_freq{};
static std::string g_timing_text;
static ComPtr<ID3D11Device> g_helper_device;
static ComPtr<ID3D11DeviceContext> g_helper_ctx;
static ComPtr<ID3D11Texture2D> g_obs_in; // opened view of the OBS input texture
static IDXGIKeyedMutex *g_obs_in_km = nullptr;
static ComPtr<ID3D11Texture2D> g_relay_in;  // NTHANDLE shared, opened in D3D12
static ComPtr<ID3D11Texture2D> g_relay_out; // NTHANDLE shared, written by NGX
static IDXGIKeyedMutex *g_relay_in_km = nullptr;
static IDXGIKeyedMutex *g_relay_out_km = nullptr;
static ComPtr<ID3D11Texture2D> g_obs_out; // legacy shared, drawn by OBS
static HANDLE g_relay_in_nt = nullptr;
static HANDLE g_relay_out_nt = nullptr;
static ComPtr<ID3D12Resource> g_shared_color;  // = g_relay_in in D3D12
static ComPtr<ID3D12Resource> g_shared_output; // = g_relay_out in D3D12

static ComPtr<IDXGIAdapter1> FindAdapter(int nvidia_index)
{
	ComPtr<IDXGIFactory4> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return nullptr;
	int seen = 0;
	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 desc{};
		adapter->GetDesc1(&desc);
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE)
			continue;
		if (seen++ == nvidia_index)
			return adapter;
	}
	return nullptr;
}

static void ReleaseSharedState()
{
	if (g_obs_in_km) {
		g_obs_in_km->Release();
		g_obs_in_km = nullptr;
	}
	if (g_relay_in_km) {
		g_relay_in_km->Release();
		g_relay_in_km = nullptr;
	}
	if (g_relay_out_km) {
		g_relay_out_km->Release();
		g_relay_out_km = nullptr;
	}
	g_obs_in.Reset();
	g_relay_in.Reset();
	g_relay_out.Reset();
	g_obs_out.Reset();
	g_shared_color.Reset();
	g_shared_output.Reset();
	if (g_relay_in_nt) {
		CloseHandle(g_relay_in_nt);
		g_relay_in_nt = nullptr;
	}
	if (g_relay_out_nt) {
		CloseHandle(g_relay_out_nt);
		g_relay_out_nt = nullptr;
	}
	g_helper_ctx.Reset();
	g_helper_device.Reset();
	g_shared_w = g_shared_h = 0;
	g_gpu_mode = false;
}

static void SetError(const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_last_error = buf;
}

static std::wstring Join(const std::wstring &a, const std::wstring &b)
{
	if (a.empty())
		return b;
	wchar_t c = a.back();
	if (c == L'\\' || c == L'/')
		return a + b;
	return a + L"\\" + b;
}

static bool FileExists(const std::wstring &p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned long long FileTimeKey(const FILETIME &ft)
{
	ULARGE_INTEGER u{};
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return u.QuadPart;
}

static HMODULE LoadCoreNGX(const std::wstring &runtime)
{
	// 1) Explicit local override (quickest workaround if DriverStore
	// auto-discovery ever misses a vendor-specific INF name).
	const std::wstring local = Join(runtime, L"_nvngx.dll");
	if (FileExists(local)) {
		if (HMODULE m = LoadLibraryW(local.c_str()))
			return m;
	}

	// 2) Normal loader search (works on systems where NVIDIA exposes it).
	if (HMODULE m = LoadLibraryW(L"_nvngx.dll"))
		return m;

	// 3) NVIDIA ships NGX core inside the active display-driver package in
	// DriverStore. The INF prefix varies by OEM/package generation, so scan
	// every NVIDIA-looking *.inf_* package and prefer the newest.
	wchar_t windows_dir[MAX_PATH] = {};
	UINT windows_len = GetWindowsDirectoryW(windows_dir, MAX_PATH);
	if (windows_len == 0 || windows_len >= MAX_PATH)
		return nullptr;
	const std::wstring repo = std::wstring(windows_dir) + L"\\System32\\DriverStore\\FileRepository";
	const std::wstring pat = repo + L"\\nv*.inf_*";

	struct Candidate {
		std::wstring path;
		unsigned long long stamp;
	};
	std::vector<Candidate> candidates;

	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW(pat.c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				continue;
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
				continue;

			std::wstring candidate = repo + L"\\" + fd.cFileName + L"\\_nvngx.dll";
			WIN32_FILE_ATTRIBUTE_DATA fad{};
			if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad) &&
			    !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				candidates.push_back({candidate, FileTimeKey(fad.ftLastWriteTime)});
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}

	std::sort(candidates.begin(), candidates.end(),
		  [](const Candidate &a, const Candidate &b) { return a.stamp > b.stamp; });

	for (const Candidate &c : candidates) {
		if (HMODULE m = LoadLibraryW(c.path.c_str()))
			return m;
	}

	return nullptr;
}

static ComPtr<ID3D12Device> CreateDevice(int nvidia_index)
{
	ComPtr<IDXGIFactory4> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return nullptr;

	int seen = 0;
	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 desc{};
		adapter->GetDesc1(&desc);
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE)
			continue;
		if (seen++ != nvidia_index)
			continue;
		char gpu_utf8[512] = {};
		WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu_utf8, static_cast<int>(sizeof(gpu_utf8)),
				    nullptr, nullptr);
		if (gpu_utf8[0])
			g_gpu_name = gpu_utf8;
		ComPtr<ID3D12Device> d;
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d))))
			return d;
		return nullptr;
	}
	return nullptr;
}

static bool SetupD3D12()
{
	g_device = CreateDevice(g_gpu_index);
	if (!g_device) {
		SetError("Could not create a D3D12 device for NVIDIA GPU index %d", g_gpu_index);
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC q{};
	q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&g_queue)))) {
		SetError("CreateCommandQueue failed");
		return false;
	}
	if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_alloc)))) {
		SetError("CreateCommandAllocator failed");
		return false;
	}
	if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmd_alloc.Get(), nullptr,
					       IID_PPV_ARGS(&g_cmd)))) {
		SetError("CreateCommandList failed");
		return false;
	}
	if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
		SetError("CreateFence failed");
		return false;
	}
	return true;
}

static bool ExecuteAndWait()
{
	HRESULT hr = g_cmd->Close();
	if (FAILED(hr)) {
		SetError("CommandList::Close failed (0x%08X)", static_cast<unsigned>(hr));
		return false;
	}
	ID3D12CommandList *lists[] = {g_cmd.Get()};
	g_queue->ExecuteCommandLists(1, lists);
	++g_fence_value;
	hr = g_queue->Signal(g_fence.Get(), g_fence_value);
	if (FAILED(hr)) {
		SetError("Queue::Signal failed (0x%08X)", static_cast<unsigned>(hr));
		return false;
	}
	if (g_fence->GetCompletedValue() < g_fence_value) {
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!ev) {
			SetError("CreateEvent failed");
			return false;
		}
		g_fence->SetEventOnCompletion(g_fence_value, ev);
		DWORD w = WaitForSingleObject(ev, FENCE_TIMEOUT_MS);
		CloseHandle(ev);
		if (w != WAIT_OBJECT_0) {
			SetError("Timed out waiting for DLSS NR GPU work (%u ms)", FENCE_TIMEOUT_MS);
			return false;
		}
	}
	g_cmd_alloc->Reset();
	g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
	return true;
}

static void WaitQueueIdle()
{
	if (!g_queue || !g_fence)
		return;
	++g_fence_value;
	if (SUCCEEDED(g_queue->Signal(g_fence.Get(), g_fence_value)) && g_fence->GetCompletedValue() < g_fence_value) {
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (ev) {
			g_fence->SetEventOnCompletion(g_fence_value, ev);
			WaitForSingleObject(ev, FENCE_TIMEOUT_MS);
			CloseHandle(ev);
		}
	}
}

static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = r;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	return b;
}

static ComPtr<ID3D12Resource> CreateTexture(UINT w, UINT h, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
					    DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT)
{
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	d.Flags = flags;
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	ComPtr<ID3D12Resource> r;
	if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
		return nullptr;
	return r;
}

static ComPtr<ID3D12Resource> CreateLinearBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	d.Width = bytes;
	d.Height = 1;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = type;
	ComPtr<ID3D12Resource> r;
	if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
		return nullptr;
	return r;
}

static uint16_t FloatToHalf(float f)
{
	uint32_t x;
	memcpy(&x, &f, sizeof(x));
	uint32_t s = (x >> 16) & 0x8000u;
	int32_t e = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
	uint32_t m = x & 0x7fffffu;
	if (e <= 0) {
		if (e < -10)
			return static_cast<uint16_t>(s);
		m = (m | 0x800000u) >> (1 - e);
		return static_cast<uint16_t>(s | (m >> 13));
	}
	if (e >= 31)
		return static_cast<uint16_t>(s | 0x7c00u);
	return static_cast<uint16_t>(s | (static_cast<uint32_t>(e) << 10) | (m >> 13));
}

// 8-bit -> half conversion is exact and range-limited: precompute it.
static uint16_t g_lut8_to_half[256];
static bool g_lut8_ready = false;

static float HalfToFloat(uint16_t h)
{
	uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
	if (e == 0) {
		if (m == 0) {
			x = s << 31;
		} else {
			e = 1;
			while (!(m & 0x400)) {
				m <<= 1;
				--e;
			}
			m &= 0x3ff;
			x = (s << 31) | ((e + 112) << 23) | (m << 13);
		}
	} else if (e == 0x1f) {
		x = (s << 31) | 0x7f800000u | (m << 13);
	} else {
		x = (s << 31) | ((e + 112) << 23) | (m << 13);
	}
	float f;
	memcpy(&f, &x, sizeof(f));
	return f;
}

static bool UploadMotionVectorTexture(const NvofFlowFrame *flow, bool execute_now)
{
	if (!g_mvec || !g_mvec_upload || g_width == 0 || g_height == 0) {
		SetError("Motion-vector resources are not allocated");
		return false;
	}

	void *mapped = nullptr;
	HRESULT hr = g_mvec_upload->Map(0, nullptr, &mapped);
	if (FAILED(hr) || !mapped) {
		SetError("Motion-vector upload Map failed: 0x%08X", static_cast<unsigned>(hr));
		return false;
	}
	memset(mapped, 0, static_cast<size_t>(g_mvec_total_bytes));

	if (flow && flow->has_flow) {
		if (flow->width == 0 || flow->height == 0 ||
		    flow->xy.size() < static_cast<size_t>(flow->width) * flow->height * 2) {
			g_mvec_upload->Unmap(0, nullptr);
			SetError("NVIDIA Optical Flow returned an invalid flow field");
			return false;
		}

		// NVOF returns S10.5 current->previous displacement in pixels. DLSS's
		// contract stores normalized UV and multiplies by MVecScale=(W,H), so:
		//   stored = fixed / 32 / axis_size
		// Use nearest reconstruction from the NVOF grid to avoid inventing
		// vectors across disocclusion boundaries.
		auto *dst_base = static_cast<uint8_t *>(mapped);
		for (UINT y = 0; y < g_height; ++y) {
			auto *row = reinterpret_cast<uint16_t *>(dst_base + static_cast<size_t>(y) * g_mvec_row_pitch);
			const UINT cy =
				std::min<UINT>(static_cast<UINT>((static_cast<uint64_t>(y) * flow->height) / g_height),
					       flow->height - 1);
			for (UINT x = 0; x < g_width; ++x) {
				const UINT cx = std::min<UINT>(
					static_cast<UINT>((static_cast<uint64_t>(x) * flow->width) / g_width),
					flow->width - 1);
				const size_t i = (static_cast<size_t>(cy) * flow->width + cx) * 2;
				const float uvx = static_cast<float>(flow->xy[i + 0]) * (1.0f / 32.0f) /
						  static_cast<float>(g_width);
				const float uvy = static_cast<float>(flow->xy[i + 1]) * (1.0f / 32.0f) /
						  static_cast<float>(g_height);
				row[x * 2 + 0] = FloatToHalf(uvx);
				row[x * 2 + 1] = FloatToHalf(uvy);
			}
		}
	}
	g_mvec_upload->Unmap(0, nullptr);

	auto to_copy =
		Barrier(g_mvec.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	g_cmd->ResourceBarrier(1, &to_copy);

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = g_mvec.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = g_mvec_upload.Get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
	src.PlacedFootprint.Footprint.Width = g_width;
	src.PlacedFootprint.Footprint.Height = g_height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = g_mvec_row_pitch;
	g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	auto to_read =
		Barrier(g_mvec.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	g_cmd->ResourceBarrier(1, &to_read);

	return !execute_now || ExecuteAndWait();
}

static void ReleaseFeatureAndResources()
{
	WaitQueueIdle();
	if (g_feature) {
		if (g_nr_release && g_shim_release)
			g_shim_release(reinterpret_cast<void *>(g_nr_release), g_feature);
		else if (g_core_release)
			g_core_release(g_feature);
		g_feature = nullptr;
	}
	// In GPU mode the shared textures outlive feature rebuilds; only the
	// CPU-staging resources are owned here.
	// NGX parameter objects do not necessarily AddRef resources stored in them.
	if (g_params)
		g_params->Set("DLSSNR.MVec", static_cast<ID3D12Resource *>(nullptr));
	g_color.Reset();
	g_output.Reset();
	g_upload.Reset();
	g_readback.Reset();
	g_mvec.Reset();
	g_mvec_upload.Reset();
	g_mvec_row_pitch = 0;
	g_mvec_total_bytes = 0;
	if (!g_gpu_mode)
		g_width = g_height = g_row_pitch = 0;
	g_total_bytes = 0;
	g_feature_style = -999;
	g_feature_preset = -999;
	g_feature_ui_correction = -999;
	g_feature_motion = -1;
}

static bool AllocateFrameResources(UINT w, UINT h, bool temporal)
{
	g_color = CreateTexture(w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	g_output =
		CreateTexture(w, h, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	if (!g_color || !g_output) {
		SetError("Failed to create RGBA16F D3D12 textures");
		return false;
	}

	g_row_pitch = (w * 8u + 255u) & ~255u;
	g_total_bytes = static_cast<UINT64>(g_row_pitch) * h;
	g_upload = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
	g_readback = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!g_upload || !g_readback) {
		SetError("Failed to create D3D12 upload/readback buffers");
		return false;
	}
	g_width = w;
	g_height = h;

	if (temporal) {
		g_mvec = CreateTexture(w, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE,
				       DXGI_FORMAT_R16G16_FLOAT);
		if (!g_mvec) {
			SetError("Failed to create R16G16_FLOAT motion-vector texture");
			return false;
		}
		g_mvec_row_pitch = (w * 4u + 255u) & ~255u;
		g_mvec_total_bytes = static_cast<UINT64>(g_mvec_row_pitch) * h;
		g_mvec_upload = CreateLinearBuffer(g_mvec_total_bytes, D3D12_HEAP_TYPE_UPLOAD,
						   D3D12_RESOURCE_STATE_GENERIC_READ);
		if (!g_mvec_upload) {
			SetError("Failed to create motion-vector upload buffer");
			return false;
		}
		// Feature creation always starts with a defined zero-MV resource. The
		// first temporal frame also uses this; NVOF has no previous frame yet.
		if (!UploadMotionVectorTexture(nullptr, true))
			return false;
	}
	return true;
}

static void SetCommonParams(int style, int preset, float intensity, float tone, float structure, float skin,
			    int automask, int ui_correction, int reset, bool temporal)
{
	ID3D12Resource *color = g_gpu_mode ? g_shared_color.Get() : g_color.Get();
	ID3D12Resource *output = g_gpu_mode ? g_shared_output.Get() : g_output.Get();
	g_params->Set("DLSSNR.Width", g_width);
	g_params->Set("DLSSNR.Height", g_height);
	g_params->Set("DLSSNR.Enabled", 1);
	g_params->Set("DLSSNR.Reset", reset);
	g_params->Set("DLSSNR.Style", style);
	g_params->Set("DLSSNR.Hint.Render.Preset", preset);
	g_params->Set("DLSSNR.Intensity", intensity);
	g_params->Set("DLSSNR.LocalToneStrength", tone);
	g_params->Set("DLSSNR.LocalStructureStrength", structure);
	g_params->Set("DLSSNR.SkinStructureStrength", skin);
	g_params->Set("DLSSNR.UseAutoMask", automask);
	g_params->Set("DLSSNR.UICorrection", ui_correction);
	g_params->Set("DLSSNR.DepthInverted", 1);
	g_params->Set("DLSSNR.ScalingRatio", 1.0f);
	if (temporal && g_mvec) {
		g_params->Set("DLSSNR.MVec", g_mvec.Get());
		// g_mvec stores normalized UV. Multiplying by dimensions reconstructs
		// the original pixel displacement from NVOF exactly.
		g_params->Set("DLSSNR.MVecScaleX", static_cast<float>(g_width));
		g_params->Set("DLSSNR.MVecScaleY", static_cast<float>(g_height));
		g_params->Set("DLSSNR.MVecSubrectBaseX", 0);
		g_params->Set("DLSSNR.MVecSubrectBaseY", 0);
		g_params->Set("DLSSNR.MVecSubrectWidth", g_width);
		g_params->Set("DLSSNR.MVecSubrectHeight", g_height);
	} else {
		g_params->Set("DLSSNR.MVec", static_cast<ID3D12Resource *>(nullptr));
		g_params->Set("DLSSNR.MVecScaleX", 1.0f);
		g_params->Set("DLSSNR.MVecScaleY", 1.0f);
		g_params->Set("DLSSNR.MVecSubrectBaseX", 0);
		g_params->Set("DLSSNR.MVecSubrectBaseY", 0);
		g_params->Set("DLSSNR.MVecSubrectWidth", 0);
		g_params->Set("DLSSNR.MVecSubrectHeight", 0);
	}
	g_params->Set("DLSSNR.Color", color);
	g_params->Set("DLSSNR.Output", output);
	g_params->Set("DLSSNR.Backbuffer", output);
	g_params->Set("DLSSNR.ColorSubrectBaseX", 0);
	g_params->Set("DLSSNR.ColorSubrectBaseY", 0);
	g_params->Set("DLSSNR.ColorSubrectWidth", g_width);
	g_params->Set("DLSSNR.ColorSubrectHeight", g_height);
	g_params->Set("DLSSNR.OutputSubrectBaseX", 0);
	g_params->Set("DLSSNR.OutputSubrectBaseY", 0);
	g_params->Set("DLSSNR.OutputSubrectWidth", g_width);
	g_params->Set("DLSSNR.OutputSubrectHeight", g_height);
}

static void __cdecl NGXLog(const char *message, NGXLoggingLevel, int component)
{
	// NGX can invoke this from its own threads: no bridge lock or NGX calls.
	if (message && g_diagnostics.load(std::memory_order_relaxed))
		blog(LOG_INFO, "[obs-dlss5-nr][NGX:%d] %s", component, message);
}

static void LogModule(const char *label, HMODULE module)
{
	wchar_t path[32768]{};
	if (!GetModuleFileNameW(module, path, ARRAYSIZE(path)))
		return;
	std::string version = "unknown";
	DWORD unused = 0;
	const DWORD bytes = GetFileVersionInfoSizeW(path, &unused);
	std::vector<uint8_t> data(bytes);
	if (bytes && GetFileVersionInfoW(path, 0, bytes, data.data())) {
		VS_FIXEDFILEINFO *info = nullptr;
		UINT size = 0;
		if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &size) && info &&
		    size >= sizeof(*info)) {
			char text[80];
			snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS),
				 LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
				 LOWORD(info->dwFileVersionLS));
			version = text;
		}
	}
	const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
	std::string utf8(utf8_size > 0 ? utf8_size : 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), (int)utf8.size(), nullptr, nullptr);
	WIN32_FILE_ATTRIBUTE_DATA attributes{};
	uint64_t file_bytes = 0;
	if (GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
		file_bytes = (uint64_t(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
	blog(LOG_INFO, "[obs-dlss5-nr] diagnostic %s: version=%s bytes=%llu path=%s", label, version.c_str(),
	     (unsigned long long)file_bytes, utf8.c_str());
}

static void LogCreateParams(UINT w, UINT h, int style, int preset, float intensity, float tone, float structure,
			    float skin, int automask, int ui_correction, bool temporal)
{
	DXGI_ADAPTER_DESC1 adapter{};
	LARGE_INTEGER driver{};
	if (g_adapter) {
		g_adapter->GetDesc1(&adapter);
		g_adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
	}
	blog(LOG_INFO,
	     "[obs-dlss5-nr] CreateFeature(18) attempt: gpu=%s vendor=0x%04X device=0x%04X "
	     "luid=%08X:%08X driver=%u.%u.%u.%u size=%ux%u style=%d preset=%d intensity=%.3f tone=%.3f "
	     "structure=%.3f skin=%.3f automask=%d ui=%d temporal=%d path=%s",
	     g_gpu_name.c_str(), adapter.VendorId, adapter.DeviceId, (unsigned)adapter.AdapterLuid.HighPart,
	     adapter.AdapterLuid.LowPart, HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart),
	     LOWORD(driver.LowPart), w, h, style, preset, intensity, tone, structure, skin, automask, ui_correction,
	     temporal, g_gpu_mode ? "GPU sharing" : "CPU staging");
	for (const auto &resource : {g_gpu_mode ? g_shared_color : g_color, g_gpu_mode ? g_shared_output : g_output}) {
		if (resource) {
			const auto desc = resource->GetDesc();
			blog(LOG_INFO, "[obs-dlss5-nr] CreateFeature resource: format=%u flags=0x%X size=%llux%u",
			     (unsigned)desc.Format, (unsigned)desc.Flags, (unsigned long long)desc.Width, desc.Height);
		}
	}
}

static bool EnsureFeature(UINT w, UINT h, int style, int preset, float intensity, float tone, float structure,
			  float skin, int automask, int ui_correction, bool temporal)
{
	// Style, preset and UI correction are latched by the model at feature
	// creation; changing them requires a rebuild (which also resets the
	// temporal history). Intensity/tone/structure/skin/mask are per-frame.
	const int motion_key = temporal ? 1 : 0;
	const bool rebuild = !g_feature || (!g_gpu_mode && (w != g_width || h != g_height)) ||
			     style != g_feature_style || preset != g_feature_preset ||
			     ui_correction != g_feature_ui_correction || motion_key != g_feature_motion;
	if (!rebuild) {
		SetCommonParams(style, preset, intensity, tone, structure, skin, automask, ui_correction, 0, temporal);
		return true;
	}

	const ngx_diagnostics::CreateKey key{w,
					     h,
					     style,
					     preset,
					     ui_correction,
					     temporal,
					     g_gpu_mode,
					     g_retry_generation.load(std::memory_order_relaxed)};
	if (!g_create_retry.allowed(key, GetTickCount64())) {
		SetError("%s", g_create_error.c_str());
		return false;
	}
	ReleaseFeatureAndResources();
	if (!g_gpu_mode) {
		if (!AllocateFrameResources(w, h, temporal))
			return false;
	} else {
		g_width = g_shared_w;
		g_height = g_shared_h;
	}
	// The runtime latches model parameters at creation; reset the temporal
	// history on the first evaluated frame.
	SetCommonParams(style, preset, intensity, tone, structure, skin, automask, ui_correction, 1, temporal);

	if (g_diagnostics.load(std::memory_order_relaxed))
		LogCreateParams(w, h, style, preset, intensity, tone, structure, skin, automask, ui_correction,
				temporal);
	NGXResult r;
	if (g_nr_create && g_shim_create)
		r = g_shim_create(reinterpret_cast<void *>(g_nr_create), g_cmd.Get(), NR_FEATURE_ID, g_params,
				  &g_feature);
	else
		r = g_core_create(g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
	if (r != NGX_SUCCESS || !g_feature) {
		const char *hint =
			static_cast<uint32_t>(r) == 0xBAD00001u
				? "This GPU/driver/runtime combination does not support the feature; this code alone does not identify which component."
				: "Enable diagnostic logging for creation parameters and module versions.";
		SetError(
			"CreateFeature(18) failed: 0x%08X (%s). %s Retrying at most every 5 s; use Retry Feature to retry now.",
			static_cast<unsigned>(r),
			r == NGX_SUCCESS ? "MissingFeatureHandle"
					 : ngx_diagnostics::result_name(static_cast<uint32_t>(r)),
			hint);
		g_create_error = g_last_error;
		g_create_retry.failure(key, GetTickCount64());
		return false;
	}
	g_create_retry.reset();
	g_create_error.clear();
	g_feature_style = style;
	g_feature_preset = preset;
	g_feature_ui_correction = ui_correction;
	g_feature_motion = motion_key;
	return true;
}

static bool LoadNGX(const std::wstring &shim_dir)
{
	g_core_mod = LoadCoreNGX(g_runtime_dir);
	if (!g_core_mod) {
		SetError("Could not load NVIDIA NGX core _nvngx.dll. Tried runtime\\_nvngx.dll, normal DLL search, "
			 "and NVIDIA DriverStore packages matching nv*.inf_*.");
		return false;
	}

	const std::wstring nr_path = Join(g_runtime_dir, L"nvngx_dlssnr.dll");
	if (!FileExists(nr_path)) {
		SetError("nvngx_dlssnr.dll not found in runtime folder");
		return false;
	}
	g_nr_mod = LoadLibraryW(nr_path.c_str());
	if (!g_nr_mod) {
		SetError("LoadLibrary(nvngx_dlssnr.dll) failed: Win32 %lu", GetLastError());
		return false;
	}

	// Caller shim candidates: explicit dir next to the plugin binary, the
	// runtime dir, then the plain loader search.
	const std::wstring shim_candidates[3] = {
		shim_dir.empty() ? std::wstring() : Join(shim_dir, SHIM_NAME),
		Join(g_runtime_dir, SHIM_NAME),
		SHIM_NAME,
	};
	for (const std::wstring &cand : shim_candidates) {
		if (cand.empty())
			continue;
		g_shim_mod = LoadLibraryW(cand.c_str());
		if (g_shim_mod)
			break;
	}
	if (!g_shim_mod) {
		SetError("caller shim %ls not found (expected next to obs-dlss5-nr.dll or in the runtime folder)",
			 SHIM_NAME);
		return false;
	}

	g_core_init_ext = reinterpret_cast<InitExtFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_Ext"));
	g_core_init_project =
		reinterpret_cast<InitProjectIdFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_ProjectID"));
	g_alloc_params =
		reinterpret_cast<AllocParamsFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_AllocateParameters"));
	g_core_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_CreateFeature"));
	g_core_eval =
		reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
	g_core_release =
		reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));
	g_core_shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Shutdown"));

	g_nr_init = reinterpret_cast<SnippetInitFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_Init_Ext"));
	g_nr_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_CreateFeature"));
	g_nr_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
	g_nr_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));

	g_set_log_callback = reinterpret_cast<SetLogCallbackFn>(GetProcAddress(g_shim_mod, "DLSSNR_SetLogCallback"));
	g_log_proxy = reinterpret_cast<NGXLogCallback>(GetProcAddress(g_shim_mod, "DLSSNR_LogMessage"));
	g_shim_init = reinterpret_cast<ShimInitFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallInit"));
	g_shim_create = reinterpret_cast<ShimCreateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallCreate"));
	g_shim_eval = reinterpret_cast<ShimEvaluateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallEvaluate"));
	g_shim_release = reinterpret_cast<ShimReleaseFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallRelease"));

	if (!g_core_init_ext || !g_alloc_params || !g_core_create || !g_core_eval || !g_core_release ||
	    !g_core_shutdown) {
		SetError("Required NGX core exports are missing");
		return false;
	}
	if (!g_nr_init || !g_nr_create || !g_nr_eval || !g_nr_release) {
		SetError("Required DLSSNR exports are missing from nvngx_dlssnr.dll");
		return false;
	}
	if (!g_shim_init || !g_shim_create || !g_shim_eval || !g_shim_release || !g_set_log_callback || !g_log_proxy) {
		SetError(
			"Required caller shim exports are missing; install the matching plugin and caller shim together");
		return false;
	}
	g_set_log_callback(NGXLog);
	return true;
}

static bool InitNGXSession()
{
	const wchar_t *paths[1] = {g_runtime_dir.c_str()};
	NGXPathListInfo pli{paths, 1};
	NGXFeatureCommonInfo fci{};
	fci.PathListInfo = pli;
	fci.LoggingInfo.Callback = g_log_proxy;
	fci.LoggingInfo.LoggingLevel = g_diagnostics.load() ? NGX_LOG_VERBOSE : NGX_LOG_OFF;
	fci.LoggingInfo.DisableOtherLoggingSinks = true;

	bool core_ok = false;
	if (g_core_init_project) {
		for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
			NGXResult r = g_core_init_project(PROJECT_ID, 0, "0.4.0", g_runtime_dir.c_str(), g_device.Get(),
							  ver, &fci);
			core_ok = (r == NGX_SUCCESS);
		}
	}
	if (!core_ok) {
		for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
			NGXResult r = g_core_init_ext(APP_ID, g_runtime_dir.c_str(), g_device.Get(), ver, &fci);
			core_ok = (r == NGX_SUCCESS);
		}
	}
	if (!core_ok) {
		SetError("NGX core initialization failed for API versions 0x13..0x20");
		return false;
	}

	NGXResult sr = g_shim_init(reinterpret_cast<void *>(g_nr_init), APP_ID, g_runtime_dir.c_str(), g_device.Get(),
				   0x15, &fci);
	if (sr != NGX_SUCCESS) {
		SetError("DLSSNR snippet Init_Ext via caller shim failed: 0x%08X", static_cast<unsigned>(sr));
		return false;
	}

	NGXResult ar = g_alloc_params(&g_params);
	if (ar != NGX_SUCCESS || !g_params) {
		SetError("NVSDK_NGX_D3D12_AllocateParameters failed: 0x%08X", static_cast<unsigned>(ar));
		return false;
	}
	return true;
}

static void ShutdownUnlocked()
{
	if (g_set_log_callback)
		g_set_log_callback(nullptr); // Drain callbacks before this plugin can unload.
	g_create_retry.reset();
	g_create_error.clear();
	ReleaseFeatureAndResources();
	ReleaseSharedState();
	// The OFA session is driver state but not NGX/D3D12-UMD state; the public
	// NVOF API owns its teardown and nvofapi64.dll is safe to unload.
	NvofShutdown();
	if (g_core_shutdown)
		g_core_shutdown();
	g_params = nullptr;
	g_device.Reset();
	g_queue.Reset();
	g_cmd_alloc.Reset();
	g_cmd.Reset();
	g_fence.Reset();
	g_adapter.Reset(); // Release DXGI references before DLL/process-detach loader lock.
	// The NGX core and the NR runtime own driver worker threads and live D3D12
	// state. Unloading them here deadlocks inside the NVIDIA D3D12 UMD:
	// FreeLibrary(g_nr_mod) never returns, which wedges the OBS destroy queue
	// that dlss5nr_destroy runs on, and OBS then hangs on exit waiting in
	// obs_wait_for_destroy_queue. Leave them mapped and let process teardown
	// reclaim them; a later init reloads them and reuses the mapped image.
	g_shim_mod = g_nr_mod = g_core_mod = nullptr;

	g_core_init_ext = nullptr;
	g_core_init_project = nullptr;
	g_alloc_params = nullptr;
	g_core_create = nullptr;
	g_core_eval = nullptr;
	g_core_release = nullptr;
	g_core_shutdown = nullptr;
	g_nr_init = nullptr;
	g_nr_create = nullptr;
	g_nr_eval = nullptr;
	g_nr_release = nullptr;
	g_shim_init = nullptr;
	g_set_log_callback = nullptr;
	g_log_proxy = nullptr;
	g_shim_create = nullptr;
	g_shim_eval = nullptr;
	g_shim_release = nullptr;

	g_initialized = false;
}

// Some runtime builds return R,G,B,A in the output slots, others B,G,R,A.
// Decide per frame by comparing both interpretations of the result against the
// BGRA8 source at low resolution (NR relights the image, but should not swap
// red and blue globally).
static bool OutputSlotsAreBgra(const uint8_t *src_bgra, int src_row_pitch, const uint8_t *out_half_bytes,
			       int out_pitch_bytes, int w, int h)
{
	const int step_y = std::max(1, h / 128);
	const int step_x = std::max(1, w / 128);
	double raw_mae = 0.0, swp_mae = 0.0, raw_mean = 0.0, swp_mean = 0.0, src_mean = 0.0;
	long long n = 0;
	for (int y = 0; y < h; y += step_y) {
		const uint8_t *srow = src_bgra + (size_t)y * src_row_pitch;
		const auto *orow = reinterpret_cast<const uint16_t *>(out_half_bytes + (size_t)y * out_pitch_bytes);
		for (int x = 0; x < w; x += step_x) {
			double sr = srow[x * 4 + 2] / 255.0;
			double sg = srow[x * 4 + 1] / 255.0;
			double sb = srow[x * 4 + 0] / 255.0;
			double o0 = HalfToFloat(orow[x * 4 + 0]);
			double o1 = HalfToFloat(orow[x * 4 + 1]);
			double o2 = HalfToFloat(orow[x * 4 + 2]);
			// raw: slots are R,G,B,A. swapped: slots are B,G,R,A.
			raw_mae += std::fabs(o0 - sr) + std::fabs(o1 - sg) + std::fabs(o2 - sb);
			swp_mae += std::fabs(o2 - sr) + std::fabs(o1 - sg) + std::fabs(o0 - sb);
			raw_mean += o0 + o1 + o2;
			swp_mean += o2 + o1 + o0;
			src_mean += sr + sg + sb;
			++n;
		}
	}
	if (!n)
		return false;
	raw_mae /= (double)n;
	swp_mae /= (double)n;
	src_mean /= (double)n;
	raw_mean /= (double)n;
	swp_mean /= (double)n;
	return (swp_mae + std::fabs(swp_mean - src_mean)) < (raw_mae + std::fabs(raw_mean - src_mean));
}

namespace nrbridge {

bool init(int gpu_index, const wchar_t *runtime_dir, const wchar_t *shim_dir)
{
	std::lock_guard<std::mutex> guard(g_mutex);
	g_last_error.clear();
	if (g_initialized)
		return true;
	if (!runtime_dir || !*runtime_dir) {
		SetError("runtime_dir is empty");
		return false;
	}

	g_gpu_index = gpu_index;
	g_runtime_dir = runtime_dir;
	g_adapter = FindAdapter(gpu_index);
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	if (!g_lut8_ready) {
		for (int i = 0; i < 256; ++i)
			g_lut8_to_half[i] = FloatToHalf(i / 255.0f);
		g_lut8_ready = true;
	}

	std::wstring shim_w = shim_dir ? std::wstring(shim_dir) : std::wstring();
	if (!SetupD3D12() || !LoadNGX(shim_w) || !InitNGXSession()) {
		ShutdownUnlocked();
		return false;
	}
	g_initialized = true;
	if (g_diagnostics.load()) {
		LogModule("NGX core", g_core_mod);
		LogModule("NR runtime", g_nr_mod);
		LogModule("caller shim", g_shim_mod);
	}
	return true;
}

void set_diagnostics(bool enabled)
{
	g_diagnostics.store(enabled, std::memory_order_relaxed);
}

void request_retry()
{
	g_retry_generation.fetch_add(1, std::memory_order_relaxed);
}

bool process(const uint8_t *src_bgra, int src_row_pitch, uint8_t *dst_bgra, int dst_row_pitch, int width, int height,
	     const NrBridgeParams &params)
{
	std::lock_guard<std::mutex> guard(g_mutex);
	g_last_error.clear();
	if (!g_initialized) {
		SetError("NR bridge is not initialized");
		return false;
	}
	// The two paths use mutually exclusive resources; a shared-texture
	// attach means the staging buffers do not exist.
	if (g_gpu_mode) {
		SetError("CPU staging unavailable while GPU path is attached");
		return false;
	}
	if (!src_bgra || !dst_bgra || width <= 0 || height <= 0) {
		SetError("Invalid image buffer/dimensions");
		return false;
	}
	if (width > 16384 || height > 16384) {
		SetError("Image dimensions are unreasonably large");
		return false;
	}

	// Temporal mode needs the CPU staging path: the zero-copy path never has
	// pixels on the CPU to feed the optical-flow estimator.
	const bool temporal = params.temporal != 0 && !g_gpu_mode;
	if (!temporal)
		// Still mode owns no temporal history or OFA resources; this also
		// releases OFA VRAM when the user switches temporal back off.
		NvofReleaseSession();

	if (!EnsureFeature(static_cast<UINT>(width), static_cast<UINT>(height), params.style, params.preset,
			   params.intensity, params.tone, params.structure, params.skin, params.automask,
			   params.ui_correction, temporal)) {
		return false;
	}

	if (temporal) {
		NvofFlowFrame flow;
		std::string of_error;
		if (!NvofPrepareFrame(g_adapter.Get(), src_bgra, src_row_pitch, static_cast<UINT>(width),
				      static_cast<UINT>(height), params.reset != 0, flow, of_error)) {
			SetError("%s", of_error.c_str());
			return false;
		}
		// First frame: no previous frame exists, so this deliberately writes
		// zero MVs. Later frames upload NVOFA current->previous optical flow.
		if (!UploadMotionVectorTexture(flow.has_flow ? &flow : nullptr, false))
			return false;
	}

	SetCommonParams(params.style, params.preset, params.intensity, params.tone, params.structure, params.skin,
			params.automask, params.ui_correction, params.reset ? 1 : 0, temporal);

	// Pack BGRA8 input into the RGBA16F upload buffer. Row padding is never
	// read by footprint copies, so it is left untouched.
	void *mapped = nullptr;
	HRESULT hr = g_upload->Map(0, nullptr, &mapped);
	if (FAILED(hr) || !mapped) {
		SetError("Upload buffer Map failed: 0x%08X", static_cast<unsigned>(hr));
		return false;
	}
	auto *dst_base = static_cast<uint8_t *>(mapped);
	for (int y = 0; y < height; ++y) {
		auto *row = reinterpret_cast<uint16_t *>(dst_base + (size_t)y * g_row_pitch);
		const uint8_t *src = src_bgra + (size_t)y * src_row_pitch;
		for (int x = 0; x < width; ++x) {
			const uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2];
			row[x * 4 + 0] = g_lut8_to_half[r];
			row[x * 4 + 1] = g_lut8_to_half[g];
			row[x * 4 + 2] = g_lut8_to_half[b];
			row[x * 4 + 3] = FloatToHalf(1.0f);
		}
	}
	g_upload->Unmap(0, nullptr);

	auto b1 =
		Barrier(g_color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	g_cmd->ResourceBarrier(1, &b1);
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = g_color.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = g_upload.Get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	src.PlacedFootprint.Footprint.Width = width;
	src.PlacedFootprint.Footprint.Height = height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
	g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	auto b2 =
		Barrier(g_color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	g_cmd->ResourceBarrier(1, &b2);

	NGXResult er = g_shim_eval(reinterpret_cast<void *>(g_nr_eval), g_cmd.Get(), g_feature, g_params, nullptr);
	if (er != NGX_SUCCESS) {
		SetError("DLSSNR EvaluateFeature failed: 0x%08X (%s)", static_cast<unsigned>(er),
			 ngx_diagnostics::result_name(static_cast<uint32_t>(er)));
		ExecuteAndWait(); // reset the command list to a clean state
		return false;
	}

	auto b3 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	g_cmd->ResourceBarrier(1, &b3);
	D3D12_TEXTURE_COPY_LOCATION rd{};
	rd.pResource = g_readback.Get();
	rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	rd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	rd.PlacedFootprint.Footprint.Width = width;
	rd.PlacedFootprint.Footprint.Height = height;
	rd.PlacedFootprint.Footprint.Depth = 1;
	rd.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
	D3D12_TEXTURE_COPY_LOCATION rs{};
	rs.pResource = g_output.Get();
	rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	g_cmd->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
	auto b4 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	g_cmd->ResourceBarrier(1, &b4);

	if (!ExecuteAndWait())
		return false;

	void *rmap = nullptr;
	hr = g_readback->Map(0, nullptr, &rmap);
	if (FAILED(hr) || !rmap) {
		SetError("Readback Map failed: 0x%08X", static_cast<unsigned>(hr));
		return false;
	}
	const auto *out_base = static_cast<const uint8_t *>(rmap);
	const bool slots_are_bgra = OutputSlotsAreBgra(src_bgra, src_row_pitch, out_base, g_row_pitch, width, height);
	for (int y = 0; y < height; ++y) {
		const auto *row = reinterpret_cast<const uint16_t *>(out_base + (size_t)y * g_row_pitch);
		uint8_t *dstp = dst_bgra + (size_t)y * dst_row_pitch;
		for (int x = 0; x < width; ++x) {
			float c0 = std::clamp(HalfToFloat(row[x * 4 + 0]), 0.0f, 1.0f);
			float c1 = std::clamp(HalfToFloat(row[x * 4 + 1]), 0.0f, 1.0f);
			float c2 = std::clamp(HalfToFloat(row[x * 4 + 2]), 0.0f, 1.0f);
			float r = slots_are_bgra ? c2 : c0;
			float b = slots_are_bgra ? c0 : c2;
			dstp[x * 4 + 0] = (uint8_t)(b * 255.0f + 0.5f);
			dstp[x * 4 + 1] = (uint8_t)(c1 * 255.0f + 0.5f);
			dstp[x * 4 + 2] = (uint8_t)(r * 255.0f + 0.5f);
			dstp[x * 4 + 3] = 255;
		}
	}
	g_readback->Unmap(0, nullptr);
	return true;
}

bool attach_shared(uint32_t in_handle, uint32_t width, uint32_t height, uint32_t *out_handle)
{
	std::lock_guard<std::mutex> guard(g_mutex);
	g_last_error.clear();
	if (!g_initialized) {
		SetError("NR bridge is not initialized");
		return false;
	}
	if (!out_handle || width == 0 || height == 0 || width > 16384 || height > 16384) {
		SetError("attach_shared: invalid arguments");
		return false;
	}
	if (g_gpu_mode && g_shared_w == width && g_shared_h == height) {
		*out_handle = 0; // caller already has it
		return true;
	}

	ReleaseSharedState();

	ComPtr<IDXGIAdapter1> adapter = FindAdapter(g_gpu_index);
	if (!adapter) {
		SetError("attach_shared: NVIDIA adapter not found");
		return false;
	}
	// NTHANDLE sharing requires an explicit D3D11.1 feature level request.
	static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, ARRAYSIZE(levels),
				       D3D11_SDK_VERSION, g_helper_device.ReleaseAndGetAddressOf(), nullptr,
				       g_helper_ctx.ReleaseAndGetAddressOf());
	if (FAILED(hr)) {
		SetError("attach_shared: helper D3D11 device creation failed: 0x%08lX", (unsigned long)hr);
		return false;
	}

	// Open the OBS-side input texture (legacy keyed-mutex shared).
	hr = g_helper_device->OpenSharedResource((HANDLE)(uintptr_t)in_handle, IID_PPV_ARGS(&g_obs_in));
	if (FAILED(hr)) {
		SetError("attach_shared: OpenSharedResource(input) failed: 0x%08lX", (unsigned long)hr);
		return false;
	}
	hr = g_obs_in->QueryInterface(IID_PPV_ARGS(&g_obs_in_km));
	if (FAILED(hr))
		g_obs_in_km = nullptr; // keyed mutex optional; CPU serialization still applies

	D3D11_TEXTURE2D_DESC relay{};
	relay.Width = width;
	relay.Height = height;
	relay.MipLevels = 1;
	relay.ArraySize = 1;
	relay.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	relay.SampleDesc.Count = 1;
	relay.Usage = D3D11_USAGE_DEFAULT;
	relay.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	// NTHANDLE sharing is only valid together with the keyed mutex.
	relay.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	hr = g_helper_device->CreateTexture2D(&relay, nullptr, &g_relay_in);
	if (FAILED(hr)) {
		SetError("attach_shared: relay input creation failed: 0x%08lX", (unsigned long)hr);
		return false;
	}
	g_relay_in->QueryInterface(IID_PPV_ARGS(&g_relay_in_km));
	hr = g_helper_device->CreateTexture2D(&relay, nullptr, &g_relay_out);
	if (FAILED(hr)) {
		SetError("attach_shared: relay output creation failed: 0x%08lX", (unsigned long)hr);
		return false;
	}
	g_relay_out->QueryInterface(IID_PPV_ARGS(&g_relay_out_km));

	// NTHANDLE shared handles for the D3D12 side.
	ComPtr<IDXGIResource1> res1;
	HANDLE nt = nullptr;
	hr = g_relay_in.As(&res1);
	if (SUCCEEDED(hr))
		hr = res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &nt);
	if (FAILED(hr)) {
		SetError("attach_shared: input CreateSharedHandle failed: 0x%08lX", (unsigned long)hr);
		return false;
	}
	g_relay_in_nt = nt;
	res1.Reset();
	nt = nullptr;
	hr = g_relay_out.As(&res1);
	if (SUCCEEDED(hr))
		hr = res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &nt);
	if (FAILED(hr)) {
		SetError("attach_shared: output CreateSharedHandle failed: 0x%08lX", (unsigned long)hr);
		return false;
	}
	g_relay_out_nt = nt;
	res1.Reset();

	hr = g_device->OpenSharedHandle(g_relay_in_nt, IID_PPV_ARGS(&g_shared_color));
	if (FAILED(hr)) {
		SetError("attach_shared: D3D12 OpenSharedHandle(input) failed: 0x%08lX", (unsigned long)hr);
		return false;
	}
	hr = g_device->OpenSharedHandle(g_relay_out_nt, IID_PPV_ARGS(&g_shared_output));
	if (FAILED(hr)) {
		SetError("attach_shared: D3D12 OpenSharedHandle(output) failed: 0x%08lX", (unsigned long)hr);
		return false;
	}

	// OBS-visible output: legacy shared so libobs can open it.
	D3D11_TEXTURE2D_DESC outDesc = relay;
	outDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	hr = g_helper_device->CreateTexture2D(&outDesc, nullptr, &g_obs_out);
	if (FAILED(hr)) {
		SetError("attach_shared: output texture creation failed: 0x%08lX", (unsigned long)hr);
		return false;
	}
	ComPtr<IDXGIResource> dxgiRes;
	hr = g_obs_out.As(&dxgiRes);
	if (FAILED(hr)) {
		SetError("attach_shared: output QI(IDXGIResource) failed");
		return false;
	}
	HANDLE legacy = nullptr;
	hr = dxgiRes->GetSharedHandle(&legacy);
	if (FAILED(hr)) {
		SetError("attach_shared: output GetSharedHandle failed: 0x%08lX", (unsigned long)hr);
		return false;
	}
	*out_handle = (uint32_t)(uintptr_t)legacy;

	g_shared_w = width;
	g_shared_h = height;
	g_gpu_mode = true;
	return true;
}

bool gpu_mode()
{
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_gpu_mode;
}

void detach_shared()
{
	std::lock_guard<std::mutex> guard(g_mutex);
	ReleaseFeatureAndResources();
	ReleaseSharedState();
}

bool process_gpu(int width, int height, const NrBridgeParams &params)
{
	std::lock_guard<std::mutex> guard(g_mutex);
	g_last_error.clear();
	if (!g_initialized || !g_gpu_mode) {
		SetError("GPU path not active");
		return false;
	}
	if ((UINT)width != g_shared_w || (UINT)height != g_shared_h) {
		SetError("process_gpu: size changed (%ux%u != %ux%u) — re-attach", width, height, g_shared_w,
			 g_shared_h);
		return false;
	}

	if (!EnsureFeature(g_shared_w, g_shared_h, params.style, params.preset, params.intensity, params.tone,
			   params.structure, params.skin, params.automask, params.ui_correction,
			   /*temporal=*/false)) {
		return false;
	}
	SetCommonParams(params.style, params.preset, params.intensity, params.tone, params.structure, params.skin,
			params.automask, params.ui_correction, params.reset ? 1 : 0, /*temporal=*/false);

	// 1) Helper D3D11: copy the OBS-rendered frame into the D3D12-visible
	// relay texture. The keyed mutex (when present) orders this against the
	// OBS render; Flush() submits immediately.
	LARGE_INTEGER t0{}, t1{}, t2{}, t3{};
	if (!g_qpc_freq.QuadPart)
		QueryPerformanceFrequency(&g_qpc_freq);
	QueryPerformanceCounter(&t0);

	if (g_obs_in_km) {
		if (FAILED(g_obs_in_km->AcquireSync(0, 100))) {
			SetError("process_gpu: timed out acquiring input mutex");
			return false;
		}
	}
	g_helper_ctx->CopyResource(g_relay_in.Get(), g_obs_in.Get());
	if (g_obs_in_km)
		g_obs_in_km->ReleaseSync(0);
	g_helper_ctx->Flush();
	QueryPerformanceCounter(&t1);

	// 2) D3D12: NGX consumes the relay input and writes the relay output.
	// Opened cross-API resources start out in the COMMON state and decay
	// back to COMMON after ExecuteCommandLists.
	D3D12_RESOURCE_BARRIER toColor = Barrier(g_shared_color.Get(), D3D12_RESOURCE_STATE_COMMON,
						 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	D3D12_RESOURCE_BARRIER toOutput =
		Barrier(g_shared_output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	D3D12_RESOURCE_BARRIER toCommon[2] = {
		Barrier(g_shared_color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COMMON),
		Barrier(g_shared_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
	};
	g_cmd->ResourceBarrier(1, &toColor);
	g_cmd->ResourceBarrier(1, &toOutput);

	NGXResult er = g_shim_eval(reinterpret_cast<void *>(g_nr_eval), g_cmd.Get(), g_feature, g_params, nullptr);
	g_cmd->ResourceBarrier(2, toCommon);
	if (er != NGX_SUCCESS) {
		SetError("DLSSNR EvaluateFeature failed: 0x%08X (%s)", static_cast<unsigned>(er),
			 ngx_diagnostics::result_name(static_cast<uint32_t>(er)));
		ExecuteAndWait(); // reset the command list to a clean state
		return false;
	}
	if (!ExecuteAndWait())
		return false;
	QueryPerformanceCounter(&t2);

	// 3) Helper D3D11: publish the result to the OBS-visible output.
	g_helper_ctx->CopyResource(g_obs_out.Get(), g_relay_out.Get());
	g_helper_ctx->Flush();
	QueryPerformanceCounter(&t3);

	// Running-average timings (EMA), logged every 300 frames.
	const double relay_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpc_freq.QuadPart;
	const double eval_ms = (t2.QuadPart - t1.QuadPart) * 1000.0 / g_qpc_freq.QuadPart;
	const double pub_ms = (t3.QuadPart - t2.QuadPart) * 1000.0 / g_qpc_freq.QuadPart;
	g_ms_relay = g_ms_relay * 0.95 + relay_ms * 0.05;
	g_ms_eval = g_ms_eval * 0.95 + eval_ms * 0.05;
	g_ms_publish = g_ms_publish * 0.95 + pub_ms * 0.05;
	if (++g_ms_frames >= 300) {
		char buf[256];
		snprintf(buf, sizeof(buf),
			 "GPU avg/%u frames: relay %.2f ms, NR eval %.2f ms, publish %.2f ms (total %.2f ms)",
			 g_ms_frames, g_ms_relay, g_ms_eval, g_ms_publish, g_ms_relay + g_ms_eval + g_ms_publish);
		g_timing_text = buf;
		g_ms_frames = 0;
	}
	return true;
}

void shutdown()
{
	std::lock_guard<std::mutex> guard(g_mutex);
	ShutdownUnlocked();
}

bool ready()
{
	// The graphics thread polls this while the worker holds g_mutex across
	// inference and GPU waits. Readiness must never wait for a processed frame.
	return g_initialized.load(std::memory_order_acquire);
}

const char *gpu_name()
{
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_gpu_name.c_str();
}

const char *last_error()
{
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_last_error.c_str();
}

const char *timing_text()
{
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_timing_text.c_str();
}

} // namespace nrbridge
