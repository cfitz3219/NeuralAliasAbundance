// Live Frame Generation on the route where NeuralAlias adds DLSS itself (Route B): the pure
// decisions, kept apart from D3D12 so the host tests can check them.
//
//  - The menu choice: how many pictures are generated per rendered frame, remembered per game.
//    It is a count, not an on/off switch (this build offers Off and x2 only).
//  - Who presents: the game's own thread (Frame Generation Off, exactly one picture per Present)
//    or the presenter's thread (pictures paced across the frame interval).
//  - The pacing plan: when each picture of one rendered frame is shown.
//  - The HUD sheet, learned by its shape from one traced frame: the output-size target whose run
//    of blended, depth-less draws ends right before the one draw into the shown buffer.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace na::fglive {

// ---- The menu choice -------------------------------------------------------------------------
// Generated pictures per rendered frame this build offers at most (x2). The count stays a value
// so x3 and Dynamic can be added without changing what is stored.
constexpr uint32_t kMaxGeneratedThisBuild = 1;

// Stored per game as the shown multiplier: 0 = Off, 2 = x2, 3 = x3 ... (1 is read as Off).
inline uint32_t GeneratedFromStored(uint32_t stored, uint32_t max_generated = kMaxGeneratedThisBuild)
{
    if (stored < 2) return 0;
    const uint32_t generated = stored - 1;
    return generated > max_generated ? max_generated : generated;
}
inline uint32_t StoredFromGenerated(uint32_t generated) { return generated ? generated + 1 : 0; }
// The menu's items: index 0 = Off, index n = (n + 1)x.
inline const char* ChoiceLabel(uint32_t generated)
{
    switch (generated) {
    case 0: return "Off";
    case 1: return "x2";
    case 2: return "x3";
    case 3: return "x4";
    default: return "x?";
    }
}

// ---- Who presents ----------------------------------------------------------------------------
// With Frame Generation off the game's own thread presents each frame, one Present per game
// Present, as before Frame Generation existed. Once it is on, the presenter's thread shows the
// pictures. Turning it off hands presentation back only when the thread has shown everything
// queued, so two threads never present the same chain at once.
enum class Owner { GameThread, PresenterThread };
inline Owner NextOwner(bool fg_on, Owner current, uint32_t queued)
{
    if (fg_on) return Owner::PresenterThread;
    if (current == Owner::PresenterThread && queued) return Owner::PresenterThread;
    return Owner::GameThread;
}

// ---- Pacing ----------------------------------------------------------------------------------
// One rendered frame becomes `generated + 1` pictures spread evenly over the frame interval:
// picture i is shown i * interval / (generated + 1) after the anchor (the moment the generated
// pictures exist). For x2 the generated picture is shown at the anchor and the real frame half an
// interval later, so real and generated pictures alternate at an even rate.
struct Plan {
    uint32_t pictures = 1;
    double offset_ms[8]{};
};
inline Plan PlanPictures(double frame_ms, uint32_t generated)
{
    Plan p;
    if (generated > 7) generated = 7;
    if (!(frame_ms > 0.0)) frame_ms = 0.0;
    p.pictures = generated + 1;
    for (uint32_t i = 0; i < p.pictures; ++i) p.offset_ms[i] = frame_ms * static_cast<double>(i) / static_cast<double>(p.pictures);
    return p;
}
// A generated picture already a whole step late is not shown (it would only push the real frame
// later still). The step is never taken as less than 4 ms (scheduler jitter alone must not drop
// pictures when steps are tiny).
inline bool GeneratedTooLate(double late_by_ms, double step_ms) { return late_by_ms > (step_ms > 4.0 ? step_ms : 4.0); }

