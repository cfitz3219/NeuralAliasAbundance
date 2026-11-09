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
// "stream" uploads textures on every frame, resizes the chain, and checks
// copied pixels on both sides of the resize (translated native chain only).
// "load=<draws>" (with optional "textures=<n>", default 3000 / 64) records a
// game-sized frame: each draw maps a dynamic constant buffer, binds one of n
// textures and draws a tiny quad, so the translator builds descriptor tables,
// root bindings and upload copies per draw the way it does for a real DX11
// game. Presents without vsync and prints wall/CPU time per frame, so the
// same run under (a) native D3D11, (b) the bridge with [observe] producers=0
// and (c) producers=1 profile_hooks=1 measures what the observers cost on the
// translation thread (CoD WWII 2026-11-09: that thread capped the game at 30 fps).
// Exit 0 = device wrapped and alive, 1 = not wrapped or removed.
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// The campaign creates textures with initial data on a loader thread while
// the backend thread presents. Exercise that overlap as well as frame uploads.
struct TextureLoader {
    void Start(ID3D11Device* device, D3D11_TEXTURE2D_DESC desc)
    {
        thread = std::thread([this, device, desc] {
            std::array<unsigned, 256 * 256> data{};
            const D3D11_SUBRESOURCE_DATA initial{data.data(), 256 * sizeof(unsigned), 0};
            while (!stop.load(std::memory_order_relaxed)) {
                ComPtr<ID3D11Texture2D> texture;
                result = device->CreateTexture2D(&desc, &initial, &texture);
                if (FAILED(result)) break;
                ++created;
                Sleep(1);
            }
        });
    }

    void Join()
    {
        stop.store(true, std::memory_order_relaxed);
        if (thread.joinable()) thread.join();
    }

    ~TextureLoader() { Join(); }

