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
#include "host_dx12/background/work.hpp"
#include "host_dx12/diag/event_log.hpp"
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
ComPtr<ID3D12Resource> MotionTexture(test::Device& d, test::List& l, const std::vector<float>& m, std::vector<ComPtr<ID3D12Resource>>& keep,
                                        UINT width = rs::kW, UINT height = rs::kH)
{
    ComPtr<ID3D12Resource> t = test::CreateTexture(d.device.Get(), width, height, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!t || !Upload(d, l, t.Get(), m.data(), size_t(width) * 8, keep)) return nullptr;
    const D3D12_RESOURCE_BARRIER b = test::Transition(t.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NPSR);
    l.list->ResourceBarrier(1, &b);
    return t;
}
// The presented image: 8 bits a channel, in PRESENT (= COMMON) as a swap chain's buffer is.
ComPtr<ID3D12Resource> ScreenTexture(test::Device& d, test::List& l, const std::vector<uint8_t>& v,
                                     std::vector<ComPtr<ID3D12Resource>>& keep, uint32_t w = rs::kW, uint32_t h = rs::kH)
{
    std::vector<uint8_t> px(v.size() * 4);
    for (size_t i = 0; i < v.size(); ++i) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v[i]; px[i * 4 + 3] = 255; }
    ComPtr<ID3D12Resource> t = test::CreateTexture(d.device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!t || !Upload(d, l, t.Get(), px.data(), size_t(w) * 4, keep)) return nullptr;
    const D3D12_RESOURCE_BARRIER b = test::Transition(t.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    l.list->ResourceBarrier(1, &b);
    return t;
}

struct Images { ID3D12Resource *prev, *input, *output, *motion, *screen; };

