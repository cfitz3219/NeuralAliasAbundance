// One presenter for both APIs, stage 2 (Off path): a native DX11 "game" (its own D3D11 device) creates
// swap chains through the factory with [present] dx11_proxy=1. Each supported chain becomes a real D3D12
// chain on our queue; the picture the game drew must reach the D3D12 buffer bit for bit, equal to the
// picture the same draws give on a chain that is NOT proxied. Covered: blt (DISCARD, SEQUENTIAL, one
// buffer, legacy CreateSwapChain) and flip (FLIP_DISCARD, FLIP_SEQUENTIAL, CreateSwapChainForHwnd) models;
// RGBA8 / BGRA8 / RGBA8 sRGB / 10-bit / FP16; an sRGB render-target view on an 8-bit UNORM chain and a
// view with no description; GetDesc of the back buffer; ResizeBuffers; the Off path's GPU time from the
// status line; refusal (multisampled, GDI-compatible: the game keeps its own chain and presents); the
// Home menu drawing on a proxied chain. NA_TEST_FULLSCREEN=1 adds a windowed -> fullscreen -> windowed
// transition (it changes what the display shows, so it is not run by default).
#include "test_support.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
#include "host_dx12/host_boot/process_log.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/settings.hpp"

#include <d3d11_1.h>
#include <d3d11_3.h>
#include <dxgi1_3.h>
#include <dxgi1_2.h>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using test::ComPtr;

namespace {

// What the copy observer reads back from the real D3D12 buffer, right after our copy.
struct Observed {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 value = 0;
    UINT width = 0, height = 0;
    bool seen = false;
    ComPtr<ID3D12CommandQueue> queue;  // where the last copy went (for a second read after Present)
    ComPtr<ID3D12Resource> target;
};

void observe(ID3D12CommandQueue* queue, ID3D12Resource* target, void* user)
{
    auto& o = *static_cast<Observed*>(user);
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return;
    if (!o.alloc) {
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&o.alloc));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, o.alloc.Get(), nullptr, IID_PPV_ARGS(&o.list));
        o.list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&o.fence));
    }
    const auto desc = target->GetDesc();
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &o.fp, nullptr, nullptr, &total);
    o.readback = test::CreateBuffer(device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    o.width = static_cast<UINT>(desc.Width);
    o.height = desc.Height;
    o.alloc->Reset();
    o.list->Reset(o.alloc.Get(), nullptr);
    auto b = test::Transition(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    o.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = o.readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = o.fp;
    src.pResource = target; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    o.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    o.list->ResourceBarrier(1, &b);
    o.list->Close();
    ID3D12CommandList* lists[] = {o.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(o.fence.Get(), ++o.value);
    o.seen = true;
    o.queue = queue;
    o.target = target;
}

uint32_t bytes_per_pixel(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u; }

// The game's drawing: a clear with its default view, a rectangle cleared through a second view (an
// explicit sRGB view when asked), so a wrong view format or a lost write shows in the bytes.
std::vector<uint8_t> draw_and_read(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* chain, DXGI_FORMAT format, bool srgb_view, int frame)
{
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return {};
    D3D11_TEXTURE2D_DESC bd{};
    buffer->GetDesc(&bd);
    CHECK(bd.Format == format); // the game sees its own format
    CHECK((bd.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) == 0);
    ComPtr<ID3D11RenderTargetView> rtv, second;
    if (FAILED(dev->CreateRenderTargetView(buffer.Get(), nullptr, &rtv))) { CHECK(!"render-target view with no description"); return {}; }
    if (srgb_view) {
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        vd.Format = format == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(buffer.Get(), &vd, &second))) { CHECK(!"sRGB render-target view"); return {}; }
    } else {
        second = rtv;
    }
    ComPtr<ID3D11ShaderResourceView> srv; // a game may also sample its back buffer: a view with no description
    CHECK(SUCCEEDED(dev->CreateShaderResourceView(buffer.Get(), nullptr, &srv)));
    ComPtr<ID3D11Device3> dev3; // and the D3D11.3 calls for the same views
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev3)))) {
        ComPtr<ID3D11RenderTargetView1> rtv1;
        ComPtr<ID3D11ShaderResourceView1> srv1;
        CHECK(SUCCEEDED(dev3->CreateRenderTargetView1(buffer.Get(), nullptr, &rtv1)));
        CHECK(SUCCEEDED(dev3->CreateShaderResourceView1(buffer.Get(), nullptr, &srv1)));
    }
    const float a[4] = {0.1f + 0.05f * frame, 0.4f, 0.7f, 1.f}, b[4] = {0.9f, 0.25f, 0.05f + 0.1f * frame, 1.f};
    ctx->ClearRenderTargetView(rtv.Get(), a);
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
        const D3D11_RECT r{static_cast<LONG>(bd.Width / 4), static_cast<LONG>(bd.Height / 3), static_cast<LONG>(bd.Width / 2), static_cast<LONG>(bd.Height * 2 / 3)};
        ctx1->ClearView(second.Get(), b, &r, 1);
    }
    D3D11_TEXTURE2D_DESC sd = bd;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    sd.SampleDesc = {1, 0};
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) return {};
    if (bd.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = sd; rd.Usage = D3D11_USAGE_DEFAULT; rd.CPUAccessFlags = 0;
        ComPtr<ID3D11Texture2D> resolved;
        dev->CreateTexture2D(&rd, nullptr, &resolved);
        ctx->ResolveSubresource(resolved.Get(), 0, buffer.Get(), 0, format);
        ctx->CopyResource(staging.Get(), resolved.Get());
    } else {
        ctx->CopyResource(staging.Get(), buffer.Get());
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
    const uint32_t row = bd.Width * bytes_per_pixel(format);
    std::vector<uint8_t> out(size_t(row) * bd.Height);
    for (UINT y = 0; y < bd.Height; ++y) std::memcpy(out.data() + size_t(y) * row, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, row);
    ctx->Unmap(staging.Get(), 0);
    return out;
}

