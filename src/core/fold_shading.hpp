#pragma once
// Fold the shading (Cort 2026-11-25, live test): the game keeps drawing at full size, but its scene
// draws are shaded once per block of pixels (Direct3D coarse shading: 2x1 blocks = half the shading,
// 2x2 = a quarter). One pixel of every block is then kept, which gives DLSS a smaller picture, and
// DLSS rebuilds the full-size one. The game is never told and its HUD is never touched.
//   Quality     2x1 blocks: the picture DLSS gets is half as wide, full height
//   Performance 2x2 blocks: half as wide, half as high
// This header is the arithmetic and the hand-over rule only (no Direct3D), so it is tested on any machine.
#include <cstdint>

namespace na::fold {
enum class Mode : uint8_t { Off = 0, Quality = 1, Performance = 2 };
constexpr uint32_t kModes = 3;
constexpr Mode ModeFrom(uint32_t v) { return v == 1 ? Mode::Quality : v == 2 ? Mode::Performance : Mode::Off; }
constexpr const char* Name(Mode m) { return m == Mode::Quality ? "Quality" : m == Mode::Performance ? "Performance" : "DLAA"; }

struct Block { uint32_t x = 1, y = 1; };
constexpr Block BlockFor(Mode m) { return m == Mode::Performance ? Block{2, 2} : m == Mode::Quality ? Block{2, 1} : Block{1, 1}; }
// D3D12_SHADING_RATE: 1X1 = 0, 2X1 = 4, 2X2 = 5.
constexpr uint32_t ShadingRateFor(Mode m) { return m == Mode::Performance ? 5u : m == Mode::Quality ? 4u : 0u; }
// The folded picture's size (a last odd column or row is left out).
constexpr uint32_t Folded(uint32_t full, uint32_t block) { return block ? full / block : full; }

// The kept pixel of each block: always the same one, the last (2x2: the lower right). Live run
// f98ae48c kept each pixel in turn and told DLSS the block's centre; Cort saw small objects shake.
// Under coarse shading only the inside of a surface is shaded per block: edges, depth and every
// full-screen pass are still per pixel, so a kept pixel that moves makes them step half a block
// against the declared sample. One fixed pixel has one true sample position. The last pixel is the
// one a full-screen pass shaded per block computes exactly (the block's centre, taken as a whole
// pixel coordinate, is that pixel), so it stays exact when those passes are coarse too.
constexpr Block Pick(Mode m)
{
    const Block b = BlockFor(m);
    return {b.x - 1, b.y - 1};
}

// The jitter DLSS is told, in pixels of the folded picture, for one axis. `full` is the offset the
// game's draws really carried, in full-size pixels (sample = pixel centre - offset). The kept
// pixel's own centre sits (block / 2 - pick - 0.5) full-size pixels before the block's centre.
constexpr float DeclaredJitter(uint32_t block, uint32_t pick, float full)
{
    const float b = static_cast<float>(block ? block : 1);
    return (b * 0.5f - static_cast<float>(pick) - 0.5f + full) / b;
}
// What the draws' offset needs added, in full-size pixels on one axis, so that the kept pixel's
// sample moves around the block's centre and the declared jitter stays inside DLSS's half-pixel
// range. (Build fb0d69cd left this out: it declared up to 0.74 and Cort saw dots and lines.)
// With draws offset by block * j + KeptPixelCentring, DeclaredJitter gives exactly j.
constexpr float KeptPixelCentring(uint32_t block, uint32_t pick)
{
    return static_cast<float>(pick) + 0.5f - static_cast<float>(block ? block : 1) * 0.5f;
}
// Jitter positions per cycle: NVIDIA's 8 x (output pixels per rendered pixel), never under DLAA's count.
constexpr uint32_t JitterPhases(Mode m, uint32_t dlaa_phases)
{
    const Block b = BlockFor(m);
    const uint32_t want = 8u * b.x * b.y;
    return want > dlaa_phases ? want : dlaa_phases;
}

// The hand-over. Cort: "Make sure that it keeps DLAA going until the handover is truly ready, no need to
// leave me sitting at a frozen picture wondering if it'll crash." While a new mode is asked for, the
// mode on screen keeps running untouched, and the new one is run beside it into a picture nobody
// sees. Only after kWarmFrames of those in a row succeeded does the screen change. If the new mode
// keeps failing, the screen stays as it is and the reason is kept for the menu and the log.
struct Handover {
    static constexpr uint32_t kWarmFrames = 8;
    static constexpr uint32_t kGiveUp = 120; // failed warm-up frames before the request is refused
    Mode shown = Mode::Off;
    Mode warming = Mode::Off;
    Mode refused = Mode::Off; // a request that was given up on (cleared when another mode is asked for)
    uint32_t good = 0, failed = 0;
    uint64_t switches = 0, refusals = 0;

    struct Step {
        Mode show = Mode::Off;     // what this frame puts on screen
        Mode warm = Mode::Off;     // also run this one, unseen (Off: nothing)
        bool show_reset = false;   // the shown mode starts a new history this frame
        bool warm_reset = false;   // the warming mode starts a new history this frame
        bool switched = false;     // the screen changes mode with this frame
    };
    // Once per delivered frame, before DLSS runs. `able` false: the fold cannot run on this picture.
    constexpr Step Begin(Mode asked, bool able)
    {
        if (!able) asked = Mode::Off;
        if (asked != refused) refused = asked == Mode::Off ? Mode::Off : refused == asked ? refused : Mode::Off;
        if (asked == refused && asked != Mode::Off) asked = shown; // given up on: stay where we are
        Step s;
        if (asked == shown) { warming = Mode::Off; good = failed = 0; s.show = shown; return s; }
        if (asked == Mode::Off) { // back to DLAA: its feature is still there, only its history is old
            shown = Mode::Off; warming = Mode::Off; good = failed = 0; ++switches;
            s.show = Mode::Off; s.show_reset = true; s.switched = true;
            return s;
        }
        if (warming != asked) { warming = asked; good = failed = 0; }
        if (good >= kWarmFrames) { // proven: the screen changes now, on a history that is already warm
            shown = asked; warming = Mode::Off; good = failed = 0; ++switches;
            s.show = shown; s.switched = true;
            return s;
        }
        s.show = shown; s.warm = asked; s.warm_reset = good == 0;
        return s;
    }
    // After the unseen run of a warming frame.
    constexpr void WarmResult(bool ok)
    {
        if (warming == Mode::Off) return;
        if (ok) { ++good; return; }
        good = 0;
        if (++failed >= kGiveUp) { refused = warming; warming = Mode::Off; failed = 0; ++refusals; }
    }
};
} // namespace na::fold