    unsigned created = 0;
    HRESULT result = S_OK;
    std::atomic<bool> stop{false};
    std::thread thread;
};

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int frames = argc > 1 ? std::atoi(argv[1]) : 300;
    bool press_home = false, draw_scene = false, stream = false;
    int load_draws = 0, load_textures = 64;
    for (int arg = 2; arg < argc; ++arg) {
        press_home |= std::strcmp(argv[arg], "menu") == 0;
        draw_scene |= std::strcmp(argv[arg], "scene") == 0;
        stream |= std::strcmp(argv[arg], "stream") == 0;
        if (std::strncmp(argv[arg], "load=", 5) == 0) load_draws = std::atoi(argv[arg] + 5);
        if (std::strncmp(argv[arg], "textures=", 9) == 0) load_textures = std::atoi(argv[arg] + 9);
    }
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); // waits for the host install, like a game's adapter scan
    std::printf("factory hr=0x%08lX\n", static_cast<unsigned long>(hr));
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NAOn12Probe";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"NeuralAlias dx11on12 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               100, 100, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 2;
    d.BufferDesc.Width = 1280; d.BufferDesc.Height = 720; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = wnd; d.SampleDesc.Count = 1; d.Windowed = TRUE;
    // The pixel check reads a buffer after Present, so its contents must be preserved.
    d.SwapEffect = stream ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_FLIP_DISCARD;
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
    ComPtr<ID3D11PixelShader> ps, ps_load;
    ComPtr<ID3D11Buffer> load_cb;
    ComPtr<ID3D11SamplerState> load_sampler;
    std::vector<ComPtr<ID3D11ShaderResourceView>> load_srvs;
    if (load_draws > 0) draw_scene = true;
    if (load_draws > 0 && load_textures < 1) load_textures = 1;
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
}
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
cbuffer Tint : register(b0) { float4 tint; };
float4 ps_load(float4 p : SV_Position) : SV_Target {
    return t0.Sample(s0, p.xy / float2(1280, 720)) * tint;
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
        if (load_draws > 0) {
            ComPtr<ID3DBlob> load_code;
            if (FAILED(D3DCompile(shaders, sizeof(shaders)-1, nullptr, nullptr, nullptr, "ps_load", "ps_5_0", 0, 0, &load_code, nullptr)) ||
                FAILED(dev->CreatePixelShader(load_code->GetBufferPointer(), load_code->GetBufferSize(), nullptr, &ps_load))) return 2;
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 16;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&cb, nullptr, &load_cb))) return 2;
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(dev->CreateSamplerState(&sd, &load_sampler))) return 2;
            std::array<unsigned, 64 * 64> texels{};
            D3D11_TEXTURE2D_DESC lt{};
            lt.Width = lt.Height = 64;
            lt.MipLevels = lt.ArraySize = lt.SampleDesc.Count = 1;
            lt.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            lt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            for (int t = 0; t < load_textures; ++t) {
                texels.fill(0xff000000u | (static_cast<unsigned>(t) * 2654435761u & 0xffffffu));
                const D3D11_SUBRESOURCE_DATA initial{texels.data(), 64 * sizeof(unsigned), 0};
                ComPtr<ID3D11Texture2D> texture;
                ComPtr<ID3D11ShaderResourceView> srv;
                if (FAILED(dev->CreateTexture2D(&lt, &initial, &texture)) ||
                    FAILED(dev->CreateShaderResourceView(texture.Get(), nullptr, &srv))) return 2;
                load_srvs.push_back(srv);
            }
        }
    }
    ComPtr<IDXGISwapChain3> sc3;
    ComPtr<ID3D11Texture2D> uploads[4], readback;
    std::array<unsigned, 256 * 256> pixels{};
    TextureLoader loader;
    if (stream) {
        if (frames < 8 || press_home || !on12 || FAILED(sc.As(&sc3)) || !back || !rtv) return 2;
        D3D11_TEXTURE2D_DESC upload{};
        upload.Width = upload.Height = 256;
        upload.MipLevels = upload.ArraySize = upload.SampleDesc.Count = 1;
        upload.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (auto& tex : uploads) {
            if (FAILED(dev->CreateTexture2D(&upload, nullptr, &tex))) return 2;
        }
        const auto streamed = upload;
        upload.Width = upload.Height = 1;
        upload.Usage = D3D11_USAGE_STAGING;
        upload.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&upload, nullptr, &readback))) return 2;
        loader.Start(dev.Get(), streamed);
    }
    int presented = 0, checked = 0;
    LARGE_INTEGER qpf{}, load_begin{}, load_end{};
    FILETIME ft_create{}, ft_exit{}, cpu_kernel_begin{}, cpu_user_begin{}, cpu_kernel_end{}, cpu_user_end{};
    QueryPerformanceFrequency(&qpf);
    if (load_draws > 0) {
        QueryPerformanceCounter(&load_begin);
        GetProcessTimes(GetCurrentProcess(), &ft_create, &ft_exit, &cpu_kernel_begin, &cpu_user_begin);
    }
    for (int i = 0; i < frames; ++i) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (stream && i == frames / 2) {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            back.Reset();
            if (!draw_scene) rtv.Reset();
            hr = sc->ResizeBuffers(3, 1280, 720, DXGI_FORMAT_UNKNOWN, 0);
            std::printf("resize hr=0x%08lX\n", static_cast<unsigned long>(hr));
            if (FAILED(hr) || FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) ||
                (!draw_scene && FAILED(dev->CreateRenderTargetView(back.Get(), nullptr, &rtv)))) return 2;
        }
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
            if (load_draws > 0) {
                // Game-sized recording: per draw a dynamic constant update, a texture
                // binding and a tiny quad (8x8 pixels keeps the GPU out of the picture).
                const D3D11_VIEWPORT tiny{0, 0, 8, 8, 0, 1};
                ctx->RSSetViewports(1, &tiny);
                ctx->PSSetShader(ps_load.Get(), nullptr, 0);
                ID3D11SamplerState* sampler = load_sampler.Get();
                ctx->PSSetSamplers(0, 1, &sampler);
                ID3D11Buffer* cb = load_cb.Get();
                ctx->PSSetConstantBuffers(0, 1, &cb);
                for (int draw = 0; draw < load_draws; ++draw) {
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(ctx->Map(load_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        const float tint[4] = {1.f, 1.f - 0.001f * (draw % 1000), 1.f, 1.f};
                        std::memcpy(mapped.pData, tint, sizeof(tint));
                        ctx->Unmap(load_cb.Get(), 0);
                    }
                    ID3D11ShaderResourceView* srv = load_srvs[static_cast<size_t>(draw) % load_srvs.size()].Get();
                    ctx->PSSetShaderResources(0, 1, &srv);
                    ctx->Draw(6, 0);
                }
                ID3D11ShaderResourceView* none = nullptr;
                ctx->PSSetShaderResources(0, 1, &none);
                ctx->RSSetViewports(1, &vp);
            }
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->CopyResource(back.Get(), scene.Get());
        }
        const unsigned expected = 0xff000000u | (static_cast<unsigned>(i) * 675929u & 0xffffffu);
        ComPtr<ID3D12Resource> actual;
        if (stream) {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            for (unsigned n = 0; n < 4; ++n) {
                pixels.fill(expected ^ n);
                ctx->UpdateSubresource(uploads[n].Get(), 0, nullptr, pixels.data(), 256 * sizeof(unsigned), 0);
                const D3D11_BOX box{0, 0, 0, 16, 16, 1};
                ctx->CopySubresourceRegion(back.Get(), 0, 100 + 16 * n, 100, 0, uploads[n].Get(), 0, &box);
            }
            const bool verify = (i >= frames / 2 - 2 && i < frames / 2) || i >= frames - 3;
            if (verify && FAILED(sc->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&actual)))) return 2;
        }
        if (FAILED(hr = sc->Present(load_draws > 0 ? 0 : 1, 0))) { std::printf("present %d hr=0x%08lX\n", i, static_cast<unsigned long>(hr)); break; }
        if (actual) {
            D3D11_RESOURCE_FLAGS flags{};
            ComPtr<ID3D11Resource> image;
            hr = on12->CreateWrappedResource(actual.Get(), &flags, D3D12_RESOURCE_STATE_PRESENT,
                                             D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&image));
            if (FAILED(hr)) return 2;
            const D3D11_BOX pixel{100, 100, 0, 101, 101, 1};
            ctx->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0, image.Get(), 0, &pixel);
            ID3D11Resource* wrapped_image = image.Get();
            on12->ReleaseWrappedResources(&wrapped_image, 1);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 2;
            unsigned received = 0;
            std::memcpy(&received, mapped.pData, sizeof(received));
            ctx->Unmap(readback.Get(), 0);
            std::printf("frame=%d copied_pixel=0x%08X expected=0x%08X\n", i, received, expected);
            if (received != expected) return 2;
            ++checked;
        }
        ++presented;
    }
    const HRESULT removed = dev->GetDeviceRemovedReason();
    if (load_draws > 0 && presented > 0) {
        QueryPerformanceCounter(&load_end);
        GetProcessTimes(GetCurrentProcess(), &ft_create, &ft_exit, &cpu_kernel_end, &cpu_user_end);
        const auto ft_ms = [](const FILETIME& a, const FILETIME& b) {
            ULARGE_INTEGER ua{a.dwLowDateTime, a.dwHighDateTime}, ub{b.dwLowDateTime, b.dwHighDateTime};
            return static_cast<double>(ub.QuadPart - ua.QuadPart) / 10000.0;
        };
        const double wall_ms = static_cast<double>(load_end.QuadPart - load_begin.QuadPart) * 1000.0 / static_cast<double>(qpf.QuadPart);
        const double cpu_ms = ft_ms(cpu_kernel_begin, cpu_kernel_end) + ft_ms(cpu_user_begin, cpu_user_end);
        // wall_ms_per_frame is the frame time (the translation thread's pace when it is the
        // bottleneck); process_cpu_ms_per_frame sums every thread, ours and the translator's.
        std::printf("load draws_per_frame=%d textures=%d frames=%d wall_ms=%.1f fps=%.1f wall_ms_per_frame=%.3f process_cpu_ms_per_frame=%.3f\n",
                    load_draws, load_textures, presented, wall_ms, presented * 1000.0 / wall_ms, wall_ms / presented, cpu_ms / presented);
    }
    std::printf("presented=%d of %d device_removed_reason=0x%08lX\n", presented, frames, static_cast<unsigned long>(removed));
    ULONG chain_refs = 0;
    if (stream) {
        loader.Join();
        std::printf("loader_textures=%u loader_result=0x%08lX\n", loader.created, static_cast<unsigned long>(loader.result));
        ctx->ClearState();
        rtv.Reset();
        back.Reset();
        sc3.Reset();
        chain_refs = sc.Detach()->Release();
        std::printf("swapchain_refs_after_release=%lu\n", chain_refs);
    }
    const bool stream_passed = checked == 5 && chain_refs == 0 && loader.created > 0 && SUCCEEDED(loader.result);
    const bool passed = wrapped && SUCCEEDED(removed) && presented == frames && (!stream || stream_passed);
    std::printf("RESULT %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
