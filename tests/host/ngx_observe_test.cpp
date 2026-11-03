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
#include "host_dx12/settings.hpp"
#include "host_dx12/nr/native_fg.hpp"
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
#if defined(NA_HAVE_NGX_SDK)
int32_t __cdecl OptimalA(void* p) { static_cast<NVSDK_NGX_Parameter*>(p)->Set("witness", 11u); return 1; }
int32_t __cdecl OptimalB(void* p) { static_cast<NVSDK_NGX_Parameter*>(p)->Set("witness", 22u); return 1; }
#endif
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
    auto release_result=reinterpret_cast<void(__cdecl*)(int32_t)>(GetProcAddress(stub,"NgxStubSetReleaseResult"));
    REQUIRE(release_result);
    release_result(static_cast<int32_t>(NVSDK_NGX_Result_Fail));
    CHECK(release(fg_handle)==static_cast<int32_t>(NVSDK_NGX_Result_Fail));
    CHECK(na::nr::native::OwnsRoute());
    CHECK(eval(nullptr,fg_handle,&fg_params,nullptr)==1);
    CHECK(na::ngx::GetStats().fg_evaluates==fg0.fg_evaluates+2);
    release_result(1);
    CHECK(release(fg_handle) == 1);
    // Released identities must not remain classified as FG.
    CHECK(eval(nullptr, fg_handle, nullptr, nullptr) == 1);
    CHECK(na::ngx::GetStats().fg_evaluates == fg0.fg_evaluates + 2);
