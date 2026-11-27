#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>

namespace neural_alias {
// Independent sampling math, applied by the verified Alien camera adapter.
// The adapter owns sequence advancement/reset and must select the main scene camera.
struct JitterSample { double x_pixels, y_pixels; };

// Applies equally to a scene prepass and its color draws. Once an adapter
// proves the scene has resolved, the game's subsequent composition stays
// on its original raster grid.
constexpr bool SceneDrawTakesJitter(bool active, uint64_t bound_depth, uint64_t scene_depth,
                                    bool tests_depth, bool after_resolve)
{
    return active && scene_depth && bound_depth == scene_depth && tests_depth && !after_resolve;
}
// VARIANT: jitter/shader-depth-readers
// A scene can write a depth-like layer as colour with its depth-tested draws and compare against
// that layer later in its mesh shaders, with the hardware depth test off (Three Kingdoms terrain,
// 2026-11-21: road and ground patches painted black on the samples that moved the layer furthest).
// Those readers are scene geometry on the layer's raster grid: they take the frame's full sample,
// like the draws that wrote the layer. Each field is a fact the host observed for this one draw;
// nothing here names a game, a format or a resource.
struct ShaderDepthReader {
    bool active = false;            // a sample is in force for this draw's frame
    bool tests_depth = false;       // hardware depth test on: the ordinary rule already decides
    bool scene_depth_bound = false; // the scene's depth buffer is the bound depth
    bool layer_took_sample = false; // a full-size depth-like colour layer was written by sampled, depth-tested scene draws
    bool reads_layer = false;       // this draw's pixel shader has that layer bound
    bool writes_layer = false;      // ... and must not be writing it
    bool mesh = false;              // more vertices than a full-screen pass
    bool several_targets = false;   // a material pass, not a composite into one picture
    bool scene_viewport = false;    // one viewport covering the whole scene target
};
constexpr bool ShaderDepthReaderTakesJitter(const ShaderDepthReader& r)
{
    return r.active && !r.tests_depth && r.scene_depth_bound && r.layer_took_sample && r.reads_layer &&
           !r.writes_layer && r.mesh && r.several_targets && r.scene_viewport;
}
// Why a candidate reader (depth test off, scene depth bound, a mesh) kept the game's own viewport.
enum class ShaderDepthRefusal : uint8_t { None, NoLayer, NotRead, WritesLayer, OneTarget, Viewport, Count };
constexpr ShaderDepthRefusal ShaderDepthReaderRefusal(const ShaderDepthReader& r)
{
    if (!r.layer_took_sample) return ShaderDepthRefusal::NoLayer;
    if (!r.several_targets) return ShaderDepthRefusal::OneTarget;
    if (!r.scene_viewport) return ShaderDepthRefusal::Viewport;
    if (r.writes_layer) return ShaderDepthRefusal::WritesLayer;
    if (!r.reads_layer) return ShaderDepthRefusal::NotRead;
    return ShaderDepthRefusal::None;
}
// VARIANT: referee/sample-locked-dropout
// The Referee's reaction to pixels that go exactly black on some samples and come back on others
// (core/referee_dropout.hpp): while it asks, every scene mesh the fixed rules above left on the
// game's own grid takes the frame's full sample too, whatever it reads and wherever it draws. The
// Referee keeps the move only if the dropouts stop on the samples that produced them, and takes it
// back otherwise. A full-screen pass is never moved, and nothing moves once the scene is handed over
// (the hosts clear the sample or prove the resolve there, as for every other scene draw).
struct StillMesh {
    bool asked = false;             // the Referee asks for the move on this scene
    bool active = false;            // a sample is in force for this draw's frame
    bool tests_depth = false;       // hardware depth test on: the ordinary rule already decides
    bool scene_depth_bound = false; // the scene's depth buffer is the bound depth
    bool mesh = false;              // more vertices than a full-screen pass
    bool scene_viewport = false;    // one viewport covering the whole scene target
};
constexpr bool StillMeshTakesJitter(const StillMesh& m)
{
    return m.asked && m.active && !m.tests_depth && m.scene_depth_bound && m.mesh && m.scene_viewport;
}
// VARIANT: jitter/scene-overlays (default) | jitter/depth-tested-only (the earlier rule)
// A mesh drawn into the scene's own colour picture while the frame's sample is in force, through the
// scene viewport with the scene depth bound, is scene content even with the hardware depth test off:
// map overlays, lines, decals and labels laid over the terrain (Hearts of Iron IV, 2026-11-27: rails
// and border bands drawn this way stayed on the game's grid, wobbled by the jitter under DLAA and
// smeared at Quality and Performance). It takes the frame's full sample like the depth-tested draws.
// Full-screen passes, other targets and anything after the scene is handed over are not scene
// overlays and keep the game's grid. The earlier rule (depth-tested draws only) is chosen again by
// the Referee's dropout watch when pixels drop out on the samples after the move (the hosts log the
// variant in force with the reason).
struct SceneOverlay {
    // VARIANT: jitter/depth-tested-only when `enabled` is false (the Referee's overlay watch chose it)
    bool enabled = true;            // jitter/scene-overlays in force (false: jitter/depth-tested-only)
    bool active = false;            // a sample is in force for this draw's frame
    bool tests_depth = false;       // hardware depth test on: the ordinary rule already decides
    bool scene_depth_bound = false; // the scene's depth buffer is the bound depth
    bool scene_colour = false;      // the scene's own colour picture is a bound target
    bool mesh = false;              // more vertices than a full-screen pass
    bool scene_viewport = false;    // one viewport covering the whole scene target
    bool after_handover = false;    // the scene was already handed to DLSS (or proven resolved)
    // VARIANT: jitter/stationary-source-readers (Astra, 2026-11-26): the mesh's pixel shader reads a depth-like
    // layer written THIS frame on the game's own grid (by draws that did not take the sample), or a proven exact
    // copy of one. Its exact texel lookups only agree with that grid: it stays on it, or it reads the wrong texel
    // (black on every sample in Astra's fixture). A mesh reading a layer written on the sample's grid is the
    // shader-depth rule's; a mesh reading no such layer (the rails) takes the sample.
    bool reads_stationary_layer = false;
};
constexpr bool SceneOverlayTakesJitter(const SceneOverlay& o)
{
    return o.enabled && o.active && !o.tests_depth && o.scene_depth_bound && o.scene_colour && o.mesh && o.scene_viewport &&
           !o.after_handover && !o.reads_stationary_layer;
}
// The overlay owns this session-only transform. NVIDIA's F9/F10 hotkeys
// have no reported state we can reliably return to zero.
struct JitterDiagnostic {
    struct Scale { float x, y; };
    struct Configuration { float x, y; const char* name; };
    inline static constexpr Configuration scales[] = {
        {1, 1, "Normal jitter"},
        {.5f, .5f, "Half both ways"},
        {.5f, -.5f, "Half both ways. Flip up/down."},
        {-.5f, .5f, "Half both ways. Flip left/right."},
        {-.5f, -.5f, "Half both ways. Flip both."},
        {2, 2, "Double both ways"},
        {2, -2, "Double both ways. Flip up/down."},
        {-2, 2, "Double both ways. Flip left/right."},
        {-2, -2, "Double both ways. Flip both."},
        {1, -1, "Flip up/down only"},
        {-1, 1, "Flip left/right only"},
        {-1, -1, "Flip both directions"},
        {.5f, 1, "Half sideways only"}, {1, .5f, "Half up/down only"},
        {-.5f, 1, "Half and flip sideways only"}, {1, -.5f, "Half and flip up/down only"},
        {2, 1, "Double sideways only"}, {1, 2, "Double up/down only"},
        {-2, 1, "Double and flip sideways only"}, {1, -2, "Double and flip up/down only"}
    };
    static constexpr uint32_t count = sizeof(scales) / sizeof(scales[0]);
    uint32_t value = 0; // first count = normal axes, next count = swapped
    constexpr uint32_t Config() const { return value % count; }
    constexpr bool Swapped() const { return value >= count; }
    constexpr uint32_t WithConfig(uint32_t config) const { return (Swapped() ? count : 0) + config % count; }
    constexpr uint32_t ToggleAxes() const { return Config() + (Swapped() ? 0 : count); }
    constexpr Scale Declared(float x, float y) const {
        const auto scale = scales[Config()];
        const float sx = x * scale.x, sy = y * scale.y;
        return Swapped() ? Scale{sy, sx} : Scale{sx, sy};
    }
};
enum class JitterMode : uint8_t { Unset, Unshifted, Shifted };

// A recorded frame can still carry the previous setting. The history changes
// mode only on an evaluated frame whose raster agrees with the new request.
constexpr JitterMode JitterHistoryForFrame(JitterMode previous, bool requested, bool raster_shifted,
                                          JitterDiagnostic diagnostic = {}) {
    if (requested != raster_shifted) return previous;
    // Each declared transform has its own shifted history on this view.
    return raster_shifted ? static_cast<JitterMode>(static_cast<unsigned>(JitterMode::Shifted) + diagnostic.value)
                          : JitterMode::Unshifted;
}
constexpr bool JitterHistoryChanged(JitterMode previous, JitterMode next) {
    return previous != JitterMode::Unset && next != JitterMode::Unset && previous != next;
}
inline double radical_inverse(uint32_t index, uint32_t base) {
    double result=0, weight=1;
    while(index) { weight/=base; result+=double(index%base)*weight; index/=base; }
    return result;
}
inline JitterSample temporal_jitter(uint32_t phase, uint32_t period) {
    if(period<2 || period>4096) throw std::invalid_argument("jitter period must be 2..4096");
    const uint32_t index=phase%period+1;
    return {radical_inverse(index,2)-0.5,radical_inverse(index,3)-0.5};
}
// Martin Roberts' R2 low-discrepancy sequence (plastic constant), on the same cycle as
// temporal_jitter (index wraps at period, so every consumer that assumes a repeating
// pattern keeps working): x = frac(0.5 + n*a1) - 0.5, y = frac(0.5 + n*a2) - 0.5.
inline JitterSample temporal_jitter_r2(uint32_t phase, uint32_t period) {
    if(period<2 || period>4096) throw std::invalid_argument("jitter period must be 2..4096");
    const double a1=0.7548776662466927, a2=0.5698402909980532;
    const double n=double(phase%period+1);
    const auto frac=[](double v){ return v-double(static_cast<int64_t>(v)); };
    return {frac(0.5+n*a1)-0.5,frac(0.5+n*a2)-0.5};
}
// Explicit convention: row-major storage, column-vector multiplication, D3D viewport.
// Pixel X points right and pixel Y down. Translate clip X/Y by W so displacement
// is constant after perspective division. Preserve clip Z/W, including reverse-Z.
// Caller must start from an UNJITTERED matrix, not apply this repeatedly to a result.
inline std::array<double,16> jitter_projection(const std::array<double,16>& unjittered,
    JitterSample sample,uint32_t width,uint32_t height) {
    if(!width || !height) throw std::invalid_argument("zero render extent");
    auto result=unjittered;
    const double dx=2*sample.x_pixels/double(width),dy=-2*sample.y_pixels/double(height);
    for(unsigned col=0;col<4;++col) {
        result[col]+=dx*unjittered[12+col];
        result[4+col]+=dy*unjittered[12+col];
    }
    return result;
}
} // namespace neural_alias
