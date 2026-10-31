// Route A boot -> early factory gate -> recorded queue -> Home -> real GPU
// draw. Observation producers are OFF, as in Ghost. No game or model is run.
#include "test_support.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
#include "host_dx12/host_boot/process_log.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/settings.hpp"
#include <filesystem>

int main(int argc,char**) {
    SetEnvironmentVariableW(L"NEURALALIAS_TEST_WARP",L"1");
    wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
    const auto dir=std::filesystem::path(exe).parent_path();
    const auto suffix=std::to_wstring(GetCurrentProcessId());
    const auto name=L"overlay-bootstrap-"+suffix+L".ini";
    const auto ini=dir/name;
    const auto scope=L"OverlayBootstrap-"+suffix;
    const auto startup=(dir/(L"overlay-bootstrap-"+suffix+L".log")).wstring();
    auto set=[&](const wchar_t* section,const wchar_t* key,const wchar_t* value){
        CHECK(WritePrivateProfileStringW(section,key,value,ini.c_str())!=0);
    };
    set(L"host",L"enabled",L"1");set(L"host",L"startup_log",startup.c_str());
    set(L"route_a",L"enabled",L"1");set(L"overlay",L"enabled",L"1");
    set(L"observe",L"producers",L"0");set(L"observe",L"early_install",L"1");
    set(L"adapter",L"enabled",L"0");set(L"fsr",L"enabled",L"0");set(L"xess",L"enabled",L"0");
    set(L"unreal",L"probe",L"0");set(L"unreal",L"screen_percentage",L"0");
    set(L"settings",L"scope",scope.c_str());set(L"present",L"proxy",L"0");
    set(L"dlss",L"enabled",L"0");
    if(argc>1){
        na::settings::SetIniPath(ini.c_str());na::settings::Load();
        const auto marker=std::wstring(na::settings::UserIniPath())+L".session";
        HANDLE f=CreateFileW(marker.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        REQUIRE(f!=INVALID_HANDLE_VALUE);CloseHandle(f);
    }
    REQUIRE(na::host::StartFromIni(GetModuleHandleW(nullptr),name.c_str(),"BOOT TEST"));
    // Same ordering as our DXGI proxy's exported CreateDXGIFactory entry.
    na::host::WaitForObserveInstall();
    test::Device d;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0,IID_PPV_ARGS(&d.factory))));
    test::ComPtr<IDXGIAdapter> warp;
    REQUIRE(SUCCEEDED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d.device))));
    D3D12_COMMAND_QUEUE_DESC q{};
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q,IID_PPV_ARGS(&d.queue))));
    REQUIRE(SUCCEEDED(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&d.fence))));
    d.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HWND window=test::CreateTestWindow(L"Route A bootstrap test");REQUIRE(window);
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=320;desc.Height=180;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    test::ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(),window,&desc,nullptr,nullptr,&chain)));
    // No Execute or transition has occurred: only early creation can supply it.
    test::ComPtr<ID3D12CommandQueue> observed;
    observed.Attach(na::overlay::AcquirePresentQueue(chain.Get()));
    CHECK(observed.Get()==d.queue.Get());
    REQUIRE(SUCCEEDED(chain->Present(0,0)));d.WaitIdle();
    CHECK(na::overlay::GetStats().initialized==1);
    SendMessageW(window,WM_KEYDOWN,VK_HOME,1);
    REQUIRE(SUCCEEDED(chain->Present(0,0)));d.WaitIdle();
    CHECK(na::overlay::GetStats().open && na::overlay::GetStats().frames_rendered>0);
    CHECK(na::host::GetStatus().host_started==1);
    CHECK(na::settings::SafeStart()==(argc>1));
    CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    REQUIRE(na::overlay::Stop());chain.Reset();DestroyWindow(window);
    DeleteFileW(ini.c_str());
    wchar_t processLog[MAX_PATH]{};
    na::host::ProcessLogPath(processLog,MAX_PATH,startup.c_str());DeleteFileW(processLog);
    // Worker log/session threads end with the test process; no game is touched.
    return test::finish("overlay_bootstrap_test");
}
