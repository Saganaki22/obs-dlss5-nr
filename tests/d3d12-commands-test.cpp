#include "../src/bridge/d3d12-commands.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static constexpr int TEST_SKIP = 77;
static constexpr DWORD RECOVERY_DEADLINE_MS = 5000;
static constexpr uint64_t BUFFER_SIZE = 2 * sizeof(uint32_t);

static int fail(const char *message, const char *detail = "")
{
	std::fprintf(stderr, "FAIL: %s %s\n", message, detail);
	return 1;
}

static ComPtr<ID3D12Resource> make_buffer(ID3D12Device *device, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = heap_type;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = BUFFER_SIZE;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> buffer;
	device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
	return buffer;
}

static bool fence_completed(ID3D12Fence *fence)
{
	const auto deadline = GetTickCount64() + RECOVERY_DEADLINE_MS;
	while (GetTickCount64() < deadline) {
		const uint64_t completed = fence->GetCompletedValue();
		if (completed != 0)
			return completed == 1;
		Sleep(1);
	}
	return false;
}

int main()
{
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter> warp;
	ComPtr<ID3D12Device> device;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
	    FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
	    FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
		std::puts("SKIP: D3D12 WARP adapter unavailable");
		return TEST_SKIP;
	}

	auto upload = make_buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
	auto readback = make_buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!upload || !readback)
		return fail("allocate copy buffers");
	void *mapped = nullptr;
	const D3D12_RANGE no_read{0, 0};
	if (FAILED(upload->Map(0, &no_read, &mapped)))
		return fail("map upload buffer");
	auto *input = static_cast<uint32_t *>(mapped);
	constexpr uint32_t first_value = 0x12345678u;
	constexpr uint32_t second_value = 0xDEADBEEFu;
	input[0] = first_value;
	input[1] = second_value;
	upload->Unmap(0, nullptr);

	D3D12Commands commands;
	if (!commands.initialize(device.Get()))
		return fail("initialize command context", commands.error().c_str());
	if (!commands.prepare() || !commands.execute_and_wait(RECOVERY_DEADLINE_MS) || !commands.prepare())
		return fail("submit and reset without a stall", commands.error().c_str());

	ComPtr<ID3D12Fence> gate;
	ComPtr<ID3D12Fence> finished;
	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) ||
	    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finished))))
		return fail("create test fences");
	if (FAILED(commands.queue()->Wait(gate.Get(), 1)))
		return fail("insert unsignaled fence gate");

	// The CPU controls the gate, so a short wait expires regardless of GPU speed.
	commands.list()->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, sizeof(uint32_t));
	const bool timed_out = !commands.execute_and_wait(10);
	const auto timeout_error = commands.error();
	const bool first_poll_blocked = !commands.prepare();
	const bool second_poll_blocked = !commands.prepare();
	const bool teardown_blocked = !commands.drained();
	const HRESULT mark_finished = commands.queue()->Signal(finished.Get(), 1);
	const HRESULT unblock = gate->Signal(1);
	if (FAILED(mark_finished) || FAILED(unblock) || !fence_completed(finished.Get()))
		return fail("drain queue after opening gate");

	if (!timed_out || timeout_error.find("Timed out") == std::string::npos)
		return fail("gated submission must report a timeout", timeout_error.c_str());
	if (!first_poll_blocked || !second_poll_blocked || !teardown_blocked)
		return fail("pending GPU work must block recording and teardown");
	if (!commands.drained() || !commands.prepare())
		return fail("recover recording context after its fence completes", commands.error().c_str());

	// A real second copy proves the recovered list was reset and can record and execute new work.
	commands.list()->CopyBufferRegion(readback.Get(), sizeof(uint32_t), upload.Get(), sizeof(uint32_t),
					  sizeof(uint32_t));
	if (!commands.execute_and_wait(RECOVERY_DEADLINE_MS))
		return fail("submit after timeout recovery", commands.error().c_str());
	const D3D12_RANGE read_range{0, BUFFER_SIZE};
	if (FAILED(readback->Map(0, &read_range, &mapped)))
		return fail("map readback buffer");
	const auto *output = static_cast<const uint32_t *>(mapped);
	const bool copies_succeeded = output[0] == first_value && output[1] == second_value;
	readback->Unmap(0, &no_read);
	if (!copies_succeeded)
		return fail("verify copies before and after recovery");
	if (!commands.drained())
		return fail("successful submission leaves queue safe to release");
	commands.clear();
	std::puts("PASS: blocked D3D12 queue kept its context pending and resumed GPU copies after completion");
}