std::vector<uint8_t> observed_bytes(Observed& o, DXGI_FORMAT format)
{
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (o.fence->GetCompletedValue() < o.value) { o.fence->SetEventOnCompletion(o.value, ev); WaitForSingleObject(ev, 5000); }
    CloseHandle(ev);
    const uint32_t row = o.width * bytes_per_pixel(format);
    std::vector<uint8_t> out(size_t(row) * o.height);
    uint8_t* m = nullptr;
    if (FAILED(o.readback->Map(0, nullptr, reinterpret_cast<void**>(&m)))) return {};
    for (UINT y = 0; y < o.height; ++y) std::memcpy(out.data() + size_t(y) * row, m + o.fp.Offset + size_t(y) * o.fp.Footprint.RowPitch, row);
    o.readback->Unmap(0, nullptr);
    return out;
}

struct Case {
    const char* label;
    bool legacy;            // IDXGIFactory::CreateSwapChain (blt models possible) or CreateSwapChainForHwnd
    DXGI_SWAP_EFFECT effect;
    UINT buffers;
    DXGI_FORMAT format;
    bool srgb_view;
    UINT samples = 1;
    UINT flags = 0;
    bool expect_proxy = true;
};

ComPtr<IDXGISwapChain> make_chain(IDXGIFactory2* factory, ID3D11Device* dev, HWND window, const Case& k, UINT w, UINT h)
{
    ComPtr<IDXGISwapChain> chain;
    if (k.legacy) {
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferDesc.Width = w; d.BufferDesc.Height = h; d.BufferDesc.Format = k.format;
        d.SampleDesc = {k.samples, 0};
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        d.BufferCount = k.buffers; d.OutputWindow = window; d.Windowed = TRUE; d.SwapEffect = k.effect; d.Flags = k.flags;
        if (FAILED(factory->CreateSwapChain(dev, &d, &chain))) return {};
    } else {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w; d.Height = h; d.Format = k.format; d.SampleDesc = {1, 0};
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        d.BufferCount = k.buffers; d.SwapEffect = k.effect; d.Flags = k.flags;
        ComPtr<IDXGISwapChain1> c1;
        if (FAILED(factory->CreateSwapChainForHwnd(dev, window, &d, nullptr, nullptr, &c1))) return {};
        chain = c1;
    }
    return chain;
}

