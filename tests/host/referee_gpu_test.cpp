// The referee on the card (capture/referee_gpu.cpp), end to end on a D3D12 device (hardware, else
// WARP, so it runs in CI). Pictures from referee_gpu_scene.hpp go through the same calls the
// program makes: Record / Submitted / OnFrameClosed / OnPresent for two consecutive evaluations.
// Checked:
//   1. the card's numbers equal the processor reference (core/referee.hpp) for every block;
//   2. right pictures: all four measurements pass;
//   3. a screen that shows DLSS's input instead of its output: result fails;
//   4. vectors half the real motion: motion fails.
#include "test_support.hpp"
#include "referee_gpu_scene.hpp"
#include "host_dx12/capture/referee_gpu.hpp"
#include "host_dx12/settings.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace {
using test::ComPtr;
namespace rs = referee_scene;
namespace rg = na::referee::gpu;
using na::referee::Verdict;
using na::referee::VerdictName;

constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void __stdcall Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) { q->ExecuteCommandLists(n, lists); }

void Run(test::Device& d, test::List& l)
{
    l.list->Close();
    ID3D12CommandList* one[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, one);
    d.WaitIdle();
    l.Reset();
}

// Fills `tex` (in COPY_DEST) from rows of `row_bytes`; the upload buffer is kept in `keep`.
bool Upload(test::Device& d, test::List& l, ID3D12Resource* tex, const void* data, size_t row_bytes,
            std::vector<ComPtr<ID3D12Resource>>& keep)
{
    const D3D12_RESOURCE_DESC td = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_size = 0, total = 0;
    d.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_size, &total);
    ComPtr<ID3D12Resource> up = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) return false;
    uint8_t* p = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(up->Map(0, &none, reinterpret_cast<void**>(&p)))) return false;
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(p + fp.Offset + size_t(y) * fp.Footprint.RowPitch, static_cast<const uint8_t*>(data) + size_t(y) * row_bytes, row_bytes);
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = up.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    keep.push_back(up);
    return true;
}

// A linear-luminance picture as RGBA32F (r = g = b), as a game's colour or DLSS's output.
ComPtr<ID3D12Resource> Picture(test::Device& d, test::List& l, const na::referee::Image& im, D3D12_RESOURCE_STATES end,
                               D3D12_RESOURCE_FLAGS flags, std::vector<ComPtr<ID3D12Resource>>& keep)
{
    std::vector<float> px(size_t(im.w) * im.h * 4);
    for (size_t i = 0; i < im.v.size(); ++i) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = im.v[i]; px[i * 4 + 3] = 1; }
    ComPtr<ID3D12Resource> t = test::CreateTexture(d.device.Get(), im.w, im.h, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, flags);
    if (!t || !Upload(d, l, t.Get(), px.data(), size_t(im.w) * 16, keep)) return nullptr;
    const D3D12_RESOURCE_BARRIER b = test::Transition(t.Get(), D3D12_RESOURCE_STATE_COPY_DEST, end);
    l.list->ResourceBarrier(1, &b);
    return t;
}
ComPtr<ID3D12Resource> MotionTexture(test::Device& d, test::List& l, const std::vector<float>& m, std::vector<ComPtr<ID3D12Resource>>& keep)
{
    ComPtr<ID3D12Resource> t = test::CreateTexture(d.device.Get(), rs::kW, rs::kH, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!t || !Upload(d, l, t.Get(), m.data(), size_t(rs::kW) * 8, keep)) return nullptr;
    const D3D12_RESOURCE_BARRIER b = test::Transition(t.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NPSR);
    l.list->ResourceBarrier(1, &b);
    return t;
}
// The presented image: 8 bits a channel, in PRESENT (= COMMON) as a swap chain's buffer is.
ComPtr<ID3D12Resource> ScreenTexture(test::Device& d, test::List& l, const std::vector<uint8_t>& v, std::vector<ComPtr<ID3D12Resource>>& keep)
{
    std::vector<uint8_t> px(v.size() * 4);
    for (size_t i = 0; i < v.size(); ++i) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v[i]; px[i * 4 + 3] = 255; }
    ComPtr<ID3D12Resource> t = test::CreateTexture(d.device.Get(), rs::kW, rs::kH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!t || !Upload(d, l, t.Get(), px.data(), size_t(rs::kW) * 4, keep)) return nullptr;
    const D3D12_RESOURCE_BARRIER b = test::Transition(t.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    l.list->ResourceBarrier(1, &b);
    return t;
}

