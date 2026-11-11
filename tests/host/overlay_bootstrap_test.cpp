// Route A boot -> early factory gate -> recorded queue -> Home -> real GPU
// draw. Observation producers are OFF, as in Ghost. No game or model is run.
#include "test_support.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
#include "host_dx12/host_boot/process_log.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/native_present/size_probe.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/scale/auto_scene.hpp"
#include "host_dx12/scale/live_driver.hpp"
#include "host_dx12/scale/native_control.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"
#include <d3d11.h>
#include <d3d11on12.h>
#include <filesystem>

namespace {std::atomic<unsigned> submits{0},devices{0};
void submitted(na::overlay::ExecuteContext*){submits.fetch_add(1);}
void device_seen(ID3D12Device*,void*){devices.fetch_add(1);}}
namespace {
struct PresentOrder {
    uint64_t frame;
    std::atomic<unsigned> submissions{0}, wrong_frame{0};
};
void record_present_order(na::overlay::ExecuteContext* context)
{
    auto& test = *static_cast<PresentOrder*>(context->user);
    if (na::eventlog::Frame() != test.frame) ++test.wrong_frame;
    ++test.submissions;
}

int native_on12_present_order(IDXGIFactory4* factory, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc)
{
    test::ComPtr<ID3D11Device> device;
    test::ComPtr<ID3D11DeviceContext> context;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, &context)));
    test::ComPtr<ID3D11On12Device> translated;
    REQUIRE(SUCCEEDED(device.As(&translated)));
    test::ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &chain)));
    test::ComPtr<ID3D11Texture2D> buffer;
    REQUIRE(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer))));
    test::ComPtr<ID3D12Resource> native_buffer;
    CHECK(FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&native_buffer))));
    test::ComPtr<ID3D11RenderTargetView> view;
    REQUIRE(SUCCEEDED(device->CreateRenderTargetView(buffer.Get(), nullptr, &view)));
    ShowWindow(window, SW_SHOWNOACTIVATE);
    REQUIRE(SUCCEEDED(chain->Present(0, 0))); // finish hook/bootstrap work before measuring
    for(unsigned i=0;i<400 && !na::host::SessionReady();++i)Sleep(5);
    REQUIRE(na::j2::Enabled() && !na::j2::Paused());
    test::Device backend;
    backend.queue.Attach(na::overlay::AcquirePresentQueue(chain.Get()));
    REQUIRE(backend.queue);
    REQUIRE(SUCCEEDED(backend.queue->GetDevice(IID_PPV_ARGS(&backend.device))));
    REQUIRE(SUCCEEDED(backend.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&backend.fence))));
    backend.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(backend.event);
    test::List control;
    REQUIRE(control.Create(backend.device.Get()));
    REQUIRE(SUCCEEDED(control.list->Close()));
    REQUIRE(na::overlay::UnsubscribeExecute()); // the warm-up can still be submitting
    PresentOrder order{na::eventlog::Frame()};
    REQUIRE(na::overlay::SubscribeExecute(nullptr, &record_present_order, &order));
    ID3D12CommandList* lists[]{control.list.Get()};
    backend.queue->ExecuteCommandLists(1, lists);
    CHECK(order.submissions.exchange(0) == 1);
    const float color[]{0.25f, 0.5f, 0.75f, 1.0f};
    context->ClearRenderTargetView(view.Get(), color); // intentionally leave it batched for Present
    const HRESULT presented = chain->Present(0, 0);
    for (unsigned i = 0; i < 100 && !order.submissions.load(); ++i) Sleep(1);
    REQUIRE(na::overlay::UnsubscribeExecute());
    CHECK(SUCCEEDED(presented));
    std::printf("D3D11On12 submissions=%u assigned_to_next_frame=%u\n",
                order.submissions.load(), order.wrong_frame.load());
    CHECK(order.submissions.load() > 0);
    CHECK(order.wrong_frame.load() == 0);
    CHECK(na::eventlog::Frame() == order.frame + 1);
    backend.WaitIdle();
    return 0;
}
} // namespace

