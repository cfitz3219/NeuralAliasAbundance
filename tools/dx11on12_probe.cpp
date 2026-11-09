// DX11-on-DX12 bridge probe (no game needed). Run it in a folder that holds
// NeuralAliasDXGI.dll renamed to dxgi.dll plus a NeuralAliasHost.ini with
// [dx11on12] enabled=1 (and the usual host/observe/j2 keys). It behaves like a
// small DX11 game: DXGI factory first (as games enumerate adapters), then
// D3D11CreateDeviceAndSwapChain, then N frames of clears and Present. Add
// "scene" after the frame count (or after "menu") for actual depth-writing
// geometry and an offscreen scene color target. It answers:
//   1. did the game's device become an 11on12 device on our D3D12 queue?
//   2. can the swap chain's back buffer be read as an ID3D12Resource (the menu needs it)?
//   3. does the device survive N presents (no removal)?
// "dx11on12_probe <frames> menu" presses Home a third of the way in, so the menu draws
// through the borrowed back buffer (dx11on12 menu_buffer{...} in the status log).
// Exit 0 = device wrapped and alive, 1 = not wrapped or removed.
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv)
{
    const int frames = argc > 1 ? std::atoi(argv[1]) : 300;
    bool press_home = false, draw_scene = false;
    for (int arg = 2; arg < argc; ++arg) {
        press_home |= std::strcmp(argv[arg], "menu") == 0;
        draw_scene |= std::strcmp(argv[arg], "scene") == 0;
    }
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
    ComPtr<ID3D11Texture2D> scene;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    if (draw_scene) {
        if (!back || !dsv) return 2;
        static constexpr char shaders[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p[6] = {float2(-1,-1),float2(-1,1),float2(1,-1),
                   float2(1,-1),float2(-1,1),float2(1,1)};
    return float4(p[id], 0.5f + 0.2f * p[id].x, 1);
}
float4 ps(float4 p : SV_Position) : SV_Target {
    return float4(frac(p.x / 64), frac(p.y / 64), 0.25, 1);
})";
        ComPtr<ID3DBlob> vs_code, ps_code;
        if (FAILED(D3DCompile(shaders, sizeof(shaders)-1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs_code, nullptr)) ||
            FAILED(D3DCompile(shaders, sizeof(shaders)-1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps_code, nullptr)) ||
            FAILED(dev->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs)) ||
            FAILED(dev->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps))) return 2;
        back->GetDesc(&td);
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &scene)) ||
            FAILED(dev->CreateRenderTargetView(scene.Get(), nullptr, rtv.ReleaseAndGetAddressOf()))) return 2;
    }
    int presented = 0;
    for (int i = 0; i < frames; ++i) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (press_home && i == frames / 3) {
            // Straight to our own window (the host hooks its WndProc), so the probe never takes
            // focus or sends a key to whatever else is in front on the desktop.
            SendMessageW(wnd, WM_KEYDOWN, VK_HOME, 1);
            SendMessageW(wnd, WM_KEYUP, VK_HOME, 0xC0000001);
            std::printf("pressed Home at frame %d\n", i);
        }
        const float c[4] = {0.1f + 0.4f * ((i % 60) / 60.f), 0.2f, 0.3f, 1.f};
        ID3D11RenderTargetView* r = rtv.Get();
        ctx->OMSetRenderTargets(1, &r, dsv.Get());
        ctx->ClearRenderTargetView(rtv.Get(), c);
        if (dsv) ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
        if (draw_scene) {
            const D3D11_VIEWPORT vp{0, 0, 1280, 720, 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            // Go past discovery's small depth-only loading-screen filter.
            for (int draw = 0; draw < 12; ++draw) ctx->Draw(6, 0);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->CopyResource(back.Get(), scene.Get());
        }
        if (FAILED(hr = sc->Present(1, 0))) { std::printf("present %d hr=0x%08lX\n", i, static_cast<unsigned long>(hr)); break; }
        ++presented;
    }
    const HRESULT removed = dev->GetDeviceRemovedReason();
    std::printf("presented=%d of %d device_removed_reason=0x%08lX\n", presented, frames, static_cast<unsigned long>(removed));
    std::printf("RESULT %s\n", wrapped && SUCCEEDED(removed) && presented == frames ? "PASS" : "FAIL");
    return wrapped && SUCCEEDED(removed) && presented == frames ? 0 : 1;
}
