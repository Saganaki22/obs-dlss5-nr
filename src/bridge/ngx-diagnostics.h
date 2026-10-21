#pragma once

#include <cstddef>
#include <cstdint>

// Matches NVIDIA's public NGX 1.4+ FeatureCommonInfo ABI. The snippet Init_Ext
// argument order is different from the core's; the caller shim handles that.
enum NGXLoggingLevel { NGX_LOG_OFF = 0, NGX_LOG_ON = 1, NGX_LOG_VERBOSE = 2 };
#ifdef _WIN32
#define NR_NGX_CALL __cdecl
#else
#define NR_NGX_CALL
#endif
using NGXLogCallback = void(NR_NGX_CALL *)(const char *, NGXLoggingLevel, int);
struct NGXPathListInfo {
	const wchar_t *const *Path;
	unsigned int Length;
};
struct NGXLoggingInfo {
	NGXLogCallback Callback;
	NGXLoggingLevel LoggingLevel;
	bool DisableOtherLoggingSinks;
};
struct NGXFeatureCommonInfo {
	NGXPathListInfo PathListInfo;
	void *InternalData;
	NGXLoggingInfo LoggingInfo;
};

#ifdef _WIN64
static_assert(sizeof(NGXLoggingInfo) == 16);
static_assert(offsetof(NGXLoggingInfo, Callback) == 0);
static_assert(offsetof(NGXLoggingInfo, LoggingLevel) == 8);
static_assert(offsetof(NGXLoggingInfo, DisableOtherLoggingSinks) == 12);
static_assert(offsetof(NGXFeatureCommonInfo, LoggingInfo) == 24);
static_assert(sizeof(NGXFeatureCommonInfo) == 40);
#endif

namespace ngx_diagnostics {
inline const char *result_name(uint32_t result)
{
	switch (result) {
	case 1:
		return "Success";
	case 0xBAD00000:
		return "Fail";
	case 0xBAD00001:
		return "FeatureNotSupported";
	case 0xBAD00002:
		return "PlatformError";
	case 0xBAD00003:
		return "FeatureAlreadyExists";
	case 0xBAD00004:
		return "FeatureNotFound";
	case 0xBAD00005:
		return "InvalidParameter";
	case 0xBAD00006:
		return "ScratchBufferTooSmall";
	case 0xBAD00007:
		return "NotInitialized";
	case 0xBAD00008:
		return "UnsupportedInputFormat";
	case 0xBAD00009:
		return "RWFlagMissing";
	case 0xBAD0000A:
		return "MissingInput";
	case 0xBAD0000B:
		return "UnableToInitializeFeature";
	case 0xBAD0000C:
		return "OutOfDate";
	case 0xBAD0000D:
		return "OutOfGPUMemory";
	case 0xBAD0000E:
		return "UnsupportedFormat";
	case 0xBAD0000F:
		return "UnableToWriteToAppDataPath";
	case 0xBAD00010:
		return "UnsupportedParameter";
	case 0xBAD00011:
		return "Denied";
	case 0xBAD00012:
		return "NotImplemented";
	default:
		return "UnknownResult";
	}
}

struct CreateKey {
	uint32_t width, height;
	int style, preset, ui_correction;
	bool temporal, gpu;
	uint64_t generation;
	bool operator==(const CreateKey &) const = default;
};

class CreateRetry {
	CreateKey failed_key{};
	uint64_t deadline_ms = 0;
	bool failed = false;

public:
	static constexpr uint64_t cooldown_ms = 5000;
	bool allowed(const CreateKey &key, uint64_t now_ms) const
	{
		return !failed || !(key == failed_key) || now_ms >= deadline_ms;
	}
	void failure(const CreateKey &key, uint64_t now_ms)
	{
		failed_key = key;
		deadline_ms = now_ms + cooldown_ms;
		failed = true;
	}
	void reset() { failed = false; }
};
} // namespace ngx_diagnostics
