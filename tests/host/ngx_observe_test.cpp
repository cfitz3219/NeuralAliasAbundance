// Reconstructed 2026-10 (C3). Mirrors WORKLOG-ROUTE-A A7/A43: loads the fake
// NGX runtime (nvngx_stub.dll), hooks it with ngx_observe::InstallForModule,
// then calls its exports the way a game would and proves:
//   - core-module name rule: _nvngx.dll / nvngx.dll / the stub accepted, every
//     feature DLL (nvngx_dlss*, case-insensitive, bare or full path) refused;
//   - hooks installed exactly once per module (idempotent re-install);
//   - full passthrough: results (including failures), out-handles and the
//     arguments the runtime sees are unchanged;
//   - counters and the record ring match what happened; drained ring empty;
//   - a fake command-list pointer is never dereferenced (no device set);
//   - parameter reading without an object reports valid=0.
// Usage: ngx_observe_test <path to nvngx_stub.dll>
#include "test_support.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"

#include <cwchar>
#include "host_dx12/dlss/call_origin.hpp"
#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_params.hpp"
#endif

namespace {
struct StubState { // mirror of ngx_stub.cpp State
    uint64_t inits, creates, evaluates, releases, shutdowns, getcaps, allocs, destroys;
    uint64_t last_app_id, last_handle_in;
    uint32_t last_feature_id, last_sdk_version;
    void* last_cmd_list;
    int32_t next_create_result, next_eval_result;
};
using PfnInitExt = int32_t(__cdecl*)(unsigned long long, const wchar_t*, void*, uint32_t, const void*);
using PfnCreate = int32_t(__cdecl*)(void*, uint32_t, void*, void**);
using PfnEval = int32_t(__cdecl*)(void*, const void*, const void*, void*);
using PfnRelease = int32_t(__cdecl*)(void*);
using PfnShutdown = int32_t(__cdecl*)(void*);
using PfnState = const StubState*(__cdecl*)();
using PfnSetResults = void(__cdecl*)(int32_t, int32_t);

bool Name(const wchar_t* s) { return na::ngx::IsNgxCoreModuleName(s, std::wcslen(s)); }
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { std::printf("usage: ngx_observe_test <nvngx_stub.dll>\n"); return 2; }

