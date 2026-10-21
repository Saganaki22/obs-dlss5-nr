// Test fixture only: isolate fault-injected NR from NVIDIA's real core state.
#include <cstdint>

struct FakeParameter {
	virtual void Set(const char *, unsigned long long) {}
	virtual void Set(const char *, float) {}
	virtual void Set(const char *, double) {}
	virtual void Set(const char *, unsigned int) {}
	virtual void Set(const char *, int) {}
	virtual void Set(const char *, struct ID3D11Resource *) {}
	virtual void Set(const char *, struct ID3D12Resource *) {}
	virtual void Set(const char *, void *) {}
	virtual int Get(const char *, unsigned long long *) const { return -1; }
	virtual int Get(const char *, float *) const { return -1; }
	virtual int Get(const char *, double *) const { return -1; }
	virtual int Get(const char *, unsigned int *) const { return -1; }
	virtual int Get(const char *, int *) const { return -1; }
	virtual int Get(const char *, struct ID3D11Resource **) const { return -1; }
	virtual int Get(const char *, struct ID3D12Resource **) const { return -1; }
	virtual int Get(const char *, void **) const { return -1; }
	virtual void Reset() {}
};

extern "C" {
__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_Init_Ext(unsigned long long, const wchar_t *, void *, int,
							   const void *)
{
	return 1;
}
__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_AllocateParameters(FakeParameter **params)
{
	static FakeParameter parameter;
	*params = &parameter;
	return 1;
}
__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_CreateFeature(void *, int, void *, void **)
{
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
__declspec(dllexport) int __cdecl NVSDK_NGX_D3D12_Shutdown()
{
	return 1;
}
}
