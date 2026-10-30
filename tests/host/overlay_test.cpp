// Reconstructed 2026-10 (C3). Mirrors WORKLOG-ROUTE-A A16/A20: the in-game
// overlay attached to a real window + swap chain in this process. Uses WARP
// for both the overlay's vtable-reading dummy device (NEURALALIAS_TEST_WARP)
// and the "game" device, so the hooked vtables are the ones the game uses.
//   - Start succeeds; presents route through the hooks (presents_seen grows);
//   - lazy init happens on the first game Present, the DIRECT queue is captured
//     from ExecuteCommandLists;
//   - nothing is drawn while closed; Home opens it, frames render, Home closes;
//   - while open, mouse/keyboard messages are consumed (not passed to the game WndProc);
//   - ResizeBuffers survives; Stop() tears down cleanly.
#include "test_support.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/overlay/present_queues.hpp"
#include "host_dx12/overlay/route_a_panel.hpp"
#include "host_dx12/settings.hpp"
#include <filesystem>

#include <atomic>

namespace {
std::atomic<uint32_t> g_game_mouse{0}, g_game_keys{0};
LRESULT CALLBACK GameWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN) g_game_mouse.fetch_add(1);
    if (m == WM_KEYDOWN && w != VK_HOME) g_game_keys.fetch_add(1);
    return DefWindowProcW(h, m, w, l);
}
void Pump() { MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
} // namespace

