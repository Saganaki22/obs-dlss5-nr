// SPDX-License-Identifier: MIT
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <string>

// The bridge serializes all access. A submitted list and all of its resources
// must stay alive until the queue fence completes or the device is removed.
class D3D12Commands {
public:
	bool initialize(ID3D12Device *device);
	bool execute_and_wait(DWORD timeout_ms);
	bool prepare(); // Poll and return the list to recording when safe.
	bool drained(); // Poll without resetting the list. Safe to release when true.
	void clear();   // Call only after drained() succeeds.
	void abandon(); // Final module unload only, when completion is still unknown.

	ID3D12GraphicsCommandList *list() const { return command_list.Get(); }
	ID3D12CommandQueue *queue() const { return command_queue.Get(); }
	const std::string &error() const { return last_error; }

private:
	bool signal_submission();
	void report_device_removed();
	bool reset_recording();
	void set_hresult_error(const char *operation, HRESULT hr);

	Microsoft::WRL::ComPtr<ID3D12Device> device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> command_queue;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
	Microsoft::WRL::ComPtr<ID3D12Fence> fence;
	HANDLE fence_event = nullptr;
	UINT64 fence_value = 0;
	bool submitted = false;
	bool signaled = false;
	bool unusable = false;
	std::string last_error;
};
