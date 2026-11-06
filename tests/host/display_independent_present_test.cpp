// Display-independent scene route, presentation half (COMMS 2026-11-06, Sol's split).
// The game NEVER resizes its source buffers and ignores window notifications: its back
// buffers stay full size in every window mode. The reduced scene is reconstructed by J2
// into the game's full-size scene colour before Present (Sol's lane). This test proves the
// presenter's half of that contract with real DX12 swap chains on WARP:
//   - the full-size picture the game hands to Present reaches the real back buffer
//     byte-for-byte (no stretch, crop, shift, clamp or older picture copied over it);
//   - Present runs no evaluation of its own (no upscaler attached, upscaled=0, scaled=0);
//   - physical extent, window rect and style never change across "Quality", "Performance"
//     and "DLAA" frames;
//   - SDR, 10-bit and HDR float formats, and a format change (resource recreation), keep it.
// The per-mode pictures are a FAKE model (seeded per-pixel patterns standing in for DLSS
// output), labelled as such. Hidden windows: this is not physical exclusive-fullscreen or
// game-visual evidence. Test-only CPU waits/readbacks; no game is launched.
#include "test_support.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/settings.hpp"
#include <cstring>
#include <vector>
namespace na::overlay { long ReadNativeBufferForTests(IDXGISwapChain*,unsigned,REFIID,void**); }
namespace {
struct Image { std::vector<unsigned char> rows; UINT row_bytes = 0, height = 0; };
UINT bytes_per_pixel(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u; }
// Copies `image` (PRESENT state, returned in it) to tightly packed CPU rows.
bool read_image(test::Device& d, ID3D12Resource* image, Image& out) {
    const auto desc = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 row_size = 0, bytes = 0;
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    auto read = test::CreateBuffer(d.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    test::List commands; if (!read || !commands.Create(d.device.Get())) return false;
    auto pre = test::Transition(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands.list->ResourceBarrier(1, &pre);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = image; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = read.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto post = test::Transition(image, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    commands.list->ResourceBarrier(1, &post); commands.list->Close();
    ID3D12CommandList* lists[] = {commands.list.Get()}; d.queue->ExecuteCommandLists(1, lists); d.WaitIdle();
    void* data = nullptr; D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    if (FAILED(read->Map(0, &range, &data))) return false;
    out.row_bytes = static_cast<UINT>(row_size); out.height = rows; out.rows.resize(size_t(row_size) * rows);
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(out.rows.data() + size_t(y) * row_size, static_cast<const unsigned char*>(data) + fp.Footprint.RowPitch * y, size_t(row_size));
    D3D12_RANGE none{0, 0}; read->Unmap(0, &none);
    return true;
}
// The fake reconstruction: a seeded per-pixel pattern, every byte position distinct enough
// that any stretch, shift or crop changes it. For HDR float it includes values above 1.0.
Image pattern(UINT w, UINT h, DXGI_FORMAT f, uint32_t seed) {
    Image img; const UINT bpp = bytes_per_pixel(f);
    img.row_bytes = w * bpp; img.height = h; img.rows.resize(size_t(img.row_bytes) * h);
    uint32_t s = seed * 2654435761u + 12345u;
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        unsigned char* p = img.rows.data() + size_t(y) * img.row_bytes + size_t(x) * bpp;
        s = s * 1664525u + 1013904223u;
        if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            // Half floats in 0.25..~8: exponent 13..17, random mantissa; alpha 1.0.
            for (int c = 0; c < 3; ++c) {
                const uint16_t exp = static_cast<uint16_t>(13 + ((s >> (c * 3)) % 5));
                const uint16_t mant = static_cast<uint16_t>((s >> (9 + c * 7)) & 0x3ff);
                const uint16_t half = static_cast<uint16_t>((exp << 10) | mant);
                std::memcpy(p + c * 2, &half, 2);
            }
            const uint16_t one = 0x3c00; std::memcpy(p + 6, &one, 2);
        } else {
            uint32_t v = s ^ (x * 73856093u) ^ (y * 19349663u);
            if (f == DXGI_FORMAT_R10G10B10A2_UNORM) v |= 0xC0000000u; // alpha 3 (opaque)
            else v |= 0xFF000000u;
            std::memcpy(p, &v, 4);
        }
    }
    return img;
}
// The game's frame: the reconstructed full-size picture written into its (proxy) back buffer.
bool upload(test::Device& d, ID3D12Resource* target, const Image& img) {
    const auto desc = target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes = 0;
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    auto up = test::CreateBuffer(d.device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) return false;
    void* data = nullptr; D3D12_RANGE none{0, 0};
    if (FAILED(up->Map(0, &none, &data))) return false;
    for (UINT y = 0; y < img.height; ++y)
        std::memcpy(static_cast<unsigned char*>(data) + fp.Footprint.RowPitch * y, img.rows.data() + size_t(y) * img.row_bytes, img.row_bytes);
    up->Unmap(0, nullptr);
    test::List commands; if (!commands.Create(d.device.Get())) return false;
    auto pre = test::Transition(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    commands.list->ResourceBarrier(1, &pre);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = target; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    commands.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto post = test::Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    commands.list->ResourceBarrier(1, &post); commands.list->Close();
    ID3D12CommandList* lists[] = {commands.list.Get()}; d.queue->ExecuteCommandLists(1, lists); d.WaitIdle();
    return true;
}
bool same(const Image& a, const Image& b) {
    return a.row_bytes == b.row_bytes && a.height == b.height && a.rows == b.rows;
}
// One window mode x one starting format; a format change midway recreates every resource.
int run(test::Device& d, bool borderless, DXGI_FORMAT first, DXGI_FORMAT second) {
    HWND window = borderless
        ? CreateWindowExW(0, L"STATIC", L"display-independent borderless", WS_POPUP | WS_MAXIMIZE, 0, 0, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)
        : test::CreateTestWindow(L"display-independent windowed");
    REQUIRE(window && !IsWindowVisible(window));
    RECT client{}; REQUIRE(GetClientRect(window, &client));
    const UINT W = UINT(client.right), H = UINT(client.bottom);
    REQUIRE(W > 8 && H > 8);
    RECT bounds{}; REQUIRE(GetWindowRect(window, &bounds));
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = W; desc.Height = H; desc.BufferCount = 2;
    desc.Format = first; desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    test::ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(), window, &desc, nullptr, nullptr, &chain)));
    test::ComPtr<IDXGISwapChain3> sc; REQUIRE(SUCCEEDED(chain.As(&sc)));
    const auto before = na::present::GetStats();
    uint32_t seed = borderless ? 100 : 1;
    test::ComPtr<ID3D12Resource> retained_game_buffer;
    for (DXGI_FORMAT format : {first, second}) {
        if (format != first) {
            // The game recreates its swap-chain resources (e.g. an HDR toggle). Same extent.
            REQUIRE(SUCCEEDED(sc->ResizeBuffers(0, W, H, format, 0)));
            // Scene groups retain game-facing resources through submitted
            // work. Holding that OLD full-size proxy must not pin a physical
            // DXGI backbuffer and prevent mode/format recreation.
            REQUIRE(retained_game_buffer);
            CHECK(retained_game_buffer->GetDesc().Format == first);
        }
        // FAKE model: the three pictures stand in for DLSS output at Quality, Performance and
        // native DLAA. The game's buffers are full size for all three; nothing resizes.
        for (const char* mode : {"Quality", "Performance", "DLAA", "Quality"}) {
            const UINT index = sc->GetCurrentBackBufferIndex();
            test::ComPtr<ID3D12Resource> back; REQUIRE(SUCCEEDED(sc->GetBuffer(index, IID_PPV_ARGS(&back))));
            CHECK(na::present::Proxied(sc.Get()));
            const auto bd = back->GetDesc();
            CHECK(bd.Width == W && bd.Height == H && bd.Format == format);
            if(format == first)retained_game_buffer=back;
            else CHECK(retained_game_buffer.Get()!=back.Get());
            const Image expected = pattern(W, H, format, seed++);
            REQUIRE(upload(d, back.Get(), expected));
            back.Reset();
            REQUIRE(SUCCEEDED(sc->Present(0, 0)));
            d.WaitIdle();
            // Test-only drain, then read every real buffer: the one just presented must hold
            // exactly the reconstructed full-size picture.
            na::present::BeforeResize(sc.Get()); na::present::AfterResize(sc.Get(), E_FAIL);
            bool delivered = false;
            for (UINT i = 0; i < 2 && !delivered; ++i) {
                test::ComPtr<ID3D12Resource> real;
                REQUIRE(SUCCEEDED(na::overlay::ReadNativeBufferForTests(sc.Get(), i, IID_PPV_ARGS(&real))));
                const auto rd = real->GetDesc();
                CHECK(rd.Width == W && rd.Height == H && rd.Format == format); // physical output unchanged
                Image got; REQUIRE(read_image(d, real.Get(), got));
                delivered = same(got, expected);
            }
            CHECK(delivered);
            std::printf("%s %s format=%d %ux%u: delivered=%d\n", borderless ? "borderless" : "windowed", mode, int(format), W, H, delivered ? 1 : 0);
        }
    }
    const auto after = na::present::GetStats();
    CHECK(after.upscaled == before.upscaled && after.upscale_failed == before.upscale_failed); // Present evaluated nothing
    CHECK(after.scaled_w == 0 && after.scaled_h == 0 && !na::present::Scaling());
    CHECK(!na::j2::RenderSmall());
    RECT now{}; REQUIRE(GetWindowRect(window, &now));
    CHECK(std::memcmp(&now, &bounds, sizeof now) == 0);
    CHECK(GetWindowLongPtrW(window, GWL_STYLE) == style);
    sc.Reset(); chain.Reset(); DestroyWindow(window);
    return 0;
}
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetEnvironmentVariableW(L"NEURALALIAS_TEST_WARP", L"1");
    test::Device d; REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory))));
    test::ComPtr<IDXGIAdapter> warp; REQUIRE(SUCCEEDED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    REQUIRE(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device))));
    D3D12_COMMAND_QUEUE_DESC q{}; REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q, IID_PPV_ARGS(&d.queue))));
    REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence))));
    d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // The display-independent route runs with render-small OFF: no upscaler is attached to
    // the presenter and the size/virtual-screen hooks are not installed.
    na::j2::SetRenderSmall(false);
    REQUIRE(na::hooking::Initialize()); REQUIRE(na::overlay::StartCore("DISPLAYINDEP", nullptr));
    na::settings::Get().nr_enabled.store(0); na::present::SetEnabled(true);
    REQUIRE(run(d, false, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT) == 0);
    REQUIRE(run(d, true, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM) == 0);
    REQUIRE(d.device->GetDeviceRemovedReason() == S_OK);
    REQUIRE(na::overlay::Stop());
    return test::finish("display_independent_present_test");
}