int run_case(IDXGIFactory2* factory, ID3D11Device* dev, ID3D11DeviceContext* ctx, HWND window, const Case& k)
{
    // The same draws on a chain that is NOT proxied: the reference picture.
    na::on12::SetNativeProxyForTests(false);
    std::vector<uint8_t> reference;
    {
        auto plain = make_chain(factory, dev, window, k, 320, 180);
        REQUIRE(plain);
        CHECK(!na::on12::NativeProxy(plain.Get()));
        reference = draw_and_read(dev, ctx, plain.Get(), k.format, k.srgb_view && k.samples == 1, 0);
        CHECK(SUCCEEDED(plain->Present(0, 0)));
    }
    ctx->ClearState(); ctx->Flush();
    na::on12::SetNativeProxyForTests(true);
    Observed o;
    na::on12::SetCopyObserverForTests(&observe, &o);
    auto chain = make_chain(factory, dev, window, k, 320, 180);
    REQUIRE(chain);
    const bool proxied = na::on12::NativeProxy(chain.Get());
    CHECK(proxied == k.expect_proxy);
    if (k.expect_proxy) {
        DXGI_SWAP_CHAIN_DESC gd{};
        REQUIRE(SUCCEEDED(chain->GetDesc(&gd)));
        // This test is linked into the program's own module, whose descriptor queries see the real
        // chain; a game's call is rewritten by the same function the GetDesc hook applies.
        na::on12::RewriteDesc(chain.Get(), &gd);
        CHECK(gd.BufferDesc.Format == k.format && gd.SwapEffect == k.effect && gd.BufferCount == k.buffers); // the game's own description
        DXGI_FORMAT game = DXGI_FORMAT_UNKNOWN, real = DXGI_FORMAT_UNKNOWN;
        CHECK(na::on12::ProxyEncoding(chain.Get(), &game, &real) && game == k.format && real != DXGI_FORMAT_UNKNOWN);
    }
    auto drawn = draw_and_read(dev, ctx, chain.Get(), k.format, k.srgb_view && k.samples == 1, 0);
    CHECK(!drawn.empty() && drawn == reference);
    CHECK(SUCCEEDED(chain->Present(0, 0)));
    uint32_t mismatched = 0;
    if (proxied) {
        CHECK(o.seen);
        const auto presented = observed_bytes(o, k.format);
        CHECK(presented.size() == drawn.size());
        for (size_t i = 0; i < (std::min)(presented.size(), drawn.size()); ++i) mismatched += presented[i] != drawn[i];
        CHECK(mismatched == 0);
    }
    // ResizeBuffers (the game released its views and buffer: draw_and_read keeps none).
    ctx->ClearState(); ctx->Flush();
    o.target.Reset(); o.queue.Reset(); // DXGI resizes only when nothing holds the real buffers
    const HRESULT rh = chain->ResizeBuffers(0, 400, 240, DXGI_FORMAT_UNKNOWN, k.flags);
    CHECK(SUCCEEDED(rh));
    o.seen = false;
    auto drawn2 = draw_and_read(dev, ctx, chain.Get(), k.format, k.srgb_view && k.samples == 1, 1);
    CHECK(drawn2.size() == size_t(400) * 240 * bytes_per_pixel(k.format));
    CHECK(SUCCEEDED(chain->Present(1, 0)));
    uint32_t mismatched2 = 0;
    if (proxied) {
        CHECK(o.seen);
        const auto presented = observed_bytes(o, k.format);
        CHECK(presented.size() == drawn2.size());
        for (size_t i = 0; i < (std::min)(presented.size(), drawn2.size()); ++i) mismatched2 += presented[i] != drawn2[i];
        CHECK(mismatched2 == 0);
    }
    na::on12::SetCopyObserverForTests(nullptr, nullptr);
    ctx->ClearState(); ctx->Flush();
    std::printf("%-34s proxied=%d same-as-unproxied=%d presented-mismatch=%u after-resize=%u\n", k.label, proxied ? 1 : 0,
                drawn == reference ? 1 : 0, mismatched, mismatched2);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto dir = std::filesystem::path(exe).parent_path();
    const auto suffix = std::to_wstring(GetCurrentProcessId());
    const auto name = L"dx11-proxy-" + suffix + L".ini";
    const auto ini = dir / name;
    const auto scope = L"Dx11Proxy-" + suffix;
    const auto startup = (dir / (L"dx11-proxy-" + suffix + L".log")).wstring();
    auto set = [&](const wchar_t* section, const wchar_t* key, const wchar_t* value) { CHECK(WritePrivateProfileStringW(section, key, value, ini.c_str()) != 0); };
    set(L"host", L"enabled", L"1"); set(L"host", L"startup_log", startup.c_str()); set(L"host", L"graphics_api", L"auto");
    set(L"route_a", L"enabled", L"0"); set(L"overlay", L"enabled", L"1");
    set(L"observe", L"producers", L"0"); set(L"observe", L"early_install", L"1");
    set(L"adapter", L"enabled", L"0"); set(L"fsr", L"enabled", L"0"); set(L"xess", L"enabled", L"0");
    set(L"unreal", L"probe", L"0"); set(L"unreal", L"screen_percentage", L"0");
    set(L"settings", L"scope", scope.c_str()); set(L"present", L"proxy", L"0"); set(L"present", L"dx11_proxy", L"1");
    set(L"dlss", L"enabled", L"0"); set(L"j2", L"enabled", L"0");
    REQUIRE(na::host::StartFromIni(GetModuleHandleW(nullptr), name.c_str(), "DX11 PROXY TEST"));
    na::host::WaitForObserveInstall();
    for (unsigned i = 0; i < 2000 && !na::host::GetStatus().init_complete; ++i) Sleep(5);
    REQUIRE(na::host::GetStatus().init_complete == 1);
    REQUIRE(na::on12::NativeProxyOn());

    ComPtr<IDXGIFactory2> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        return test::SkipNoDevice("dx11_proxy_chain_test");
    HWND window = test::CreateTestWindow(L"DX11 proxy chain test");
    REQUIRE(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);

    const Case cases[] = {
        {"blt DISCARD BGRA8, 1 buffer", true, DXGI_SWAP_EFFECT_DISCARD, 1, DXGI_FORMAT_B8G8R8A8_UNORM, true},
        {"blt SEQUENTIAL RGBA8 sRGB, 1 buffer", true, DXGI_SWAP_EFFECT_SEQUENTIAL, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false},
        {"flip DISCARD RGBA8 + sRGB view", false, DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, DXGI_FORMAT_R8G8B8A8_UNORM, true},
        {"flip SEQUENTIAL 10-bit, 3 buffers", false, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 3, DXGI_FORMAT_R10G10B10A2_UNORM, false},
        {"flip DISCARD FP16", false, DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, DXGI_FORMAT_R16G16B16A16_FLOAT, false},
        {"refused: blt 4x multisampled", true, DXGI_SWAP_EFFECT_DISCARD, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, 4, 0, false},
        {"refused: GDI-compatible BGRA8", true, DXGI_SWAP_EFFECT_DISCARD, 1, DXGI_FORMAT_B8G8R8A8_UNORM, false, 1, DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE, false},
    };
    for (const Case& k : cases) {
        UINT q = 0;
        if (k.samples > 1 && (FAILED(dev->CheckMultisampleQualityLevels(k.format, k.samples, &q)) || !q)) continue;
        REQUIRE(run_case(factory.Get(), dev.Get(), ctx.Get(), window, k) == 0);
    }

    // Off path cost and the Home menu on a proxied chain, at 1920x1080.
    {
        const Case k{"flip SEQUENTIAL RGBA8 1920x1080", false, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 2, DXGI_FORMAT_R8G8B8A8_UNORM, false};
        auto chain = make_chain(factory.Get(), dev.Get(), window, k, 1920, 1080);
        REQUIRE(chain && na::on12::NativeProxy(chain.Get()));
        char status[4096];
        na::on12::FormatStatus(status, sizeof status); // starts a fresh timing window
        for (int i = 0; i < 60; ++i) {
            ComPtr<ID3D11Texture2D> b;
            REQUIRE(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&b))));
            ComPtr<ID3D11RenderTargetView> v;
            REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(b.Get(), nullptr, &v)));
            const float c[4] = {i / 60.f, 0.2f, 0.3f, 1.f};
            ctx->ClearRenderTargetView(v.Get(), c);
            CHECK(SUCCEEDED(chain->Present(0, 0)));
        }
        Sleep(200);
        { ComPtr<ID3D11Texture2D> b; chain->GetBuffer(0, IID_PPV_ARGS(&b)); chain->Present(0, 0); } // reads the last timings back
        na::on12::FormatStatus(status, sizeof status);
        const char* p = std::strstr(status, "dx11_proxy ");
        REQUIRE(p);
        double avg = -1, mx = -1;
        unsigned long long samples = 0;
        const char* g = std::strstr(p, "copy_gpu_ms=");
        REQUIRE(g && sscanf_s(g, "copy_gpu_ms=%lf/max%lf samples=%llu", &avg, &mx, &samples) == 3);
        std::printf("Off path at 1920x1080 RGBA8: copy on our queue %.3f ms average, %.3f ms max, over %llu frames\n", avg, mx, samples);
        CHECK(samples > 0);
        CHECK(avg >= 0 && avg <= 0.3);
        // Home menu: the native route draws it onto the game's (proxied) back buffer.
        const auto before = na::overlay::GetStats().frames_rendered;
        SendMessageW(window, WM_KEYDOWN, VK_HOME, 1);
        for (int i = 0; i < 10; ++i) chain->Present(0, 0);
        const auto after = na::overlay::GetStats().frames_rendered;
        std::printf("Home menu on the proxied chain: frames rendered %llu -> %llu\n", static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
        CHECK(after > before);
        {   // the game's frame latency (IDXGIDevice1) is honoured by the D3D12 chain through its waitable object
            ComPtr<IDXGIDevice1> dd;
            REQUIRE(SUCCEEDED(dev.As(&dd)));
            REQUIRE(SUCCEEDED(dd->SetMaximumFrameLatency(1)));
            for (int i = 0; i < 5; ++i) chain->Present(0, 0);
            unsigned applied = 0;
            unsigned long long waits = 0;
            CHECK(na::on12::ProxyLatency(chain.Get(), &applied, &waits));
            ComPtr<IDXGISwapChain2> sc2;
            UINT real = 0;
            if (SUCCEEDED(chain.As(&sc2))) sc2->GetMaximumFrameLatency(&real);
            std::printf("frame latency: game set 1, proxy applied %u, real chain reports %u, waits %llu\n", applied, real, waits);
            CHECK(applied == 1 && real == 1 && waits > 0);
            DXGI_SWAP_CHAIN_DESC gd{};
            chain->GetDesc(&gd);
            na::on12::RewriteDesc(chain.Get(), &gd);
            CHECK(!(gd.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)); // our flag is not the game's
            dd->SetMaximumFrameLatency(3);
        }
        {   // ResizeTarget and frame statistics pass through to the real chain
            DXGI_MODE_DESC md{};
            md.Width = 800; md.Height = 450;
            CHECK(SUCCEEDED(chain->ResizeTarget(&md)));
            DXGI_FRAME_STATISTICS fs{};
            const HRESULT st = chain->GetFrameStatistics(&fs);
            UINT count = 0;
            CHECK(SUCCEEDED(chain->GetLastPresentCount(&count)) && count > 0);
            std::printf("ResizeTarget ok; GetFrameStatistics 0x%08lX; last present count %u\n", static_cast<unsigned long>(st), count);
            CHECK(SUCCEEDED(st) || st == DXGI_ERROR_FRAME_STATISTICS_DISJOINT);
            ctx->ClearState(); ctx->Flush();
            CHECK(SUCCEEDED(chain->ResizeBuffers(0, 1920, 1080, DXGI_FORMAT_UNKNOWN, 0)));
        }
        {   // the menu's pixels reach the presented picture: compare a presented frame with the menu open
            // against the game's own picture (the copy observer reads the real buffer after our copy, i.e.
            // before the menu; the D3D12 menu draw follows on the same queue before DXGI's Present).
            Observed o;
            na::on12::SetCopyObserverForTests(&observe, &o);
            REQUIRE(SUCCEEDED(chain->Present(0, 0)));
            na::on12::SetCopyObserverForTests(nullptr, nullptr);
            REQUIRE(o.seen);
            const auto game = observed_bytes(o, DXGI_FORMAT_R8G8B8A8_UNORM);  // our copy alone
            observe(o.queue.Get(), o.target.Get(), &o);                        // the same buffer after Present (flip sequential keeps it)
            const auto shown = observed_bytes(o, DXGI_FORMAT_R8G8B8A8_UNORM);
            size_t menu_pixels = 0;
            for (size_t i = 0; i + 4 <= (std::min)(game.size(), shown.size()); i += 4) menu_pixels += std::memcmp(&game[i], &shown[i], 4) != 0;
            std::printf("Home menu pixels in the presented picture: %zu\n", menu_pixels);
            CHECK(menu_pixels > 1000);
            o.target.Reset(); o.queue.Reset();
        }
        SendMessageW(window, WM_KEYDOWN, VK_HOME, 1);
        chain->Present(0, 0);
        char e[8] = {};
        if (GetEnvironmentVariableA("NA_TEST_FULLSCREEN", e, sizeof e) && e[0] == '1') {
            ctx->ClearState(); ctx->Flush();
            const HRESULT f1 = chain->SetFullscreenState(TRUE, nullptr);
            std::printf("fullscreen on: 0x%08lX\n", static_cast<unsigned long>(f1));
            if (SUCCEEDED(f1)) {
                CHECK(SUCCEEDED(chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0)));
                for (int i = 0; i < 30; ++i) CHECK(SUCCEEDED(chain->Present(1, 0)));
                CHECK(SUCCEEDED(chain->SetFullscreenState(FALSE, nullptr)));
                CHECK(SUCCEEDED(chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0)));
                CHECK(SUCCEEDED(chain->Present(0, 0)));
            }
        }
        std::printf("%s\n", p);
    }
    na::host::StopSessionForTests();
    REQUIRE(na::overlay::Stop());
    DestroyWindow(window);
    DeleteFileW(ini.c_str());
    wchar_t log[MAX_PATH]{};
    na::host::ProcessLogPath(log, MAX_PATH, startup.c_str());
    DeleteFileW(log);
    return test::finish("dx11_proxy_chain_test");
}
