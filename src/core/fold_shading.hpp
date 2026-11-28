#pragma once
// Fold the shading (Cort 2026-11-25): the game keeps drawing at full size, but its scene
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
// The level setting (settings.hpp j2_fold_mode): 0 DLAA, 1 Quality, 2 Performance, 3 Balanced. The Fold has no
// Balanced (its blocks are two pixels or four). Balanced is only truly had when the game's 3D picture is drawn
// small; asked for while that cannot be done, the Fold shows Quality, the nearest level that keeps more detail,
// and never DLAA: a level never falls to full size by itself.
constexpr uint32_t kLevelBalanced = 3;
constexpr Mode ModeFrom(uint32_t v) { return v == 1 || v == kLevelBalanced ? Mode::Quality : v == 2 ? Mode::Performance : Mode::Off; }
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
// The sample spacing a frame is drawn with: the shown level's blocks; while another level warms beside it,
// the smaller block on each axis. Both levels' declared jitter (DeclaredJitter) then stays inside DLSS's
// half-pixel range. With the shown level's spacing alone, a Quality level warming beside Performance was
// told up to 1.5 px on the vertical axis and DLSS refused every warm-up frame.
struct Spacing { Block block, pick; };
constexpr Spacing JitterSpacing(Mode shown, Mode warming)
{
    Block b = BlockFor(shown);
    if (warming != Mode::Off) {
        const Block w = BlockFor(warming);
        if (w.x < b.x) b.x = w.x;
        if (w.y < b.y) b.y = w.y;
    }
    return {b, {b.x - 1, b.y - 1}}; // the kept pixel is the last of each block (Pick)
}
constexpr uint32_t JitterPhases(Spacing s, uint32_t dlaa_phases)
{
    const uint32_t want = 8u * s.block.x * s.block.y;
    return s.block.x * s.block.y > 1 && want > dlaa_phases ? want : dlaa_phases;
}

// The hand-over. Cort: "Make sure that it keeps DLAA going until the handover is truly ready, no need to
// leave me sitting at a frozen picture wondering if it'll crash." While a new mode is asked for, the mode
// on screen keeps running untouched and everything the new one needs is made beside it, off the frame:
// its folded planes, its measured-motion working set, and last its DLSS feature (coordinator 2026-11-27:
// one DLSS feature at a time, so nothing is run unseen and no second feature is held). The screen changes
// on the first frame after all of it exists, with a new history (the size changes, so DLSS starts over
// anyway). A mode whose feature NVIDIA refuses, or that fails on screen, is given up on with the reason
// kept for the menu and the log.
struct Handover {
    Mode shown = Mode::Off;
    Mode preparing = Mode::Off; // asked for and being made ready (Off: nothing)
    Mode refused = Mode::Off;   // a request that was given up on (cleared when another mode is asked for)
    uint32_t waited = 0;        // frames the mode being prepared has waited so far
    uint64_t switches = 0, refusals = 0;

    struct Step {
        Mode show = Mode::Off;     // what this frame puts on screen
        Mode prepare = Mode::Off;  // asked for but not ready: keep making it (Off: nothing)
        bool show_reset = false;   // the shown mode starts a new history this frame
        bool switched = false;     // the screen changes mode with this frame
    };
    // Once per delivered frame, before DLSS runs. `able` false: the fold cannot run on this picture (the
    // screen goes back to DLAA at once). `ready`: everything mode `asked` needs exists now.
    constexpr Step Begin(Mode asked, bool able, bool ready)
    {
        if (!able) { asked = Mode::Off; ready = true; }
        if (asked != refused) refused = asked == Mode::Off ? Mode::Off : refused == asked ? refused : Mode::Off;
        if (asked == refused && asked != Mode::Off) asked = shown; // given up on: stay where we are
        Step s;
        if (asked == shown) { preparing = Mode::Off; waited = 0; s.show = shown; return s; }
        if (preparing != asked) { preparing = asked; waited = 0; }
        if (ready) { // the screen changes now
            shown = asked; preparing = Mode::Off; waited = 0; ++switches;
            s.show = shown; s.show_reset = true; s.switched = true;
            return s;
        }
        ++waited;
        s.show = shown; s.prepare = asked;
        return s;
    }
    // Mode m cannot run here (NVIDIA refused its feature, or it failed on screen): it is given up on until
    // another mode is asked for; if it was on screen, DLAA takes over.
    constexpr void Refuse(Mode m)
    {
        if (m == Mode::Off) return;
        refused = m; ++refusals; waited = 0;
        if (preparing == m) preparing = Mode::Off;
        if (shown == m) { shown = Mode::Off; ++switches; }
    }
};
} // namespace na::fold
