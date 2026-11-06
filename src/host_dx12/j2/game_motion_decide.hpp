// Game motion (Cort 2026-11-06, SE5 menu: the swaying character's sweatshirt smears). Our
// motion is camera reprojection plus flow; a skinned character that sways ~3.5 px/frame in a
// still menu gets ~0, so DLSS blends its history into mush. Many engines keep their own
// per-pixel motion in a two-channel texture at render size, even with motion blur off.
//
// Generic rule, no game data: every candidate texture (two channels, the scene's extent) is
// read under each of 12 encodings (pixels, UV, NDC; either sign per axis). A frame scores an
// encoding by how well the previous picture, moved by that motion, predicts this one, against
// not moving at all, on textured pixels where the candidate holds motion. Only an encoding that
// predicts clearly better on most measured frames is used, and it is dropped again when it
// stops doing so. An empty, stale or unrelated texture never predicts better, so it is never
// used; nothing changes for a game without one.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace na::j2::game_motion {

constexpr uint32_t kHypotheses = 12;
constexpr uint32_t kCandidates = 2;
// GPU counters per candidate (uint32 each): warp error per hypothesis, the zero-motion error,
// the scored pixel count, pixels holding motion, non-finite pixels, max |x| and |y| (float bits).
enum Slot : uint32_t {
    kErr0 = 0, kErrZero = kHypotheses, kScored, kMoving, kNonFinite, kMaxX, kMaxY, kSlots
};
constexpr uint32_t kErrorScale = 1024; // fixed point of a per-pixel error in [0, 1]

// Encoding h: scale kind h / 4 (0 pixels, 1 UV, 2 NDC), sign of x bit 0, sign of y bit 1.
// Result in the arbiter convention: v = p_previous - p_current in pixels, +y down.
inline void Scale(uint32_t h, uint32_t w, uint32_t hgt, float* sx, float* sy)
{
    const uint32_t kind = h / 4;
    float ax = 1.f, ay = 1.f;
    if (kind == 1) { ax = static_cast<float>(w); ay = static_cast<float>(hgt); }
    if (kind == 2) { ax = 0.5f * static_cast<float>(w); ay = 0.5f * static_cast<float>(hgt); }
    *sx = (h & 1u) ? -ax : ax;
    *sy = (h & 2u) ? -ay : ay;
}
inline const char* KindName(uint32_t h) { return h / 4 == 0 ? "px" : h / 4 == 1 ? "uv" : "ndc"; }

struct Measure {
    uint64_t err[kHypotheses]{};
    uint64_t err_zero = 0, scored = 0, moving = 0, non_finite = 0;
    float max_x = 0, max_y = 0;
};

// CPU reference of the GPU score pass (game_motion.cpp mirrors it; the tests use this one).
// cand: 2 floats per pixel as sampled (UNORM already in [0,1]); luma: tonemapped [0,1].
// Every `step`-th pixel in x and y; textured (3x3 luma range >= kTexture) pixels whose
// candidate value is finite and non-zero are scored.
constexpr float kTexture = 0.02f, kZero = 1e-6f;
inline float Bilinear(const float* img, uint32_t w, uint32_t h, float x, float y)
{
    // pixel centres at +0.5, clamped (same as a clamp sampler)
    x -= 0.5f; y -= 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float ax = x - fx, ay = y - fy;
    auto at = [&](int xi, int yi) {
        xi = xi < 0 ? 0 : xi >= static_cast<int>(w) ? static_cast<int>(w) - 1 : xi;
        yi = yi < 0 ? 0 : yi >= static_cast<int>(h) ? static_cast<int>(h) - 1 : yi;
        return img[static_cast<size_t>(yi) * w + static_cast<size_t>(xi)];
    };
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    return (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
}
inline void ScoreCpu(const float* cand, const float* cur, const float* prev, uint32_t w, uint32_t h, bool unorm,
                     uint32_t step, Measure* m)
{
    for (uint32_t y = 1; y + 1 < h; y += step)
        for (uint32_t x = 1; x + 1 < w; x += step) {
            float rx = cand[(static_cast<size_t>(y) * w + x) * 2], ry = cand[(static_cast<size_t>(y) * w + x) * 2 + 1];
            if (!std::isfinite(rx) || !std::isfinite(ry)) { ++m->non_finite; continue; }
            if (unorm) { rx = rx * 2 - 1; ry = ry * 2 - 1; }
            if (std::fabs(rx) <= kZero && std::fabs(ry) <= kZero) continue;
            ++m->moving;
            m->max_x = std::fmax(m->max_x, std::fabs(rx)); m->max_y = std::fmax(m->max_y, std::fabs(ry));
            float lo = 1, hi = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) { const float v = cur[(y + j) * w + (x + i)]; lo = std::fmin(lo, v); hi = std::fmax(hi, v); }
            if (hi - lo < kTexture) continue;
            const float c = cur[static_cast<size_t>(y) * w + x];
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            ++m->scored;
            m->err_zero += static_cast<uint64_t>(std::fmin(std::fabs(c - prev[static_cast<size_t>(y) * w + x]), 1.f) * kErrorScale);
            for (uint32_t k = 0; k < kHypotheses; ++k) {
                float sx, sy; Scale(k, w, h, &sx, &sy);
                const float e = std::fabs(c - Bilinear(prev, w, h, px + rx * sx, py + ry * sy));
                m->err[k] += static_cast<uint64_t>(std::fmin(e, 1.f) * kErrorScale);
            }
        }
}

