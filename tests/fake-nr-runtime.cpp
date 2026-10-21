// Test fixture only. Never install or package this as a runtime.
#include "../src/bridge/ngx-diagnostics.h"
#include <util/base.h>
#include <atomic>

static std::atomic<unsigned> attempts{0};

extern "C" {
__declspec(dllexport) unsigned __cdecl NRTest_GetCreateCount()
{
	return attempts.load();
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t *, void *,
							   const NGXFeatureCommonInfo *info, int)
{
	if (!info || !info->LoggingInfo.Callback || info->LoggingInfo.LoggingLevel != NGX_LOG_VERBOSE)
		return static_cast<int>(0xBAD00005u);
	info->LoggingInfo.Callback("TEST runtime callback active (ABI validated)", NGX_LOG_ON, 18);
	return 1;
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_CreateFeature(void *, int, void *, void **)
{
	blog(LOG_INFO, "[test-nr-runtime] CreateFeature rejected: attempt=%u", ++attempts);
	return static_cast<int>(0xBAD00001u);
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_EvaluateFeature(void *, void *, void *, void *)
{
	return 1;
}

__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_ReleaseFeature(void *)
{
	return 1;
}
}
