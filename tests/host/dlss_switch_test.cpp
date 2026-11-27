// One DLSS feature at a time, made off the frame (dlss_backend.hpp PrepareFeature; coordinator
// 2026-11-27, after Cort saw 10 of 15 GB in use). Real NVIDIA runtime, label gpu;nvidia.
//
// A frame loop like the routes' fold block: DLAA -> Performance -> Quality -> DLAA at the picture size
// given (default 3440x1440, FP16 HDR), at a slow and a fast frame rate (the rest of each frame is
// slept, like a game's own work). The level asked for is shown on the frame after its feature was
// started (right after the previous frame's Evaluate); that frame waits only for what is left of
// NVIDIA's work. Measured: the longest CPU time of our part of a frame across each switch, beside the
// steady frame; the DLSS features held (exactly one once a switch has settled); NVIDIA's video memory
// figure for them.
//
// The 310.9.16 reasoning (coordinator): a smaller feature made after a larger one spoils the larger one,
// but only when the larger one is evaluated again. Here the old feature is never evaluated after the new
// one exists, so the new feature's output must be the same as that of a feature made with no other
// feature ever existing: compared byte for byte after the same frames.
//
// Usage: dlss_switch_test <data dir> [WxH]. SKIP (77) without an NVIDIA GPU.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/diag/vram_ledger.hpp"
#include "core/fold_shading.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
using na::fold::Mode;
constexpr uint64_t kDlaaView = 0x5157, kFoldView = kDlaaView + 0x10;

uint16_t Half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t m = x & 0x7FFFFFu;
    if (e <= 0) return static_cast<uint16_t>(sign);
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | ((static_cast<uint32_t>(e) << 10) | (m >> 13)));
}
float Halton(uint32_t i, uint32_t b)
{
    float f = 1.f, r = 0.f;
    while (i) { f /= static_cast<float>(b); r += f * static_cast<float>(i % b); i /= b; }
    return r;
}

struct Inputs { test::ComPtr<ID3D12Resource> color, depth, motion; uint32_t w = 0, h = 0; };