class Decider {
public:
    struct Limits {
        uint64_t min_scored = 256;   // textured pixels with motion for a frame to count
        double good_ratio = 0.8;     // best warp error / zero-motion error to call a frame good
        double margin = 0.95;        // best must beat the next encoding by this ratio too
        uint32_t window = 32;        // measured frames remembered
        uint32_t lock_good = 24;     // good frames (same encoding) in the window to start using it
        uint32_t keep_good = 8;      // fewer than this many good frames in the window stops it
    };
    Decider() = default;
    explicit Decider(const Limits& l) : lim_(l) {}

    // One frame's counters for candidate c. Returns true when the used (candidate, encoding)
    // changed.
    bool Add(uint32_t c, const Measure& m)
    {
        if (c >= kCandidates) return false;
        ++measured_;
        last_ = m; last_c_ = c;
        int best = -1;
        double rb = 0, r2 = 0;
        if (m.scored >= lim_.min_scored && m.err_zero > 0) {
            for (uint32_t h = 0; h < kHypotheses; ++h) {
                const double r = static_cast<double>(m.err[h]) / static_cast<double>(m.err_zero);
                if (best < 0 || r < rb) { r2 = best < 0 ? 1e9 : rb; rb = r; best = static_cast<int>(h); }
                else if (r < r2) r2 = r;
            }
        }
        last_best_ = best; last_ratio_ = best >= 0 ? rb : 0; last_second_ = best >= 0 ? r2 : 0;
        const bool good = best >= 0 && rb <= lim_.good_ratio && rb <= lim_.margin * r2;
        if (!good) best = -1;
        auto& hist = hist_[c];
        hist[pos_[c]] = static_cast<int8_t>(best);
        pos_[c] = (pos_[c] + 1) % lim_.window;
        if (filled_[c] < lim_.window) ++filled_[c];
        if (good) ++good_frames_;
        const int before_c = used_c_, before_h = used_h_;
        if (used_c_ == static_cast<int>(c)) {
            if (count(c, used_h_) < lim_.keep_good && filled_[c] >= lim_.window) { used_c_ = -1; used_h_ = -1; ++dropped_; }
        }
        if (used_c_ < 0) {
            for (uint32_t h = 0; h < kHypotheses; ++h)
                if (count(c, static_cast<int>(h)) >= lim_.lock_good) { used_c_ = static_cast<int>(c); used_h_ = static_cast<int>(h); ++locked_; break; }
        }
        return before_c != used_c_ || before_h != used_h_;
    }
    void Reset() { *this = Decider(lim_); }
    // Candidate set changed: forget its frames, drop it if it was used.
    void ResetCandidate(uint32_t c)
    {
        if (c >= kCandidates) return;
        hist_[c].fill(-1); pos_[c] = 0; filled_[c] = 0;
        if (used_c_ == static_cast<int>(c)) { used_c_ = -1; used_h_ = -1; ++dropped_; }
    }
    int UsedCandidate() const { return used_c_; }
    int UsedEncoding() const { return used_h_; }
    uint64_t Measured() const { return measured_; }
    uint64_t GoodFrames() const { return good_frames_; }
    uint64_t Locked() const { return locked_; }
    uint64_t Dropped() const { return dropped_; }
    const Measure& Last() const { return last_; }
    uint32_t LastCandidate() const { return last_c_; }
    int LastBest() const { return last_best_; }
    double LastRatio() const { return last_ratio_; }
    double LastSecond() const { return last_second_; }

private:
    uint32_t count(uint32_t c, int h) const
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < filled_[c]; ++i) n += hist_[c][i] == h ? 1u : 0u;
        return n;
    }
    Limits lim_;
    std::array<std::array<int8_t, 64>, kCandidates> hist_{};
    uint32_t pos_[kCandidates]{}, filled_[kCandidates]{};
    int used_c_ = -1, used_h_ = -1;
    uint64_t measured_ = 0, good_frames_ = 0, locked_ = 0, dropped_ = 0;
    Measure last_{};
    uint32_t last_c_ = 0;
    int last_best_ = -1;
    double last_ratio_ = 0, last_second_ = 0;
};

} // namespace na::j2::game_motion
