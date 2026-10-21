#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/bridge/ngx-diagnostics.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

static std::atomic<unsigned> calls{0};
static void __cdecl consume(const char *, NGXLoggingLevel, int)
{
	++calls;
	Sleep(2);
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	HMODULE shim = LoadLibraryA(argv[1]);
	if (!shim)
		return 3;
	auto set_callback =
		reinterpret_cast<void(__cdecl *)(NGXLogCallback)>(GetProcAddress(shim, "DLSSNR_SetLogCallback"));
	auto log = reinterpret_cast<NGXLogCallback>(GetProcAddress(shim, "DLSSNR_LogMessage"));
	if (!set_callback || !log)
		return 4;
	set_callback(consume);
	std::atomic<bool> stop{false};
	std::thread caller([&] {
		while (!stop.load())
			log("lifetime test", NGX_LOG_ON, 18);
	});
	while (!calls.load())
		Sleep(1);
	set_callback(nullptr); // Must drain any callback that was already active.
	const unsigned detached_count = calls.load();
	for (int i = 0; i < 1000; ++i)
		log("after detach", NGX_LOG_ON, 18);
	Sleep(10);
	const bool detached = calls.load() == detached_count;
	stop.store(true);
	caller.join();
	set_callback(consume);
	log("reattach", NGX_LOG_ON, 18);
	const bool reattached = calls.load() == detached_count + 1;
	set_callback(nullptr);
	FreeLibrary(shim);
	if (!detached || !reattached) {
		std::fputs("FAIL: NGX callback outlived its consumer or could not reattach\n", stderr);
		return 1;
	}
	std::puts("PASS: callback consumer drained before detach; late logging safely ignored; reattach works");
}
