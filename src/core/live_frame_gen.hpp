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

// ---- The interface drawn straight onto the shown picture (snapshot variant) ------------------
// Many games draw the scene (or its tone-mapped composite) into the shown picture and then the
// interface on top of it, with no separate sheet. The HUD-free picture is then the shown picture
// itself just before the first interface draw: a copy of it taken right there. Learned from one
// traced frame by shape: walking back from the last write to the shown picture, a run of blended,
// depth-less draws into it (other ops in between that do not write it are passed over), preceded
// by a write to it that is not interface-like (the scene or its composite). The copy point is "the
// k-th draw into the shown picture in that recording" (k = draws into it before the run there), so
// the composite and the first interface draw must be in the same recording of the same list.
// Op: as for LearnSheet, plus uint64_t list, recording.
enum class SnapshotVerdict {
    Found,           // copy point known
    NoShownWrite,    // nothing in the traced frame wrote the shown picture
    ShownNotDrawn,   // the last write to the shown picture is a copy or a compute pass
    NoRun,           // the last write to the shown picture is not an interface-like (blended, depth-less) draw
    ShortRun,        // fewer than `min_run` interface draws
    NoScene,         // no non-interface write to the shown picture before the run: nothing HUD-free to copy
    SplitRecording,  // the scene's write and the first interface draw are in different recordings
};
struct Snapshot {
    SnapshotVerdict verdict = SnapshotVerdict::NoShownWrite;
    uint32_t run = 0;
    size_t first_hud_op = 0;   // index of the first interface draw
    uint32_t draws_before = 0; // draws into the shown picture in that recording before it (the copy point)
    uint64_t list = 0, recording = 0;
};
inline const char* SnapshotWhy(SnapshotVerdict v)
{
    switch (v) {
    case SnapshotVerdict::Found: return "interface drawn on the shown picture: it is copied just before the first interface draw";
    case SnapshotVerdict::NoShownWrite: return "nothing in the traced frame wrote the shown picture";
    case SnapshotVerdict::ShownNotDrawn: return "the shown picture is finished by a copy or compute pass, not by interface draws";
    case SnapshotVerdict::NoRun: return "the shown picture's last write is not an interface-like draw (blended, without depth)";
    case SnapshotVerdict::ShortRun: return "fewer than 4 interface draws on the shown picture";
    case SnapshotVerdict::NoScene: return "nothing but interface-like draws writes the shown picture, so there is no HUD-free point to copy";
    case SnapshotVerdict::SplitRecording: return "the scene and the interface are drawn into the shown picture from different command lists";
    }
    return "unknown";
}
template <class Ops>
Snapshot LearnSnapshot(const Ops& ops, uint64_t shown, uint32_t min_run = 4)
{
    Snapshot r;
    const size_t n = ops.size();
    auto writes_shown = [&](size_t i) { for (const auto& o : ops[i].out) if (o.id == shown) return true; return false; };
    auto hud_like = [&](size_t i) { const auto& op = ops[i]; return !op.compute && !op.transfer && op.blend && !op.depth && !op.out.empty() && op.out[0].id == shown; };
    size_t last = n;
    for (size_t i = n; i-- > 0;) if (writes_shown(i)) { last = i; break; }
    if (last == n) { r.verdict = SnapshotVerdict::NoShownWrite; return r; }
    if (ops[last].compute || ops[last].transfer) { r.verdict = SnapshotVerdict::ShownNotDrawn; return r; }
    if (!hud_like(last)) { r.verdict = SnapshotVerdict::NoRun; return r; }
    size_t first = last, scene = n;
    for (size_t i = last + 1; i-- > 0;) {
        if (!writes_shown(i)) continue;
        if (hud_like(i)) { ++r.run; first = i; continue; }
        scene = i;
        break;
    }
    r.first_hud_op = first;
    if (r.run < min_run) { r.verdict = SnapshotVerdict::ShortRun; return r; }
    if (scene == n) { r.verdict = SnapshotVerdict::NoScene; return r; }
    r.list = ops[first].list;
    r.recording = ops[first].recording;
    if (ops[scene].list != r.list || ops[scene].recording != r.recording) { r.verdict = SnapshotVerdict::SplitRecording; return r; }
    for (size_t i = 0; i < first; ++i) {
        const auto& op = ops[i];
        if (op.list == r.list && op.recording == r.recording && !op.compute && !op.transfer && !op.out.empty() && op.out[0].id == shown) ++r.draws_before;
    }
    r.verdict = SnapshotVerdict::Found;
    return r;
}

