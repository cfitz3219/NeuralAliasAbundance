#include "host_dx12/capture/referee_route_a.hpp"

#include "host_dx12/diag/event_log.hpp"

#include <cmath>
#include <mutex>

namespace na::referee::route_a {
namespace {

struct State {
    std::mutex m;
    uint64_t frame = 1;              // ordinal the next present will carry
    bool unowned = false;            // the last present had no other owner
    bool evaluated = false;          // the game ran DLSS for this frame
    bool recorded = false;           // a capture was recorded for this frame
    bool submitted = false;          // ...and the list that holds it was submitted
    ID3D12CommandList* list = nullptr; // holds a capture that has not been submitted yet
    uint32_t token = 0;
    uint64_t recorded_frame = 0;
    uint64_t feature = 0, scene = 0; // scene changes with the feature or its render size
    uint32_t width = 0, height = 0;
    bool announced = false, refused_announced = false;
    Stats stats;
};
State& S()
{
    static State& state = *new State; // process lifetime: nothing to destroy at exit
    return state;
}

} // namespace

void OnEvaluated(ID3D12GraphicsCommandList* list, const Evaluated& e)
{
    if (!list || !e.color || !e.output) return;
    auto& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    ++s.stats.evaluations;
    s.evaluated = true;
    // One capture per frame, none while an earlier one still waits for its list,
    // and only while this route owns the receipt.
    if (!s.unowned || s.recorded || s.list || !e.feature || !gpu::Wanted()) return;
    if (!std::isfinite(e.jitter_x) || !std::isfinite(e.jitter_y) || std::fabs(e.jitter_x) > 1.f || std::fabs(e.jitter_y) > 1.f ||
        !std::isfinite(e.mv_scale_x) || !std::isfinite(e.mv_scale_y)) return;
    if (e.feature != s.feature || e.render_w != s.width || e.render_h != s.height) {
        s.feature = e.feature;
        s.width = e.render_w;
        s.height = e.render_h;
        ++s.scene;
    }
    gpu::Evaluation evaluation;
    // The states DLSS itself requires of the game for these resources.
    evaluation.color = e.color;
    evaluation.motion = e.motion;
    evaluation.output = e.output;
    evaluation.present_ordinal = s.frame;
    evaluation.render_w = e.render_w; evaluation.render_h = e.render_h;
    evaluation.output_w = e.output_w; evaluation.output_h = e.output_h;
    evaluation.jitter_x = e.jitter_x; evaluation.jitter_y = e.jitter_y;
    evaluation.mv_scale_x = e.mv_scale_x; evaluation.mv_scale_y = e.mv_scale_y;
    evaluation.reset = e.reset;
    evaluation.mv_jittered = e.mv_jittered;
    evaluation.mv_display_res = !e.mv_low_res;
    evaluation.context.placement = {shadow::Path::Game, 0};
    evaluation.context.color = e.feature;
    evaluation.context.depth = 1; // not read on this route; the scene is identified by the game's DLSS feature
    evaluation.context.generation = s.scene;
    evaluation.context.width = e.render_w;
    evaluation.context.height = e.render_h;
    s.token = gpu::Record(list, evaluation);
    if (!s.token) return;
    s.list = list;
    s.recorded = true;
    s.recorded_frame = s.frame;
    ++s.stats.recorded;
}

void OnExecuted(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists)
{
    if (!queue || !lists) return;
    auto& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    if (!s.token || !s.list) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (lists[i] != s.list) continue;
        s.list = nullptr;
        if (s.recorded_frame == s.frame) {
            gpu::Submitted(s.token, queue);
            s.submitted = true;
            ++s.stats.submitted;
        } else {
            // A present came between recording and submitting: the capture cannot be tied
            // to one present. Its copies have run now, so it is simply dropped.
            gpu::Discard(s.token, false);
            if (++s.stats.other_frame == 60 && !s.stats.submitted && !s.refused_announced) {
                s.refused_announced = true;
                eventlog::Emit("referee_route_a", "action=no_witness frame=%llu why=%s", (unsigned long long)s.frame,
                               "the game records its DLSS list in one frame and submits it in another: no capture can be tied to a present, so nothing is judged");
            }
        }
        s.token = 0;
        return;
    }
}

void OnPresent(gpu::Presentation& receipt)
{
    auto& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    const bool unowned = receipt.swapchain && !receipt.ordinal;
    if (unowned && s.unowned && s.evaluated) {
        receipt.ordinal = s.frame;
        receipt.eligible = true;
        receipt.submitted = s.submitted;
        ++s.stats.presents;
        if (!s.announced) {
            s.announced = true;
            eventlog::Emit("referee_route_a", "action=witness frame=%llu why=%s", (unsigned long long)s.frame,
                           "the game runs its own DLSS and no added route owns the picture: the Referee watches it and changes nothing");
        }
    }
    s.stats.active = unowned && s.evaluated;
    s.unowned = unowned;
    s.evaluated = s.recorded = s.submitted = false;
    ++s.frame;
}

Stats GetStats()
{
    auto& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    return s.stats;
}

} // namespace na::referee::route_a