    // Name rule (A43).
    CHECK(Name(L"_nvngx.dll"));
    CHECK(Name(L"NVNGX.DLL"));
    CHECK(Name(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv.inf_amd64\\_nvngx.dll"));
    CHECK(Name(L"nvngx_stub.dll"));
    CHECK(!Name(L"nvngx_dlss.dll"));
    CHECK(!Name(L"nvngx_dlssnr.dll"));
    CHECK(!Name(L"NVNGX_DLSSG.DLL"));
    CHECK(!Name(L"D:\\Games\\TLOU\\nvngx_dlss.dll"));
    CHECK(!Name(L"xnvngx.dll"));
    CHECK(!na::ngx::IsNgxCoreModuleName(nullptr, 0));

    na::ngx::EvalParams ep{};
    CHECK(!na::ngx::ReadEvalParams(nullptr, &ep));
    CHECK(ep.valid == 0);

    REQUIRE(na::hooking::Initialize());
    HMODULE stub = LoadLibraryW(argv[1]);
    REQUIRE(stub != nullptr);
    auto state = reinterpret_cast<PfnState>(GetProcAddress(stub, "NgxStubState"));
    auto set_results = reinterpret_cast<PfnSetResults>(GetProcAddress(stub, "NgxStubSetResults"));
    auto init = reinterpret_cast<PfnInitExt>(GetProcAddress(stub, "NVSDK_NGX_D3D12_Init_Ext"));
    auto create = reinterpret_cast<PfnCreate>(GetProcAddress(stub, "NVSDK_NGX_D3D12_CreateFeature"));
    auto eval = reinterpret_cast<PfnEval>(GetProcAddress(stub, "NVSDK_NGX_D3D12_EvaluateFeature"));
    auto release = reinterpret_cast<PfnRelease>(GetProcAddress(stub, "NVSDK_NGX_D3D12_ReleaseFeature"));
    auto shutdown = reinterpret_cast<PfnShutdown>(GetProcAddress(stub, "NVSDK_NGX_D3D12_Shutdown1"));
    REQUIRE(state && set_results && init && create && eval && release && shutdown);

    const uint32_t hooked = na::ngx::InstallForModule(stub);
    CHECK(hooked >= 6); // Init_Ext, Init_ProjectID, Create, Evaluate, Release, Shutdown1 (+GetCapabilityParameters)
    CHECK(na::ngx::InstallForModule(stub) == 0 || na::ngx::InstallForModule(stub) == hooked); // idempotent, never doubles
    const auto s0 = na::ngx::GetStats();

    // Calls through the (now hooked) exports.
    CHECK(init(0xABCDEFull, L"C:\\temp", nullptr, 0x15, nullptr) == 1);
    CHECK(state()->last_app_id == 0xABCDEFull);
    CHECK(state()->last_sdk_version == 0x15);

    void* fake_list = reinterpret_cast<void*>(0x1000); // must never be dereferenced
    void* handle = nullptr;
    CHECK(create(fake_list, 1, nullptr, &handle) == 1);
    CHECK(handle != nullptr);
    CHECK(state()->last_feature_id == 1);
    CHECK(state()->last_cmd_list == fake_list);

    for (int i = 0; i < 5; ++i) CHECK(eval(fake_list, handle, nullptr, nullptr) == 1);
    CHECK(state()->last_handle_in == reinterpret_cast<uintptr_t>(handle));

    // Failure results pass through untouched.
    set_results(static_cast<int32_t>(0xBAD00002), static_cast<int32_t>(0xBAD00005));
    void* h2 = nullptr;
    CHECK(create(fake_list, 13, nullptr, &h2) == static_cast<int32_t>(0xBAD00002));
    CHECK(h2 == nullptr);
    CHECK(eval(fake_list, handle, nullptr, nullptr) == static_cast<int32_t>(0xBAD00005));
    set_results(1, 1);

    CHECK(release(handle) == 1);
    CHECK(state()->last_handle_in == reinterpret_cast<uintptr_t>(handle));
    CHECK(shutdown(nullptr) == 1);

    const auto s1 = na::ngx::GetStats();
    CHECK(s1.inits - s0.inits == 1);
    CHECK(s1.creates - s0.creates == 2);
    CHECK(s1.evaluates - s0.evaluates == 6);
    CHECK(s1.releases - s0.releases == 1);
    CHECK(s1.shutdowns - s0.shutdowns == 1);
    CHECK(s1.rr_seen == 1); // feature 13 = Ray Reconstruction was requested (even though it failed)
    CHECK(s1.active_upscaler == 1); // ...but the game's CURRENT upscaler is the one that was created: Super Resolution
    CHECK(s1.module_attached == 1);
    CHECK(state()->inits == 1 && state()->creates == 2 && state()->evaluates == 6);

    // Record ring: Init, Create x2, Release, Shutdown are always recorded
    // (Evaluate records only with detail logging on).
    na::ngx::CallRecord recs[64];
    const uint32_t n = na::ngx::DrainRecords(recs, 64);
    uint32_t by_kind[6] = {};
    for (uint32_t i = 0; i < n; ++i) if (recs[i].kind < 6) ++by_kind[recs[i].kind];
    CHECK(by_kind[static_cast<uint32_t>(na::ngx::CallKind::Init)] == 1);
    CHECK(by_kind[static_cast<uint32_t>(na::ngx::CallKind::CreateFeature)] == 2);
    CHECK(by_kind[static_cast<uint32_t>(na::ngx::CallKind::ReleaseFeature)] == 1);
    CHECK(by_kind[static_cast<uint32_t>(na::ngx::CallKind::Shutdown)] == 1);
    for (uint32_t i = 0; i < n; ++i)
        if (recs[i].kind == static_cast<uint32_t>(na::ngx::CallKind::Init)) CHECK(recs[i].app_id == 0xABCDEFull);
    CHECK(na::ngx::DrainRecords(recs, 64) == 0);

    // G2-008: nested marked calls count as injector evidence only; the marker
    // unwinds so a subsequent unmarked call retains unknown provenance.
    const auto origin0 = na::ngx::GetStats();
    {
        na::dlss::InjectorCallScope outer;
        { na::dlss::InjectorCallScope inner;
          CHECK(create(fake_list, 1, nullptr, &handle) == 1);
          CHECK(eval(fake_list, handle, nullptr, nullptr) == 1); }
        CHECK(na::dlss::IsInjectorCall());
    }
    CHECK(!na::dlss::IsInjectorCall());
    auto origin1 = na::ngx::GetStats();
    CHECK(origin1.sr_creates_injector == origin0.sr_creates_injector + 1);
    CHECK(origin1.sr_creates_unknown == origin0.sr_creates_unknown);
    CHECK(create(fake_list, 1, nullptr, &handle) == 1);
    origin1 = na::ngx::GetStats();
    CHECK(origin1.sr_creates_unknown == origin0.sr_creates_unknown + 1);
    CHECK(origin1.sr_creates == origin0.sr_creates + 2);
#if defined(NA_HAVE_NGX_SDK)
    // Native FG uses a different parameter contract. Even when its object
    // retains upscaler keys, it must never be read as another SR/RR frame.
    test::FakeParams fg_params;
    fg_params.Set(NVSDK_NGX_Parameter_Width, 123u);
    fg_params.Set("DLSSG.UserInterfaceRecompositionEnabled", 1u);
    fg_params.Set("DLSSG.BackbufferFrameID", 0ull); // zero is a supplied identity
    void* fg_handle = nullptr;
    CHECK(create(nullptr, 11, &fg_params, &fg_handle) == 1);
    const auto fg0 = na::ngx::GetStats();
    CHECK(fg0.fg_last_create_uir == 1);
    const auto reads = fg_params.non_fg_reads;
    na::ngx::SetDetailLogging(true);
    CHECK(eval(nullptr, fg_handle, &fg_params, nullptr) == 1);
    CHECK(fg_params.non_fg_reads == reads);
    CHECK(na::ngx::GetStats().fg_evaluates == fg0.fg_evaluates + 1);
    CHECK(na::ngx::GetStats().fg_with_frame_id == fg0.fg_with_frame_id + 1);
    CHECK(release(fg_handle) == 1);
    // Released identities must not remain classified as FG.
    CHECK(eval(nullptr, fg_handle, nullptr, nullptr) == 1);
    CHECK(na::ngx::GetStats().fg_evaluates == fg0.fg_evaluates + 1);
#endif
    na::hooking::Shutdown();
    return test::finish("ngx_observe_test");
}
