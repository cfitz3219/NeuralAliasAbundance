// Gate 2 hardware session (label gpu — Cort's RTX sessions only): measures the
// jitter sign convention the OFFICIAL FSR 3.1 and XeSS runtimes expect, with
// the same synthetic method that pinned DLSS's (dlss_real_test): a zone plate
// rendered on the CPU at sub-pixel positions  centre + (sx*jx, sy*jy)  while
// the runtime is told jitter (jx, jy). The (sx, sy) combination whose
// accumulated output best matches the exact picture is the runtime's
// convention. DLSS measured sample = centre - jitter on both axes, so a
// runtime with the same best combination maps with sign +1 per axis.
// argv[1] = folder holding amd_fidelityfx_dx12.dll (or the 2.x loader) and
// libxess.dll; a runtime that is missing is reported and skipped.
#include "test_support.hpp"
#include "host_dx12/upscalers/xess_abi.hpp"

#include <Kits/FidelityFX/api/include/ffx_api.h>
#include <Kits/FidelityFX/upscalers/include/ffx_upscale.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// From AMD's ffx_api_dx12.h (MIT): the DX12 backend descriptor chained to a create.
#define NA_FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 0x0000002u
struct NaFfxCreateBackendDX12Desc {
    ffxCreateContextDescHeader header;
    ID3D12Device* device;
};