// One referee sample: two consecutive evaluations and their Presents, as the program calls them.
// Returns false (with a FAIL line) when the referee did not take it.
bool TakeSample(test::Device& d, test::List& l, uint64_t& ordinal, const Images& im, rg::LastSample& got,
                bool late = false, ID3D12CommandQueue* blocked = nullptr, ID3D12Fence* release = nullptr,
                bool changed_scene = false, bool reset = false, bool display_vectors = false, bool unsampled_reset = false,
                uint64_t native_proof = 0)
{
    const uint64_t before = rg::GetStats().samples;
    CHECK(rg::Wanted());
    rg::Evaluation e;
    e.color = im.prev; e.output = im.output; e.motion = im.motion;
    e.render_w = rs::kW; e.render_h = rs::kH;
    e.output_w = display_vectors ? 2 * rs::kW : rs::kW;
    e.output_h = display_vectors ? 2 * rs::kH : rs::kH;
    e.mv_display_res = display_vectors;
    e.context = {{native_proof ? na::referee::shadow::Path::Native11 : na::referee::shadow::Path::Inline, 22},
                 1, 2, 3, 0, rs::kW, rs::kH, native_proof};
    const bool native11 = native_proof != 0;
    e.present_ordinal = ++ordinal;
    e.jitter_x = rs::kJitterBefore[0]; e.jitter_y = rs::kJitterBefore[1];
    const uint32_t pre = rg::Record(l.list.Get(), e);
    CHECK(pre != 0);
    test::List next_list;
    if (blocked) {
        if (FAILED(l.list->Close()) || !next_list.Create(d.device.Get())) return false;
        ID3D12CommandList* first[] = {l.list.Get()};
        blocked->ExecuteCommandLists(1, first);
        rg::Submitted(pre, blocked);
    } else {
        Run(d, l);
        rg::Submitted(pre, d.queue.Get());
    }
    CHECK(!rg::NeedsScreen({ordinal, &d}));
    rg::OnPresent({ordinal, &d, true, true, native11}, d.queue.Get(), nullptr, Execute);

    CHECK(rg::Wanted()); // half taken: the next evaluation is wanted
    e.color = im.input;
    if (changed_scene) ++e.context.color;
    if (unsampled_reset) rg::NotifyEvaluationReset();
    e.reset = reset;
    e.present_ordinal = ++ordinal;
    e.jitter_x = rs::kJitterNow[0]; e.jitter_y = rs::kJitterNow[1];
    auto& main = blocked ? next_list : l;
    const uint32_t second = rg::Record(main.list.Get(), e);
    CHECK(second > pre);
    if (!second) return false;
    rg::Discard(pre, false);
    Run(d, main);
    rg::Submitted(second, d.queue.Get());
    CHECK(rg::NeedsScreen({ordinal, &d, true, true, native11}));
    CHECK(!rg::NeedsScreen({ordinal, &d, true, false, native11}));
    rg::OnPresent({ordinal, &l, true, true, native11}, d.queue.Get(), im.screen, Execute);
    CHECK(!rg::NeedsScreen({ordinal + 7, &d}));
    CHECK(!rg::NeedsScreen({ordinal, &l}));
    if (late && !blocked) ordinal += 7;
    rg::OnPresent({ordinal, &d, true, true, native11}, d.queue.Get(), im.screen, Execute);
    if (blocked) {
        ComPtr<ID3D12Fence> presenting_done, producer_done;
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool fenced = event && SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presenting_done))) &&
                            SUCCEEDED(d.queue->Signal(presenting_done.Get(), 1)) &&
                            SUCCEEDED(presenting_done->SetEventOnCompletion(1, event));
        const bool presented = fenced && WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
        CHECK(presented);
        CHECK(SUCCEEDED(release->Signal(1)));
        d.WaitIdle();
        if (event) CloseHandle(event);
        if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producer_done))) ||
            FAILED(blocked->Signal(producer_done.Get(), 1)) ||
            FAILED(producer_done->SetEventOnCompletion(1, nullptr))) return false;
        rg::OnPresent({++ordinal, &d}, d.queue.Get(), im.screen, Execute);
    }
    d.WaitIdle();
    rg::Wanted();
    if (!na::background::WaitIdle(3000)) return false;
    const rg::Stats s = rg::GetStats();
    if (s.samples != before + 1) {
        std::printf("FAIL: no sample taken (samples %llu, stopped=%d why=\"%s\")\n",
                    static_cast<unsigned long long>(s.samples), int(s.stopped), s.why);
        ++test::g_failures;
        return false;
    }
    got = rg::GetLastSample();
    CHECK(got.screen == !late && got.tiles == (!late && !display_vectors) && got.pair == (im.motion && !changed_scene && !reset && !unsampled_reset));
    CHECK(std::fabs(got.jitter_change - (got.pair ? rs::JitterChange() : 0.f)) < 1e-4f);
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
#ifndef NA_HAVE_REFEREE_SHADERS
    std::puts("SKIP: Windows SDK FXC was unavailable");
    return test::kSkip;
