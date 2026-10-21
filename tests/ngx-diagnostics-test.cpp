#include "../src/bridge/ngx-diagnostics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(bool value, const char *reason)
{
	if (!value) {
		std::fprintf(stderr, "FAIL: %s\n", reason);
		std::exit(1);
	}
}

int main()
{
	using namespace ngx_diagnostics;
	check(std::strcmp(result_name(0xBAD00001u), "FeatureNotSupported") == 0, "unsupported is decoded");
	check(std::strcmp(result_name(0xBAD00005u), "InvalidParameter") == 0, "invalid parameters are distinct");
	check(std::strcmp(result_name(0xBAD0000Du), "OutOfGPUMemory") == 0, "VRAM error is decoded");
	check(std::strcmp(result_name(0xBAD00011u), "Denied") == 0, "application denial is distinct");
	check(std::strcmp(result_name(0x12345678u), "UnknownResult") == 0, "unknown results stay unknown");
	CreateRetry retry;
	CreateKey key{1920, 1080, 1, 3, 0, false, false, 0};
	check(retry.allowed(key, 0), "initial creation allowed at clock zero");
	retry.failure(key, 0);
	for (uint64_t ms = 1; ms < 5000; ++ms)
		check(!retry.allowed(key, ms), "all per-frame retries blocked during cooldown");
	check(retry.allowed(key, 5000), "creation allowed at five seconds");
	retry.failure(key, 5000);
	check(!retry.allowed(key, 9999), "repeated failure restarts cooldown");
	for (int field = 0; field < 8; ++field) {
		auto changed = key;
		switch (field) {
		case 0:
			changed.width = 1280;
			break;
		case 1:
			changed.height = 720;
			break;
		case 2:
			changed.style = 2;
			break;
		case 3:
			changed.preset = 0;
			break;
		case 4:
			changed.ui_correction = 1;
			break;
		case 5:
			changed.temporal = true;
			break;
		case 6:
			changed.gpu = true;
			break;
		case 7:
			changed.generation = 1;
			break;
		}
		check(retry.allowed(changed, 5001), "changed creation setting or manual retry bypasses cooldown");
	}
	retry.reset();
	check(retry.allowed(key, 5001), "successful creation/session reset clears failed state");
	std::puts("PASS: NGX result decoding, ABI layout and feature creation retry policy");
}
