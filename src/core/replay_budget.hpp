#pragma once
// How much of a frame draw replay may run again (Cort's Sniper Elite Resistance runs, 2026-11-17:
// replay took the frame from 80 to about 41 fps because it expanded about 3 million vertices every
// frame, filled its buffer with the first 50 to 80 draws in submission order, refused about 2,300
// more, and still covered only a tenth of the screen).
// The rule, with no game in it:
//   - a frame may replay about `budget` expanded vertices (0 = no budget, the old behaviour);
//   - which draws are replayed is decided by a number made from the draw's own identity (the same
//     identity replay matches on), compared with a threshold. The same draw therefore gets the same
//     answer in adjacent frames, which matching needs, and a lower threshold admits a subset of a
//     higher one, so a changing threshold only loses the draws at the edge;
//   - the threshold follows what the last frame offered and admitted, so the admitted share lands on
//     the budget;
//   - the identity number changes every `rotate_frames` frames, so over time every draw is tried;
//   - one draw may take at most half the budget, and a frame stops at 5/4 of it whatever happens.
// A pixel replay does not cover keeps the measured motion (the arbiter's slot behind tier 1).
#include <cstdint>

namespace na::replay_budget {

constexpr uint32_t kFull = 65536; // threshold that admits every draw

enum class Verdict : uint32_t { Admit, NotSampled, DrawTooLarge, FrameFull };

// A full 64-bit finalizer per step: a small change of any input (the period counts 0, 1, 2 ...)
// must change every output bit, or the rotation would only slide the admitted set along.
inline uint64_t Mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
    return h ^ (h >> 33);
}

// 0..65535, the same for a draw in every frame of one rotation period.
inline uint32_t DrawValue(uint64_t sig, uint64_t vb, uint64_t frame, uint32_t rotate_frames)
{
    const uint64_t epoch = rotate_frames ? frame / rotate_frames : 0;
    return static_cast<uint32_t>(Mix(Mix(Mix(0x243f6a8885a308d3ull, sig), vb), epoch) >> 48);
}

// The rotating share's number (core/replay_movers.hpp): the draw's fixed number seen from `phase`.
// The caller moves `phase` on by the threshold at each period, so each period takes the next slice
// of draws and every draw is tried once in kFull / threshold periods, none twice before that.
inline uint32_t SweepValue(uint64_t sig, uint64_t vb, uint32_t phase)
{
    return (DrawValue(sig, vb, 0, 0) - phase) & (kFull - 1);
}
inline uint32_t NextPhase(uint32_t phase, uint32_t threshold) { return (phase + threshold) & (kFull - 1); }

inline uint64_t DrawLimit(uint64_t budget) { return budget / 2; }
inline uint64_t FrameLimit(uint64_t budget) { return budget + budget / 4; }

// The same decision against limits given outright. `used`: vertices already admitted this frame.
inline Verdict DecideWithin(uint32_t value, uint32_t threshold, uint64_t verts, uint64_t used, uint64_t draw_limit, uint64_t frame_limit)
{
    if (verts > draw_limit) return Verdict::DrawTooLarge;
    if (value >= threshold) return Verdict::NotSampled;
    if (used + verts > frame_limit) return Verdict::FrameFull;
    return Verdict::Admit;
}

inline Verdict Decide(uint32_t value, uint32_t threshold, uint64_t verts, uint64_t used, uint64_t budget)
{
    if (!budget) return Verdict::Admit;
    return DecideWithin(value, threshold, verts, used, DrawLimit(budget), FrameLimit(budget));
}

// The rotating share (core/replay_movers.hpp: known movers are replayed first, every other draw is
// tried in turn on `share` vertices a frame). A draw may still be as large as half the whole budget,
// or a detailed character could never be tried, so a frame's limit is never under that.
inline uint64_t ProbeFrameLimit(uint64_t budget, uint64_t share)
{
    const uint64_t a = FrameLimit(share), b = DrawLimit(budget);
    return a > b ? a : b;
}

