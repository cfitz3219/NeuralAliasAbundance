// DX11 bridge round trips through the real DX12 helper process, in its
// --copy-test mode (the helper copies colour to output instead of DLSS), so
// the transport is checked on any GPU. Tries hardware first, then WARP.
//
// Case 1, plain: shader views as inputs, FP16 colour, upscaling sizes
//   (render 64x36 -> output 96x54), a changing picture, the game's viewport
//   survives, a deferred context is refused, stopping stops the helper.
// Case 2, "Dunia-like" (Far Cry 5 / New Dawn review, docs/DX11-BRIDGE.md):
//   a SINGLETHREADED device; colour is an 8-bit typeless texture without shader
//   access (like a back buffer), depth is D24S8 without shader access, motion is
//   typeless without shader access; the target is an sRGB view. The picture must
//   come back at the same brightness, and the game's bound shader view, render
//   target, blend factor and viewport must survive.
// Case 3, multisampled colour and a resize: 4x MSAA colour is resolved; a
//   target of a new size gives NotReady + NeedsRestart (never an old-size
//   picture); Stop + Start at the new size delivers again.
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
#include <cstdlib>
#include <cstring>
#include <string>
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

ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT f, UINT bind, const void* data, UINT pitch, UINT samples = 1)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = f;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
    ComPtr<ID3D11Texture2D> t;
    d->CreateTexture2D(&desc, data ? &init : nullptr, &t);
    return t;
}

ComPtr<ID3D11Texture2D> MakeStaging(ID3D11Device* d, ID3D11Texture2D* like)
{
    D3D11_TEXTURE2D_DESC sd{};
    like->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> t;
    d->CreateTexture2D(&sd, nullptr, &t);
    return t;
}

float Pattern(int frame, int x, int y, int c) { return float((frame * 7 + x * 3 + y * 5 + c * 11) % 64) / 64.f; }
uint8_t Byte(float v) { return static_cast<uint8_t>(std::lround(v * 255.f)); }

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
};
bool MakeDevice(bool warp, UINT flags, Gpu* out)
{
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    return SUCCEEDED(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1,
                                       D3D11_SDK_VERSION, &out->device, nullptr, &out->ctx));
}

Config MakeConfig(const std::wstring& helper, const std::wstring& data, UINT rw, UINT rh, UINT ow, UINT oh)
{
    Config cfg;
    cfg.limits = SessionLimits{rw, rh, ow, oh};
    cfg.backend = Backend::Helper;
    cfg.copy_test = true;
    cfg.helper_path = helper;
    cfg.data_path = data;
    cfg.submit_deadline_ms = 2000; // generous: CI machines are slow, WARP more so
    return cfg;
}

FrameRecord MakeFrame(int frame, UINT rw, UINT rh, UINT ow, UINT oh)
{
    FrameRecord f;
    f.source_frame = uint64_t(frame);
    f.render_w = rw; f.render_h = rh; f.output_w = ow; f.output_h = oh;
    f.mode = static_cast<uint32_t>(ModeForScale(rw, rh, ow, oh));
    f.jitter_x = 0.25f; f.jitter_y = -0.25f;
    f.mv_scale_x = float(rw); f.mv_scale_y = float(rh);
    if (frame == 0) f.flags = kReset;
    return f;
}

// Calls Process until it returns Ok (or gives up); returns the last result.
template <class Call>
Result UntilOk(Call call, int tries = 300)
{
    Result r = Result::NotReady;
    for (int i = 0; i < tries; ++i) {
        r = call();
        if (r == Result::Ok || r == Result::Failed) return r;
        Sleep(20);
    }
    return r;
}

