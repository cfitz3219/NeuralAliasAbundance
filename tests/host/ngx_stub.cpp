// Reconstructed 2026-10 (C3). Fake NGX runtime built as nvngx_stub.dll (the
// one non-driver name IsNgxCoreModuleName accepts). Exports the entry points
// ngx_observe hooks plus the parameter allocation pair, with sentinel
// behaviour so tests can prove passthrough: results, out-handles and
// argument echoes are observable through NgxStubState().
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_params.hpp"
#endif

namespace {
struct State {
    uint64_t inits, creates, evaluates, releases, shutdowns, getcaps, allocs, destroys;
    uint64_t last_app_id, last_handle_in;
    uint32_t last_feature_id, last_sdk_version;
    void* last_cmd_list;
    int32_t next_create_result; // what CreateFeature returns (1 = NVSDK_NGX_Result_Success)
    int32_t next_eval_result;
};
State g_state{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, nullptr, 1, 1};
uint64_t g_next_handle = 0xA000;
// A parameter object is a plain 4 KiB block here (opaque to the host unless
// the NGX SDK is present, in which case the host reads through the real
// vtable; this stub is only used for the SDK-less observation tests).
#if defined(NA_HAVE_NGX_SDK)
using Params = test::FakeParams;
#else
struct Params { uint32_t magic; uint8_t pad[4092]; };
#endif
void* g_query_params = nullptr;
int32_t g_query_result = 1;
} // namespace

#define NGX_EXPORT extern "C" __declspec(dllexport)

NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_Init_Ext(unsigned long long app_id, const wchar_t*, void*, uint32_t sdk_version, const void*)
{ ++g_state.inits; g_state.last_app_id = app_id; g_state.last_sdk_version = sdk_version; return 1; }
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_Init_ProjectID(const char*, uint32_t, const char*, const wchar_t*, void*, uint32_t sdk_version, const void*)
{ ++g_state.inits; g_state.last_app_id = 0x5EED; g_state.last_sdk_version = sdk_version; return 1; }
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_CreateFeature(void* cmd_list, uint32_t feature_id, void*, void** out_handle)
{
    ++g_state.creates; g_state.last_cmd_list = cmd_list; g_state.last_feature_id = feature_id;
    if (g_state.next_create_result != 1) return g_state.next_create_result;
    if (out_handle) *out_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(++g_next_handle));
    return 1;
}
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_EvaluateFeature(void* cmd_list, const void* handle, const void*, void*)
{ ++g_state.evaluates; g_state.last_cmd_list = cmd_list; g_state.last_handle_in = reinterpret_cast<uintptr_t>(handle); return g_state.next_eval_result; }
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_ReleaseFeature(void* handle)
{ ++g_state.releases; g_state.last_handle_in = reinterpret_cast<uintptr_t>(handle); return 1; }
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_Shutdown1(void*) { ++g_state.shutdowns; return 1; }
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_GetCapabilityParameters(void** out)
{ ++g_state.getcaps; static Params caps{}; if (out) *out = g_query_params ? g_query_params : &caps; return g_query_result; }
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_GetParameters(void** out)
{ static Params params{}; if (out) *out = g_query_params ? g_query_params : &params; return g_query_result; }
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_AllocateParameters(void** out)
{ ++g_state.allocs; if (!out) return -1; *out = g_query_params ? g_query_params : new Params{}; return g_query_result; }
NGX_EXPORT int32_t __cdecl NVSDK_NGX_D3D12_DestroyParameters(void* p)
{ ++g_state.destroys; delete static_cast<Params*>(p); return 1; }

// Test-side introspection and control.
NGX_EXPORT const State* __cdecl NgxStubState() { return &g_state; }
NGX_EXPORT void __cdecl NgxStubSetResults(int32_t create_result, int32_t eval_result)
{ g_state.next_create_result = create_result; g_state.next_eval_result = eval_result; }
NGX_EXPORT void __cdecl NgxStubSetQuery(void* params, int32_t result)
{ g_query_params = params; g_query_result = result; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h); return TRUE; }