namespace {
uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign); // tiny -> 0 (inputs are 0..1)
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    // Round to nearest; a mantissa carry correctly bumps the exponent.
    const uint32_t bits = (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    return static_cast<uint16_t>(sign | (bits + ((mant >> 12) & 1u)));
}
float FromHalf(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1F;
    const uint32_t mant = h & 0x3FFu;
    uint32_t x;
    if (exp == 0) {
        if (!mant) { x = sign; }
        else { float v = std::ldexp(static_cast<float>(mant), -24); return (h & 0x8000u) ? -v : v; }
    } else if (exp == 31) {
        x = sign | 0x7F800000u | (mant << 13);
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
float Halton(uint32_t i, uint32_t b)
{
    float f = 1.f, r = 0.f;
    while (i) { f /= static_cast<float>(b); r += f * static_cast<float>(i % b); i /= b; }
    return r;
}
// Zone plate on [0,1]^2: local frequency grows with radius. K is set so the
// finest detail (corners) stays just below the OUTPUT Nyquist limit.
double Zone(double u, double v, double k)
{
    const double du = u - 0.5, dv = v - 0.5;
    return 0.5 + 0.5 * std::cos(k * (du * du + dv * dv));
}
double Psnr(const std::vector<float>& a, const std::vector<double>& truth, uint32_t w, uint32_t h, uint32_t border)
{
    double se = 0;
    uint64_t n = 0;
    for (uint32_t y = border; y < h - border; ++y)
        for (uint32_t x = border; x < w - border; ++x) {
            const double e = a[y * w + x] - truth[y * w + x];
            se += e * e;
            ++n;
        }
    const double mse = se / static_cast<double>(n);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}

constexpr uint32_t kRw = 1280, kRh = 720, kOw = 1920, kOh = 1080, kFrames = 48, kBorder = 16;
const double kZone = 900.0 * 3.14159265358979 / 0.7071;

struct Scene {
    test::Device* d = nullptr;
    test::ComPtr<ID3D12Resource> color, depth, motion, output, up_c, rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fc{}, fo{};
    test::List l;
    std::vector<double> truth;
    double psnr_bilinear = 0;
    bool color_read_state = false;
};

void CopyIn(test::List& l, ID3D12Resource* dst, ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f)
{
    D3D12_TEXTURE_COPY_LOCATION s{}, t{};
    s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = f;
    t.pResource = dst; t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.list->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
}

bool MakeScene(test::Device& d, Scene& s)
{
    ID3D12Device* dev = d.device.Get();
    s.d = &d;
    s.color = test::CreateTexture(dev, kRw, kRh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    s.depth = test::CreateTexture(dev, kRw, kRh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    s.motion = test::CreateTexture(dev, kRw, kRh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    s.output = test::CreateTexture(dev, kOw, kOh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!s.color || !s.depth || !s.motion || !s.output) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fd{}, fm{};
    UINT64 bc = 0, bd = 0, bm = 0, bo = 0;
    D3D12_RESOURCE_DESC rd = s.color->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &s.fc, nullptr, nullptr, &bc);
    rd = s.depth->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fd, nullptr, nullptr, &bd);
    rd = s.motion->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fm, nullptr, nullptr, &bm);
    rd = s.output->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &s.fo, nullptr, nullptr, &bo);
    s.up_c = test::CreateBuffer(dev, bc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto up_d = test::CreateBuffer(dev, bd, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto up_m = test::CreateBuffer(dev, bm, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    s.rb = test::CreateBuffer(dev, bo, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!s.up_c || !up_d || !up_m || !s.rb) return false;
    uint8_t* p = nullptr;
    if (FAILED(up_d->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    for (uint32_t y = 0; y < kRh; ++y) {
        float* row = reinterpret_cast<float*>(p + y * fd.Footprint.RowPitch);
        for (uint32_t x = 0; x < kRw; ++x) row[x] = 0.5f;
    }
    up_d->Unmap(0, nullptr);
    if (FAILED(up_m->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    std::memset(p, 0, static_cast<size_t>(bm));
    up_m->Unmap(0, nullptr);
    if (!s.l.Create(dev)) return false;
    CopyIn(s.l, s.depth.Get(), up_d.Get(), fd);
    CopyIn(s.l, s.motion.Get(), up_m.Get(), fm);
    D3D12_RESOURCE_BARRIER init[] = {
        test::Transition(s.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        test::Transition(s.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    s.l.list->ResourceBarrier(2, init);
    if (FAILED(s.l.list->Close())) return false;
    { ID3D12CommandList* ls[] = {s.l.list.Get()}; d.queue->ExecuteCommandLists(1, ls); }
    d.WaitIdle();
    s.truth.resize(static_cast<size_t>(kOw) * kOh);
    for (uint32_t y = 0; y < kOh; ++y)
        for (uint32_t x = 0; x < kOw; ++x) s.truth[y * kOw + x] = Zone((x + 0.5) / kOw, (y + 0.5) / kOh, kZone);
    std::vector<float> bil(static_cast<size_t>(kOw) * kOh);
    std::vector<double> low(static_cast<size_t>(kRw) * kRh);
    for (uint32_t y = 0; y < kRh; ++y)
        for (uint32_t x = 0; x < kRw; ++x) low[y * kRw + x] = Zone((x + 0.5) / kRw, (y + 0.5) / kRh, kZone);
    for (uint32_t y = 0; y < kOh; ++y)
        for (uint32_t x = 0; x < kOw; ++x) {
            const double sx = (x + 0.5) * kRw / kOw - 0.5, sy = (y + 0.5) * kRh / kOh - 0.5;
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const double fx = sx - x0, fy = sy - y0;
            const auto at = [&](int xx, int yy) {
                xx = xx < 0 ? 0 : (xx >= static_cast<int>(kRw) ? static_cast<int>(kRw) - 1 : xx);
                yy = yy < 0 ? 0 : (yy >= static_cast<int>(kRh) ? static_cast<int>(kRh) - 1 : yy);
                return low[static_cast<size_t>(yy) * kRw + xx];
            };
            bil[y * kOw + x] = static_cast<float>((at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
                                                  (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy);
        }
    s.psnr_bilinear = Psnr(bil, s.truth, kOw, kOh, kBorder);
    return true;
}

// Records `upscale(list, jx, jy, reset)` for kFrames frames of the scene
// sampled at centre + (sx*jx, sy*jy); returns the output PSNR (or -1).
using UpscaleFn = std::function<bool(ID3D12GraphicsCommandList*, float, float, bool)>;
double Run(Scene& s, int sx, int sy, const UpscaleFn& upscale)
{
    test::Device& d = *s.d;
    for (uint32_t i = 0; i < kFrames; ++i) {
        const float jx = Halton((i % 32) + 1, 2) - 0.5f, jy = Halton((i % 32) + 1, 3) - 0.5f;
        uint8_t* p = nullptr;
        if (FAILED(s.up_c->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return -1;
        for (uint32_t y = 0; y < kRh; ++y) {
            uint16_t* row = reinterpret_cast<uint16_t*>(p + y * s.fc.Footprint.RowPitch);
            for (uint32_t x = 0; x < kRw; ++x) {
                const double u = (x + 0.5 + sx * jx) / kRw, v = (y + 0.5 + sy * jy) / kRh;
                const uint16_t c = ToHalf(static_cast<float>(Zone(u, v, kZone)));
                row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = c;
                row[x * 4 + 3] = ToHalf(1.f);
            }
        }
        s.up_c->Unmap(0, nullptr);
        s.l.Reset();
        if (s.color_read_state) {
            D3D12_RESOURCE_BARRIER b = test::Transition(s.color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            s.l.list->ResourceBarrier(1, &b);
        }
        CopyIn(s.l, s.color.Get(), s.up_c.Get(), s.fc);
        D3D12_RESOURCE_BARRIER b = test::Transition(s.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.l.list->ResourceBarrier(1, &b);
        s.color_read_state = true;
        if (!upscale(s.l.list.Get(), jx, jy, i == 0)) {
            s.l.list->Close();
            std::printf("FAIL upscale call refused at frame %u\n", i);
            return -1;
        }
        if (i == kFrames - 1) {
            D3D12_RESOURCE_BARRIER ob = test::Transition(s.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            s.l.list->ResourceBarrier(1, &ob);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = s.output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = s.rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = s.fo;
            s.l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            ob = test::Transition(s.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.l.list->ResourceBarrier(1, &ob);
        }
        if (FAILED(s.l.list->Close())) return -1;
        ID3D12CommandList* ls[] = {s.l.list.Get()};
        d.queue->ExecuteCommandLists(1, ls);
        d.WaitIdle();
    }
    std::vector<float> out(static_cast<size_t>(kOw) * kOh);
    uint8_t* p = nullptr;
    if (FAILED(s.rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return -1;
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t y = 0; y < kOh; ++y) {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(p + y * s.fo.Footprint.RowPitch);
        for (uint32_t x = 0; x < kOw; ++x) {
            const float v = FromHalf(row[x * 4]);
            if (!std::isfinite(v)) { lo = -1e9f; }
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
            out[y * kOw + x] = v;
        }
    }
    D3D12_RANGE none{0, 0};
    s.rb->Unmap(0, &none);
    if (!(hi - lo > 0.5f) || lo < -1e8f) {
        std::printf("FAIL output flat or not finite (range %.3f..%.3f)\n", lo, hi);
        return -1;
    }
    return Psnr(out, s.truth, kOw, kOh, kBorder);
}

// Runs the four (sx, sy) conventions; prints them; returns false if none beat bilinear.
bool Measure(const char* name, Scene& s, const UpscaleFn& upscale, const std::function<void()>& reset_runtime)
{
    double best = -1;
    int bsx = 0, bsy = 0;
    for (int sx : {-1, +1})
        for (int sy : {-1, +1}) {
            reset_runtime();
            const double psnr = Run(s, sx, sy, upscale);
            std::printf("%s: scene sample = centre %+d*jx, %+d*jy -> PSNR %.2f dB (bilinear %.2f, delta %+.2f)\n", name, sx,
                        sy, psnr, s.psnr_bilinear, psnr - s.psnr_bilinear);
            if (psnr > best) { best = psnr; bsx = sx; bsy = sy; }
        }
    std::printf("%s RESULT: sample = centre %s jitter.x, centre %s jitter.y (best %.2f dB vs bilinear %.2f) -> "
                "to-DLSS sign x=%+d y=%+d\n",
                name, bsx < 0 ? "-" : "+", bsy < 0 ? "-" : "+", best, s.psnr_bilinear, -bsx, -bsy);
    return best > s.psnr_bilinear;
}

FfxApiResource Res(ID3D12Resource* r, uint32_t fmt, uint32_t w, uint32_t h, uint32_t state, uint32_t usage)
{
    FfxApiResource x{};
    x.resource = r;
    x.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    x.description.format = fmt;
    x.description.width = w;
    x.description.height = h;
    x.description.depth = 1;
    x.description.mipCount = 1;
    x.description.flags = 0;
    x.description.usage = usage;
    x.state = state;
    return x;
}

void FfxMessage(uint32_t type, const wchar_t* msg) { std::printf("  [FSR message %u] %ls\n", type, msg ? msg : L""); }
} // namespace

int main(int argc, char** argv)
{
    test::Device d;
    if (!test::CreateDevice(d, /*allow_warp=*/false)) {
        std::printf("upscaler_jitter_test: SKIP (no hardware D3D12 device)\n");
        return test::kSkip;
    }
    std::wstring dir = L".";
    if (argc > 1) { const std::string a = argv[1]; dir.assign(a.begin(), a.end()); }
    Scene s;
    REQUIRE(MakeScene(d, s));
    std::printf("scene: zone plate %ux%u -> %ux%u, bilinear PSNR %.2f dB, %u frames per convention\n", kRw, kRh, kOw, kOh,
                s.psnr_bilinear, kFrames);
    int measured = 0;

    // ---- FSR 3.1 (FidelityFX API) ----
    HMODULE ffx = LoadLibraryW((dir + L"\\amd_fidelityfx_dx12.dll").c_str());
    if (!ffx) ffx = LoadLibraryW((dir + L"\\amd_fidelityfx_loader_dx12.dll").c_str());
    if (!ffx) {
        std::printf("FSR: runtime DLL not found in %ls (skipped)\n", dir.c_str());
    } else {
        auto create = reinterpret_cast<PfnFfxCreateContext>(reinterpret_cast<void*>(GetProcAddress(ffx, "ffxCreateContext")));
        auto destroy = reinterpret_cast<decltype(&ffxDestroyContext)>(reinterpret_cast<void*>(GetProcAddress(ffx, "ffxDestroyContext")));
        auto dispatch = reinterpret_cast<decltype(&ffxDispatch)>(reinterpret_cast<void*>(GetProcAddress(ffx, "ffxDispatch")));
        REQUIRE(create && destroy && dispatch);
        ffxContext ctx = nullptr;
        const auto make = [&] {
            if (ctx) { d.WaitIdle(); destroy(&ctx, nullptr); ctx = nullptr; }
            NaFfxCreateBackendDX12Desc be{};
            be.header.type = NA_FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            be.device = d.device.Get();
            ffxCreateContextDescUpscale cd{};
            cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            cd.header.pNext = &be.header;
            cd.maxRenderSize = {kRw, kRh};
            cd.maxUpscaleSize = {kOw, kOh};
            cd.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
            cd.fpMessage = &FfxMessage;
            const ffxReturnCode_t rc = create(&ctx, &cd.header, nullptr);
            if (rc != FFX_API_RETURN_OK) { std::printf("FAIL FSR ffxCreateContext returned %u\n", rc); ctx = nullptr; }
        };
        const auto upscale = [&](ID3D12GraphicsCommandList* list, float jx, float jy, bool reset) {
            if (!ctx) return false;
            ffxDispatchDescUpscale u{};
            u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            u.commandList = list;
            u.color = Res(s.color.Get(), FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, kRw, kRh, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
            u.depth = Res(s.depth.Get(), FFX_API_SURFACE_FORMAT_R32_FLOAT, kRw, kRh, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
            u.motionVectors = Res(s.motion.Get(), FFX_API_SURFACE_FORMAT_R16G16_FLOAT, kRw, kRh, FFX_API_RESOURCE_STATE_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
            u.output = Res(s.output.Get(), FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, kOw, kOh, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
            u.jitterOffset = {jx, jy};
            u.motionVectorScale = {1.f, 1.f};
            u.renderSize = {kRw, kRh};
            u.upscaleSize = {kOw, kOh};
            u.frameTimeDelta = 16.6f;
            u.preExposure = 1.f;
            u.reset = reset;
            u.cameraNear = 0.1f;
            u.cameraFar = 1000.f;
            u.cameraFovAngleVertical = 1.0f;
            u.viewSpaceToMetersFactor = 1.f;
            const ffxReturnCode_t rc = dispatch(&ctx, &u.header);
            if (rc != FFX_API_RETURN_OK) std::printf("FSR ffxDispatch returned %u\n", rc);
            return rc == FFX_API_RETURN_OK;
        };
        CHECK(Measure("FSR", s, upscale, make));
        ++measured;
        if (ctx) { d.WaitIdle(); destroy(&ctx, nullptr); }
    }

    // ---- XeSS ----
    using namespace na::xess::abi;
    HMODULE xs = LoadLibraryW((dir + L"\\libxess.dll").c_str());
    if (!xs) {
        std::printf("XeSS: libxess.dll not found in %ls (skipped)\n", dir.c_str());
    } else {
        auto create = reinterpret_cast<PfnD3D12CreateContext>(reinterpret_cast<void*>(GetProcAddress(xs, "xessD3D12CreateContext")));
        auto init = reinterpret_cast<PfnD3D12Init>(reinterpret_cast<void*>(GetProcAddress(xs, "xessD3D12Init")));
        auto exec = reinterpret_cast<PfnD3D12Execute>(reinterpret_cast<void*>(GetProcAddress(xs, "xessD3D12Execute")));
        auto destroy = reinterpret_cast<PfnDestroyContext>(reinterpret_cast<void*>(GetProcAddress(xs, "xessDestroyContext")));
        REQUIRE(create && init && exec && destroy);
        Context ctx = nullptr;
        const auto make = [&] {
            if (ctx) { d.WaitIdle(); destroy(ctx); ctx = nullptr; }
            Result r = create(d.device.Get(), &ctx);
            if (r != kSuccess) { std::printf("FAIL xessD3D12CreateContext returned %d\n", r); ctx = nullptr; return; }
            D3D12InitParams ip{};
            ip.outputResolution = {kOw, kOh};
            ip.qualitySetting = kUltraQuality; // 1.5x: 1280x720 -> 1920x1080
            ip.creationNodeMask = 1;
            ip.visibleNodeMask = 1;
            r = init(ctx, &ip);
            if (r != kSuccess) { std::printf("FAIL xessD3D12Init returned %d\n", r); destroy(ctx); ctx = nullptr; }
        };
        const auto upscale = [&](ID3D12GraphicsCommandList* list, float jx, float jy, bool reset) {
            if (!ctx) return false;
            D3D12ExecuteParams p{};
            p.pColorTexture = s.color.Get();
            p.pVelocityTexture = s.motion.Get();
            p.pDepthTexture = s.depth.Get();
            p.pOutputTexture = s.output.Get();
            p.jitterOffsetX = jx;
            p.jitterOffsetY = jy;
            p.exposureScale = 1.f;
            p.resetHistory = reset ? 1 : 0;
            p.inputWidth = kRw;
            p.inputHeight = kRh;
            const Result r = exec(ctx, list, &p);
            if (r != kSuccess) std::printf("XeSS xessD3D12Execute returned %d\n", r);
            return r == kSuccess;
        };
        CHECK(Measure("XeSS", s, upscale, make));
        ++measured;
        if (ctx) { d.WaitIdle(); destroy(ctx); }
    }
    if (!measured) {
        std::printf("upscaler_jitter_test: SKIP (no FSR or XeSS runtime in %ls)\n", dir.c_str());
        return test::kSkip;
    }
    return test::finish("upscaler_jitter_test");
}