// ---- Case 1 ----------------------------------------------------------------
// Returns 77 when WARP cannot share (the whole test then skips).
int CasePlain(const Gpu& g, bool warp, const std::wstring& helper, const std::wstring& data)
{
    std::printf("-- case 1: shader views, FP16, upscaling sizes\n");
    ID3D11Device* device = g.device.Get();
    ID3D11DeviceContext* ctx = g.ctx.Get();
    const UINT rw = 64, rh = 36, ow = 96, oh = 54;
    Dx11Bridge bridge;
    if (const char* why = bridge.Start(ctx, MakeConfig(helper, data, rw, rh, ow, oh))) {
        std::printf("bridge start: %s\n", why);
        if (warp) { std::printf("SKIP: WARP cannot share across DX11/DX12 here\n"); return 77; }
        expect(false, "the bridge starts on hardware");
        return 1;
    }
    expect(bridge.ActiveBackend() == Backend::Helper, "the helper backend is active");

    std::vector<uint16_t> zeros(size_t(rw) * rh * 2, 0);
    std::vector<float> depth_px(size_t(rw) * rh, 0.5f);
    auto depth = MakeTexture(device, rw, rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth_px.data(), rw * 4);
    auto motion = MakeTexture(device, rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, zeros.data(), rw * 4);
    auto target = MakeTexture(device, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET, nullptr, 0);
    auto staging = MakeStaging(device, target.Get());
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
    ComPtr<ID3D11ShaderResourceView> last_color;
    for (int frame = 0; frame < 12 && GetTickCount() - start < 30000;) {
        std::vector<uint16_t> px(size_t(rw) * rh * 4);
        for (UINT y = 0; y < rh; ++y)
            for (UINT x = 0; x < rw; ++x)
                for (int c = 0; c < 4; ++c) px[(size_t(y) * rw + x) * 4 + c] = Half(Pattern(frame, int(x), int(y), c));
        auto color = MakeTexture(device, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, px.data(), rw * 8);
        ComPtr<ID3D11ShaderResourceView> color_srv;
        device->CreateShaderResourceView(color.Get(), nullptr, &color_srv);
        last_color = color_srv;

        const Result r = bridge.Process(ctx, Inputs{color_srv.Get(), depth_srv.Get(), motion_srv.Get()}, target_rtv.Get(), MakeFrame(frame, rw, rh, ow, oh));
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

    // R7: a deferred context (how Dunia records its frame) is refused per call.
    ComPtr<ID3D11DeviceContext> deferred;
    if (SUCCEEDED(device->CreateDeferredContext(0, &deferred))) {
        const Result r = bridge.Process(deferred.Get(), Inputs{last_color.Get(), depth_srv.Get(), motion_srv.Get()}, target_rtv.Get(),
                                        MakeFrame(99, rw, rh, ow, oh));
        expect(r == Result::NotReady && std::strstr(bridge.Reason(), "deferred") != nullptr, "a deferred context is refused, not used");
    }

    bridge.Stop();
    expect(!bridge.Started(), "the bridge stops cleanly (helper shut down)");
    return 0;
}

// ---- Case 2 ----------------------------------------------------------------
void CaseDunia(bool warp, const std::wstring& helper, const std::wstring& data)
{
    std::printf("-- case 2: single-threaded device, inputs without shader access, sRGB target\n");
    Gpu g;
    if (!MakeDevice(warp, D3D11_CREATE_DEVICE_SINGLETHREADED, &g)) { expect(false, "a SINGLETHREADED device was created"); return; }
    ID3D11Device* device = g.device.Get();
    ID3D11DeviceContext* ctx = g.ctx.Get();
    const UINT rw = 64, rh = 36, ow = 96, oh = 54;
    Dx11Bridge bridge;
    if (const char* why = bridge.Start(ctx, MakeConfig(helper, data, rw, rh, ow, oh))) {
        std::printf("bridge start: %s\n", why);
        expect(false, "the bridge starts on a SINGLETHREADED device");
        return;
    }

    // Colour like a back buffer: typeless 8-bit, render-target only.
    auto color = MakeTexture(device, rw, rh, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET, nullptr, 0);
    // Depth like a depth buffer made without shader access.
    auto depth = MakeTexture(device, rw, rh, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL, nullptr, 0);
    // Motion typeless, render-target only.
    auto motion = MakeTexture(device, rw, rh, DXGI_FORMAT_R16G16_TYPELESS, D3D11_BIND_RENDER_TARGET, nullptr, 0);
    // Target: typeless storage with an sRGB view, as many games present.
    auto target = MakeTexture(device, ow, oh, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET, nullptr, 0);
    ComPtr<ID3D11Texture2D> staging;
    if (target) staging = MakeStaging(device, target.Get());
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11RenderTargetView> motion_rtv, target_rtv;
    if (depth) device->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
    D3D11_RENDER_TARGET_VIEW_DESC mv{};
    mv.Format = DXGI_FORMAT_R16G16_FLOAT;
    mv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (motion) device->CreateRenderTargetView(motion.Get(), &mv, &motion_rtv);
    D3D11_RENDER_TARGET_VIEW_DESC tv{};
    tv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    tv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (target) device->CreateRenderTargetView(target.Get(), &tv, &target_rtv);
    expect(color && depth && motion && target && staging && dsv && motion_rtv && target_rtv, "Dunia-like resources were created");
    if (!(color && depth && motion && target && staging && dsv && motion_rtv && target_rtv)) return;

    // Game state the bridge must leave alone.
    auto dummy = MakeTexture(device, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, nullptr, 0);
    ComPtr<ID3D11ShaderResourceView> dummy_srv;
    ComPtr<ID3D11RenderTargetView> dummy_rtv;
    device->CreateShaderResourceView(dummy.Get(), nullptr, &dummy_srv);
    device->CreateRenderTargetView(dummy.Get(), nullptr, &dummy_rtv);
    ID3D11ShaderResourceView* srv0 = dummy_srv.Get();
    ID3D11RenderTargetView* rtv0 = dummy_rtv.Get();
    const FLOAT blend_factor[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    const D3D11_VIEWPORT game_vp{1.f, 2.f, 5.f, 6.f, 0.f, 1.f};

    int delivered = 0, mismatches = 0;
    const DWORD start = GetTickCount();
    for (int frame = 0; frame < 8 && GetTickCount() - start < 30000;) {
        std::vector<uint8_t> px(size_t(rw) * rh * 4);
        for (UINT y = 0; y < rh; ++y)
            for (UINT x = 0; x < rw; ++x)
                for (int c = 0; c < 4; ++c) px[(size_t(y) * rw + x) * 4 + c] = Byte(Pattern(frame, int(x), int(y), c));
        ctx->UpdateSubresource(color.Get(), 0, nullptr, px.data(), rw * 4, 0);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 0);
        const FLOAT zero[4] = {};
        ctx->ClearRenderTargetView(motion_rtv.Get(), zero);

        ctx->PSSetShaderResources(0, 1, &srv0);
        ctx->OMSetRenderTargets(1, &rtv0, nullptr);
        ctx->OMSetBlendState(nullptr, blend_factor, 0xffffffffu);
        ctx->RSSetViewports(1, &game_vp);

        Inputs in;
        in.color_texture = color.Get();
        in.depth_texture = depth.Get();
        in.motion_texture = motion.Get();
        FrameRecord f = MakeFrame(frame, rw, rh, ow, oh);
        f.flags |= kDepthInverted;
        const Result r = bridge.Process(ctx, in, target_rtv.Get(), f);
        if (r == Result::NotReady) { Sleep(20); continue; }
        if (r == Result::Failed) { std::printf("bridge failed: %s\n", bridge.Reason()); break; }
        if (r != Result::Ok) { std::printf("frame %d: %s\n", frame, ResultName(r)); ++frame; continue; }

        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT y = 0; y < rh; y += 5)
                for (UINT x = 0; x < rw; x += 7) {
                    const uint8_t* p = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch + size_t(x) * 4;
                    for (int c = 0; c < 3; ++c)
                        if (std::abs(int(p[c]) - int(Byte(Pattern(frame, int(x), int(y), c)))) > 2) ++mismatches;
                    if (p[3] != 255) ++mismatches;
                }
            ctx->Unmap(staging.Get(), 0);
        }
        ++delivered;
        ++frame;
    }
    expect(delivered >= 6, "at least 6 of 8 frames came back from textures without shader access");
    expect(mismatches == 0, "brightness is unchanged through an sRGB target (no washed-out or dark picture)");
    std::printf("delivered=%d repeats=%llu reason='%s'\n", delivered, static_cast<unsigned long long>(bridge.Repeats()), bridge.Reason());

    ComPtr<ID3D11ShaderResourceView> got_srv;
    ComPtr<ID3D11RenderTargetView> got_rtv;
    ctx->PSGetShaderResources(0, 1, &got_srv);
    ctx->OMGetRenderTargets(1, &got_rtv, nullptr);
    ComPtr<ID3D11BlendState> bs;
    FLOAT got_factor[4] = {};
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, got_factor, &mask);
    D3D11_VIEWPORT vp{};
    UINT n = 1;
    ctx->RSGetViewports(&n, &vp);
    expect(got_srv.Get() == dummy_srv.Get(), "the game's pixel-shader view survived");
    expect(got_rtv.Get() == dummy_rtv.Get(), "the game's render target survived");
    expect(got_factor[0] == 0.1f && got_factor[3] == 0.4f && mask == 0xffffffffu, "the game's blend factor survived");
    expect(n == 1 && vp.TopLeftX == 1.f && vp.Width == 5.f, "the game's viewport survived");
    bridge.Stop();
}