// Which HUD-free variant a traced frame supports (docs/VARIANTS.md names): the separate sheet re-drawn first, else the
// snapshot before the first interface draw, else none (x2 waits with both reasons).
enum class HudVariant { None, SheetRedraw, Snapshot };
inline const char* HudVariantName(HudVariant v)
{
    return v == HudVariant::SheetRedraw ? "hudfree/sheet-redraw: the HUD-free picture by running the game's final draw again (separate HUD sheet)"
         : v == HudVariant::Snapshot    ? "hudfree/snapshot-before-interface: the HUD-free picture copied just before the first interface draw (interface on the shown picture)"
                                        : "none: HUD not separable, real frames only";
}
// VARIANT: hudfree/sheet-redraw
// VARIANT: hudfree/snapshot-before-interface
template <class Ops>
HudVariant ChooseHudVariant(const Ops& ops, uint64_t shown, uint32_t shown_w, uint32_t shown_h, Sheet* sheet, Snapshot* snap)
{
    const Sheet a = LearnSheet(ops, shown, shown_w, shown_h);
    if (sheet) *sheet = a;
    if (a.verdict == SheetVerdict::Found) return HudVariant::SheetRedraw;
    const Snapshot b = LearnSnapshot(ops, shown);
    if (snap) *snap = b;
    return b.verdict == SnapshotVerdict::Found ? HudVariant::Snapshot : HudVariant::None;
}

// ---- The sheet's state at the final draw -----------------------------------------------------
// To swap the sheet's contents just before the game's final draw, the exact state the game left
// it in is needed (a wrong barrier state is a device removal, never a guess). The proof used:
// the sheet was the bound render target at its first HUD draw (the copy is taken there) and is
// read by the final draw. When both happen in the SAME recording of the same command list, the
// game must have changed its state inside that recording between the two, and with legacy
// barriers the state after the last change inside a list is the state the GPU sees at that point,
// whatever other lists do or in which order they were recorded. So only states recorded in this
// recording count: the last whole-resource transition, the last transition naming subresource 0,
// and the independent per-list barrier tracker. Every source that knows must agree.
enum class Lookup : uint8_t { Known, Absent, Unavailable };
struct StateSource {
    Lookup lookup = Lookup::Unavailable;
    uint32_t state = 0;
};
struct SheetStateInputs {
    bool same_recording_as_copy = false; // the pre-HUD copy (sheet as render target) was taken in this recording
    StateSource whole, sub0, tracker;
};
struct SheetState {
    bool known = false;
    uint32_t state = 0;
    const char* source = "";
    const char* why = "";
};
constexpr uint32_t kRenderTarget = 0x4; // D3D12_RESOURCE_STATE_RENDER_TARGET
inline SheetState ResolveSheetState(const SheetStateInputs& in)
{
    SheetState r;
    if (!in.same_recording_as_copy) {
        r.why = "the HUD is drawn in another command list (or recording) than the final draw, so the sheet's state there cannot be proven";
        return r;
    }
    const StateSource* sources[3] = {&in.whole, &in.sub0, &in.tracker};
    const char* names[3] = {"whole-resource transition", "subresource-0 transition", "barrier tracker"};
    int first = -1;
    for (int i = 0; i < 3; ++i) {
        if (sources[i]->lookup != Lookup::Known) continue;
        if (first < 0) { first = i; continue; }
        if (sources[i]->state != sources[first]->state) {
            r.why = "two records of the game's barriers disagree about the HUD sheet's state";
            return r;
        }
    }
    if (first < 0) {
        const bool all_absent = in.whole.lookup == Lookup::Absent && in.sub0.lookup == Lookup::Absent && in.tracker.lookup == Lookup::Absent;
        r.why = all_absent ? "no change of the HUD sheet's state was seen in this list although it was drawn into and is now read (a barrier method that is not observed)"
                           : "this list's record of barriers is incomplete, so the HUD sheet's state is not known";
        return r;
    }
    const uint32_t s = sources[first]->state;
    if (s == 0) { r.why = "the HUD sheet was put in the COMMON state, whose real state after use cannot be known"; return r; }
    if (s & kRenderTarget) { r.why = "the HUD sheet is still a render target at the final draw (not a state it can be read in)"; return r; }
    r.known = true;
    r.state = s;
    r.source = names[first];
    return r;
}

