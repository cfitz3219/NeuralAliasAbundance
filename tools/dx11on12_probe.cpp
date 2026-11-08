// DX11-on-DX12 bridge probe (no game needed). Run it in a folder that holds
// NeuralAliasDXGI.dll renamed to dxgi.dll plus a NeuralAliasHost.ini with
// [dx11on12] enabled=1 (and the usual host/observe/j2 keys). It behaves like a
// small DX11 game: DXGI factory first (as games enumerate adapters), then
// D3D11CreateDeviceAndSwapChain, then N frames of clears + one triangle-free
// draw call and Present. It answers, in its own output and the host's status log:
//   1. did the game's device become an 11on12 device on our D3D12 queue?
//   2. can the swap chain's back buffer be read as an ID3D12Resource (the menu needs it)?
//   3. does the device survive N presents (no removal)?
// Exit 0 = device wrapped and alive, 1 = not wrapped or removed.
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv)
{
    const int frames = argc > 1 ? std::atoi(argv[1]) : 300;
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); // waits for the host install, like a game's adapter scan
    std::printf("factory hr=0x%08lX\n", static_cast<unsigned long>(hr));
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NAOn12Probe";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"NeuralAlias dx11on12 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               100, 100, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 2;
    d.BufferDesc.Width = 1280; d.BufferDesc.Height = 720; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = wnd; d.SampleDesc.Count = 1; d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL got{};
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                       &d, &sc, &dev, &got, &ctx);
    std::printf("create hr=0x%08lX level=0x%X\n", static_cast<unsigned long>(hr), static_cast<unsigned>(got));
    if (FAILED(hr)) return 1;
    ComPtr<ID3D11On12Device> on12;
    const bool wrapped = SUCCEEDED(dev.As(&on12));
    std::printf("wrapped_by_11on12=%d\n", wrapped ? 1 : 0);
    ComPtr<ID3D12Resource> as12;
    hr = sc->GetBuffer(0, IID_PPV_ARGS(&as12));
    std::printf("getbuffer_as_d3d12 hr=0x%08lX ok=%d\n", static_cast<unsigned long>(hr), as12 ? 1 : 0);
    as12.Reset();
    ComPtr<ID3D12Device> dev12;
    hr = sc->GetDevice(IID_PPV_ARGS(&dev12));
    std::printf("swapchain_getdevice_as_d3d12 hr=0x%08lX\n", static_cast<unsigned long>(hr));
    dev12.Reset();
    ComPtr<ID3D11Texture2D> back;
    sc->GetBuffer(0, IID_PPV_ARGS(&back));
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    // A small scene with depth so discovery has something to look at.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1280; td.Height = 720; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    dev->CreateTexture2D(&td, nullptr, &depth);
    if (depth) dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
    int presented = 0;
    for (int i = 0; i < frames; ++i) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        const float c[4] = {0.1f + 0.4f * ((i % 60) / 60.f), 0.2f, 0.3f, 1.f};
        ID3D11RenderTargetView* r = rtv.Get();
        ctx->OMSetRenderTargets(1, &r, dsv.Get());
        ctx->ClearRenderTargetView(rtv.Get(), c);
        if (dsv) ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
        if (FAILED(hr = sc->Present(1, 0))) { std::printf("present %d hr=0x%08lX\n", i, static_cast<unsigned long>(hr)); break; }
        ++presented;
    }
    const HRESULT removed = dev->GetDeviceRemovedReason();
    std::printf("presented=%d of %d device_removed_reason=0x%08lX\n", presented, frames, static_cast<unsigned long>(removed));
    std::printf("RESULT %s\n", wrapped && SUCCEEDED(removed) && presented == frames ? "PASS" : "FAIL");
    return wrapped && SUCCEEDED(removed) && presented == frames ? 0 : 1;
}