int main(int argc,char** argv) {
    const bool menu_failure=argc>1 && !strcmp(argv[1],"--menu-failure");
    const bool cursor_cleanup=argc>1 && !strcmp(argv[1],"--cursor-cleanup");
    const bool optional_input=argc>1 && !strcmp(argv[1],"--optional-input");
    const bool create_failure=argc>1 && !strcmp(argv[1],"--menu-create-failure");
    const bool recovery=argc>1 && !strcmp(argv[1],"--recovery");
    const bool added_upscaling_off=argc>1 && !strcmp(argv[1],"--added-upscaling-off");
    const bool on12_ownership=argc>1 && !strcmp(argv[1],"--on12-ownership");
    const bool on12_present_order=argc>1 && !strcmp(argv[1],"--on12-present-order");
    SetEnvironmentVariableW(L"NEURALALIAS_TEST_WARP",on12_present_order?L"0":L"1");
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
    set(L"observe",L"producers",on12_present_order?L"1":L"0");set(L"observe",L"early_install",L"1");
    set(L"adapter",L"enabled",L"0");set(L"fsr",L"enabled",L"0");set(L"xess",L"enabled",L"0");
    set(L"unreal",L"probe",L"0");set(L"unreal",L"screen_percentage",L"0");
    set(L"settings",L"scope",scope.c_str());set(L"present",L"proxy",L"0");
    set(L"dlss",L"enabled",L"0");
    if(on12_ownership || on12_present_order)set(L"dx11on12",L"enabled",L"1");
    if(on12_present_order)set(L"dx11on12",L"keep_game_swapchain",L"1");
    if(on12_present_order)set(L"j2",L"enabled",L"1");
    if (added_upscaling_off) {
        set(L"route_a", L"enabled", L"0");
        set(L"fsr", L"enabled", L"1");
        set(L"fsr", L"replace", L"1");
        set(L"xess", L"enabled", L"1");
        set(L"xess", L"replace", L"1");
        set(L"observe", L"producers", L"1");
        set(L"j2", L"enabled", L"1");
        set(L"j2", L"scale_mode", L"3");
        set(L"j2", L"native_scale", L"1");
        set(L"j2", L"read_hook", L"1");
        set(L"j2", L"window", L"1");
        set(L"present", L"proxy", L"1");
        set(L"present", L"render_small", L"1");
        set(L"present", L"scene_resources", L"1");
        set(L"layer2", L"enabled", L"1");
    }
    if(recovery){
        na::settings::SetIniPath(ini.c_str());na::settings::Load();
        const auto marker=std::wstring(na::settings::UserIniPath())+L".session";
        HANDLE f=CreateFileW(marker.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        REQUIRE(f!=INVALID_HANDLE_VALUE);CloseHandle(f);
    }
    if(menu_failure)na::overlay::FailNextMenuStartForTests();
    if(optional_input)na::overlay::FailOptionalInputForTests(3);
    if(cursor_cleanup)na::overlay::FailCursorCleanupForTests();
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
    if(on12_present_order){
        for(unsigned i=0;i<400 && !na::host::GetStatus().init_complete;++i)Sleep(5);
        REQUIRE(na::host::GetStatus().init_complete==1);
        REQUIRE(native_on12_present_order(d.factory.Get(),window,desc)==0);
        na::host::StopSessionForTests();
        REQUIRE(na::overlay::Stop());
        DestroyWindow(window);
        DeleteFileW(ini.c_str());
        return test::finish("overlay_bootstrap_test on12 present order");
    }
    test::ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(),window,&desc,nullptr,nullptr,&chain)));
    // No Execute or transition has occurred: only early creation can supply it.
    test::ComPtr<ID3D12CommandQueue> observed;
    observed.Attach(na::overlay::AcquirePresentQueue(chain.Get()));
    CHECK(observed.Get()==d.queue.Get());
    if (added_upscaling_off) {
        // This WARP fixture also installs the DX12 observation producers. Allow
        // that setup to finish before checking policy, even if the factory gate
        // released the test thread first. The production gate stays unchanged.
        const ULONGLONG init_deadline = GetTickCount64() + 20000;
        while (!na::host::GetStatus().init_complete && GetTickCount64() < init_deadline) Sleep(5);
    } else {
        for(unsigned i=0;i<400 && !na::host::GetStatus().init_complete;++i)Sleep(5);
    }
    REQUIRE(na::host::GetStatus().init_complete==1);
    if(on12_ownership){
        const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
        auto make_device=[&](bool translated){
            test::ComPtr<ID3D11Device> dev;
            test::ComPtr<ID3D11DeviceContext> context;
            REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
                &level,1,D3D11_SDK_VERSION,&dev,nullptr,&context)));
            test::ComPtr<ID3D11On12Device> bridge;
            CHECK(SUCCEEDED(dev.As(&bridge))==translated);
            test::ComPtr<ID3D12CommandQueue> queue;
            queue.Attach(na::on12::QueueFor(dev.Get()));
            CHECK(bool(queue)==translated);
            return 0;
        };
        {
            // A DX11 helper created while our bootstrap owns graphics setup
            // must remain native. Returning to the game thread restores wrapping.
            na::host::InternalFactoryScope internal;
            {na::host::InternalFactoryScope nested;REQUIRE(make_device(false)==0);}
            REQUIRE(make_device(false)==0);
        }
        REQUIRE(make_device(true)==0);
    }
    if(create_failure)na::overlay::FailNextMenuCreateForTests();
    REQUIRE(SUCCEEDED(chain->Present(0,0)));d.WaitIdle();
    if(cursor_cleanup){
        auto* target=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetCursor"));
        CHECK(na::hooking::DeadHookCount()>0);
        CHECK(na::overlay::CursorOriginalForTests()==na::hooking::OriginalFor(target));
        CHECK(na::overlay::CursorOriginalForTests()!=target);
    }
    if(optional_input)CHECK(na::overlay::GetStats().menu_enabled==1 && na::overlay::GetStats().input_hooks==7);
    if(create_failure){
        CHECK(na::overlay::GetStats().core_ready==1);
        CHECK(na::overlay::GetStats().menu_enabled==1 && na::overlay::GetStats().initialized==0);
        REQUIRE(na::overlay::SubscribeExecute(nullptr,&submitted,nullptr));
        REQUIRE(na::overlay::SubscribeDiscovery(&device_seen,nullptr,nullptr,nullptr));
        test::List work;REQUIRE(work.Create(d.device.Get()));REQUIRE(SUCCEEDED(work.list->Close()));
        ID3D12CommandList* lists[]={work.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        CHECK(submits.load()==1);
        for(unsigned i=0;i<100 && !devices.load();++i)Sleep(5);
        CHECK(devices.load()>0);
        CHECK(na::host::GetStatus().route_a_started==1);
        REQUIRE(na::overlay::UnsubscribeExecute());na::overlay::UnsubscribeDiscovery();
        REQUIRE(na::overlay::StartMenu()); // retry input setup, leaving core hooks untouched
        REQUIRE(SUCCEEDED(chain->Present(0,0)));d.WaitIdle();
    }
    CHECK(na::overlay::GetStats().initialized==1);
    SendMessageW(window,WM_KEYDOWN,VK_HOME,1);
    REQUIRE(SUCCEEDED(chain->Present(0,0)));d.WaitIdle();
    CHECK(na::overlay::GetStats().open && na::overlay::GetStats().frames_rendered>0);
    CHECK(na::host::GetStatus().host_started==1);
    for(unsigned i=0;i<400 && !na::host::SessionReady();++i)Sleep(5);
    REQUIRE(na::host::SessionReady());
    CHECK(na::settings::SafeStart()==recovery);
    if (added_upscaling_off) {
        CHECK(na::j2::Configured());
        CHECK(na::ffx::GetMode() == na::ffx::Mode::Observe);
        CHECK(na::xess::GetMode() == na::xess::Mode::Observe);
        CHECK(na::settings::Get().j2_scale_mode.load() == 3); // keep the old preference
        CHECK(!na::j2::RenderSmall() && !na::j2::ProjectionWindow());
        CHECK(!na::scale::SliderRouteActive() && !na::scale::native::Available());
        CHECK(!na::scale::GetAutomaticSceneStats().enabled);
        CHECK(!na::native_present::SizeProbeInstalled());
        CHECK(!na::present::Scaling() && na::present::RenderScale() == 1.0f);
        DXGI_SWAP_CHAIN_DESC1 actual{};
        REQUIRE(SUCCEEDED(chain->GetDesc1(&actual)));
        CHECK(actual.Width == desc.Width && actual.Height == desc.Height);
    }
    CHECK(d.device->GetDeviceRemovedReason()==S_OK);
    na::host::StopSessionForTests();
    REQUIRE(na::overlay::Stop());chain.Reset();DestroyWindow(window);
    DeleteFileW(ini.c_str());
    wchar_t processLog[MAX_PATH]{};
    na::host::ProcessLogPath(processLog,MAX_PATH,startup.c_str());DeleteFileW(processLog);
    // Worker log/session threads end with the test process; no game is touched.
    return test::finish("overlay_bootstrap_test");
}