struct Images { ID3D12Resource *prev, *input, *output, *motion, *screen; };

// One referee sample: two consecutive evaluations and their Presents, as the program calls them.
// Returns false (with a FAIL line) when the referee did not take it.
bool TakeSample(test::Device& d, test::List& l, uint64_t& ordinal, const Images& im, rg::LastSample& got)
{
    const uint64_t before = rg::GetStats().samples;
    CHECK(rg::Wanted());
    rg::Evaluation e;
    e.color = im.prev; e.output = im.output; e.motion = im.motion;
    e.present_ordinal = ++ordinal;
    e.jitter_x = rs::kJitterBefore[0]; e.jitter_y = rs::kJitterBefore[1];
    const uint32_t pre = rg::Record(l.list.Get(), e);
    CHECK(pre == 1);
    Run(d, l);
    rg::Submitted(pre, d.queue.Get());
    rg::OnFrameClosed(ordinal);
    rg::OnPresent(d.queue.Get(), im.screen, Execute);

    CHECK(rg::Wanted()); // half taken: the next evaluation is wanted
    e.color = im.input;
    e.present_ordinal = ++ordinal;
    e.jitter_x = rs::kJitterNow[0]; e.jitter_y = rs::kJitterNow[1];
    const uint32_t second = rg::Record(l.list.Get(), e);
    CHECK(second == 2);
    Run(d, l);
    rg::Submitted(second, d.queue.Get());
    rg::OnFrameClosed(ordinal);
    rg::OnPresent(d.queue.Get(), im.screen, Execute); // the reduction is queued here, no wait
    d.WaitIdle();
    rg::Wanted(); // reads the finished reduction
    const rg::Stats s = rg::GetStats();
    if (s.samples != before + 1) {
        std::printf("FAIL: no sample taken (samples %llu, stopped=%d why=\"%s\")\n",
                    static_cast<unsigned long long>(s.samples), int(s.stopped), s.why);
        ++test::g_failures;
        return false;
    }
    got = rg::GetLastSample();
    CHECK(got.screen && got.tiles && got.pair);
    CHECK(std::fabs(got.jitter_change - rs::JitterChange()) < 1e-4f);
    CHECK(got.data.size() == na::referee::kResultFloats);
    return got.data.size() == na::referee::kResultFloats;
}

// Every float the card wrote against the processor reference.
void Compare(const char* what, const std::vector<float>& card, const std::vector<float>& ref)
{
    using namespace na::referee;
    static const char* names[kBlocks] = {"thumb input", "thumb output", "thumb screen", "tile input", "tile output", "tile screen", "points"};
    uint32_t bad = 0;
    float worst = 0;
    for (uint32_t i = 0; i < kResultFloats; ++i) {
        const float diff = std::fabs(card[i] - ref[i]);
        worst = (std::max)(worst, diff);
        if (diff <= 2e-3f + 1e-3f * std::fabs(ref[i])) continue;
        if (bad++ < 6) {
            uint32_t b = 0;
            while (b + 1 < kBlocks && i >= BlockOffset(Block(b + 1))) ++b;
            std::printf("  %s: %s [%u] card %.6f, processor %.6f\n", what, names[b], i - BlockOffset(Block(b)), double(card[i]), double(ref[i]));
        }
    }
    std::printf("%s: %u of %u values differ from the processor reference (largest difference %.6f)\n", what, bad, kResultFloats, double(worst));
    CHECK(bad == 0);
}