// The next frame's threshold. `offered`: vertices of every draw that asked last frame and was not
// too large (admitted or not); `used`: the vertices admitted. Within a rotation period a draw's
// number is fixed, so the admitted amount is a fixed rising function of the threshold: the
// threshold is scaled by budget / used, kept within a factor of two of what the offered amount
// alone says (so one odd frame cannot throw it), and left alone once the frame is within a tenth
// of the budget. `hit_full`: last frame stopped at its limit, so
// `offered` is short (draws after the stop are not counted): come down by a quarter at least.
inline uint32_t NextThreshold(uint32_t current, uint64_t offered, uint64_t used, uint64_t budget, bool hit_full)
{
    if (!budget) return kFull;
    if (!offered) return current ? current : kFull; // nothing asked: no evidence, keep it
    if (!current) current = 1;
    uint64_t t;
    if (hit_full) {
        t = uint64_t(current) * 3 / 4;
        if (used > budget) { const uint64_t scaled = uint64_t(current) * budget / used; if (scaled < t) t = scaled; }
    } else {
        // Close enough: leave it, so adjacent frames admit exactly the same draws.
        if (offered > budget && used * 10 >= budget * 9 && used * 10 <= budget * 11) return current;
        const uint64_t estimate = offered <= budget ? kFull : (budget * kFull) / offered;
        t = used ? uint64_t(current) * budget / used : estimate;
        if (t > estimate * 2) t = estimate * 2;
        if (t < estimate / 2) t = estimate / 2;
        if (offered <= budget) t = kFull; // the whole view fits
    }
    if (t < 1) t = 1;
    if (t > kFull) t = kFull;
    return static_cast<uint32_t>(t);
}

// The per-frame state and the decision for one draw, the same for every replay producer (the DX12
// observe layer and the native DX11 provider): known movers first up to the whole budget, then the
// sample (VARIANT replay/movers-first), or the sample alone. The caller keeps one FrameBudget per producer.
struct FrameBudget { uint64_t frame = 0, used = 0, offered = 0, mover_used = 0; uint32_t threshold = kFull, phase = 0; bool full = false, movers = false; };
struct Rules { uint32_t rotate_frames = 240, probe_share = 4, probe_frames = 24; };
enum class Choice : uint32_t { Admit, MoverAdmit, MoverLeftOut, DrawTooLarge, NotSampled, FrameFull };
// `is_mover`: the movers table names this draw (only asked when `movers` is on). `*closed`: set when
// the frame is full and, without movers, nothing later in it can be admitted (the caller may then
// turn every later draw of the frame away at the door). Needs a budget (non-zero).
inline Choice Choose(FrameBudget& s, uint64_t frame, uint64_t budget, bool movers, bool is_mover, uint64_t sig, uint64_t vb, uint64_t verts,
                     const Rules& r, bool* closed = nullptr)
{
    if (closed) *closed = false;
    const uint64_t share = movers && r.probe_share ? budget / r.probe_share : budget; // what the sampled draws may take
    if (s.frame != frame) { // a new frame: the threshold follows what the last one offered and admitted
        if (s.movers != movers) { s.threshold = kFull; s.movers = movers; } // another rule: start it afresh
        else if (s.frame) s.threshold = NextThreshold(s.threshold, s.offered, s.used, share, s.full);
        if (r.probe_frames && frame / r.probe_frames != s.frame / r.probe_frames) s.phase = NextPhase(s.phase, s.threshold);
        s.frame = frame; s.used = 0; s.offered = 0; s.mover_used = 0; s.full = false;
    }
    if (movers && is_mover) {
        // A draw known to move by itself: replayed while the movers together fit the budget.
        if (s.mover_used + verts > budget) return Choice::MoverLeftOut;
        s.mover_used += verts;
        return Choice::MoverAdmit;
    }
    const Verdict verdict = movers
        ? DecideWithin(SweepValue(sig, vb, s.phase), s.threshold, verts, s.used, DrawLimit(budget), ProbeFrameLimit(budget, share))
        : Decide(DrawValue(sig, vb, frame, r.rotate_frames), s.threshold, verts, s.used, budget);
    switch (verdict) {
    case Verdict::DrawTooLarge: return Choice::DrawTooLarge;
    case Verdict::NotSampled: s.offered += verts; return Choice::NotSampled;
    case Verdict::FrameFull:
        s.full = true;
        if (movers) s.offered += verts;
        else if (closed) *closed = true;
        return Choice::FrameFull;
    case Verdict::Admit: break;
    }
    s.offered += verts; s.used += verts;
    return Choice::Admit;
}

} // namespace na::replay_budget
