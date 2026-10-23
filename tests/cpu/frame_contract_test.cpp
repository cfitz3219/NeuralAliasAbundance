// reconstructed 2026-10 — frame metadata, feature gates and continuity.
#include "test_support.hpp"
#include "core/frame_contract.hpp"
#include <limits>
using namespace abundance;
namespace {
Frame frame(uint64_t id = 1) {
    Frame f{}; f.id = id; f.generation = 7; f.route = Route::adapter;
    f.color = {10, id, 7, {640, 360}}; f.depth = {11, id, 7, {640, 360}};
    f.motion = {12, id, 7, {640, 360}}; f.output = {1280, 720};
    f.color_space = ColorSpace::linear_hdr; f.motion_space = MotionSpace::pixels_current_to_previous;
    f.jitter_known = f.exposure_policy_known = f.clean_scene = f.motion_excludes_jitter = true;
    return f;
}
const BackendSupport backend{true, true, true};
void rejected(const std::function<void(Frame&)>& change) {
    auto f = frame(); change(f); CHECK(!evaluate(f, Feature::reconstruction, backend).ready);
}
}
int main() { return test::run({
    {"provider evidence and priority", [] {
        ProviderEvidence native{Route::native_upscaler, false, true, false};
        ProviderEvidence adapter{Route::adapter, false, true, true};
        CHECK(choose_route(native, adapter) == Route::adapter);
        native.evaluation_observed = true; CHECK(choose_route(native, adapter) == Route::native_upscaler);
        native.inputs_validated = false; adapter.inputs_validated = false;
        CHECK(choose_route(native, adapter) == Route::none);
        adapter = {Route::optical_flow_experimental, false, true, true};
        CHECK(choose_route(native, adapter) == Route::optical_flow_experimental);
    }},
    {"valid inputs still require backend qualification", [] {
        CHECK(evaluate(frame(), Feature::reconstruction, backend).ready);
        CHECK(!evaluate(frame(), Feature::reconstruction, {}).ready);
        CHECK(!evaluate(frame(), Feature::neural_rendering, {true, true, false}).ready);
        CHECK(evaluate(frame(), Feature::neural_rendering, backend).ready);
        CHECK(!evaluate(frame(), Feature::frame_generation, backend).ready);
    }},
    {"missing, stale and unknown inputs rejected", [] {
        rejected([](Frame& f) { f.id = 0; }); rejected([](Frame& f) { f.generation = 0; });
        rejected([](Frame& f) { f.route = Route::none; }); rejected([](Frame& f) { f.color.identity = 0; });
        rejected([](Frame& f) { --f.motion.frame; }); rejected([](Frame& f) { ++f.depth.generation; });
        rejected([](Frame& f) { f.clean_scene = false; }); rejected([](Frame& f) { f.jitter_known = false; });
        rejected([](Frame& f) { f.jitter_x = std::numeric_limits<float>::quiet_NaN(); });
        rejected([](Frame& f) { f.jitter_y = std::numeric_limits<float>::infinity(); });
        rejected([](Frame& f) { f.exposure_policy_known = false; });
        rejected([](Frame& f) { f.motion_excludes_jitter = false; });
        rejected([](Frame& f) { f.motion_space = MotionSpace::unknown; });
        rejected([](Frame& f) { f.color_space = ColorSpace::unknown; });
    }},
    {"extent and aspect constraints", [] {
        rejected([](Frame& f) { f.output = {0, 720}; });
        rejected([](Frame& f) { f.output = {320, 180}; });
        rejected([](Frame& f) { f.output = {1280, 800}; });
        rejected([](Frame& f) { ++f.depth.extent.width; });
        rejected([](Frame& f) { ++f.motion.extent.height; });
        auto f = frame(); f.motion.extent = f.output;
        CHECK(evaluate(f, Feature::reconstruction, backend).ready);
    }},
    {"continuity, gaps, invalid frames and cuts", [] {
        Continuity c; CHECK(c.observe(frame(1), Feature::reconstruction, backend).reset);
        CHECK(!c.observe(frame(2), Feature::reconstruction, backend).reset);
        CHECK(c.observe(frame(4), Feature::reconstruction, backend).reset);
        CHECK(c.observe(frame(4), Feature::reconstruction, backend).reset);
        auto bad = frame(5); bad.clean_scene = false;
        CHECK(!c.observe(bad, Feature::reconstruction, backend).ready);
        CHECK(c.observe(frame(6), Feature::reconstruction, backend).reset);
        auto cut = frame(7); cut.camera_cut = true;
        CHECK(c.observe(cut, Feature::reconstruction, backend).reset);
        auto switched = frame(8); switched.route = Route::native_upscaler;
        CHECK(c.observe(switched, Feature::reconstruction, backend).reset);
        c.invalidate(); CHECK(c.observe(frame(9), Feature::reconstruction, backend).reset);
    }},
}); }