void Report(const char* what, const na::referee::Verdicts& v)
{
    std::printf("%s: picture=%s result=%s (%s) motion=%s (%s) jitter=%s (%s)\n", what, VerdictName(v.picture), VerdictName(v.result),
                v.result_why, VerdictName(v.motion), v.motion_why, VerdictName(v.jitter), v.jitter_why);
}

} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("referee_gpu_test");
    na::settings::Get().referee_enabled.store(1);
    na::settings::Get().referee_every_ms.store(100);
    test::List l;
    REQUIRE(l.Create(d.device.Get()));

    const na::referee::Image prev = rs::Previous(), input = rs::Input(), output = rs::Output();
    const std::vector<float> motion = rs::Motion();
    std::vector<float> half = motion;
    for (float& v : half) v *= .5f;
    const std::vector<uint8_t> shows_output = rs::ScreenBytes(output), shows_input = rs::ScreenBytes(input);

    std::vector<ComPtr<ID3D12Resource>> keep;
    ComPtr<ID3D12Resource> t_prev = Picture(d, l, prev, NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
    ComPtr<ID3D12Resource> t_input = Picture(d, l, input, NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
    ComPtr<ID3D12Resource> t_output = Picture(d, l, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, keep);
    ComPtr<ID3D12Resource> t_motion = MotionTexture(d, l, motion, keep), t_half = MotionTexture(d, l, half, keep);
    ComPtr<ID3D12Resource> t_screen = ScreenTexture(d, l, shows_output, keep), t_screen_input = ScreenTexture(d, l, shows_input, keep);
    REQUIRE(t_prev && t_input && t_output && t_motion && t_half && t_screen && t_screen_input);
    Run(d, l);
    keep.clear();

    uint64_t ordinal = 0;
    rg::LastSample got;

    // 1. Right pictures.
    if (TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), t_motion.Get(), t_screen.Get()}, got)) {
        Compare("right pictures", got.data, rs::Expected(prev, input, output, rs::ScreenImage(shows_output), motion));
        const na::referee::Verdicts v = rs::JudgeAlone(got.data);
        Report("right pictures", v);
        CHECK(v.picture == Verdict::Pass);
        CHECK(v.result == Verdict::Pass);
        CHECK(v.motion == Verdict::Pass);
        CHECK(v.jitter == Verdict::Pass);
    }

    // 2. The screen shows DLSS's input.
    Sleep(150);
    if (TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), t_motion.Get(), t_screen_input.Get()}, got)) {
        Compare("screen shows the input", got.data, rs::Expected(prev, input, output, rs::ScreenImage(shows_input), motion));
        const na::referee::Verdicts v = rs::JudgeAlone(got.data);
        Report("screen shows the input", v);
        CHECK(v.result == Verdict::Fail);
        CHECK(v.motion == Verdict::Pass);
    }

    // 3. Vectors half the real motion.
    Sleep(150);
    if (TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), t_half.Get(), t_screen.Get()}, got)) {
        Compare("half vectors", got.data, rs::Expected(prev, input, output, rs::ScreenImage(shows_output), half));
        const na::referee::Verdicts v = rs::JudgeAlone(got.data);
        Report("half vectors", v);
        CHECK(v.motion == Verdict::Fail);
        CHECK(v.result == Verdict::Pass);
    }

    const rg::Stats s = rg::GetStats();
    std::printf("stats: samples=%llu pairs=%llu screens=%llu pre=%llu main=%llu failed=%llu unpaired=%llu\n",
                static_cast<unsigned long long>(s.samples), static_cast<unsigned long long>(s.pairs),
                static_cast<unsigned long long>(s.screens), static_cast<unsigned long long>(s.pre),
                static_cast<unsigned long long>(s.main), static_cast<unsigned long long>(s.failed),
                static_cast<unsigned long long>(s.unpaired));
    CHECK(s.samples == 3 && s.pairs == 3 && s.failed == 0 && s.unpaired == 0 && !s.stopped);
    return test::finish("referee_gpu_test");
}
