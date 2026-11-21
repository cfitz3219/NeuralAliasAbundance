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
#include "host_dx12/capture/screen_marker.hpp"
#include "core/referee_recovery.hpp"
#include "host_dx12/background/work.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "core/referee_placement.hpp"
#include "core/referee_jitter.hpp"
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

struct Images {
    ID3D12Resource *prev, *input, *output, *motion, *screen;
    bool unknown_geometry = false;
};
using PlacementPath = na::referee::shadow::Path;
struct TracePair {
    ID3D12Resource* before_output;
    ID3D12Resource* before_screen;
    bool accept = true;
    uint64_t ticket = 0, configuration = 0;
    bool returned_present = true;
    uint8_t tools = 0, applied = 0, main_tools = 255;
    PlacementPath path = PlacementPath::Inline, alternate = PlacementPath::None;
    uint32_t recovery_region = 0;
    float recovery_floor = 0.f;
    uint8_t jitter_amount = 0;
    float before_jitter[2]{}, after_jitter[2]{};
};

// One referee sample: two consecutive evaluations and their Presents, as the program calls them.
// Returns false (with a FAIL line) when the referee did not take it.
bool TakeSample(test::Device& d, test::List& l, uint64_t& ordinal, const Images& im, rg::LastSample& got,
                bool late = false, ID3D12CommandQueue* blocked = nullptr, ID3D12Fence* release = nullptr,
                bool changed_scene = false, bool reset = false, bool display_vectors = false, bool unsampled_reset = false,
                uint64_t native_proof = 0, bool rotate = false, const TracePair* trace = nullptr)
{
    const uint64_t before = rg::GetStats().samples;
    CHECK(rg::Wanted());
    rg::Evaluation e;
    e.color = im.prev; e.output = trace ? trace->before_output : im.output; e.motion = im.motion;
    e.render_w = rs::kW; e.render_h = rs::kH;
    e.output_w = display_vectors ? 2 * rs::kW : rs::kW;
    e.output_h = display_vectors ? 2 * rs::kH : rs::kH;
    e.mv_display_res = display_vectors;
    e.context = {{native_proof ? na::referee::shadow::Path::Native11 : na::referee::shadow::Path::Inline, 22},
                 1, 2, 3, 0, rs::kW, rs::kH, native_proof};
    if (trace) {
        e.context.trial_ticket = trace->ticket;
        e.context.trial_configuration = trace->configuration;
        e.context.recovery_region = trace->recovery_region;
        e.context.recovery_floor = trace->recovery_floor;
        if (!native_proof) e.context.placement.path = trace->path;
    }
    const bool native11 = native_proof != 0;
    e.present_ordinal = ++ordinal;
    e.jitter_x = rs::kJitterBefore[0]; e.jitter_y = rs::kJitterBefore[1];
    if (trace && trace->jitter_amount) {
        e.scene_jitter_amount = trace->jitter_amount;
        e.jitter_x = trace->before_jitter[0];
        e.jitter_y = trace->before_jitter[1];
    }
    uint32_t pre = rg::Record(l.list.Get(), e);
    if (!pre && trace && trace->jitter_amount) {
        // Native proof sampling may prefer the other present parity.
        e.present_ordinal = ++ordinal;
        pre = rg::Record(l.list.Get(), e);
    }
    CHECK(pre != 0);
    test::List next_list;
    if (blocked) {
        if (FAILED(l.list->Close()) || !next_list.Create(d.device.Get())) return false;
        ID3D12CommandList* first[] = {l.list.Get()};
        blocked->ExecuteCommandLists(1, first);
        rg::Submitted(pre, blocked);
    } else {
        Run(d, l);
        rg::Submitted(pre, d.queue.Get(), trace ? trace->tools : 0, trace ? trace->applied : 0);
    }
    CHECK(!rg::NeedsScreen({ordinal, &d}));
    rg::Presentation earlier{ordinal, &d, true, true, native11};
    if (trace) earlier.await_result = true;
    rg::OnPresent(earlier, d.queue.Get(), trace ? trace->before_screen : nullptr, Execute);
    if (trace) rg::PresentResult(&d, ordinal, true);

    CHECK(rg::Wanted()); // half taken: the next evaluation is wanted
    e.color = im.input;
    e.output = im.output;
    if (changed_scene) ++e.context.color;
    if (rotate) {
        ++e.context.color;
        ++e.context.native_screen_proof;
    }
    if (unsampled_reset) rg::NotifyEvaluationReset();
    e.reset = reset;
    e.present_ordinal = ++ordinal;
    e.jitter_x = rs::kJitterNow[0]; e.jitter_y = rs::kJitterNow[1];
    if (trace && trace->jitter_amount) {
        e.jitter_x = trace->after_jitter[0];
        e.jitter_y = trace->after_jitter[1];
    }
    auto& main = blocked ? next_list : l;
    const uint32_t second = rg::Record(main.list.Get(), e);
    CHECK(second > pre);
    if (!second) return false;
    rg::Discard(pre, false);
    Run(d, main);
    rg::Submitted(second, d.queue.Get(), trace ? trace->tools & trace->main_tools : 0, trace ? trace->applied : 0);
    CHECK(rg::NeedsScreen({ordinal, &d, true, true, native11}));
    CHECK(!rg::NeedsScreen({ordinal, &d, true, false, native11}));
    rg::OnPresent({ordinal, &l, true, true, native11}, d.queue.Get(), im.screen, Execute);
    CHECK(!rg::NeedsScreen({ordinal + 7, &d}));
    CHECK(!rg::NeedsScreen({ordinal, &l}));
    if (late && !blocked) ordinal += 7;
    rg::Presentation current{ordinal, &d, true, true, native11};
    if (trace) {
        current.await_result = trace->returned_present;
        current.alternate = e.context;
        current.alternate.placement = {trace->alternate, 0};
    }
    rg::OnPresent(current, d.queue.Get(), im.screen, Execute);
    if (trace && trace->returned_present) {
        d.WaitIdle();
        rg::PresentResult(&l, ordinal, true); // a different chain cannot sign for this scene
        rg::PresentResult(&d, ordinal + 1, true); // nor a different frame
        rg::Wanted();
        CHECK(na::background::WaitIdle(3000));
        CHECK(rg::GetStats().samples == before);
        rg::PresentResult(&d, ordinal, trace->accept);
        if (!trace->accept) {
            rg::Wanted();
            CHECK(na::background::WaitIdle(3000));
            return rg::GetStats().samples == before;
        }
    }
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
    CHECK(got.screen == !late && got.tiles == (!late && !display_vectors && !im.unknown_geometry) &&
          got.pair == (im.motion && !changed_scene && !reset && !unsampled_reset));
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


int TestAdaptiveJitter(test::Device& d, test::List& l)
{
    uint64_t ordinal = 0;
    rg::LastSample got;
    std::vector<ComPtr<ID3D12Resource>> keep;
    {
        using namespace na::referee;
        for (const auto path : {PlacementPath::Inline, PlacementPath::Native11}) {
            rg::InvalidatePresentation(&d);
            const uint64_t proof = path == PlacementPath::Native11 ? 90010 : 0;
            const auto amount = [&] { return rg::SceneJitterAmount(&d, path, 1, 2, 3, ordinal, proof); };
            const auto window = [&](uint8_t strength) {
                for (unsigned i = 0; i < 8; ++i) {
                    const float x = i % 2 ? -.004f * strength : .004f * strength;
                    const float y = i % 4 < 2 ? -.0035f * strength : .0035f * strength;
                    const auto a = rs::Render(-x + 1.4f, -y, 0.f, 0), b = rs::Render(x + 1.52f, y, 0.f, 0);
                    const auto c = rs::Render(-.28f * x, -.28f * y, 0.f, 0);
                    const auto e = rs::Render(.12f + .28f * x, .28f * y, 0.f, 0);
                    auto old_input = Picture(d, l, a, NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
                    auto new_input = Picture(d, l, b, NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
                    constexpr auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    constexpr auto allow_uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    auto old_output = Picture(d, l, c, uav, allow_uav, keep);
                    auto new_output = Picture(d, l, e, uav, allow_uav, keep);
                    auto old_screen = ScreenTexture(d, l, rs::ScreenBytes(c), keep);
                    auto new_screen = ScreenTexture(d, l, rs::ScreenBytes(e), keep);
                    if (!old_input || !new_input || !old_output || !new_output || !old_screen || !new_screen) return false;
                    Run(d, l);
                    keep.clear();
                    TracePair trace{old_output.Get(), old_screen.Get(), true, 28160, 90};
                    trace.path = path;
                    trace.jitter_amount = strength;
                    trace.before_jitter[0] = -x; trace.before_jitter[1] = -y;
                    trace.after_jitter[0] = x; trace.after_jitter[1] = y;
                    Sleep(150);
                    if (!TakeSample(d, l, ordinal, {old_input.Get(), new_input.Get(), new_output.Get(), nullptr, new_screen.Get()},
                                    got, false, nullptr, nullptr, false, false, false, false, proof, false, &trace)) return false;
                    TrialSample sample;
                    if (!rg::TakeTrialSample(trace.ticket, sample)) return false;
                    CHECK(sample.delivered);
                    for (const auto& line : na::eventlog::Drain())
                        if (line.find("referee_jitter_geometry") != std::string::npos ||
                            line.find("referee_jitter_trial") != std::string::npos) std::puts(line.c_str());
                    if (amount() != strength) return true;
                }
                return true;
            };
            REQUIRE(window(normal_jitter));
            CHECK(amount() == reduced_jitter);
            REQUIRE(window(reduced_jitter));
            CHECK(amount() == normal_jitter);
            REQUIRE(window(normal_jitter));
            CHECK(amount() == reduced_jitter);
            CHECK(rg::SceneJitterAmount(&d, path, 1, 3, 3, ordinal, proof) == normal_jitter);
            CHECK(rg::SceneJitterAmount(&l, path, 1, 2, 3, ordinal, proof) == normal_jitter);
            std::printf("Referee jitter %s: kept strength %u\n", shadow::Name(path), unsigned(amount()));
            na::settings::Get().referee_enabled.store(0);
            CHECK(amount() == normal_jitter);
            CHECK(!rg::Wanted());
            na::settings::Get().referee_enabled.store(1);
            CHECK(amount() == normal_jitter);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    {
        using namespace na::screen_marker;
        Vote real, inverted, parity, discarded;
        for (uint32_t i = 1; i < kSession; ++i) {
            float prev[3], next[3], inv_prev[3], inv_next[3], natural_prev[3], natural_next[3], flat[3]{};
            for (uint32_t k = 0; k < 3; ++k) {
                prev[k] = ((kPatterns[i - 1] >> k) & 1u) ? .7f : .2f;
                next[k] = ((kPatterns[i] >> k) & 1u) ? .7f : .2f;
                inv_prev[k] = 1.f - prev[k];
                inv_next[k] = 1.f - next[k];
                natural_prev[k] = (i & 1u) ? .2f : .7f;
                natural_next[k] = 1.f - natural_prev[k];
            }
            const uint32_t before = kPatterns[i - 1], after = kPatterns[i];
            real.Add(before, prev, after, next);
            inverted.Add(before, inv_prev, after, inv_next);
            parity.Add(before, natural_prev, after, natural_next);
            discarded.Add(before, flat, after, flat);
        }
        CHECK(real.ResultOf() == Vote::Result::Reached);
        CHECK(inverted.ResultOf() == Vote::Result::Reached);
        CHECK(parity.ResultOf() != Vote::Result::Reached);
        CHECK(discarded.ResultOf() == Vote::Result::Discarded);
    }
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

    if (argc > 1 && std::strcmp(argv[1], "--jitter") == 0) {
        REQUIRE(TestAdaptiveJitter(d, l) == 0);
        CHECK(na::background::Stop(3000));
        return test::finish("referee_gpu_jitter_test");
    }

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
    REQUIRE(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), nullptr, tiny_output.Get()}, got,
                       false, nullptr, nullptr, false, false, false, false, 19));
    CHECK(!rg::TakeNativeScreenLoss(&d));
    ++ordinal; // prefer the other parity for this second witness of proof 18
    Sleep(150);
    REQUIRE(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), nullptr, tiny_input.Get()}, got,
                       false, nullptr, nullptr, false, false, false, false, 18));
    CHECK(rg::TakeNativeScreenLoss(&d) == 18);
    CHECK(!rg::TakeNativeScreenLoss(&d)); // proof 19 stays qualified
    Sleep(150);
    REQUIRE(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), t_motion.Get(), tiny_output.Get()},
                       got, false, nullptr, nullptr, false, false, false, false, 31, true));
    CHECK(got.pair); // different physical sources and proofs, one verified scene history
    for (unsigned i = 0; i < 2; ++i) {
        if (i) ++ordinal;
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), nullptr, tiny_input.Get()},
                           got, false, nullptr, nullptr, false, false, false, false, 41));
    }
    rg::NotifyEvaluationReset();
    CHECK(!rg::TakeNativeScreenLoss(&d)); // pending pre-reset decisions cannot retire the live reader
    // Identical scene and output size, seven additional rows on the display. The same
    // registration must identify a shown input, shown output, and unrelated overlay.
    // Exercise both roundings of the odd margin and the existing scaled layout.
    for (int which = 0; which < 4; ++which) {
        const uint32_t pw = rs::kW, ph = rs::kH + 7;
        const auto& source = which == 0 ? shows_input : shows_output;
        std::vector<uint8_t> pixels(size_t(pw) * ph, 0);
        for (uint32_t y = 0; y < ph; ++y) {
            for (uint32_t x = 0; x < pw; ++x) {
                if (which == 2) {
                    pixels[size_t(y) * pw + x] = uint8_t((rs::Grain(x, y, 37) + .5f) * 255.f);
                } else if (which == 3) {
                    float sy = std::clamp((float(y) + .5f) * rs::kH / ph - .5f, 0.f, float(rs::kH - 1));
                    uint32_t lo = uint32_t(sy), hi = (std::min)(lo + 1, rs::kH - 1);
                    pixels[size_t(y) * pw + x] = uint8_t(std::lround(source[size_t(lo) * pw + x] * (1 - (sy - lo)) +
                                                                      source[size_t(hi) * pw + x] * (sy - lo)));
                } else {
                    const uint32_t top = which == 0 ? 3 : 4;
                    if (y >= top && y - top < rs::kH) pixels[size_t(y) * pw + x] = source[size_t(y - top) * pw + x];
                }
            }
        }
        auto composition = ScreenTexture(d, l, pixels, keep, pw, ph);
        REQUIRE(composition);
        Run(d, l);
        keep.clear();
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, {t_prev.Get(), t_input.Get(), t_output.Get(), nullptr, composition.Get(), which == 2}, got));
        const auto vote = na::referee::shadow::VoteScreen(na::referee::Score({got.data.data(), got.screen, got.tiles, false, 0}));
        std::printf("padded screen case %d: layout %u, vote %d\n", which, got.centered_layout, int(vote));
        CHECK(vote == (which == 0 ? ScreenVote::Input : which == 2 ? ScreenVote::Unknown : ScreenVote::Output));
        CHECK(got.centered_layout == (which < 2 ? uint32_t(which + 1) : 0u));
    }
    // Isolate the first changed stage with the same two luma levels, then
    // deny a Present receipt to make sure a copied picture cannot vote.
    auto bright = output;
    for (uint32_t y = bright.h / 2; y < bright.h * 5 / 8; ++y)
        for (uint32_t x = bright.w / 2; x < bright.w * 5 / 8; ++x)
            bright.v[size_t(y) * bright.w + x] *= 1.8f;
    auto lit_in = Picture(d, l, bright, NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
    auto lit_out = Picture(d, l, bright, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, keep);
    auto lit_display = ScreenTexture(d, l, rs::ScreenBytes(bright), keep);
    Run(d, l);
    keep.clear();
    REQUIRE(lit_in && lit_out && lit_display);
    const auto unlit_in = Picture(d, l, output, NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
    const auto padded = [&](const na::referee::Image& image) {
        const auto pixels = rs::ScreenBytes(image);
        std::vector<uint8_t> aligned(size_t(image.w) * (image.h + 7));
        std::copy(pixels.begin(), pixels.end(), aligned.begin() + 3 * image.w);
        return ScreenTexture(d, l, aligned, keep, image.w, image.h + 7);
    };
    const auto unlit_padded = padded(output), lit_padded = padded(bright);
    Run(d, l);
    keep.clear();
    REQUIRE(unlit_in && unlit_padded && lit_padded);
    using na::referee::FlickerStage;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        const unsigned first = (std::min)(scenario, 2u);
        const bool has_padding = scenario == 3;
        rg::NotifyEvaluationReset();
        for (unsigned reverse = 0; reverse < 2; ++reverse) {
            const bool up = reverse == 0;
            auto* old_in = first == 0 && !up ? lit_in.Get() : unlit_in.Get();
            auto* new_in = first == 0 && up ? lit_in.Get() : unlit_in.Get();
            auto* old_output = first <= 1 && !up ? lit_out.Get() : t_output.Get();
            auto* new_output = first <= 1 && up ? lit_out.Get() : t_output.Get();
            auto* lit_screen = has_padding ? lit_padded.Get() : lit_display.Get();
            auto* unlit_screen = has_padding ? unlit_padded.Get() : t_screen.Get();
            TracePair trace{old_output, up ? unlit_screen : lit_screen};
            Sleep(150);
            REQUIRE(TakeSample(d, l, ordinal, {old_in, new_in, new_output, nullptr,
                                              up ? lit_screen : unlit_screen}, got,
                              false, nullptr, nullptr, false, false, false, false, 0, false, &trace));
            const auto stage = static_cast<FlickerStage>(got.flicker_stage);
            std::printf("trace expected=%u reversal=%u layout=%u actual=%s\n", first + 1, reverse, got.centered_layout, na::referee::StageName(stage));
            CHECK(got.flicker_stage == (reverse ? first + 1 : 0));
        }
    }
    TracePair rejected{t_output.Get(), t_screen.Get(), false, 62};
    Sleep(150);
    REQUIRE(TakeSample(d, l, ordinal, {unlit_in.Get(), unlit_in.Get(), t_output.Get(), nullptr, lit_display.Get()},
                       got, false, nullptr, nullptr, false, false, false, false, 0, false, &rejected));
    CHECK(rg::GetStats().trace_pairs == 8 && rg::GetStats().trace_reversals == 4 && rg::GetStats().rejected_presents == 1);
    {
        using namespace na::referee;
        TrialSample measured;
        CHECK(!rg::TakeTrialSample(rejected.ticket, measured));
        // The left and right halves of this reflection change in opposite
        // directions. A single luma mean would miss it entirely.
        constexpr uint32_t neighbor = 37;
        auto shifting_reflection = output;
        auto two_reflections = bright;
        auto stronger_neighbor = output;
        for (unsigned y = output.h / 2; y < output.h * 5 / 8; ++y)
            for (unsigned x = output.w * 5 / 8; x < output.w * 6 / 8; ++x) {
                const bool left = x < output.w * 11 / 16;
                const size_t at = size_t(y) * output.w + x;
                shifting_reflection.v[at] *= left ? 1.12f : 1.f / 1.12f;
                two_reflections.v[at] *= left ? 1.39f : 1.f / 1.39f;
                stronger_neighbor.v[at] = two_reflections.v[at];
            }
        auto shifting_screen = ScreenTexture(d, l, rs::ScreenBytes(shifting_reflection), keep);
        auto two_output = Picture(d, l, two_reflections, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, keep);
        auto two_screen = ScreenTexture(d, l, rs::ScreenBytes(two_reflections), keep);
        auto introduced_screen = ScreenTexture(d, l, rs::ScreenBytes(stronger_neighbor), keep);
        Run(d, l);
        keep.clear();
        REQUIRE(shifting_screen && two_output && two_screen && introduced_screen);
        TracePair subregion{t_output.Get(), t_screen.Get(), true, 9539, 70};
        subregion.recovery_region = neighbor;
        subregion.recovery_floor = .025f;
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, {unlit_in.Get(), unlit_in.Get(), t_output.Get(), nullptr, shifting_screen.Get()},
                           got, false, nullptr, nullptr, false, false, false, false, 0, false, &subregion));
        REQUIRE(rg::TakeTrialSample(subregion.ticket, measured));
        std::printf("residual split reflection: region=%u stage=%s stops=%.4f\n", measured.origin.region,
                    StageName(measured.origin.stage), measured.origin.stops[2]);
        CHECK(measured.comparable && measured.origin.stage == FlickerStage::Composition &&
              measured.origin.region == neighbor && std::fabs(measured.origin.stops[2]) > .045f);
        // Even with an independently proved reader, identical input and
        // output pixels give no evidence about which reached the screen.
        TracePair identical{t_output.Get(), t_screen.Get(), true, 2051, 70};
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, {unlit_in.Get(), unlit_in.Get(), t_output.Get(), nullptr, t_screen.Get()},
                           got, false, nullptr, nullptr, false, false, false, false, 9991, false, &identical));
        REQUIRE(rg::TakeTrialSample(identical.ticket, measured));
        CHECK(!measured.delivered && !measured.bypassed);
        TracePair unacknowledged{t_output.Get(), t_screen.Get(), true, 2059, 70, false};
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_input.Get(), t_output.Get(), nullptr, t_screen.Get()},
                           got, false, nullptr, nullptr, false, false, false, false, 9992, false, &unacknowledged));
        CHECK(!rg::TakeTrialSample(unacknowledged.ticket, measured));
        TracePair resized{t_output.Get(), t_screen.Get(), true, 2067, 70};
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_input.Get(), t_output.Get(), nullptr, t_screen.Get()},
                           got, false, nullptr, nullptr, false, false, false, false, 9993, false, &resized));
        rg::InvalidatePresentation(&d);
        CHECK(!rg::TakeTrialSample(resized.ticket, measured));
        // Seed the second reader at its usual frame parity. A new pair can
        // start at the first reader and still needs the second reader's image.
        const uint64_t first_reader = 10001;
        const Images rotating{t_input.Get(), t_input.Get(), t_output.Get(), nullptr, unlit_padded.Get()};
        TracePair established{t_output.Get(), unlit_padded.Get(), true, 7363, 70};
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, rotating, got, false, nullptr, nullptr, false, false, false, false,
                           first_reader + 1, false, &established));
        REQUIRE(rg::TakeTrialSample(established.ticket, measured));
        CHECK(measured.comparable && measured.delivered);
        TracePair handoff{t_output.Get(), unlit_padded.Get(), true, 7371, 70};
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, rotating, got, false, nullptr, nullptr, false, false, false, false,
                           first_reader, true, &handoff));
        REQUIRE(rg::TakeTrialSample(handoff.ticket, measured));
        CHECK(measured.ordinal == ordinal && measured.comparable && measured.delivered);
        // A stable left-side bypass has no flicker amplitude. Sample the real GPU mailbox
        // against the same input/output textures and a tone-mapped, split final screen.
        auto split_bytes = shows_output;
        for (unsigned y = 0; y < rs::kH; ++y)
            for (unsigned x = 0; x < rs::kW / 2; ++x)
                split_bytes[y * rs::kW + x] = shows_input[y * rs::kW + x];
        const auto small_overlay = [&](unsigned mode) {
            auto bytes = shows_output;
            for (int y = TileTop(0, rs::kH); y < TileTop(0, rs::kH) + int(kTile); ++y)
                for (int x = TileLeft(0, rs::kW); x < TileLeft(0, rs::kW) + int(kTile); ++x) {
                    const auto i = size_t(y) * rs::kW + x;
                    bytes[i] = mode == 0 ? shows_input[i] : mode == 1 ? uint8_t((3 * shows_output[i] + 80) / 4) : 113;
                }
            return ScreenTexture(d, l, bytes, keep);
        };
        auto raw_patch = small_overlay(0), translucent = small_overlay(1), covered = small_overlay(2);
        auto split = ScreenTexture(d, l, split_bytes, keep);
        REQUIRE(split);
        Run(d, l);
        keep.clear();
        for (unsigned mode = 0; mode < 5; ++mode) {
            ID3D12Resource* displayed[] = {raw_patch.Get(), translucent.Get(), covered.Get(), t_screen_input.Get(), t_screen.Get()};
            TracePair trace{t_output.Get(), displayed[mode], true, 6995, 72};
            Sleep(150);
            REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_input.Get(), t_output.Get(), nullptr, displayed[mode]}, got,
                               false, nullptr, nullptr, false, false, false, false, 0, false, &trace));
            REQUIRE(rg::TakeTrialSample(trace.ticket, measured));
            std::printf("regional control %u: input=%02x output=%02x confirmed=%02x\n", mode,
                        unsigned(measured.tiles.input), unsigned(measured.tiles.output), unsigned(measured.tiles.confirmed_input));
            CHECK(measured.tiles.Mixed() == (mode == 0));
            if (mode == 0) CHECK(measured.tiles.confirmed_input == 1);
            if (mode == 1) CHECK(measured.delivered && (measured.tiles.output & 1));
            if (mode == 2) CHECK(measured.delivered && !((measured.tiles.input | measured.tiles.output) & 1));
            if (mode >= 3) CHECK(mode == 3 ? measured.bypassed : measured.delivered);
        }
        PlacementTrial regional;
        const PlacementTrial::Scope scope{1, 2, 3, 1};
        TracePair nomination{t_output.Get(), split.Get(), true, 714, 711};
        nomination.alternate = PlacementPath::Between;
        Sleep(150);
        REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_input.Get(), t_output.Get(), nullptr, split.Get()}, got,
                           false, nullptr, nullptr, false, false, false, false, 0, false, &nomination));
        REQUIRE(rg::TakeTrialSample(nomination.ticket, measured));
        REQUIRE(regional.Begin(scope, measured, ordinal, GetTickCount64()));
        for (unsigned arm = 0; arm < 3; ++arm) {
            ordinal += RouteTrial::warmup_frames + 1;
            const auto ticket = regional.Ticket(ordinal);
            REQUIRE(ticket);
            for (unsigned i = 0; i < RouteTrial::samples; ++i) {
                auto* displayed = arm == 1 ? t_screen.Get() : split.Get();
                TracePair trace{t_output.Get(), displayed, true, ticket, 711};
                trace.path = arm == 1 ? PlacementPath::Between : PlacementPath::Inline;
                trace.alternate = arm == 1 ? PlacementPath::None : PlacementPath::Between;
                Sleep(150);
                REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_input.Get(), t_output.Get(), nullptr, displayed}, got,
                                   false, nullptr, nullptr, false, false, false, false, 0, false, &trace));
                REQUIRE(rg::TakeTrialSample(ticket, measured));
                if (!i) std::printf("regional arm %u: input=%02x output=%02x comparable=%u\n",
                                    arm, unsigned(measured.tiles.input), unsigned(measured.tiles.output), unsigned(measured.comparable));
                CHECK(arm == 1 ? measured.delivered : measured.tiles.Mixed());
                regional.Tick(GetTickCount64(), true);
                measured.presented_us = 0; // picture case: fixture cadence is not a game's pace
                regional.Add(scope, ticket, measured, ordinal, GetTickCount64());
            }
        }
        CHECK(regional.Comparison().Regional() && regional.Comparison().Current() == RouteTrial::Phase::Kept);
        // The test reflector alternates about one stop. The alternative either
        // removes it, delivers the same artifact, puts the input on screen, or
        // removes the strongest flash while leaving an existing or new reflection.
        for (unsigned mode = 0; mode < 5; ++mode) {
            PlacementTrial placement;
            using Phase = RouteTrial::Phase;
            const auto& trial = placement.Comparison();
            const bool wide = mode == 3;
            const bool introduces_neighbor = mode == 4;
            auto* ordinary_output = wide ? two_output.Get() : lit_out.Get();
            auto* ordinary_screen = wide ? two_screen.Get() : lit_display.Get();
            TracePair reflection{t_output.Get(), t_screen.Get(), true, 952 + mode * 8u, 70};
            Sleep(150);
            REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_input.Get(), ordinary_output, nullptr, ordinary_screen},
                               got, false, nullptr, nullptr, false, false, false, false, 0, false, &reflection));
            REQUIRE(rg::TakeTrialSample(reflection.ticket, measured));
            CHECK(measured.delivered && !measured.tiles.Mixed());
            REQUIRE(placement.Begin(scope, measured, ordinal, GetTickCount64()));
            for (unsigned arm = 0; arm < 3 && trial.Active(); ++arm) {
                ordinal += RouteTrial::warmup_frames + 1;
                const auto ticket = placement.Ticket(ordinal);
                REQUIRE(ticket);
                const bool quiet = arm == 1 && mode == 0;
                const bool bypass = arm == 1 && mode == 2;
                for (unsigned i = 0; i < RouteTrial::samples && placement.Ticket(ordinal) == ticket; ++i) {
                    const bool reverse = i % 2 != 0;
                    auto* residual_screen = wide ? shifting_screen.Get() :
                        introduces_neighbor ? introduced_screen.Get() : nullptr;
                    const bool residual = arm == 1 && residual_screen;
                    const bool larger = !quiet && !residual;
                    auto* active_screen = residual ? residual_screen : larger ? ordinary_screen : t_screen.Get();
                    TracePair trace{larger && reverse ? ordinary_output : t_output.Get(),
                                    reverse ? active_screen : t_screen.Get(), true, ticket, 70};
                    Sleep(150);
                    REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_input.Get(),
                                      larger && !reverse ? ordinary_output : t_output.Get(),
                                      nullptr, bypass ? t_screen_input.Get() :
                                      !reverse ? active_screen : t_screen.Get()}, got,
                                      false, nullptr, nullptr, false, false, false, false, 0, false, &trace));
                    CHECK(!rg::TakeTrialSample(ticket ^ 8u, measured));
                    REQUIRE(rg::TakeTrialSample(ticket, measured));
                    CHECK(measured.ordinal == ordinal && measured.configuration == 70);
                    CHECK(measured.delivered != bypass && measured.bypassed == bypass);
                    CHECK(!rg::TakeTrialSample(ticket, measured));
                    // These cases judge the picture only. The fixture's cadence is its own
                    // sleeps on a shared runner, not a game's pace (covered in referee_test.cpp).
                    measured.presented_us = 0;
                    placement.Add(scope, ticket, measured, ordinal, GetTickCount64());
                }
            }
            std::printf("screen trial case %u: %s stops %.3f/%.3f/%.3f (%s)\n", mode, trial.Name(),
                        trial.Baseline(), trial.Candidate(), trial.Confirmation(), trial.Reason());
            CHECK(trial.Current() == (mode == 0 || wide ? Phase::Kept : Phase::Rejected));
            if (wide) {
                std::printf("neighbor trial after primary %.1f%%: recovery=%u region=%u original=%.4f\n",
                            100.f * trial.VerifiedReduction(), unsigned(placement.AllowsStageRecovery()),
                            placement.RecoveryRegion(), placement.RecoveryBaseline());
                CHECK(trial.VerifiedReduction() >= .90f && placement.AllowsStageRecovery() &&
                      placement.RecoveryRegion() == neighbor &&
                      placement.RecoveryBaseline() >= RouteTrial::minimum_flicker &&
                      placement.RecoveryBaseline() < trial.Baseline());
            }
            if (introduces_neighbor) {
                CHECK(trial.Candidate() < trial.Baseline() * .70f && trial.RuledOut() &&
                      std::strstr(trial.Reason(), "worsened recurring flicker"));
            }
            if (mode == 0) {
                ordinal += RouteTrial::warmup_frames + 1;
                const auto watch = placement.Ticket(ordinal);
                REQUIRE(watch);
                for (unsigned i = 0; i < 4; ++i) {
                    const bool reverse = i % 2 != 0;
                    TracePair trace{reverse ? lit_out.Get() : t_output.Get(),
                                    reverse ? lit_display.Get() : t_screen.Get(), true, watch, 70};
                    Sleep(150);
                    REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_input.Get(), reverse ? t_output.Get() : lit_out.Get(),
                                       nullptr, reverse ? t_screen.Get() : lit_display.Get()}, got,
                                       false, nullptr, nullptr, false, false, false, false, 0, false, &trace));
                    REQUIRE(rg::TakeTrialSample(watch, measured));
                    measured.presented_us = 0; // picture case: fixture cadence is not a game's pace
                    placement.Add(scope, watch, measured, ordinal, GetTickCount64());
                }
                CHECK(trial.Current() == Phase::Rejected && !trial.UsesCandidate());
            }
        }
    }
    {
        // The fixture controls a pre-DLAA lighting fault. Reducing bloom leaves
        // it visible, and taking AO from the current frame removes it. The
        // selector receives only the GPU's three image stages and receipts.
        using namespace na::referee;
        using Tool = RecoveryTool;
        auto bright_input = input;
        for (unsigned y = input.h / 2; y < input.h * 5 / 8; ++y)
            for (unsigned x = input.w / 2; x < input.w * 5 / 8; ++x)
                bright_input.v[size_t(y) * input.w + x] *= 1.8f;
        auto input_up = Picture(d, l, bright_input, NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
        REQUIRE(input_up);
        Run(d, l);
        keep.clear();
        StageRecovery recovery;
        recovery.SetScene({1, 2, rs::kW, rs::kH});
        bool saw_bloom = false, saw_ambient = false;
        unsigned direction = 0;
        constexpr uint8_t available = Bit(Tool::Bloom) | Bit(Tool::AmbientHistory) | Bit(Tool::Sharpen);
        for (unsigned i = 0; i < 42 && recovery.Comparison().Current() != RouteTrial::Phase::Kept; ++i) {
            ordinal += RouteTrial::warmup_frames + 1;
            const auto ticket = recovery.Ticket(ordinal);
            REQUIRE(ticket);
            const auto active = recovery.ToolInUse();
            saw_bloom |= active == Tool::Bloom;
            saw_ambient |= active == Tool::AmbientHistory;
            const bool quiet = active == Tool::AmbientHistory;
            const bool reverse = direction++ % 2 != 0;
            const bool from_lit = !quiet && reverse, to_lit = !quiet && !reverse;
            TracePair trace{from_lit ? lit_out.Get() : t_output.Get(), from_lit ? lit_display.Get() : t_screen.Get(),
                            true, ticket, 981, true, available, Bit(active)};
            if (i == 0) trace.main_tools = Bit(Tool::Sharpen); // one submission lacked both input controls
            Sleep(150);
            REQUIRE(TakeSample(d, l, ordinal, {from_lit ? input_up.Get() : t_input.Get(), to_lit ? input_up.Get() : t_input.Get(),
                              to_lit ? lit_out.Get() : t_output.Get(), nullptr, to_lit ? lit_display.Get() : t_screen.Get()},
                              got, false, nullptr, nullptr, false, false, false, false, 0, false, &trace));
            TrialSample sample;
            REQUIRE(rg::TakeTrialSample(ticket, sample));
            CHECK(sample.delivered && sample.comparable && sample.tools == (available & trace.main_tools));
            if (!quiet) CHECK(sample.origin.stage == FlickerStage::Input && sample.origin.region == 36);
            recovery.Tick(GetTickCount64());
            sample.presented_us = 0; // picture case: fixture cadence is not a game's pace
            recovery.Add(ticket, sample, ordinal + 1, GetTickCount64());
        }
        const auto& result = recovery.Comparison();
        std::printf("staged recovery: failed bloom=%d excluded=%u tried ambient=%d phase=%s stops %.3f/%.3f/%.3f\n",
                    saw_bloom, recovery.Excluded(), saw_ambient, result.Name(), result.Baseline(), result.Candidate(), result.Confirmation());
        CHECK(saw_bloom && saw_ambient && recovery.Excluded() == Bit(Tool::Bloom));
        CHECK(result.Current() == RouteTrial::Phase::Kept && recovery.ToolInUse() == Tool::AmbientHistory);
    }
    Sleep(150);
    REQUIRE(rg::Wanted());
    {
        auto relit = input;
        for (unsigned y = 0; y < relit.h; ++y)
            for (unsigned x = 0; x < relit.w; ++x)
                relit.v[size_t(y) * relit.w + x] *= ((x / 37 + y / 29) % 2) ? 2.f : .5f;
        std::vector<float> still(motion.size(), 0.f);
        auto t_lit = Picture(d, l, relit, NPSR, D3D12_RESOURCE_FLAG_NONE, keep);
        auto t_still = MotionTexture(d, l, still, keep);
        REQUIRE(t_lit && t_still);
        Run(d, l);
        keep.clear();
        REQUIRE(TakeSample(d, l, ordinal, {t_input.Get(), t_lit.Get(), t_output.Get(), t_still.Get(), t_screen.Get()}, got));
        auto measured = na::referee::Score({got.data.data(), got.screen, got.tiles, got.pair, got.jitter_change});
        const auto ref = rs::Expected(input, relit, output, rs::ScreenImage(shows_output), still);
        const auto expected = na::referee::Score({ref.data(), true, true, true, got.jitter_change});
        std::printf("patchy light: compatible=%u unresolved=%u pattern=%u of %u still points\n",
                    measured.light_compatible, measured.ambiguous_still, measured.changed_still, measured.still_points);
        CHECK(measured.light_compatible == expected.light_compatible && measured.changed_still == expected.changed_still);
        // A patch crossing two differently lit materials genuinely changes shape. Compare
        // against the known light field on the interiors, after the normal jitter gate.
        unsigned interiors = 0;
        const auto* light_samples = got.data.data() + na::referee::BlockOffset(na::referee::kPoints);
        for (unsigned j = 0; j < na::referee::kGridH; ++j) for (unsigned i = 0; i < na::referee::kGridW; ++i) {
            const int x = int(na::referee::PointX(i, relit.w)), y = int(na::referee::PointY(j, relit.h));
            if ((x - 1) / 37 != (x + 1) / 37 || (y - 1) / 29 != (y + 1) / 29) continue;
            const float* p = light_samples + (j * na::referee::kGridW + i) * na::referee::kPointValues;
            if (p[na::referee::kContrast] < .03f || p[na::referee::kStill] <= .08f ||
                p[measured.placement] <= .8f * p[na::referee::kStill]) continue;
            const double gain = ((x / 37 + y / 29) % 2) ? 2. : .5;
            double deltas[9]{}, mean = 0;
            unsigned n = 0;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const double v = (std::max)(0., double(input.At(x + dx, y + dy)));
                const double floor = na::referee::kLumaFloor;
                if ((std::max)(v, v * gain) + floor <= 1. / 1024) continue;
                deltas[n] = std::log2((v * gain + floor) / (v + floor));
                mean += deltas[n++];
            }
            if (n < 5) { CHECK(p[na::referee::kStillStructure] < 0); continue; }
            mean /= n;
            double residual = 0;
            for (unsigned k = 0; k < n; ++k) residual += std::fabs(deltas[k] - mean);
            residual /= n;
            CHECK(std::fabs(p[na::referee::kStillStructure] - residual) < .002);
            if (residual <= (std::min)(.04, .25 * p[na::referee::kStill])) ++interiors;
        }
        CHECK(interiors > 3000 && measured.light_compatible >= interiors);
    }
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