// ---- The HUD mask (CPU reference of the GPU paste) -------------------------------------------
// A pixel belongs to the newer real frame's HUD when, in display terms, any channel of the real
// frame differs from its HUD-free picture by more than 4/255. The mask is grown by one pixel (3x3).
// Exactly the offline T2 rule. "Display terms" depend on how the picture stores its values (the
// mode numbers are na::color::ShaderMode's):
//   2 linear (FP16, scRGB scale): clipped at 0, to the power 1/2.2, clipped at 1 (as proven);
//   0 SDR (8-bit or 10-bit, already display-encoded): the stored value, clipped to 0..1;
//   1 PQ (HDR10): decoded to linear on the same scale as mode 2 (1.0 = 80 nits), then as mode 2.
constexpr uint32_t kPictureSdr = 0, kPicturePq = 1, kPictureLinear = 2;
// PQ (SMPTE ST 2084) code value 0..1 to linear light with 1.0 = 80 nits (the scRGB scale).
inline float PqToLinear80(float e)
{
    constexpr float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    e = e < 0.f ? 0.f : (e > 1.f ? 1.f : e);
    const float p = std::pow(e, 1.f / m2);
    const float num = p - c1 > 0.f ? p - c1 : 0.f;
    return std::pow(num / (c2 - c3 * p), 1.f / m1) * 125.f; // 10000 / 80
}
inline float DisplayValue(float linear)
{
    if (!(linear > 0.f)) return 0.f;
    const float v = std::pow(linear, 1.f / 2.2f);
    return v > 1.f ? 1.f : v;
}
inline float DisplayValue(float stored, uint32_t mode)
{
    if (mode == kPictureSdr) return !(stored > 0.f) ? 0.f : (stored > 1.f ? 1.f : stored);
    if (mode == kPicturePq) return DisplayValue(PqToLinear80(stored));
    return DisplayValue(stored);
}
constexpr float kHudThreshold = 4.f / 255.f;
// rgb: three floats a pixel, rows of w. True where the real frame's HUD is (before growing).
inline bool HudPixel(const float* real_rgb, const float* hudfree_rgb, uint32_t mode = kPictureLinear)
{
    for (int c = 0; c < 3; ++c)
        if (std::fabs(DisplayValue(real_rgb[c], mode) - DisplayValue(hudfree_rgb[c], mode)) > kHudThreshold) return true;
    return false;
}
// out = generated, with every pixel within one pixel (3x3) of a HUD pixel replaced by the real frame.
inline void PasteHud(const float* real, const float* hudfree, float* generated, uint32_t w, uint32_t h,
                     uint32_t mode = kPictureLinear)
{
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            bool hud = false;
            for (int dy = -1; dy <= 1 && !hud; ++dy)
                for (int dx = -1; dx <= 1 && !hud; ++dx) {
                    const int64_t sx = int64_t(x) + dx, sy = int64_t(y) + dy;
                    if (sx < 0 || sy < 0 || sx >= int64_t(w) || sy >= int64_t(h)) continue;
                    const size_t i = (size_t(sy) * w + size_t(sx)) * 3;
                    hud = HudPixel(real + i, hudfree + i, mode);
                }
            if (hud) {
                const size_t i = (size_t(y) * w + x) * 3;
                for (int c = 0; c < 3; ++c) generated[i + c] = real[i + c];
            }
        }
}
} // namespace na::fglive