// A still picture with detail at every scale (FP16 colour), constant depth, zero motion.
bool MakeInputs(test::Device& d, uint32_t w, uint32_t h, Inputs* out)
{
    ID3D12Device* dev = d.device.Get();
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    out->w = w; out->h = h;
    out->color = test::CreateTexture(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    out->depth = test::CreateTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    out->motion = test::CreateTexture(dev, w, h, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!out->color || !out->depth || !out->motion) return false;
    test::List l;
    if (!l.Create(dev)) return false;
    std::vector<test::ComPtr<ID3D12Resource>> uploads;
    ID3D12Resource* res[] = {out->color.Get(), out->depth.Get(), out->motion.Get()};
    for (int i = 0; i < 3; ++i) {
        auto desc = res[i]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 bytes = 0;
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto up = test::CreateBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!up) return false;
        uint8_t* p = nullptr;
        if (FAILED(up->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = p + y * fp.Footprint.RowPitch;
            for (uint32_t x = 0; x < w; ++x) {
                if (i == 0) {
                    const double u = double(x) / w - .5, v = double(y) / h - .5;
                    const float c = static_cast<float>(.5 + .5 * std::cos(900.0 * (u * u + v * v)));
                    uint16_t* px = reinterpret_cast<uint16_t*>(row) + 4 * x;
                    px[0] = Half(c * 2.f); px[1] = Half(c); px[2] = Half(1.f - c); px[3] = Half(1.f);
                } else if (i == 1) {
                    reinterpret_cast<float*>(row)[x] = .5f;
                } else {
                    float* m = reinterpret_cast<float*>(row) + 2 * x;
                    m[0] = m[1] = 0.f;
                }
            }
        }
        up->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        dst.pResource = res[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b = test::Transition(res[i], D3D12_RESOURCE_STATE_COPY_DEST, read);
        l.list->ResourceBarrier(1, &b);
        uploads.push_back(up);
    }
    if (FAILED(l.list->Close())) return false;
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    return true;
}

struct Loop {
    test::Device& d;
    test::List list;
    test::ComPtr<ID3D12Resource> output;
    Inputs full, quality, performance;
    uint32_t w = 0, h = 0;
    Mode shown = Mode::Off;
    uint32_t frame = 0, level_frame = 0;
    double waited_ms = 0; // this frame's wait for NVIDIA (part of the frame's time)

    explicit Loop(test::Device& dev) : d(dev) {}
    na::dlss::EvalDesc Desc(Mode m) const
    {
        const Inputs& in = m == Mode::Performance ? performance : m == Mode::Quality ? quality : full;
        na::dlss::EvalDesc e{};
        e.color = in.color.Get(); e.depth = in.depth.Get(); e.motion = in.motion.Get(); e.output = output.Get();
        e.render_w = e.max_render_w = in.w; e.render_h = e.max_render_h = in.h; e.out_w = w; e.out_h = h;
        e.quality = m == Mode::Performance ? na::dlss::Quality::MaxPerformance : m == Mode::Quality ? na::dlss::Quality::MaxQuality : na::dlss::Quality::DLAA;
        e.preset = 13; e.hdr = true; e.auto_exposure = true;
        e.view_id = m == Mode::Off ? kDlaaView : kFoldView + static_cast<uint64_t>(m);
        e.ordered_feature_group = kDlaaView;
        const uint32_t i = level_frame % 32 + 1;
        e.jitter_x = Halton(i, 2) - .5f; e.jitter_y = Halton(i, 3) - .5f;
        e.reset = level_frame == 0;
        return e;
    }
    // One frame of the route's fold block; returns our part's CPU time (ms).
    double Frame(Mode asked)
    {
        const auto t0 = std::chrono::steady_clock::now();
        waited_ms = 0;
        bool switch_now = false, prepare_after = false;
        if (asked != shown) {
            const auto want = Desc(asked);
            auto st = na::dlss::PreparedState(want);
            if (st == na::dlss::Prepared::Making) st = na::dlss::WaitPrepared(want, &waited_ms);
            if (st == na::dlss::Prepared::Ready) switch_now = true;
            else if (st == na::dlss::Prepared::None) prepare_after = true;
            else std::printf("  feature for %s FAILED\n", na::fold::Name(asked));
        }
        const Mode before = shown;
        if (switch_now) { shown = asked; level_frame = 0; }
        list.Reset();
        const auto e = Desc(shown);
        const bool ok = na::dlss::Evaluate(list.list.Get(), e);
        if (switch_now) na::dlss::ReleaseView(before == Mode::Off ? kDlaaView : kFoldView + static_cast<uint64_t>(before));
        if (prepare_after) na::dlss::PrepareFeature(Desc(asked));
        list.list->Close();
        ID3D12CommandList* lists[] = {list.list.Get()};
        na::dlss::BeforeExecute(d.queue.Get(), 1, lists);
        d.queue->ExecuteCommandLists(1, lists);
        na::dlss::OnExecuted(d.queue.Get(), 1, lists);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        d.WaitIdle(); // the GPU's frame (not counted: the game's GPU work overlaps it)
        ++frame; ++level_frame;
        if (!ok) std::printf("  frame %u: Evaluate FAILED (%s)\n", frame, na::dlss::GetStats().reason);
        return ms;
    }
};

uint64_t Held() { const auto s = na::dlss::GetStats(); return s.features_created - s.features_released; }

// Reads the output back (FP16, w x h) into bytes.
std::vector<uint8_t> ReadOutput(test::Device& d, ID3D12Resource* out)
{
    auto desc = out->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    auto rb = test::CreateBuffer(d.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    test::List l;
    if (!rb || !l.Create(d.device.Get())) return {};
    auto b = test::Transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = out; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = test::Transition(out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l.list->ResourceBarrier(1, &b);
    l.list->Close();
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    std::vector<uint8_t> v(static_cast<size_t>(bytes));
    void* p = nullptr;
    if (SUCCEEDED(rb->Map(0, nullptr, &p))) { std::memcpy(v.data(), p, v.size()); rb->Unmap(0, nullptr); }
    return v;
}
} // namespace

int main(int argc, char** argv)
{
    test::Device d;
    if (!test::CreateDevice(d, false)) { std::printf("dlss_switch_test: SKIP (no hardware D3D12 device)\n"); return test::kSkip; }
    {
        test::ComPtr<IDXGIAdapter1> a;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(d.factory->EnumAdapterByLuid(d.device->GetAdapterLuid(), IID_PPV_ARGS(&a)))) a->GetDesc1(&desc);
        if (desc.VendorId != 0x10DE) { std::printf("dlss_switch_test: SKIP (not NVIDIA)\n"); return test::kSkip; }
    }
    std::wstring data_dir = L".";
    if (argc > 1) { const std::string a = argv[1]; data_dir.assign(a.begin(), a.end()); CreateDirectoryW(data_dir.c_str(), nullptr); }
    uint32_t W = 3440, H = 1440;
    if (argc > 2) sscanf_s(argv[2], "%ux%u", &W, &H);
    REQUIRE(na::dlss::InitOwn(d.device.Get(), data_dir.c_str()) && na::dlss::GetCapabilities().available);

    Loop loop(d);
    loop.w = W; loop.h = H;
    REQUIRE(loop.list.Create(d.device.Get()));
    loop.output = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(loop.output);
    const auto qb = na::fold::BlockFor(Mode::Quality), pb = na::fold::BlockFor(Mode::Performance);
    REQUIRE(MakeInputs(d, W, H, &loop.full));
    REQUIRE(MakeInputs(d, na::fold::Folded(W, qb.x), na::fold::Folded(H, qb.y), &loop.quality));
    REQUIRE(MakeInputs(d, na::fold::Folded(W, pb.x), na::fold::Folded(H, pb.y), &loop.performance));
    loop.list.list->Close();

    const auto ours = [] {
        static char line[1024];
        na::vram::FormatLine(line, sizeof line);
        return line;
    };
    for (const double frame_ms : {31.0, 7.0}) {
        std::printf("frames of %.0f ms (the rest of each frame slept, like the game's own work), %ux%u FP16:\n", frame_ms, W, H);
        const auto run = [&](Mode m, int n, double* longest, double* steady_sum, int* steady_n, int* switched_after, double* waited) {
            for (int k = 0; k < n; ++k) {
                const Mode before = loop.shown;
                const auto t0 = std::chrono::steady_clock::now();
                const double ms = loop.Frame(m);
                if (longest) *longest = (std::max)(*longest, ms);
                if (waited) *waited = (std::max)(*waited, loop.waited_ms);
                if (steady_sum && k >= n / 2) { *steady_sum += ms; ++*steady_n; }
                if (switched_after && before != loop.shown && *switched_after < 0) *switched_after = k;
                const double used = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (used < frame_ms) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((frame_ms - used) * 1000.0)));
            }
        };
        if (loop.shown != Mode::Off) { double l = 0; run(Mode::Off, 30, &l, nullptr, nullptr, nullptr, nullptr); }
        double s_sum = 0; int s_n = 0;
        run(Mode::Off, 40, nullptr, &s_sum, &s_n, nullptr, nullptr);
        const double steady = s_sum / s_n;
        std::printf("  steady DLAA: %.2f ms of ours a frame | features held %llu | %s\n", steady, static_cast<unsigned long long>(Held()), ours());
        CHECK(Held() == 1);
        struct Step { Mode to; const char* name; };
        const Step steps[] = {{Mode::Performance, "DLAA -> Performance"}, {Mode::Quality, "Performance -> Quality"}, {Mode::Off, "Quality -> DLAA"}};
        for (const auto& st : steps) {
            double longest = 0, waited = 0, sum = 0;
            int n = 0, after = -1;
            run(st.to, 40, &longest, &sum, &n, &after, &waited);
            std::printf("  %-24s longest %.2f ms (steady %.2f, +%.2f), waited for NVIDIA %.2f ms, on screen after %d frame(s) | features held %llu | %s\n",
                        st.name, longest, steady, longest - steady, waited, after, static_cast<unsigned long long>(Held()), ours());
            CHECK(after >= 1);
            CHECK(Held() == 1);
            const auto ps = na::dlss::GetPrepareStats();
            std::printf("    NVIDIA made the feature off the frame in %.1f ms (longest so far %.1f ms)\n", ps.last_ms, ps.longest_ms);
        }
    }

    // The 310.9.16 reasoning: Performance made after DLAA (DLAA then released, never evaluated again)
    // against Performance made with no other feature ever.
    {
        double l = 0; (void)l;
        while (loop.shown != Mode::Performance) loop.Frame(Mode::Performance);
        for (int k = 0; k < 24; ++k) loop.Frame(Mode::Performance);
        const auto after_dlaa = ReadOutput(d, loop.output.Get());
        na::dlss::Shutdown();
        REQUIRE(na::dlss::InitOwn(d.device.Get(), data_dir.c_str()));
        loop.shown = Mode::Off;
        loop.level_frame = 0;
        // Straight to Performance: its feature is made first (synchronously on this frame), none other exists.
        loop.shown = Mode::Performance;
        for (int k = 0; k < 25; ++k) loop.Frame(Mode::Performance);
        const auto alone = ReadOutput(d, loop.output.Get());
        size_t differ = 0;
        for (size_t i = 0; i < after_dlaa.size() && i < alone.size(); ++i) differ += after_dlaa[i] != alone[i];
        std::printf("Performance after DLAA (DLAA released unevaluated) vs Performance alone, after 25 frames: %zu of %zu bytes differ\n",
                    differ, after_dlaa.size());
        CHECK(!after_dlaa.empty() && after_dlaa.size() == alone.size());
        CHECK(differ == 0);
    }
    na::dlss::Shutdown();
    std::printf("%s\n", test::g_failures ? "FAILED" : "PASSED");
    return test::g_failures ? 1 : 0;
}
