// Actual hooked NGX startup with a real WARP D3D12 device and stand-in runtime.
// No session worker runs before the first query: this exposes asynchronous
// qualification races deterministically. No game, real DLSS model or profile.
#include "test_support.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/adapters/mode_control.hpp"
#include "host_dx12/dlss/call_origin.hpp"
#include <filesystem>
#include <thread>
#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_params.hpp"
#endif
namespace {std::atomic<unsigned> activations{0};}

int wmain(int argc,wchar_t** argv){
    REQUIRE(argc==3);
    const bool project=!wcscmp(argv[2],L"project"), recovery=!wcscmp(argv[2],L"recovery");
    wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
    const auto tag=L"NA-init-"+std::to_wstring(GetCurrentProcessId());
    const auto ini=std::filesystem::path(temp)/(tag+L".ini");
    WritePrivateProfileStringW(L"settings",L"scope",tag.c_str(),ini.c_str());
    WritePrivateProfileStringW(L"dlss",L"mode",L"3",ini.c_str()); // Performance
    WritePrivateProfileStringW(L"dlss",L"preset",L"11",ini.c_str());
    na::settings::SetIniPath(ini.c_str());na::settings::Load();
    const std::wstring user=na::settings::UserIniPath(), stale=user+L".session";
    if(recovery){HANDLE f=CreateFileW(stale.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);REQUIRE(f!=INVALID_HANDLE_VALUE);CloseHandle(f);}
    REQUIRE(na::hooking::Initialize());na::host::StartSession();
    auto stub=LoadLibraryW(argv[1]);REQUIRE(stub);
    REQUIRE(na::ngx::InstallForModule(stub)>=6);
    using Ext=int32_t(__cdecl*)(unsigned long long,const wchar_t*,void*,uint32_t,const void*);
    using Project=int32_t(__cdecl*)(const char*,uint32_t,const char*,const wchar_t*,void*,uint32_t,const void*);
    auto ext=reinterpret_cast<Ext>(GetProcAddress(stub,"NVSDK_NGX_D3D12_Init_Ext"));
    auto pid=reinterpret_cast<Project>(GetProcAddress(stub,"NVSDK_NGX_D3D12_Init_ProjectID"));
    auto set_result=reinterpret_cast<void(__cdecl*)(int32_t)>(GetProcAddress(stub,"NgxStubSetInitResult"));
    REQUIRE(ext&&pid&&set_result);
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter> warp;Microsoft::WRL::ComPtr<ID3D12Device> device;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
    auto init=[&](void* d){return project?pid("session-test",0,"test",temp,d,0x15,nullptr):ext(1,temp,d,0x15,nullptr);};
    CHECK(init(nullptr)==1);CHECK(!na::settings::EffectsReady()&&!na::host::SessionReady());
    set_result(static_cast<int32_t>(0xBAD00002));
    CHECK(init(device.Get())==static_cast<int32_t>(0xBAD00002));CHECK(!na::settings::EffectsReady());
    set_result(1);
    {na::dlss::InjectorCallScope own;CHECK(init(device.Get())==1);CHECK(!na::settings::EffectsReady());}
    CHECK(init(device.Get())==1);
    CHECK(na::host::SessionReady()&&na::settings::EffectsReady());
    CHECK(na::settings::SafeStart()==recovery);
#if defined(NA_HAVE_NGX_SDK)
    test::FakeParams query;query.Set(NVSDK_NGX_Parameter_Width,1920u);query.Set(NVSDK_NGX_Parameter_Height,1080u);
    query.Set(NVSDK_NGX_Parameter_OutWidth,1280u);query.Set(NVSDK_NGX_Parameter_OutHeight,720u);
    CHECK(na::ngx::OptimalSettingsCallback(&query)==1);
    CHECK(query.U(NVSDK_NGX_Parameter_OutWidth)==(recovery?1280u:960u));
    CHECK(query.U(NVSDK_NGX_Parameter_OutHeight)==(recovery?720u:544u));
    test::FakeParams create;create.Set(NVSDK_NGX_Parameter_Width,640u);create.Set(NVSDK_NGX_Parameter_Height,360u);
    create.Set(NVSDK_NGX_Parameter_OutWidth,1920u);create.Set(NVSDK_NGX_Parameter_OutHeight,1080u);
    create.Set(NVSDK_NGX_Parameter_PerfQualityValue,2u);
    using Create=int32_t(__cdecl*)(void*,uint32_t,void*,void**);
    auto make=reinterpret_cast<Create>(GetProcAddress(stub,"NVSDK_NGX_D3D12_CreateFeature"));REQUIRE(make);
    void* handle=nullptr;CHECK(make(nullptr,1,&create,&handle)==1);
    CHECK(create.U(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA)==(recovery?UINT32_MAX:11u));
    CHECK(na::ngx::GetStats().last_sr_out_w==1920);
#endif
    // With observed size deliberately different, startup still cannot enqueue
    // the engine Apply path. The old ActivateSavedRequests did so here.
    na::mode::Start();na::host::ActivateSavedRequests(ini.c_str());
    CHECK(na::mode::GetStatus().request_id==0);
    std::thread worker([]{na::host::RunSessionWorker([]{++activations;});});
    for(unsigned i=0;i<1000 && !activations.load();++i)Sleep(5);
    CHECK(activations.load()==1);na::host::RendererObserved();na::host::RendererInitialized();
    na::host::StopSessionForTests();worker.join();
    CHECK(activations.load()==1);
    CHECK(!std::filesystem::exists(user+L".session."+std::to_wstring(GetCurrentProcessId())));
    REQUIRE(std::filesystem::path(user).parent_path().filename()==tag);
    std::filesystem::remove_all(std::filesystem::path(user).parent_path());std::filesystem::remove(ini);
    return test::finish("session_init_test");
}