#endif
#if defined(NA_HAVE_NGX_SDK)
    // Exercise actual patched exports, including empty allocation, failures,
    // re-acquisition, SR/RR and distinct original callback implementations.
    auto query_config = reinterpret_cast<void(__cdecl*)(void*, int32_t)>(GetProcAddress(stub, "NgxStubSetQuery"));
    using Query = int32_t(__cdecl*)(void**);
    using Callback = int32_t(__cdecl*)(void*);
    const char* sr_key = "DLSSOptimalSettingsCallback";
    const char* rr_key = "DLSSDOptimalSettingsCallback";
    na::settings::Get().enabled = 0;
    void* retained = nullptr;
    for (const char* name : {"NVSDK_NGX_D3D12_GetCapabilityParameters", "NVSDK_NGX_D3D12_GetParameters", "NVSDK_NGX_D3D12_AllocateParameters"}) {
        auto query = reinterpret_cast<Query>(GetProcAddress(stub, name));
        CHECK(query && query_config);
        test::FakeParams params;
        void* out = nullptr;
        query_config(&params, 1);
        CHECK(query(&out) == 1 && out == &params);
        CHECK(!params.Has(sr_key)); // no invented callback on empty allocation
        params.Set(sr_key, reinterpret_cast<void*>(&OptimalA));
        params.Set(rr_key, reinterpret_cast<void*>(&OptimalB));
        query_config(&params, static_cast<int32_t>(NVSDK_NGX_Result_Fail));
        CHECK(query(&out) == static_cast<int32_t>(NVSDK_NGX_Result_Fail));
        void* a = nullptr; params.Get(sr_key, &a);
        CHECK(a == reinterpret_cast<void*>(&OptimalA));
        query_config(&params, 1);
        CHECK(query(&out) == 1);
        params.Get(sr_key, &a);
        void* b = nullptr; params.Get(rr_key, &b);
        CHECK(a != reinterpret_cast<void*>(&OptimalA) && b != reinterpret_cast<void*>(&OptimalB));
        CHECK(query(&out) == 1);
        void* again = nullptr; params.Get(sr_key, &again);
        CHECK(again == a); // idempotent, never wraps a wrapper
        test::FakeParams request;
        CHECK(reinterpret_cast<Callback>(a)(&request) == 1 && request.U("witness") == 11);
        CHECK(reinterpret_cast<Callback>(b)(&request) == 1 && request.U("witness") == 22);
        na::ngx::OptimalCall calls[64]{};
        const auto opt_n=na::ngx::GetOptimalCalls(0,calls,64);
        CHECK(opt_n>=2 && calls[opt_n-2].rr==0 && calls[opt_n-1].rr==1);
        retained = a;
    }
    test::FakeParams after;
    after.Set(sr_key, reinterpret_cast<void*>(&OptimalB));
    query_config(&after, 1);
    void* other = nullptr;
    auto cap_query = reinterpret_cast<Query>(GetProcAddress(stub, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    CHECK(cap_query(&other) == 1);
    after.Get(sr_key, &other);
    CHECK(reinterpret_cast<Callback>(other)(&after) == 1 && after.U("witness") == 22);
    // A later SR callback must not retarget the retained SR pointer from A.
    CHECK(reinterpret_cast<Callback>(retained)(&after) == 1 && after.U("witness") == 11);
    query_config(nullptr, 1);
    const auto opt = na::ngx::GetOptimalStats();
    CHECK(opt.swaps_capability && opt.swaps_getparams && opt.swaps_allocate);
#endif
#if defined(NA_HAVE_NGX_SDK)
    // The old eight-Evaluate retirement could release a feature while a
    // deliberately blocked queue still owned its commands. Exercise the real
    // hooks, real queue receipts and fake backend release counters together.
    {
        test::Device gpu;
        REQUIRE(test::CreateDevice(gpu));
        REQUIRE(init(7,L"",gpu.device.Get(),0x15,nullptr)==1);
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> a,b;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> first,next;
        REQUIRE(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a))));
        REQUIRE(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&b))));
        REQUIRE(SUCCEEDED(gpu.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&first))));
        REQUIRE(SUCCEEDED(gpu.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,b.Get(),nullptr,IID_PPV_ARGS(&next))));
        test::FakeParams p;p.Set("Width",64u);p.Set("Height",64u);p.Set("OutWidth",64u);p.Set("OutHeight",64u);p.Set("PerfQualityValue",0);
        void* original=nullptr;
        set_results(1,1);
        REQUIRE(create(first.Get(),1,&p,&original)==1);
        REQUIRE(eval(first.Get(),original,&p,nullptr)==1);
        REQUIRE(SUCCEEDED(first->Close()));
        Microsoft::WRL::ComPtr<ID3D12Fence> gate;
        REQUIRE(SUCCEEDED(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))));
        REQUIRE(SUCCEEDED(gpu.queue->Wait(gate.Get(),1)));
        ID3D12CommandList* lists[]={first.Get()};
        {na::nr::native::Submission submitted(gpu.queue.Get(),1,lists);gpu.queue->ExecuteCommandLists(1,lists);}
        const auto releases_before=state()->releases;
        na::settings::Get().preset=5;
        na::ngx::RequestReapply();
        REQUIRE(eval(next.Get(),original,&p,nullptr)==1);
        CHECK(state()->last_handle_in!=reinterpret_cast<uintptr_t>(original));
        for(int i=0;i<20;++i)REQUIRE(eval(next.Get(),original,&p,nullptr)==1);
        CHECK(state()->releases==releases_before);
        REQUIRE(SUCCEEDED(next->Close()));
        na::nr::native::Reset(next.Get());na::nr::native::Maintain();
        // Reset is not completion: the first submission is still blocked.
        CHECK(state()->releases==releases_before);
        REQUIRE(SUCCEEDED(gate->Signal(1)));gpu.WaitIdle();
        na::nr::native::Maintain();
        REQUIRE(SUCCEEDED(next->Reset(b.Get(),nullptr)));
        REQUIRE(eval(next.Get(),original,&p,nullptr)==1);
        CHECK(state()->releases==releases_before); // completed, but executable first list may replay
        na::nr::native::Reset(first.Get());na::nr::native::Maintain();
        release_result(static_cast<int32_t>(NVSDK_NGX_Result_Fail));
        REQUIRE(eval(next.Get(),original,&p,nullptr)==1);
        CHECK(state()->releases==releases_before+1);
        release_result(1);
        REQUIRE(eval(next.Get(),original,&p,nullptr)==1);
        CHECK(state()->releases==releases_before+2); // failed backend release was retained and retried
        REQUIRE(SUCCEEDED(next->Close()));
        REQUIRE(release(original)==1);
        CHECK(state()->releases==releases_before+2); // mapped recording still executable
        na::nr::native::Reset(next.Get());na::nr::native::Maintain();
        // Another ordinary evaluation drives nonblocking retirement collection.
        REQUIRE(SUCCEEDED(next->Reset(b.Get(),nullptr)));
        REQUIRE(eval(next.Get(),nullptr,nullptr,nullptr)==1);
        CHECK(state()->releases==releases_before+3);
        REQUIRE(SUCCEEDED(next->Close()));na::nr::native::Reset(next.Get());na::nr::native::Maintain();
        if(!gpu.warp) {
        test::Device other_gpu;
        Microsoft::WRL::ComPtr<IDXGIAdapter> warp_adapter;
        REQUIRE(SUCCEEDED(gpu.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter))));
        REQUIRE(SUCCEEDED(D3D12CreateDevice(warp_adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&other_gpu.device))));
        REQUIRE(other_gpu.device!=gpu.device); // same-adapter D3D12CreateDevice returns the same device
        REQUIRE(init(8,L"",other_gpu.device.Get(),0x15,nullptr)==1);
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> other_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> other_list;
        REQUIRE(SUCCEEDED(other_gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&other_allocator))));
        REQUIRE(SUCCEEDED(other_gpu.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,other_allocator.Get(),nullptr,IID_PPV_ARGS(&other_list))));
        void* other_original=nullptr;
        REQUIRE(create(other_list.Get(),1,&p,&other_original)==1);
        na::ngx::RequestReapply();REQUIRE(eval(other_list.Get(),other_original,&p,nullptr)==1);
        const auto other_mapped=state()->last_handle_in;
        CHECK(other_mapped!=reinterpret_cast<uintptr_t>(other_original));
        REQUIRE(shutdown(gpu.device.Get())==1);
        REQUIRE(eval(other_list.Get(),other_original,&p,nullptr)==1);
        CHECK(state()->last_handle_in==other_mapped); // one device's shutdown must not destroy the other's map
        REQUIRE(SUCCEEDED(other_list->Close()));
        na::nr::native::Reset(other_list.Get());na::nr::native::Maintain();
        REQUIRE(release(other_original)==1);
        REQUIRE(shutdown(other_gpu.device.Get())==1);
        } else {
            std::printf("two-device SR case unavailable on WARP-only host\n");
            REQUIRE(shutdown(gpu.device.Get())==1);
        }
    }
