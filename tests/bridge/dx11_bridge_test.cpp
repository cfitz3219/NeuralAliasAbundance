// DX11 bridge round trip through the real DX12 helper process, in its
// --copy-test mode (the helper copies colour to output instead of DLSS), so
// the transport is checked on any GPU. Tries hardware first, then WARP.
//
// Checks, over several frames with a changing picture:
//   * shared textures, fences, frame record and helper start-up work;
//   * the result lands in the game's target at the right place;
//   * the game's own pipeline state survives the bridge;
//   * upscaling sizes (render 64x36 -> output 96x54) travel correctly;
//   * stopping the bridge stops the helper.
// Exit 77 (SKIP) only when running on WARP and WARP cannot share fences or
// textures across DX11/DX12. On real hardware any failure is a failure.
//
// Usage: dx11_bridge_test <path to na_bridge_helper64.exe> <data folder>
#include "bridge/dx11_bridge.hpp"

#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace na::bridge;

namespace {

int g_failed = 0;
void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_failed;
}

uint16_t Half(float f)
{
    // Exact for the small multiples of 1/64 used here.
    if (f == 0.f) return 0;
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = int((x >> 23) & 0xff) - 127 + 15;
    const uint32_t mant = (x >> 13) & 0x3ffu;
    return static_cast<uint16_t>(sign | (uint32_t(exp) << 10) | mant);
}
float FromHalf(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const int exp = (h >> 10) & 0x1f;
    const uint32_t mant = h & 0x3ffu;
    if (!exp) return 0.f;
    const uint32_t x = sign | (uint32_t(exp - 15 + 127) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT f, UINT bind, const void* data, UINT pitch)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = f;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    ComPtr<ID3D11Texture2D> t;
    d->CreateTexture2D(&desc, data ? &init : nullptr, &t);
    return t;
}

float Pattern(int frame, int x, int y, int c) { return float((frame * 7 + x * 3 + y * 5 + c * 11) % 64) / 64.f; }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: dx11_bridge_test <helper exe> <data folder>\n"); return 2; }
    const std::string helper_a = argv[1], data_a = argv[2];
    const std::wstring helper(helper_a.begin(), helper_a.end()), data(data_a.begin(), data_a.end());

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    bool warp = false;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
            std::printf("SKIP: no DX11 device\n");
            return 77;
        }
        warp = true;
    }
    std::printf("device: %s\n", warp ? "WARP" : "hardware");

    const UINT rw = 64, rh = 36, ow = 96, oh = 54;
    Config cfg;
    cfg.limits = SessionLimits{rw, rh, ow, oh};
    cfg.backend = Backend::Helper;
    cfg.copy_test = true;
    cfg.helper_path = helper;
    cfg.data_path = data;
    cfg.submit_deadline_ms = 2000; // generous: CI machines are slow, WARP more so
    Dx11Bridge bridge;
    if (const char* why = bridge.Start(ctx.Get(), cfg)) {
        std::printf("bridge start: %s\n", why);
        if (warp) { std::printf("SKIP: WARP cannot share across DX11/DX12 here\n"); return 77; }
        expect(false, "the bridge starts on hardware");
        return 1;
    }
    expect(bridge.ActiveBackend() == Backend::Helper, "the helper backend is active");

    std::vector<uint16_t> zeros(size_t(rw) * rh * 2, 0);
    std::vector<float> depth_px(size_t(rw) * rh, 0.5f);
    auto depth = MakeTexture(device.Get(), rw, rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth_px.data(), rw * 4);
    auto motion = MakeTexture(device.Get(), rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, zeros.data(), rw * 4);
    auto target = MakeTexture(device.Get(), ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET, nullptr, 0);
    D3D11_TEXTURE2D_DESC sd{};
    target->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    device->CreateTexture2D(&sd, nullptr, &staging);
    ComPtr<ID3D11ShaderResourceView> depth_srv, motion_srv;
    ComPtr<ID3D11RenderTargetView> target_rtv;
    device->CreateShaderResourceView(depth.Get(), nullptr, &depth_srv);
    device->CreateShaderResourceView(motion.Get(), nullptr, &motion_srv);
    device->CreateRenderTargetView(target.Get(), nullptr, &target_rtv);
    expect(depth_srv && motion_srv && target_rtv && staging, "test resources were created");

    // A piece of "game state" the bridge must not disturb.
    const D3D11_VIEWPORT game_vp{3.f, 4.f, 17.f, 19.f, 0.f, 1.f};
    ctx->RSSetViewports(1, &game_vp);

    int delivered = 0, mismatches = 0;
    const DWORD start = GetTickCount();
    for (int frame = 0; frame < 12 && GetTickCount() - start < 30000;) {
        std::vector<uint16_t> px(size_t(rw) * rh * 4);
        for (UINT y = 0; y < rh; ++y)
            for (UINT x = 0; x < rw; ++x)
                for (int c = 0; c < 4; ++c) px[(size_t(y) * rw + x) * 4 + c] = Half(Pattern(frame, int(x), int(y), c));
        auto color = MakeTexture(device.Get(), rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, px.data(), rw * 8);
        ComPtr<ID3D11ShaderResourceView> color_srv;
        device->CreateShaderResourceView(color.Get(), nullptr, &color_srv);

        FrameRecord f;
        f.source_frame = uint64_t(frame);
        f.render_w = rw; f.render_h = rh; f.output_w = ow; f.output_h = oh;
        f.mode = static_cast<uint32_t>(ModeForScale(rw, rh, ow, oh));
        f.jitter_x = 0.25f; f.jitter_y = -0.25f;
        f.mv_scale_x = float(rw); f.mv_scale_y = float(rh);
        if (frame == 0) f.flags = kReset;
        const Result r = bridge.Process(ctx.Get(), Inputs{color_srv.Get(), depth_srv.Get(), motion_srv.Get()}, target_rtv.Get(), f);
        if (r == Result::NotReady) { Sleep(20); continue; }
        if (r == Result::Failed) { std::printf("bridge failed: %s\n", bridge.Reason()); break; }
        if (r != Result::Ok) { std::printf("frame %d: %s\n", frame, ResultName(r)); ++frame; continue; }

        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT y = 0; y < rh; y += 5)
                for (UINT x = 0; x < rw; x += 7) {
                    const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch) + size_t(x) * 4;
                    for (int c = 0; c < 3; ++c)
                        if (std::fabs(FromHalf(p[c]) - Pattern(frame, int(x), int(y), c)) > 1e-3f) ++mismatches;
                    if (std::fabs(FromHalf(p[3]) - 1.f) > 1e-3f) ++mismatches; // result is opaque
                }
            ctx->Unmap(staging.Get(), 0);
        }
        ++delivered;
        ++frame;
    }
    expect(delivered >= 10, "at least 10 of 12 frames came back through the helper");
    expect(mismatches == 0, "every sampled pixel matched this frame's input (render region of a larger output)");
    std::printf("delivered=%d repeats=%llu last wait=%.2f ms reason='%s'\n", delivered,
                static_cast<unsigned long long>(bridge.Repeats()), bridge.LastWaitMs(), bridge.Reason());

    D3D11_VIEWPORT vp{};
    UINT n = 1;
    ctx->RSGetViewports(&n, &vp);
    expect(n == 1 && vp.TopLeftX == 3.f && vp.Width == 17.f && vp.Height == 19.f, "the game's viewport survived the bridge");

    bridge.Stop();
    expect(!bridge.Started(), "the bridge stops cleanly (helper shut down)");

    std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: DX11 bridge round trip\n", g_failed);
    return g_failed ? 1 : 0;
}