#endif
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        std::puts("D3D12 debug layer enabled");
    }
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("referee_gpu_test");
    REQUIRE(na::background::Start());
    rg::EnableSampleReceiptForTests();
    na::settings::Get().referee_enabled.store(1);
    na::settings::Get().referee_every_ms.store(100);
    na::settings::Get().referee_log_s.store(1);
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
    Sleep(1050);
    if (TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), t_motion.Get(), t_screen_input.Get()}, got)) {
        Compare("screen shows the input", got.data, rs::Expected(prev, input, output, rs::ScreenImage(shows_input), motion));
        const na::referee::Verdicts v = rs::JudgeAlone(got.data);
        Report("screen shows the input", v);
        CHECK(v.result == Verdict::Fail);
        CHECK(v.motion == Verdict::Pass);
    }
    const auto events = na::eventlog::Drain();
    CHECK(std::any_of(events.begin(), events.end(), [](const auto& line) {
        return line.find("referee_choice mode=preview") != std::string::npos &&
               line.find("last=4 current=inline generation=3") != std::string::npos &&
               line.find("input=1 action=check_continuity") != std::string::npos;
    }));

    // 3. Vectors half the real motion.
    Sleep(150);
    if (TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), t_half.Get(), t_screen.Get()}, got)) {
        Compare("half vectors", got.data, rs::Expected(prev, input, output, rs::ScreenImage(shows_output), half));
        const na::referee::Verdicts v = rs::JudgeAlone(got.data);
        Report("half vectors", v);
        CHECK(v.motion == Verdict::Fail);
        CHECK(v.result == Verdict::Pass);
    }

    Sleep(150);
    CHECK(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), t_motion.Get(), t_screen.Get()}, got, true));
    CHECK(na::referee::Score({got.data.data(), got.screen, got.tiles, got.pair, got.jitter_change}).points > 0);

    Sleep(150);
    ComPtr<ID3D12CommandQueue> blocked;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&blocked))));
    ComPtr<ID3D12Fence> release;
    REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&release))));
    REQUIRE(SUCCEEDED(blocked->Wait(release.Get(), 1)));
    CHECK(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), t_motion.Get(), t_screen.Get()},
                     got, true, blocked.Get(), release.Get()));

    const rg::Stats s = rg::GetStats();
    std::printf("stats: samples=%llu pairs=%llu screens=%llu pre=%llu main=%llu failed=%llu unpaired=%llu\n",
                static_cast<unsigned long long>(s.samples), static_cast<unsigned long long>(s.pairs),
                static_cast<unsigned long long>(s.screens), static_cast<unsigned long long>(s.pre),
                static_cast<unsigned long long>(s.main), static_cast<unsigned long long>(s.failed),
                static_cast<unsigned long long>(s.unpaired));
    CHECK(s.samples == 5 && s.pairs == 5 && s.screens == 3 && s.failed == 0 && s.unpaired == 0 && !s.stopped);
    l.Reset(); // the blocked-queue case submitted its second half on a different list
    const Images complete{t_prev.Get(), t_input.Get(), t_output.Get(), t_motion.Get(), t_screen.Get()};
    for (int change = 0; change < 3; ++change) {
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, complete, got, false, nullptr, nullptr, change == 0, change == 1, false, change == 2));
        na::referee::Window window;
        window.Add(na::referee::Score({got.data.data(), got.screen, got.tiles, got.pair, got.jitter_change}));
        CHECK(Judge(window).result == Verdict::Pass && Judge(window).motion == Verdict::NotMeasured);
    }
    // The declared evaluation occupies the top left of a larger allocation.
    // Bright padding must never enter any of the sampled blocks.
    auto pad = [](const na::referee::Image& src, UINT width, UINT height) {
        na::referee::Image dst{width, height, std::vector<float>(size_t(width) * height, 80.f)};
        for (UINT y = 0; y < src.h; ++y)
            std::copy_n(src.v.data() + size_t(y) * src.w, src.w, dst.v.data() + size_t(y) * width);
        return dst;
    };
    auto large_prev = Picture(d, l, pad(prev, rs::kW + 63, rs::kH + 27), NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
    auto large_input = Picture(d, l, pad(input, rs::kW + 63, rs::kH + 27), NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
    auto large_output = Picture(d, l, pad(output, 2 * rs::kW, 2 * rs::kH), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, keep);
    std::vector<float> large_motion(size_t(rs::kW) * rs::kH * 8);
    for (size_t i = 0; i < large_motion.size(); ++i) large_motion[i] = -2.f * rs::kU[i % 2];
    auto display_mv = MotionTexture(d, l, large_motion, keep, 2 * rs::kW, 2 * rs::kH);
    REQUIRE(large_prev && large_input && large_output && display_mv);
    Run(d, l);
    keep.clear();
    Sleep(150);
    REQUIRE(TakeSample(d, l, ordinal, {large_prev.Get(), large_input.Get(), large_output.Get(), t_motion.Get(), t_screen.Get()}, got));
    Compare("padded scene and output", got.data, rs::Expected(prev, input, output, rs::ScreenImage(shows_output), motion));
    const auto cropped = got;
    Sleep(150);
    REQUIRE(TakeSample(d, l, ordinal, {large_prev.Get(), large_input.Get(), large_output.Get(), display_mv.Get(), t_screen.Get()},
                       got, false, nullptr, nullptr, false, false, true));
    const auto points = na::referee::BlockOffset(na::referee::kPoints);
    CHECK(std::equal(got.data.begin() + points, got.data.end(), cropped.data.begin() + points));
    // Every other game frame misses DLAA. No temporal pair exists; the next
    // isolated evaluation must still be compared to its own submitted screen.
    Sleep(150);
    REQUIRE(rg::Wanted());
    rg::Evaluation isolated;
    isolated.color = t_input.Get(); isolated.output = t_output.Get();
    isolated.context = {{na::referee::shadow::Path::Inline, 22}, 1, 2, 3, 0, rs::kW, rs::kH};
    auto submit_isolated = [&] {
        isolated.present_ordinal = ++ordinal;
        const auto token = rg::Record(l.list.Get(), isolated);
        CHECK(token != 0);
        Run(d, l);
        rg::Submitted(token, d.queue.Get());
    };
    submit_isolated();
    rg::OnPresent({ordinal, &d, true, true}, d.queue.Get(), nullptr, Execute);
    rg::OnPresent({++ordinal, &d, true, false}, d.queue.Get(), nullptr, Execute);
    REQUIRE(rg::Wanted());
    submit_isolated();
    CHECK(rg::NeedsScreen({ordinal, &d, true, true}));
    rg::OnPresent({ordinal, &d, true, true}, d.queue.Get(), t_screen.Get(), Execute);
    d.WaitIdle();
    rg::Wanted();
    REQUIRE(na::background::WaitIdle(3000));
    const auto solo = rg::GetLastSample();
    CHECK(solo.number == got.number + 1 && solo.screen && solo.tiles && !solo.pair);
    na::referee::Window single;
    single.Add(na::referee::Score({solo.data.data(), solo.screen, solo.tiles, solo.pair, solo.jitter_change}));
    CHECK(Judge(single).result == Verdict::Pass && Judge(single).motion == Verdict::NotMeasured);
    // The original tiles see only a featureless centre. The next sample should
    // find the textured edges from the existing thumbnails at the same budget.
    auto bands = [](na::referee::Image img) {
        for (UINT y = 0; y < img.h; ++y) {
            if ((y >= 7 && y < 34) || (y >= img.h - 34 && y < img.h - 7)) continue;
            std::fill_n(img.v.begin() + size_t(y) * img.w, img.w, .34f);
        }
        return img;
    };
    auto band_input = Picture(d, l, bands(input), NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
    auto band_output = Picture(d, l, bands(output), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, keep);
    auto band_screen = ScreenTexture(d, l, rs::ScreenBytes(bands(output)), keep);
    REQUIRE(band_input && band_output && band_screen);
    Run(d, l);
    keep.clear();
    const Images edge_detail{t_prev.Get(), band_input.Get(), band_output.Get(), t_motion.Get(), band_screen.Get()};
    for (int attempt = 0; attempt < 2; ++attempt) {
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, edge_detail, got, false, nullptr, nullptr, false, attempt == 0));
        const auto score = na::referee::Score({got.data.data(), got.screen, got.tiles, false, 0});
        std::printf("edge-only detail, sample %d: %u textured, %u output, %u input\n",
                    attempt + 1, score.tiles_textured, score.tiles_output, score.tiles_input);
        if (attempt == 0) CHECK(score.tiles_textured == 0);
        else CHECK(score.tiles_output >= 4 && score.tiles_input == 0);
    }
    // Match the roughly 1.34x scene-to-display ratio seen in the field.
    const uint32_t w = 304, h = 168;
    auto screen_bytes = [&](const na::referee::Image& picture) {
        std::vector<uint8_t> pixels(size_t(w) * h);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const float sx = (float(x) + .5f) * picture.w / w - .5f;
                const float sy = (float(y) + .5f) * picture.h / h - .5f;
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                const float fx = sx - ix, fy = sy - iy;
                const float a = picture.At(ix, iy) * (1 - fx) + picture.At(ix + 1, iy) * fx;
                const float b = picture.At(ix, iy + 1) * (1 - fx) + picture.At(ix + 1, iy + 1) * fx;
                const float v = (a * (1 - fy) + b * fy) * 1.4f;
                const float tone = std::pow(v / (1.f + v), 1.f / 2.2f);
                pixels[size_t(y) * w + x] = uint8_t(std::lround(tone * 255.f));
            }
        }
        return pixels;
    };
    auto tiny_input = ScreenTexture(d, l, screen_bytes(input), keep, w, h);
    auto tiny_output = ScreenTexture(d, l, screen_bytes(output), keep, w, h);
    REQUIRE(tiny_input && tiny_output);
    Run(d, l);
    keep.clear();
    // The pass still submits every time, but only the middle sample makes it
    // through the game's compositor. That success must not erase the others.
    using na::referee::shadow::ScreenVote;
    for (unsigned i = 0; i < 3; ++i) {
        const bool good = i == 1;
        if (i) ++ordinal; // the monitor samples the other Present parity
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), nullptr,
                                          good ? tiny_output.Get() : tiny_input.Get()}, got,
                           false, nullptr, nullptr, false, false, false, false, 17));
        auto vote = na::referee::shadow::VoteScreen(
            na::referee::Score({got.data.data(), got.screen, got.tiles, false, 0}));
        CHECK(got.screen && got.tiles && vote == (good ? ScreenVote::Output : ScreenVote::Input));
        CHECK(rg::TakeNativeScreenLoss(&l) == 0);
        CHECK(rg::TakeNativeScreenLoss(&d) == (i == 2 ? 17u : 0u));
    }
    CHECK(!rg::TakeNativeScreenLoss(&d)); // consumed
    Sleep(150);
    REQUIRE(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), nullptr, tiny_input.Get()}, got,
                       false, nullptr, nullptr, false, false, false, false, 18));
    CHECK(!rg::TakeNativeScreenLoss(&d)); // a new proof does not inherit the old failures
    Sleep(150);
    REQUIRE(rg::Wanted());
    rg::Evaluation discarded;
    discarded.color = t_input.Get();
    discarded.output = t_output.Get();
    discarded.present_ordinal = ++ordinal;
    const uint32_t outstanding = rg::Record(l.list.Get(), discarded);
    REQUIRE(outstanding != 0);
    rg::Discard(outstanding, true);
    CHECK(rg::GetStats().stopped && !rg::Wanted());
    Run(d, l);
    rg::Submitted(outstanding, d.queue.Get());
    CHECK(rg::GetStats().samples == got.number);
    ComPtr<ID3D12InfoQueue> info;
    if (debug && SUCCEEDED(d.device.As(&info))) {
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T bytes = 0;
            REQUIRE(SUCCEEDED(info->GetMessage(i, nullptr, &bytes)));
            std::vector<uint8_t> message(bytes);
            auto* entry = reinterpret_cast<D3D12_MESSAGE*>(message.data());
            REQUIRE(SUCCEEDED(info->GetMessage(i, entry, &bytes)));
            if (entry->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::printf("debug layer: %s\n", entry->pDescription);
                ++test::g_failures;
            }
        }
    }
    CHECK(na::background::Stop(3000));
    return test::finish("referee_gpu_test");
}
