// obs-dlss5-nr — DLSS 5 Neural Rendering filter for OBS Studio
// Copyright (C) 2026 Saganaki22
//
// Bridge interface between the filter and NVIDIA DLSS NR (NGX feature 18) on
// D3D12. Frames cross as CPU-staged BGRA buffers (zero-copy D3D11<->D3D12
// interop is planned as a later optimization).

#pragma once

#include <cstdint>

struct NrBridgeParams {
	int style;
	int preset;
	float intensity;
	float tone;
	float structure;
	float skin;
	int automask;
	int ui_correction;
	int reset;
	// Temporal mode: estimate per-frame motion vectors with NVIDIA Optical Flow
	// and feed them to DLSS NR. Requires the CPU staging path.
	int temporal;
};

namespace nrbridge {

// gpu_index: index among NVIDIA (VendorId 0x10DE) adapters.
// runtime_dir: directory containing the user-supplied nvngx_dlssnr.dll (UTF-16).
// shim_dir: directory containing nvngx.dll_obs.dll (UTF-16); may be null.
bool init(int gpu_index, const wchar_t *runtime_dir, const wchar_t *shim_dir);

// Zero-copy mode: in_handle is a legacy keyed-mutex-shared D3D11 texture
// (RGBA16F, render-target capable) owned by OBS; out_handle receives the
// legacy shared handle of a matching output texture the OBS side can draw.
// On success process_gpu() runs the whole pipeline on the GPU.
bool attach_shared(uint32_t in_handle, uint32_t width, uint32_t height, uint32_t *out_handle);
bool gpu_mode();
void detach_shared();

// src_bgra/dst_bgra: 8-bit BGRA rows; pitches in bytes. (CPU fallback path.)
bool process(const uint8_t *src_bgra, int src_row_pitch, uint8_t *dst_bgra, int dst_row_pitch, int width, int height,
	     const NrBridgeParams &params);

// Zero-copy path: caller has already rendered the frame into the attached
// input texture. Blocks until the GPU finished writing the output texture.
bool process_gpu(int width, int height, const NrBridgeParams &params);

// Verbose NGX logging is selected at init; restart OBS after enabling it.
// Creation diagnostics and disabling log forwarding apply immediately.
void set_diagnostics(bool enabled);
// Nonblocking: bypass a failed creation's cooldown on the next processing call.
void request_retry();

void shutdown();
bool ready();
const char *gpu_name();
const char *last_error();
const char *timing_text();

} // namespace nrbridge
