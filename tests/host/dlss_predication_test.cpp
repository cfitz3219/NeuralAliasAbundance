// Real-hardware question for exact inline placement (label gpu;nvidia, Cort's RTX PC).
// The SE5 pan flicker comes from DLAA being recorded at a "scene finished?"
// point before the true last scene draw. The planned fix records DLAA at every
// candidate point in the game's list, each wrapped in GPU predication, and at
// submission (when the whole list is known) switches on only the candidate after
// the true last scene draw. That is only safe if an NGX evaluate whose GPU work
// is predicated off leaves DLSS exactly as if it had never been recorded (no
// CPU-side history flip, no internal work outside the predicate).
//
// Four 64-frame runs on one feature (reset at frame 0 of each run), DLAA mode,
// jittered static zone plate:
//   A  one evaluate per frame (reference)
//   B  a predicated-OFF evaluate of a wrong (grey) picture, then the real evaluate
//      predicated ON: the planned pattern; predicate values are written after
//      recording, before ExecuteCommandLists, as the live path will do
//   C  the same grey evaluate NOT predicated, then the real one (sensitivity
//      control: shows the comparison can see a polluted history)
//   A2 A again (DLSS run-to-run noise floor)
// Pass: B matches A within the A/A2 noise floor, and C differs clearly.
// Every number is printed (ctest log); nothing has to be read off a screen.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

#include <cmath>
#include <cstring>
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
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
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
double Zone(double u, double v, double k)
{
    const double du = u - 0.5, dv = v - 0.5;
    return 0.5 + 0.5 * std::cos(k * (du * du + dv * dv));
}
struct Diff { double max = 0, mean = 0; };
Diff Compare(const std::vector<float>& a, const std::vector<float>& b)
{
    Diff d;
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double e = std::fabs(static_cast<double>(a[i]) - b[i]);
        sum += e;
        if (e > d.max) d.max = e;
    }
    d.mean = a.empty() ? 0 : sum / static_cast<double>(a.size());
    return d;
}
} // namespace

