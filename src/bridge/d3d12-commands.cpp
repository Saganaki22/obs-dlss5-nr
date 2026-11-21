// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "d3d12-commands.h"

#include <cstdio>

void D3D12Commands::set_hresult_error(const char *operation, HRESULT hr)
{
	char message[192];
	std::snprintf(message, sizeof(message), "%s failed (0x%08X)", operation, static_cast<unsigned>(hr));
	last_error = message;
}

bool D3D12Commands::initialize(ID3D12Device *new_device)
{
	device = new_device;
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&command_queue));
	if (FAILED(hr)) {
		set_hresult_error("CreateCommandQueue", hr);
		return false;
	}
	hr = device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator));
	if (FAILED(hr)) {
		set_hresult_error("CreateCommandAllocator", hr);
		return false;
	}
	hr = device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&command_list));
	if (FAILED(hr)) {
		set_hresult_error("CreateCommandList", hr);
		return false;
	}
	hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	if (FAILED(hr)) {
		set_hresult_error("CreateFence", hr);
		return false;
	}
	fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!fence_event) {
		set_hresult_error("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
		return false;
	}
	return true;
}

void D3D12Commands::report_device_removed()
{
	const HRESULT reason = device->GetDeviceRemovedReason();
	set_hresult_error("D3D12 device (removed)", FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED);
	last_error += "; restart OBS if re-adding the filter does not recover";
	unusable = true;
	submitted = false;
}

bool D3D12Commands::signal_submission()
{
	const HRESULT hr = command_queue->Signal(fence.Get(), ++fence_value);
	if (FAILED(hr)) {
		set_hresult_error("Queue::Signal", hr);
		return false;
	}
	signaled = true;
	return true;
}

bool D3D12Commands::drained()
{
	if (!submitted)
		return true;
	UINT64 completed = fence->GetCompletedValue();
	if (completed == UINT64_MAX) {
		report_device_removed();
		return true;
	}
	// A signal can fail after ExecuteCommandLists. Only a later queue signal
	// can establish a new fence for that work while the device is healthy.
	if (!signaled) {
		if (!signal_submission()) {
			if (FAILED(device->GetDeviceRemovedReason())) {
				report_device_removed();
				return true;
			}
			return false;
		}
		completed = fence->GetCompletedValue();
		if (completed == UINT64_MAX) {
			report_device_removed();
			return true;
		}
	}
	return completed >= fence_value;
}

bool D3D12Commands::reset_recording()
{
	submitted = false;
	HRESULT hr = allocator->Reset();
	if (FAILED(hr)) {
		set_hresult_error("CommandAllocator::Reset", hr);
	} else {
		hr = command_list->Reset(allocator.Get(), nullptr);
		if (SUCCEEDED(hr))
			return true;
		set_hresult_error("CommandList::Reset", hr);
	}
	unusable = true;
	last_error += "; re-add the filter or restart OBS";
	return false;
}

bool D3D12Commands::prepare()
{
	if (!drained()) {
		if (signaled)
			last_error = "Waiting for the previous DLSS NR GPU submission; passing video through";
		return false;
	}
	if (unusable)
		return false;
	return !submitted || reset_recording();
}

bool D3D12Commands::execute_and_wait(DWORD timeout_ms)
{
	// Defend against accidental submission before recovering a timed-out list.
	if (unusable || submitted)
		return false;
	HRESULT hr = command_list->Close();
	if (FAILED(hr)) {
		set_hresult_error("CommandList::Close", hr);
		unusable = true;
		last_error += "; re-add the filter or restart OBS";
		return false;
	}
	ID3D12CommandList *lists[] = {command_list.Get()};
	command_queue->ExecuteCommandLists(1, lists);
	submitted = true;
	signaled = false;
	if (!signal_submission())
		return false;
	if (drained())
		return !unusable && reset_recording();

	// Keep the event alive across a timeout, since D3D12 may signal it later.
	hr = fence->SetEventOnCompletion(fence_value, fence_event);
	if (FAILED(hr)) {
		set_hresult_error("Fence::SetEventOnCompletion", hr);
		return false;
	}
	const ULONGLONG deadline = GetTickCount64() + timeout_ms;
	for (;;) {
		const ULONGLONG now = GetTickCount64();
		const DWORD remaining_ms = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
		const DWORD result = WaitForSingleObject(fence_event, remaining_ms);
		if (result == WAIT_FAILED) {
			set_hresult_error("WaitForSingleObject", HRESULT_FROM_WIN32(GetLastError()));
			return false;
		}
		// A notification from an older submission may still be in the event.
		// Always confirm the current fence value before resetting.
		if (drained())
			return !unusable && reset_recording();
		if (result == WAIT_TIMEOUT || GetTickCount64() >= deadline) {
			last_error = "Timed out waiting for DLSS NR GPU work (" + std::to_string(timeout_ms) + " ms)";
			return false;
		}
	}
}

void D3D12Commands::clear()
{
	command_list.Reset();
	allocator.Reset();
	command_queue.Reset();
	fence.Reset();
	device.Reset();
	if (fence_event)
		CloseHandle(fence_event);
	fence_event = nullptr;
	fence_value = 0;
	submitted = false;
	signaled = false;
	unusable = false;
	last_error.clear();
}

void D3D12Commands::abandon()
{
	// Leave the GPU handles and registered event alive until process exit.
	command_list.Detach();
	allocator.Detach();
	command_queue.Detach();
	fence.Detach();
	device.Detach();
	fence_event = nullptr;
}