// ---- Case 3 ----------------------------------------------------------------
void CaseMsaaResize(const Gpu& g, const std::wstring& helper, const std::wstring& data)
{
    std::printf("-- case 3: 4x MSAA colour, then a resize\n");
    ID3D11Device* device = g.device.Get();
    ID3D11DeviceContext* ctx = g.ctx.Get();
    UINT q = 0;
    if (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R16G16B16A16_FLOAT, 4, &q)) || q == 0) {
        std::printf("note: 4x MSAA FP16 not supported here; case 3 skipped\n");
        return;
    }
    const UINT rw = 48, rh = 27, ow = 64, oh = 36;
    const UINT ow2 = 80, oh2 = 45; // the "new resolution"
    Dx11Bridge bridge;
    if (const char* why = bridge.Start(ctx, MakeConfig(helper, data, rw, rh, ow, oh))) {
        std::printf("bridge start: %s\n", why);
        expect(false, "the bridge starts for the MSAA case");
        return;
    }
    auto color = MakeTexture(device, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET, nullptr, 0, 4);
    std::vector<float> depth_px(size_t(ow2) * oh2, 0.5f);
    std::vector<uint16_t> zeros(size_t(ow2) * oh2 * 2, 0);
    auto depth = MakeTexture(device, ow2, oh2, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depth_px.data(), ow2 * 4);
    auto motion = MakeTexture(device, ow2, oh2, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, zeros.data(), ow2 * 4);
    ComPtr<ID3D11RenderTargetView> color_rtv;
    ComPtr<ID3D11ShaderResourceView> depth_srv, motion_srv;
    if (color) device->CreateRenderTargetView(color.Get(), nullptr, &color_rtv);
    device->CreateShaderResourceView(depth.Get(), nullptr, &depth_srv);
    device->CreateShaderResourceView(motion.Get(), nullptr, &motion_srv);
    expect(color && color_rtv && depth_srv && motion_srv, "MSAA resources were created");
    if (!(color && color_rtv && depth_srv && motion_srv)) return;
    const FLOAT fill[4] = {0.25f, 0.5f, 0.75f, 0.3f};

    auto run = [&](UINT tw, UINT th, int frame, bool* matched, int tries) {
        auto target = MakeTexture(device, tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET, nullptr, 0);
        auto staging = MakeStaging(device, target.Get());
        ComPtr<ID3D11RenderTargetView> rtv;
        device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
        Inputs in;
        in.color_texture = color.Get();
        in.depth = depth_srv.Get();
        in.motion = motion_srv.Get();
        const Result r = UntilOk([&] {
            ctx->ClearRenderTargetView(color_rtv.Get(), fill);
            return bridge.Process(ctx, in, rtv.Get(), MakeFrame(frame, rw, rh, tw, th));
        }, tries);
        if (r == Result::Ok && matched) {
            *matched = true;
            ctx->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                for (UINT y = 0; y < rh; y += 4)
                    for (UINT x = 0; x < rw; x += 5) {
                        const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch) + size_t(x) * 4;
                        for (int c = 0; c < 3; ++c)
                            if (std::fabs(FromHalf(p[c]) - fill[c]) > 1e-3f) *matched = false;
                        if (std::fabs(FromHalf(p[3]) - 1.f) > 1e-3f) *matched = false;
                    }
                ctx->Unmap(staging.Get(), 0);
            }
        }
        return r;
    };

    bool matched = false;
    expect(run(ow, oh, 0, &matched, 300) == Result::Ok && matched, "4x multisampled colour is resolved and delivered");

    // Resize: the game now presents at 80x45. No old-size picture may be drawn.
    const Result r = run(ow2, oh2, 1, nullptr, 1);
    expect(r == Result::NotReady && bridge.NeedsRestart(), "a new target size gives NotReady + NeedsRestart");
    expect(run(ow2, oh2, 2, nullptr, 1) == Result::NotReady, "it stays NotReady until restarted");

    bridge.Stop();
    if (const char* why = bridge.Start(ctx, MakeConfig(helper, data, rw, rh, ow2, oh2))) {
        std::printf("bridge restart: %s\n", why);
        expect(false, "the bridge restarts at the new size");
        return;
    }
    matched = false;
    expect(run(ow2, oh2, 0, &matched, 300) == Result::Ok && matched, "after Stop + Start at the new size, frames are delivered again");
    bridge.Stop();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { std::printf("usage: dx11_bridge_test <helper exe> <data folder>\n"); return 2; }
    const std::string helper_a = argv[1], data_a = argv[2];
    const std::wstring helper(helper_a.begin(), helper_a.end()), data(data_a.begin(), data_a.end());

    Gpu g;
    bool warp = false;
    if (!MakeDevice(false, 0, &g)) {
        if (!MakeDevice(true, 0, &g)) {
            std::printf("SKIP: no DX11 device\n");
            return 77;
        }
        warp = true;
    }
    std::printf("device: %s\n", warp ? "WARP" : "hardware");

    if (CasePlain(g, warp, helper, data) == 77) return 77;
    CaseDunia(warp, helper, data);
    CaseMsaaResize(g, helper, data);

    std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: DX11 bridge round trips\n", g_failed);
    return g_failed ? 1 : 0;
}