#endif

#if defined(NA_HAVE_NGX_SDK)
    { // Production hooks: existing handle, changed subrect, unchanged allocation.
        test::Device gpu;REQUIRE(test::CreateDevice(gpu));na::settings::Get().enabled=0;
        REQUIRE(init(9,L"",gpu.device.Get(),0x15,nullptr)==1);
        auto texture=test::CreateTexture(gpu.device.Get(),64,64,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);
        REQUIRE(texture);test::FakeParams p;
        p.Set(NVSDK_NGX_Parameter_Width,64u);p.Set(NVSDK_NGX_Parameter_Height,64u);
        p.Set(NVSDK_NGX_Parameter_OutWidth,64u);p.Set(NVSDK_NGX_Parameter_OutHeight,64u);
        p.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,2u);
        p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X,0.f);p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f);
        p.Set(NVSDK_NGX_Parameter_MV_Scale_X,1.f);p.Set(NVSDK_NGX_Parameter_MV_Scale_Y,1.f);
        for(const char* k:{NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Output})p.Set(k,texture.Get());
        for(const char* k:{NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y,
            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
            NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y,
            NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y})p.Set(k,0u);
        for(uint32_t feature:{1u,13u}) {
            void* h=nullptr;set_results(1,1);REQUIRE(create(nullptr,feature,&p,&h)==1);
            const auto id=na::ngx::ArmLiveSizeConfirmation(32,32,64,64,2);na::ngx::MarkSizeConfirmationRestored(id);
            p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,32u);p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,32u);
            REQUIRE(eval(nullptr,h,&p,nullptr)==1);REQUIRE(eval(nullptr,h,&p,nullptr)==1);
            const auto c=na::ngx::GetSizeConfirmation();CHECK(c.state==3);CHECK(c.creations==0);CHECK(c.last_w==32&&c.last_h==32);
            REQUIRE(release(h)==1);
        }
        REQUIRE(shutdown(gpu.device.Get())==1);
    }
#endif
    na::hooking::Shutdown();
    return test::finish("ngx_observe_test");
}