int main(int argc, char** argv)
{
    test::Device d;
    if (!test::CreateDevice(d, /*allow_warp=*/false)) {
        std::printf("dlss_predication_test: SKIP (no hardware D3D12 device)\n");
        return test::kSkip;
    }
    {
        test::ComPtr<IDXGIAdapter1> a;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(d.factory->EnumAdapterByLuid(d.device->GetAdapterLuid(), IID_PPV_ARGS(&a)))) a->GetDesc1(&desc);
        if (desc.VendorId != 0x10DE) {
            std::printf("dlss_predication_test: SKIP (adapter is not NVIDIA: vendor 0x%04X)\n", desc.VendorId);
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
    std::printf("NGX own instance: %s; DLSS available=%d\n", init_ok ? "started" : "FAILED", caps.available ? 1 : 0);
    if (caps.reason[0]) std::printf("reason: %s\n", caps.reason);
    REQUIRE(init_ok && caps.available);

    const uint32_t w = 1280, h = 720; // DLAA: render size == output size (as in SE5)
    const double kZone = 560.0 * 3.14159265358979 / 0.7071;
    ID3D12Device* dev = d.device.Get();
    auto color = test::CreateTexture(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto grey = test::CreateTexture(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto depth = test::CreateTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto motion = test::CreateTexture(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto output = test::CreateTexture(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(color && grey && depth && motion && output);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fc{}, fd{}, fm{}, fo{};
    UINT64 bytes_c = 0, bytes_d = 0, bytes_m = 0, bytes_o = 0;
    {
        D3D12_RESOURCE_DESC rd = color->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fc, nullptr, nullptr, &bytes_c);
        rd = depth->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fd, nullptr, nullptr, &bytes_d);
        rd = motion->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fm, nullptr, nullptr, &bytes_m);
        rd = output->GetDesc(); dev->GetCopyableFootprints(&rd, 0, 1, 0, &fo, nullptr, nullptr, &bytes_o);
    }
    auto up_c = test::CreateBuffer(dev, bytes_c, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto up_g = test::CreateBuffer(dev, bytes_c, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto up_d = test::CreateBuffer(dev, bytes_d, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto up_m = test::CreateBuffer(dev, bytes_m, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    // Predicates: an upload buffer is GENERIC_READ, which includes PREDICATION.
    // Two 8-byte slots: [0] for the early (wrong) candidate, [1] for the chosen one.
    auto pred = test::CreateBuffer(dev, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto rb = test::CreateBuffer(dev, bytes_o, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(up_c && up_g && up_d && up_m && pred && rb);
    uint64_t* pv = nullptr;
    REQUIRE(SUCCEEDED(pred->Map(0, nullptr, reinterpret_cast<void**>(&pv))));
    {
        uint8_t* p = nullptr;
        REQUIRE(SUCCEEDED(up_d->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        for (uint32_t y = 0; y < h; ++y) {
            float* row = reinterpret_cast<float*>(p + y * fd.Footprint.RowPitch);
            for (uint32_t x = 0; x < w; ++x) row[x] = 0.5f;
        }
        up_d->Unmap(0, nullptr);
        REQUIRE(SUCCEEDED(up_m->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        std::memset(p, 0, static_cast<size_t>(bytes_m));
        up_m->Unmap(0, nullptr);
        REQUIRE(SUCCEEDED(up_g->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        for (uint32_t y = 0; y < h; ++y) {
            uint16_t* row = reinterpret_cast<uint16_t*>(p + y * fc.Footprint.RowPitch);
            for (uint32_t x = 0; x < w * 4; ++x) row[x] = ToHalf(x % 4 == 3 ? 1.f : (y < h / 2 ? 0.25f : 0.f)); // "half-drawn" scene
        }
        up_g->Unmap(0, nullptr);
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
    copy_in(grey.Get(), up_g.Get(), fc);
    D3D12_RESOURCE_BARRIER init[] = {
        test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        test::Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        test::Transition(grey.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    l.list->ResourceBarrier(3, init);
    REQUIRE(SUCCEEDED(l.list->Close()));
    { ID3D12CommandList* ls[] = {l.list.Get()}; d.queue->ExecuteCommandLists(1, ls); }
    d.WaitIdle();

    enum Mode { kSingle, kPredicated, kUnpredicated, kOnOnly, kOffSameColor, kOffNoLaterOn };
    bool color_in_srv = false, all_ok = true;
    const auto run = [&](Mode mode, std::vector<float>& out) -> bool {
        const uint32_t kFrames = 64;
        for (uint32_t i = 0; i < kFrames; ++i) {
            const float jx = Halton((i % 32) + 1, 2) - 0.5f, jy = Halton((i % 32) + 1, 3) - 0.5f;
            uint8_t* p = nullptr;
            if (FAILED(up_c->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
            for (uint32_t y = 0; y < h; ++y) {
                uint16_t* row = reinterpret_cast<uint16_t*>(p + y * fc.Footprint.RowPitch);
                for (uint32_t x = 0; x < w; ++x) {
                    const double u = (x + 0.5 - jx) / w, v = (y + 0.5 - jy) / h; // sample = centre - jitter
                    const uint16_t c = ToHalf(static_cast<float>(Zone(u, v, kZone)));
                    row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = c;
                    row[x * 4 + 3] = ToHalf(1.f);
                }
            }
            up_c->Unmap(0, nullptr);
            l.Reset();
            if (color_in_srv) {
                D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                l.list->ResourceBarrier(1, &b);
            }
            copy_in(color.Get(), up_c.Get(), fc);
            D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            l.list->ResourceBarrier(1, &b);
            color_in_srv = true;
            na::dlss::EvalDesc e;
            e.depth = depth.Get();
            e.motion = motion.Get();
            e.output = output.Get();
            e.render_w = w; e.render_h = h; e.out_w = w; e.out_h = h;
            e.jitter_x = jx; e.jitter_y = jy;
            e.reset = i == 0;
            e.quality = na::dlss::Quality::DLAA;
            bool ok = true;
            // Isolation variants (informational): which part changes DLSS?
            //   kOnOnly        only the predicated-ON real evaluate (no early candidate)
            //   kOffSameColor  the early candidate sees the CORRECT picture: a difference then
            //                  comes from state outside the GPU data (CPU side), not the pixels
            //   kOffNoLaterOn  early candidate predicated off, real evaluate NOT predicated
            const bool early = mode == kPredicated || mode == kUnpredicated || mode == kOffSameColor || mode == kOffNoLaterOn;
            const bool early_pred = mode != kUnpredicated;
            const bool real_pred = mode == kPredicated || mode == kOnOnly || mode == kOffSameColor;
            if (early) {
                if (early_pred) l.list->SetPredication(pred.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                e.color = mode == kOffSameColor ? color.Get() : grey.Get();
                ok = na::dlss::Evaluate(l.list.Get(), e) && ok;
                if (early_pred) l.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            }
            if (real_pred) l.list->SetPredication(pred.Get(), 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
            e.color = color.Get();
            ok = na::dlss::Evaluate(l.list.Get(), e) && ok;
            if (real_pred) l.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            if (!ok) {
                std::printf("FAIL evaluate mode %d frame %u: %s\n", static_cast<int>(mode), i, na::dlss::GetStats().reason);
                return false;
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
            if (FAILED(l.list->Close())) return false;
            // Decided at submission, after the whole list was recorded (as the live path will).
            pv[0] = 0; // early candidate: off
            pv[1] = 1; // chosen candidate: on
            ID3D12CommandList* ls[] = {l.list.Get()};
            d.queue->ExecuteCommandLists(1, ls);
            na::dlss::OnExecuted(d.queue.Get(), 1, ls);
            d.WaitIdle();
        }
        out.assign(static_cast<size_t>(w) * h, 0.f);
        uint8_t* p = nullptr;
        if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
        for (uint32_t y = 0; y < h; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(p + y * fo.Footprint.RowPitch);
            for (uint32_t x = 0; x < w; ++x) out[y * w + x] = FromHalf(row[x * 4]);
        }
        D3D12_RANGE none{0, 0};
        rb->Unmap(0, &none);
        return true;
    };
    std::vector<float> a, b, c, a2, on_only, same_color, off_plain;
    all_ok = run(kSingle, a) && run(kPredicated, b) && run(kUnpredicated, c) && run(kSingle, a2) &&
             run(kOnOnly, on_only) && run(kOffSameColor, same_color) && run(kOffNoLaterOn, off_plain);
    REQUIRE(all_ok);
    {
        const Diff d1 = Compare(a, on_only), d2 = Compare(a, same_color), d3 = Compare(a, off_plain), d4 = Compare(b, same_color);
        std::printf("isolate on-only        A vs D: max %.6f mean %.8f (real evaluate predicated ON, nothing else)\n", d1.max, d1.mean);
        std::printf("isolate off-same-color A vs E: max %.6f mean %.8f (early candidate sees the correct picture)\n", d2.max, d2.mean);
        std::printf("isolate off-plain-real A vs F: max %.6f mean %.8f (early off, real evaluate not predicated)\n", d3.max, d3.mean);
        std::printf("isolate B vs E               : max %.6f mean %.8f (0 = the early candidate's pixels do not matter)\n", d4.max, d4.mean);
    }
    const Diff noise = Compare(a, a2), planned = Compare(a, b), control = Compare(a, c);
    std::printf("noise floor   A vs A2: max %.6f mean %.8f\n", noise.max, noise.mean);
    std::printf("planned       A vs B : max %.6f mean %.8f (predicated-off wrong evaluate, then predicated-on real one)\n",
                planned.max, planned.mean);
    std::printf("control       A vs C : max %.6f mean %.8f (wrong evaluate actually executed)\n", control.max, control.mean);
    const double tol_max = noise.max + 2e-3, tol_mean = noise.mean + 1e-5;
    const bool clean = planned.max <= tol_max && planned.mean <= tol_mean;
    const bool sensitive = control.mean > 10 * tol_mean;
    std::printf("result: predicated-off evaluate leaves DLSS untouched=%s; control detectable=%s\n",
                clean ? "yes" : "NO", sensitive ? "yes" : "NO (test inconclusive)");
    CHECK(sensitive);
    CHECK(clean);
    const auto st = na::dlss::GetStats();
    std::printf("stats: evaluates=%llu created=%llu released=%llu\n", static_cast<unsigned long long>(st.evaluates),
                static_cast<unsigned long long>(st.features_created), static_cast<unsigned long long>(st.features_released));
    CHECK(st.features_created == 1);
    pred->Unmap(0, nullptr);
    d.WaitIdle();
    na::dlss::Shutdown();
    return test::finish("dlss_predication_test");
}