// ---- The HUD sheet ---------------------------------------------------------------------------
// Op: any type with bool compute, transfer, blend, depth and a container `out` of outputs with
// uint64_t id and uint32_t w, h (the first output is the render target of a draw).
enum class SheetVerdict {
    Found,           // sheet and final draw are known
    NoShownWrite,    // nothing in the traced frame wrote the shown buffer
    ShownNotDrawn,   // the shown buffer is written by a copy or a compute pass, not a draw
    NoRun,           // the write to the shown buffer is not preceded by blended, depth-less draws into another output-size target
    ShortRun,        // fewer than `min_run` draws in that run
    OtherWrites,     // something other than the blended run also writes the sheet in the frame
};
struct Sheet {
    SheetVerdict verdict = SheetVerdict::NoShownWrite;
    uint64_t sheet = 0;
    uint32_t run = 0, other = 0;
    size_t final_op = 0; // index of the draw into the shown buffer
};
inline const char* SheetWhy(SheetVerdict v)
{
    switch (v) {
    case SheetVerdict::Found: return "HUD sheet found: its blended draws end right before the one draw into the shown picture";
    case SheetVerdict::NoShownWrite: return "nothing in the traced frame wrote the shown picture";
    case SheetVerdict::ShownNotDrawn: return "the shown picture is written by a copy or compute pass, not by a draw that could be run again";
    case SheetVerdict::NoRun: return "the draw into the shown picture is not preceded by blended, depth-less draws into another screen-size picture";
    case SheetVerdict::ShortRun: return "fewer than 4 HUD draws in the run before the shown picture";
    case SheetVerdict::OtherWrites: return "the HUD sheet is also written by draws that are not HUD-like (blended, depth-less)";
    }
    return "unknown";
}
template <class Ops>
Sheet LearnSheet(const Ops& ops, uint64_t shown, uint32_t shown_w, uint32_t shown_h, uint32_t min_run = 4)
{
    Sheet r;
    const size_t n = ops.size();
    size_t last = n;
    for (size_t i = n; i-- > 0;) {
        bool hit = false;
        for (const auto& o : ops[i].out) if (o.id == shown) hit = true;
        if (hit) { last = i; break; }
    }
    if (last == n) { r.verdict = SheetVerdict::NoShownWrite; return r; }
    r.final_op = last;
    {
        const auto& f = ops[last];
        if (f.compute || f.transfer || f.out.empty() || f.out[0].id != shown) { r.verdict = SheetVerdict::ShownNotDrawn; return r; }
    }
    for (size_t i = last; i-- > 0;) {
        const auto& op = ops[i];
        if (op.compute || op.transfer || op.out.empty()) continue;
        const auto& o = op.out[0];
        if (!r.sheet) {
            if (!op.blend || op.depth || o.id == shown || o.w != shown_w || o.h != shown_h) break;
            r.sheet = o.id; r.run = 1; continue;
        }
        if (o.id == r.sheet && op.blend && !op.depth) { ++r.run; continue; }
        break;
    }
    if (!r.sheet) { r.verdict = SheetVerdict::NoRun; return r; }
    for (const auto& op : ops)
        for (const auto& o : op.out)
            if (o.id == r.sheet && !(op.blend && !op.depth)) ++r.other;
    if (r.run < min_run) { r.verdict = SheetVerdict::ShortRun; return r; }
    if (r.other) { r.verdict = SheetVerdict::OtherWrites; return r; }
    r.verdict = SheetVerdict::Found;
    return r;
}

// ---- The HUD mask (CPU reference of the GPU paste) -------------------------------------------
// A pixel belongs to the newer real frame's HUD when, in display terms (linear light clipped at 0,
// to the power 1/2.2, clipped at 1), any channel of the real frame differs from its HUD-free
// picture by more than 4/255. The mask is grown by one pixel (3x3). Exactly the offline T2 rule.
inline float DisplayValue(float linear)
{
    if (!(linear > 0.f)) return 0.f;
    const float v = std::pow(linear, 1.f / 2.2f);
    return v > 1.f ? 1.f : v;
}
constexpr float kHudThreshold = 4.f / 255.f;
// rgb: three floats a pixel, rows of w. True where the real frame's HUD is (before growing).
inline bool HudPixel(const float* real_rgb, const float* hudfree_rgb)
{
    for (int c = 0; c < 3; ++c)
        if (std::fabs(DisplayValue(real_rgb[c]) - DisplayValue(hudfree_rgb[c])) > kHudThreshold) return true;
    return false;
}
// out = generated, with every pixel within one pixel (3x3) of a HUD pixel replaced by the real frame.
inline void PasteHud(const float* real, const float* hudfree, float* generated, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            bool hud = false;
            for (int dy = -1; dy <= 1 && !hud; ++dy)
                for (int dx = -1; dx <= 1 && !hud; ++dx) {
                    const int64_t sx = int64_t(x) + dx, sy = int64_t(y) + dy;
                    if (sx < 0 || sy < 0 || sx >= int64_t(w) || sy >= int64_t(h)) continue;
                    const size_t i = (size_t(sy) * w + size_t(sx)) * 3;
                    hud = HudPixel(real + i, hudfree + i);
                }
            if (hud) {
                const size_t i = (size_t(y) * w + x) * 3;
                for (int c = 0; c < 3; ++c) generated[i + c] = real[i + c];
            }
        }
}
} // namespace na::fglive
