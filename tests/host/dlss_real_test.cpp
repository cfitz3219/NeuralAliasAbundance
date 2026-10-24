// C6 acceptance on real hardware (label gpu;nvidia — Cort's RTX sessions only).
// Our OWN NGX instance (InitOwn) upscales a static synthetic scene: a zone plate
// whose finest detail lies between the render and output resolution limits,
// rendered on the CPU at sub-pixel jittered positions (Halton 2,3), with zero
// motion and constant depth. After the jitter sequence, DLSS's output is
// compared with the exact picture at output resolution, against a bilinear
// upscale of one unjittered render frame as the baseline.
//
// Pass: NGX starts, DLSS is available, every frame evaluates, the output is
// finite and not flat, and for at least one jitter sign convention DLSS beats
// the bilinear baseline (PSNR). Both conventions are reported, which also
// pins down the sign NGX expects (evidence for A3/C7): sample = centre - jitter.
// SKIP (77) only when the machine has no NVIDIA GPU.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

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
} // namespace

int main(int argc, char** argv)
{
    test::Device d;
    if (!test::CreateDevice(d, /*allow_warp=*/false)) {
        std::printf("dlss_real_test: SKIP (no hardware D3D12 device)\n");
        return test::kSkip;
    }
    {
        test::ComPtr<IDXGIAdapter1> a;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(d.factory->EnumAdapterByLuid(d.device->GetAdapterLuid(), IID_PPV_ARGS(&a)))) a->GetDesc1(&desc);
        if (desc.VendorId != 0x10DE) {
            std::printf("dlss_real_test: SKIP (adapter is not NVIDIA: vendor 0x%04X)\n", desc.VendorId);
            return test::kSkip;
        }
    }
    std::wstring data_dir = L".";
    if (argc > 1) {
        const std::string a = argv[1];
        data_dir.assign(a.begin(), a.end());
        CreateDirectoryW(data_dir.c_str(), nullptr);
    }
    const bool init_ok = na::dlss::InitOwn(d.device.Get(), data_dir.c_str());
    const auto caps = na::dlss::GetCapabilities();
    std::printf("NGX own instance: %s; DLSS available=%d driver-update-needed=%d (min %u.%u) init-result=0x%08X optimal-cb=%d\n",
                init_ok ? "started" : "FAILED", caps.available ? 1 : 0, caps.needs_driver_update ? 1 : 0,
                caps.min_driver_major, caps.min_driver_minor, static_cast<unsigned>(caps.feature_init_result),
                caps.optimal_settings ? 1 : 0);
    if (caps.reason[0]) std::printf("reason: %s\n", caps.reason);
    REQUIRE(init_ok && caps.available);

    const uint32_t ow = 1920, oh = 1080;
    for (na::dlss::Quality q : {na::dlss::Quality::MaxQuality, na::dlss::Quality::Balanced, na::dlss::Quality::MaxPerformance,
                                na::dlss::Quality::UltraPerformance, na::dlss::Quality::DLAA}) {
        na::dlss::OptimalSettings s;
        if (na::dlss::QueryOptimal(ow, oh, q, &s))
            std::printf("optimal %-17s %ux%u -> render %ux%u (dynamic %ux%u .. %ux%u)\n", na::dlss::QualityName(q), ow, oh,
                        s.render_w, s.render_h, s.min_w, s.min_h, s.max_w, s.max_h);
        else
            std::printf("optimal %-17s not offered\n", na::dlss::QualityName(q));
    }
    na::dlss::OptimalSettings opt;
    REQUIRE(na::dlss::QueryOptimal(ow, oh, na::dlss::Quality::MaxQuality, &opt));
    const uint32_t rw = opt.render_w, rh = opt.render_h;
    const double kZone = 900.0 * 3.14159265358979 / 0.7071; // corner frequency ~ 900 cycles/width (< 960 output Nyquist)

    ID3D12Device* dev = d.device.Get();
    auto color = test::CreateTexture(dev, rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto depth = test::CreateTexture(dev, rw, rh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto motion = test::CreateTexture(dev, rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto output = test::CreateTexture(dev, ow, oh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(color && depth && motion && output);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fc{}, fd{}, fm{}, fo{};
    UINT64 bytes_c = 0, bytes_d = 0, bytes_m = 0, bytes_o = 0;
    {
        D3D12_RESOURCE_DESC rd = color->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fc, nullptr, nullptr, &bytes_c);
        rd = depth->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fd, nullptr, nullptr, &bytes_d);
        rd = motion->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fm, nullptr, nullptr, &bytes_m);
        rd = output->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fo, nullptr, nullptr, &bytes_o);
    }
    auto up_c = test::CreateBuffer(dev, bytes_c, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto up_d = test::CreateBuffer(dev, bytes_d, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto up_m = test::CreateBuffer(dev, bytes_m, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto rb = test::CreateBuffer(dev, bytes_o, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(up_c && up_d && up_m && rb);
    { // constant depth 0.5, zero motion
        uint8_t* p = nullptr;
        REQUIRE(SUCCEEDED(up_d->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        for (uint32_t y = 0; y < rh; ++y) {
            float* row = reinterpret_cast<float*>(p + y * fd.Footprint.RowPitch);
            for (uint32_t x = 0; x < rw; ++x) row[x] = 0.5f;
        }
        up_d->Unmap(0, nullptr);
        REQUIRE(SUCCEEDED(up_m->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        std::memset(p, 0, static_cast<size_t>(bytes_m));
        up_m->Unmap(0, nullptr);
    }
    test::List l;
    REQUIRE(l.Create(dev));
    const auto copy_in = [&](ID3D12Resource* dst, ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f) {
        D3D12_TEXTURE_COPY_LOCATION s{}, t{};
        s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = f;
        t.pResource = dst; t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; t.SubresourceIndex = 0;
        l.list->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
    };
    copy_in(depth.Get(), up_d.Get(), fd);
    copy_in(motion.Get(), up_m.Get(), fm);
    D3D12_RESOURCE_BARRIER init[] = {
        test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        test::Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    l.list->ResourceBarrier(2, init);
    REQUIRE(SUCCEEDED(l.list->Close()));
    { ID3D12CommandList* ls[] = {l.list.Get()}; d.queue->ExecuteCommandLists(1, ls); }
    d.WaitIdle();

    // Exact picture at output resolution and the bilinear baseline.
    std::vector<double> truth(static_cast<size_t>(ow) * oh);
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) truth[y * ow + x] = Zone((x + 0.5) / ow, (y + 0.5) / oh, kZone);
    std::vector<float> bilinear(static_cast<size_t>(ow) * oh);
    {
        std::vector<double> low(static_cast<size_t>(rw) * rh);
        for (uint32_t y = 0; y < rh; ++y)
            for (uint32_t x = 0; x < rw; ++x) low[y * rw + x] = Zone((x + 0.5) / rw, (y + 0.5) / rh, kZone);
        for (uint32_t y = 0; y < oh; ++y)
            for (uint32_t x = 0; x < ow; ++x) {
                const double sx = (x + 0.5) * rw / ow - 0.5, sy = (y + 0.5) * rh / oh - 0.5;
                const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
                const double fx = sx - x0, fy = sy - y0;
                const auto at = [&](int xx, int yy) {
                    xx = xx < 0 ? 0 : (xx >= static_cast<int>(rw) ? static_cast<int>(rw) - 1 : xx);
                    yy = yy < 0 ? 0 : (yy >= static_cast<int>(rh) ? static_cast<int>(rh) - 1 : yy);
                    return low[static_cast<size_t>(yy) * rw + xx];
                };
                bilinear[y * ow + x] = static_cast<float>((at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
                                                          (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy);
            }
    }
    const uint32_t kBorder = 16;
    const double psnr_bilinear = Psnr(bilinear, truth, ow, oh, kBorder);
    std::printf("scene: zone plate, render %ux%u -> output %ux%u, baseline bilinear PSNR %.2f dB\n", rw, rh, ow, oh, psnr_bilinear);

    const uint32_t kFrames = 64;
    double best = -1;
    int best_sign = 0;
    bool all_ok = true;
    for (int sign : {+1, -1}) {
        for (uint32_t i = 0; i < kFrames; ++i) {
            const float jx = Halton((i % 32) + 1, 2) - 0.5f, jy = Halton((i % 32) + 1, 3) - 0.5f;
            uint8_t* p = nullptr;
            REQUIRE(SUCCEEDED(up_c->Map(0, nullptr, reinterpret_cast<void**>(&p))));
            for (uint32_t y = 0; y < rh; ++y) {
                uint16_t* row = reinterpret_cast<uint16_t*>(p + y * fc.Footprint.RowPitch);
                for (uint32_t x = 0; x < rw; ++x) {
                    const double u = (x + 0.5 + sign * jx) / rw, v = (y + 0.5 + sign * jy) / rh;
                    const uint16_t c = ToHalf(static_cast<float>(Zone(u, v, kZone)));
                    row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = c;
                    row[x * 4 + 3] = ToHalf(1.f);
                }
            }
            up_c->Unmap(0, nullptr);
            l.Reset();
            if (i != 0 || sign != +1) {
                D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                l.list->ResourceBarrier(1, &b);
            }
            copy_in(color.Get(), up_c.Get(), fc);
            D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            l.list->ResourceBarrier(1, &b);
            na::dlss::EvalDesc e;
            e.color = color.Get();
            e.depth = depth.Get();
            e.motion = motion.Get();
            e.output = output.Get();
            e.render_w = rw; e.render_h = rh; e.out_w = ow; e.out_h = oh;
            e.jitter_x = jx; e.jitter_y = jy;
            e.reset = i == 0;
            e.quality = na::dlss::Quality::MaxQuality;
            const bool ok = na::dlss::Evaluate(l.list.Get(), e);
            if (!ok) {
                std::printf("FAIL evaluate frame %u: %s\n", i, na::dlss::GetStats().reason);
                all_ok = false;
            }
            if (i == kFrames - 1) {
                D3D12_RESOURCE_BARRIER ob = test::Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l.list->ResourceBarrier(1, &ob);
                D3D12_TEXTURE_COPY_LOCATION s{}, t{};
                s.pResource = output.Get(); s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                t.pResource = rb.Get(); t.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; t.PlacedFootprint = fo;
                l.list->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
                ob = test::Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                l.list->ResourceBarrier(1, &ob);
            }
            REQUIRE(SUCCEEDED(l.list->Close()));
            ID3D12CommandList* ls[] = {l.list.Get()};
            d.queue->ExecuteCommandLists(1, ls);
            na::dlss::OnExecuted(d.queue.Get(), 1, ls);
            d.WaitIdle();
            if (!ok) break;
        }
        std::vector<float> out(static_cast<size_t>(ow) * oh);
        uint8_t* p = nullptr;
        REQUIRE(SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        bool finite = true;
        float lo = 1e9f, hi = -1e9f;
        for (uint32_t y = 0; y < oh; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(p + y * fo.Footprint.RowPitch);
            for (uint32_t x = 0; x < ow; ++x) {
                const float v = FromHalf(row[x * 4]);
                if (!std::isfinite(v)) finite = false;
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
                out[y * ow + x] = v;
            }
        }
        D3D12_RANGE none{0, 0};
        rb->Unmap(0, &none);
        const double psnr = Psnr(out, truth, ow, oh, kBorder);
        std::printf("jitter sign %+d: DLSS PSNR %.2f dB (bilinear %.2f, delta %+.2f) range [%.3f, %.3f] finite=%d\n", sign,
                    psnr, psnr_bilinear, psnr - psnr_bilinear, lo, hi, finite ? 1 : 0);
        CHECK(finite);
        CHECK(hi - lo > 0.5f); // not flat / not black
        if (finite && psnr > best) { best = psnr; best_sign = sign; }
    }
    CHECK(all_ok);
    CHECK(best > psnr_bilinear);
    // Session 3 (2026-10-24, RTX 5070 Ti): sign -1 gave +6.4 dB, sign +1 gave -2.5 dB.
    // The EvalDesc convention documents -1; a change here means NGX or our mapping changed.
    CHECK(best_sign == -1);
    std::printf("DLSS quality check: best %.2f dB with jitter sign %+d vs bilinear %.2f dB -> %s\n", best, best_sign,
                psnr_bilinear, best > psnr_bilinear ? "better" : "NOT better");
    const auto st = na::dlss::GetStats();
    std::printf("stats: evaluates=%llu created=%llu released=%llu pending=%llu leaked=%llu untracked=%llu\n",
                static_cast<unsigned long long>(st.evaluates), static_cast<unsigned long long>(st.features_created),
                static_cast<unsigned long long>(st.features_released), static_cast<unsigned long long>(st.features_pending),
                static_cast<unsigned long long>(st.features_leaked), static_cast<unsigned long long>(st.untracked));
    CHECK(st.features_created == 1); // same size/flags throughout: never recreated
    d.WaitIdle();
    na::dlss::Shutdown();
    return test::finish("dlss_real_test");
}