int main(int argc, char** argv)
{
    const bool hardware = argc > 1 && !strcmp(argv[1], "--hardware");
    SetEnvironmentVariableW(L"NEURALALIAS_TEST_WARP", hardware ? nullptr : L"1");
    wchar_t temp[MAX_PATH]{}, ini[MAX_PATH]{}, scope[64]{};
    GetTempPathW(MAX_PATH, temp);
    swprintf_s(ini,L"%sNA-overlay-test-%lu.ini",temp,GetCurrentProcessId());
    swprintf_s(scope,L"OverlayTest-%lu",GetCurrentProcessId());
    WritePrivateProfileStringW(L"settings",L"scope",scope,ini);
    WritePrivateProfileStringW(L"dlss",L"enabled",L"0",ini);
    WritePrivateProfileStringW(L"dlss",L"mode",L"4",ini);
    WritePrivateProfileStringW(L"fg",L"preset",L"3",ini);
    na::settings::SetIniPath(ini); na::settings::Load();
    test::Device d;
    // The game device must be WARP too (vtables are per driver).
    if (hardware) {
        if (!test::CreateDevice(d, false)) return test::SkipNoDevice("overlay_hardware_test");
    } else {
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory)))) return test::SkipNoDevice("overlay_test");
        test::ComPtr<IDXGIAdapter> warp;
        if (FAILED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device))))
            return test::SkipNoDevice("overlay_test");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q, IID_PPV_ARGS(&d.queue))));
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence))));
        d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    REQUIRE(na::hooking::Initialize());
    na::overlay::RegisterRouteAMenu();
    REQUIRE(na::overlay::Start("TEST", nullptr));

    WNDCLASSW wc{}; wc.lpfnWndProc = GameWndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NeuralAliasOverlayTestGame";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"overlay_test", WS_OVERLAPPEDWINDOW, 100, 100, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    REQUIRE(hwnd != nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 640; sd.Height = 360; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    test::ComPtr<IDXGISwapChain1> sc1;
    REQUIRE(SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1)));
    test::ComPtr<IDXGISwapChain3> sc;
    REQUIRE(SUCCEEDED(sc1.As(&sc)));

    // One "game frame": clear the back buffer on the game's DIRECT queue, present.
    test::List l; REQUIRE(l.Create(d.device.Get())); l.list->Close();
    auto frame = [&]() {
        test::ComPtr<ID3D12Resource> bb;
        sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
        l.Reset();
        auto b0 = test::Transition(bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        auto b1 = test::Transition(bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        l.list->ResourceBarrier(1, &b0);
        l.list->ResourceBarrier(1, &b1);
        l.list->Close();
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        const HRESULT hr = sc->Present(0, 0);
        d.WaitIdle();
        Pump();
        return hr;
    };

    for (int i = 0; i < 5; ++i) CHECK(SUCCEEDED(frame()));
    auto s = na::overlay::GetStats();
    CHECK(s.presents_seen >= 5);
    CHECK(s.initialized == 1);
    CHECK(s.have_queue == 1);
    CHECK(s.open == 0);
    const uint64_t closed_rendered = s.frames_rendered;
    for (int i = 0; i < 3; ++i) frame();
    CHECK(na::overlay::GetStats().frames_rendered == closed_rendered); // draws nothing while closed

    // Home opens the menu (first press, not auto-repeat).
    SendMessageW(hwnd, WM_KEYDOWN, VK_HOME, 1);
    for (int i = 0; i < 4; ++i) frame();
    s = na::overlay::GetStats();
    CHECK(s.open == 1);
    CHECK(s.frames_rendered > closed_rendered);

    // A two-image swapchain must not restrict ImGui to two in-flight draws.
    // Exercise the normal draw routine without DXGI Present's own throttling.
    auto menu_target=test::CreateTexture(d.device.Get(),640,360,DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    test::ComPtr<ID3D12Fence> menu_gate;
    REQUIRE(menu_target && SUCCEEDED(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&menu_gate))));
    d.WaitIdle();
    REQUIRE(SUCCEEDED(d.queue->Wait(menu_gate.Get(),1)));
    const auto before_backlog=na::overlay::GetStats().frames_rendered;
    for(int i=0;i<8;++i) na::overlay::DrawMenuForTests(d.queue.Get(),menu_target.Get());
    CHECK(na::overlay::GetStats().frames_rendered==before_backlog+8);
    na::overlay::DrawMenuForTests(d.queue.Get(),menu_target.Get());
    CHECK(na::overlay::GetStats().frames_rendered==before_backlog+8); // never overwrite a busy slot
    REQUIRE(SUCCEEDED(menu_gate->Signal(1))); d.WaitIdle();
    na::overlay::DrawMenuForTests(d.queue.Get(),menu_target.Get()); d.WaitIdle();
    CHECK(na::overlay::GetStats().frames_rendered==before_backlog+9);
    REQUIRE(d.device->GetDeviceRemovedReason()==S_OK);

    // A later independent queue must not retire an older blocked draw.
    {
        test::ComPtr<ID3D12CommandQueue> fast;
        D3D12_COMMAND_QUEUE_DESC fastDesc{};fastDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&fastDesc,IID_PPV_ARGS(&fast))));
        auto other=test::CreateTexture(d.device.Get(),640,360,DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        test::ComPtr<ID3D12Fence> completed;
        REQUIRE(other && SUCCEEDED(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completed))));
        REQUIRE(SUCCEEDED(d.queue->Wait(menu_gate.Get(),2)));
        const auto count0=na::overlay::GetStats().frames_rendered;
        na::overlay::DrawMenuForTests(d.queue.Get(),menu_target.Get());
        for(int j=0;j<7;++j)na::overlay::DrawMenuForTests(fast.Get(),other.Get());
        REQUIRE(SUCCEEDED(fast->Signal(completed.Get(),1)));
        auto wait=test::WaitFence(completed.Get(),1,d.event,5000);
        if(wait!=test::WaitResult::Ok){menu_gate->Signal(2);return 2;}
        const auto beforeReuse=na::overlay::GetStats().frames_rendered;
        CHECK(beforeReuse==count0+8);
        na::overlay::DrawMenuForTests(fast.Get(),other.Get());
        const auto afterReuse=na::overlay::GetStats().frames_rendered;
        CHECK(afterReuse==beforeReuse); // blocked draw's allocator must not be reused
        REQUIRE(SUCCEEDED(menu_gate->Signal(2)));d.WaitIdle();
        REQUIRE(SUCCEEDED(fast->Signal(completed.Get(),2)));
        REQUIRE(test::WaitFence(completed.Get(),2,d.event,5000)==test::WaitResult::Ok);
        na::overlay::DrawMenuForTests(fast.Get(),other.Get());
        CHECK(na::overlay::GetStats().frames_rendered==beforeReuse+1);
        REQUIRE(SUCCEEDED(fast->Signal(completed.Get(),3)));
        REQUIRE(test::WaitFence(completed.Get(),3,d.event,5000)==test::WaitResult::Ok);
    }

    // While open the game receives no mouse/keyboard input.
    const uint32_t mouse_before = g_game_mouse.load(), keys_before = g_game_keys.load();
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(50, 60));
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 60));
    SendMessageW(hwnd, WM_KEYDOWN, 'W', 1);
    frame();
    CHECK(g_game_mouse.load() == mouse_before);
    CHECK(g_game_keys.load() == keys_before);

    // Home again closes it and input flows to the game again.
    SendMessageW(hwnd, WM_KEYDOWN, VK_HOME, 1);
    frame();
    CHECK(na::overlay::GetStats().open == 0);
    SendMessageW(hwnd, WM_KEYDOWN, 'W', 1);
    CHECK(g_game_keys.load() == keys_before + 1);

    // Resize survives.
    d.WaitIdle();
    CHECK(SUCCEEDED(sc->ResizeBuffers(0, 800, 450, DXGI_FORMAT_UNKNOWN, 0)));
    for (int i = 0; i < 3; ++i) CHECK(SUCCEEDED(frame()));
    CHECK(na::overlay::GetStats().presents_seen >= s.presents_seen + 4);

    // A native FG transition may reassign the same chain to another queue.
    // Exercise the real ResizeBuffers1 hook, not just creation or last-submit.
    test::ComPtr<ID3D12CommandQueue> replacement;
    D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&qd,IID_PPV_ARGS(&replacement))));
    IUnknown* queues[]={replacement.Get(),replacement.Get()};UINT masks[]={1,1};
    na::overlay::PresentQueues binding;binding.Created(d.queue.Get());
    CHECK(!binding.Resized(E_FAIL,2,queues));CHECK(binding.At(1).Get()==d.queue.Get());
    IUnknown* alternating[]={d.queue.Get(),replacement.Get()};
    CHECK(binding.Resized(S_OK,2,alternating));CHECK(binding.At(0).Get()==d.queue.Get());CHECK(binding.At(1).Get()==replacement.Get());
    d.WaitIdle();
    REQUIRE(SUCCEEDED(sc->ResizeBuffers1(2,800,450,DXGI_FORMAT_UNKNOWN,0,masks,queues)));
    d.queue=replacement; // test render work uses the queue we just assigned
    na::overlay::SetOpen(true);
    for(int i=0;i<3;++i){CHECK(SUCCEEDED(frame()));CHECK(na::overlay::GetStats().present_queue==reinterpret_cast<uintptr_t>(replacement.Get()));}

    d.WaitIdle();
    bool stopped = false;
    for (int i = 0; i < 20 && !stopped; ++i) { stopped = na::overlay::Stop(); if (!stopped) Sleep(100); }
    CHECK(stopped);
    sc.Reset(); sc1.Reset();
    DestroyWindow(hwnd);
    na::hooking::Shutdown();
    DeleteFileW(na::settings::UserIniPath());
    std::filesystem::remove(std::filesystem::path(na::settings::UserIniPath()).parent_path());
    DeleteFileW(ini);
    return test::finish("overlay_test");
}
