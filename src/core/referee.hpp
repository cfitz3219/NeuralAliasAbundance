#pragma once
// The referee, stage 1: the program checks its own picks and only writes down what it found
// (plan "One program that adapts on its own", 2026-11-16; Cort 2026-11-17: "Begin on the referee").
// It changes nothing about what is shown. A few times a second the graphics card makes small
// samples of what DLSS was given and of the image that reached the screen; this file scores them.
// The same scoring runs on the processor for tests and on saved captures (stage 2), so the card
// and the replay of history give the same verdicts. No game names, no API: DX12 and DX11 hand the
// same samples (capture/referee.cpp).
//
// Four questions, each answered with numbers and a plain verdict:
//   picture: does the image DLSS ran on look like the screen? Small copies (kThumbW x kThumbH
//            cells of log luminance) of both, their edges compared (correlation of what is left
//            after removing each cell's neighbourhood mean). A material layer, a shadow map or a
//            normals layer does not look like the screen; the lit scene does, through tone
//            mapping, grading and the HUD.
//   result:  on the screen, does DLSS's output match better than the image before DLSS? Eight
//            full-resolution tiles of the input, the output and the screen, edges compared. DLAA
//            moves edges by under a pixel, so only the tiles where the two answers differ decide.
//   motion:  do the motion vectors handed to DLSS predict this frame from the last one better
//            than standing still? On a grid of points, a 3x3 patch of the current input against
//            the previous input fetched where the vector points.
//   jitter:  does the picture shift by the jitter DLSS was told? The same prediction, with the
//            change of jitter between the two frames added, taken away, or left out.
// Everything is in log2 luminance, so a gamma, an exposure or a tone curve close to a power law
// changes no correlation.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace na::referee {

constexpr uint32_t kThumbW = 128, kThumbH = 72;  // picture cells (whole image, any size)
constexpr uint32_t kThumbTaps = 4;               // taps per cell side
constexpr uint32_t kTiles = 8, kTile = 32;       // result tiles, full resolution
constexpr uint32_t kGridW = 96, kGridH = 54;     // motion points
constexpr uint32_t kPointValues = 9;             // per motion point, see Point
constexpr float kLumaFloor = 1e-4f;              // log2(luma + floor): black is not minus infinity

// Tile centres as a share of the image: off the middle (crosshair, reticle) and the outer band
// (most HUD elements), spread so that one HUD panel covers at most two.
inline constexpr float kTileAt[kTiles][2] = {
    {.22f, .30f}, {.42f, .26f}, {.62f, .30f}, {.80f, .34f},
    {.20f, .66f}, {.38f, .72f}, {.60f, .70f}, {.78f, .64f}};

// Layout of the card's result buffer (floats); the processor reference fills the same layout.
constexpr uint32_t kThumbFloats = kThumbW * kThumbH;
constexpr uint32_t kTileFloats = kTiles * kTile * kTile;
constexpr uint32_t kPointFloats = kGridW * kGridH * kPointValues;
enum Block : uint32_t { kThumbInput, kThumbOutput, kThumbScreen, kTileInput, kTileOutput, kTileScreen, kPoints, kBlocks };
constexpr uint32_t BlockOffset(Block b)
{
    return b <= kThumbScreen ? uint32_t(b) * kThumbFloats
         : b <= kTileScreen  ? 3 * kThumbFloats + (uint32_t(b) - kTileInput) * kTileFloats
                             : 3 * kThumbFloats + 3 * kTileFloats;
}
constexpr uint32_t kResultFloats = BlockOffset(kPoints) + kPointFloats;

// One motion point (kPointValues floats): mean absolute log2-luminance difference over a 3x3
// patch, the current input against the previous input fetched
//   still:   at the same place
//   vector:  where the motion vector points (p + v)
//   follows: p + v - (j_now - j_before): the picture moved by the jitter in the sense DLSS was told
//   reverse: p + v + (j_now - j_before)
//   half:    p + v/2 - (j_now - j_before)   (a vector twice too long would be right here)
//   twice:   p + 2v - (j_now - j_before)    (a vector half too short would be right here)
// plus the patch's own contrast (mean absolute step to the right and below, previous image) and
// the vector's length in pixels. A value below zero marks a point that could not be measured.
enum PointValue : uint32_t { kStill, kVector, kFollows, kReverse, kHalf, kTwice, kContrast, kLength, kStillStructure };
static_assert(kStillStructure + 1 == kPointValues);

// ---------------------------------------------------------------------------------------------
// The processor reference of the card's sampling (tests, captures). An image is linear luminance,
// row-major, w x h; motion is two floats a pixel (DLSS's vectors, before their scale).
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<float> v;
    float At(int x, int y) const
    {
        x = std::clamp(x, 0, int(w) - 1); y = std::clamp(y, 0, int(h) - 1);
        return v[size_t(y) * w + size_t(x)];
    }
};
inline float LogLuma(float l) { return std::log2((std::max)(l, 0.f) + kLumaFloor); }
// Bilinear in log luminance, pixel-centre coordinates (x = 0.5 is the middle of column 0).
inline float LogAt(const Image& im, float x, float y)
{
    const float fx = x - .5f, fy = y - .5f;
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float ax = fx - float(x0), ay = fy - float(y0);
    const float a = LogLuma(im.At(x0, y0)), b = LogLuma(im.At(x0 + 1, y0));
    const float c = LogLuma(im.At(x0, y0 + 1)), d = LogLuma(im.At(x0 + 1, y0 + 1));
    return (a * (1 - ax) + b * ax) * (1 - ay) + (c * (1 - ax) + d * ax) * ay;
}

inline void SampleThumb(const Image& im, float* out)
{
    for (uint32_t j = 0; j < kThumbH; ++j)
        for (uint32_t i = 0; i < kThumbW; ++i) {
            float s = 0;
            for (uint32_t b = 0; b < kThumbTaps; ++b)
                for (uint32_t a = 0; a < kThumbTaps; ++a) {
                    const int x = int((float(i) + (float(a) + .5f) / kThumbTaps) / kThumbW * float(im.w));
                    const int y = int((float(j) + (float(b) + .5f) / kThumbTaps) / kThumbH * float(im.h));
                    s += LogLuma(im.At(x, y));
                }
            out[j * kThumbW + i] = s / float(kThumbTaps * kThumbTaps);
        }
}
// Zero keeps the original position. Other values encode a cell in the existing thumbnail.
struct TileLocations { uint32_t cell[kTiles]{}; };
inline int TileLeft(uint32_t k, uint32_t w, const TileLocations& loc = {})
{
    const uint32_t cell = loc.cell[k];
    const int centre = cell ? int((2 * ((cell - 1) % kThumbW) + 1) * w / (2 * kThumbW)) : int(kTileAt[k][0] * float(w));
    return centre - int(kTile / 2);
}
inline int TileTop(uint32_t k, uint32_t h, const TileLocations& loc = {})
{
    const uint32_t cell = loc.cell[k];
    const int centre = cell ? int((2 * ((cell - 1) / kThumbW) + 1) * h / (2 * kThumbH)) : int(kTileAt[k][1] * float(h));
    return centre - int(kTile / 2);
}
inline void SampleTiles(const Image& im, float* out, const TileLocations& loc = {})
{
    for (uint32_t k = 0; k < kTiles; ++k)
        for (uint32_t y = 0; y < kTile; ++y)
            for (uint32_t x = 0; x < kTile; ++x)
                out[(k * kTile + y) * kTile + x] = LogLuma(im.At(TileLeft(k, im.w, loc) + int(x), TileTop(k, im.h, loc) + int(y)));
}
// Grid point (i, j) of a w x h image: a pixel centre.
inline float PointX(uint32_t i, uint32_t w) { return std::floor((float(i) + .5f) / kGridW * float(w)) + .5f; }
inline float PointY(uint32_t j, uint32_t h) { return std::floor((float(j) + .5f) / kGridH * float(h)) + .5f; }

// Keep the 3x3 comparison patch inside the observed image. A disoccluded point
// has no prior-frame measurement; clamping it to the last texel fabricates one.
inline bool PatchVisible(float x, float y, uint32_t width, uint32_t height)
{
    return x >= 1.5f && y >= 1.5f && x <= float(width) - 1.5f && y <= float(height) - 1.5f;
}

struct MotionInputs {
    const Image* current = nullptr;   // this frame's DLSS input (luminance)
    const Image* previous = nullptr;  // the previous frame's DLSS input
    const std::vector<float>* motion = nullptr; // 2 floats a pixel at motion_w x motion_h
    uint32_t motion_w = 0, motion_h = 0;
    float scale_x = 1, scale_y = 1;   // DLSS's motion scale: pixels = vector * scale
    float jitter_now[2]{}, jitter_before[2]{}; // pixels, as DLSS was told
};
inline void SamplePoints(const MotionInputs& in, float* out)
{
    const Image& c = *in.current;
    const Image& p = *in.previous;
    const float dj[2] = {in.jitter_now[0] - in.jitter_before[0], in.jitter_now[1] - in.jitter_before[1]};
    for (uint32_t j = 0; j < kGridH; ++j)
        for (uint32_t i = 0; i < kGridW; ++i) {
            float* o = out + (j * kGridW + i) * kPointValues;
            const float x = PointX(i, c.w), y = PointY(j, c.h);
            // The vector at this place, read at the motion image's own size.
            const uint32_t mx = std::min(uint32_t(x / float(c.w) * float(in.motion_w)), in.motion_w - 1);
            const uint32_t my = std::min(uint32_t(y / float(c.h) * float(in.motion_h)), in.motion_h - 1);
            const size_t mi = (size_t(my) * in.motion_w + mx) * 2;
            const float vx = (*in.motion)[mi] * in.scale_x, vy = (*in.motion)[mi + 1] * in.scale_y;
            if (p.w != c.w || p.h != c.h || !PatchVisible(x, y, c.w, c.h) ||
                !PatchVisible(x + vx, y + vy, p.w, p.h) ||
                !PatchVisible(x + vx - dj[0], y + vy - dj[1], p.w, p.h) ||
                !PatchVisible(x + vx + dj[0], y + vy + dj[1], p.w, p.h)) {
                for (uint32_t k = 0; k < kPointValues; ++k) o[k] = -1;
                continue;
            }
            float e[6]{}, contrast = 0, delta[9], sum = 0;
            unsigned support = 0, mask = 0;
            const float shift[6][2] = {{0, 0}, {vx, vy}, {vx - dj[0], vy - dj[1]}, {vx + dj[0], vy + dj[1]},
                                       {.5f * vx - dj[0], .5f * vy - dj[1]}, {2 * vx - dj[0], 2 * vy - dj[1]}};
            for (int b = -1; b <= 1; ++b)
                for (int a = -1; a <= 1; ++a) {
                    const float cv = LogAt(c, x + float(a), y + float(b));
                    const float pv = LogAt(p, x + float(a), y + float(b));
                    const float d = cv - pv;
                    const unsigned tap = (b + 1) * 3 + a + 1;
                    delta[tap] = d;
                    // Both samples near the log floor have no reliable gain. An edge that
                    // becomes visible on just one frame is still evidence of a shape change.
                    if ((std::max)(cv, pv) > -10.f) { sum += d; ++support; mask |= 1u << tap; }
                    e[kStill] += std::fabs(d);
                    for (int h = 1; h < 6; ++h) e[h] += std::fabs(cv - LogAt(p, x + float(a) + shift[h][0], y + float(b) + shift[h][1]));
                    contrast += .5f * (std::fabs(LogAt(p, x + float(a) + 1, y + float(b)) - pv) + std::fabs(LogAt(p, x + float(a), y + float(b) + 1) - pv));
                }
            for (int h = 0; h < 6; ++h) o[h] = e[h] / 9.f;
            for (int h = int(kHalf); h <= int(kTwice); ++h)
                if (!PatchVisible(x + shift[h][0], y + shift[h][1], p.w, p.h)) o[h] = -1;
            // A multiplicative light change becomes one additive offset in log luminance.
            float structure = 0;
            const float inv_support = 1.f / (std::max)(support, 1u);
            for (unsigned k = 0; k < 9; ++k)
                if (mask & (1u << k)) structure += std::fabs(delta[k] - sum * inv_support);
            o[kStillStructure] = support >= 5 ? structure * inv_support : -1.f;
            o[kContrast] = contrast / 9.f;
            o[kLength] = std::sqrt(vx * vx + vy * vy);
        }
}

// ---------------------------------------------------------------------------------------------
// Scoring.

// Correlation of two equally sized arrays after removing each value's 3x3 neighbourhood mean
// (edges, not brightness). Values below -1e30 or not finite are holes. Returns NaN with too few.
inline float EdgeCorrelation(const float* a, const float* b, uint32_t w, uint32_t h, uint32_t border = 1)
{
    auto hp = [&](const float* s, uint32_t x, uint32_t y, float& out) {
        float sum = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const float v = s[(y + dy) * w + (x + dx)];
                if (!std::isfinite(v)) return false;
                sum += v;
            }
        out = s[y * w + x] - sum / 9.f;
        return true;
    };
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    uint32_t n = 0;
    const uint32_t m = (std::max)(border, 1u);
    for (uint32_t y = m; y + m < h; ++y)
        for (uint32_t x = m; x + m < w; ++x) {
            float u, v;
            if (!hp(a, x, y, u) || !hp(b, x, y, v)) continue;
            sa += u; sb += v; saa += double(u) * u; sbb += double(v) * v; sab += double(u) * v; ++n;
        }
    if (n < 16) return std::numeric_limits<float>::quiet_NaN();
    const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    if (va <= 1e-12 * n || vb <= 1e-12 * n) return std::numeric_limits<float>::quiet_NaN();
    return float((sab - sa * sb / n) / std::sqrt(va * vb));
}
// How much edge a block holds (mean absolute high-pass), to tell a flat tile from a textured one.
inline float EdgeEnergy(const float* a, uint32_t w, uint32_t h)
{
    double s = 0;
    uint32_t n = 0;
    for (uint32_t y = 1; y + 1 < h; ++y)
        for (uint32_t x = 1; x + 1 < w; ++x) {
            float sum = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) sum += a[(y + dy) * w + (x + dx)];
            const float v = a[y * w + x] - sum / 9.f;
            if (std::isfinite(v)) { s += std::fabs(v); ++n; }
        }
    return n ? float(s / n) : 0.f;
}

// A display can scale the scene to fit or centre it at one source pixel per display pixel.
// Sample the latter for small padding or a centred picture that fills one display axis.
// Odd padding has two possible origins
// on each odd axis. The normalized screen occupies the original block even when testing these.
inline uint32_t PaddedLayouts(uint32_t w, uint32_t h, uint32_t sw, uint32_t sh)
{
    if (w < kTile || h < kTile || sw < w || sh < h || (w == sw && h == sh)) return 0;
    const uint32_t dx = sw - w, dy = sh - h;
    const bool close = dx <= (std::min)(16u, w / 16) && dy <= (std::min)(16u, h / 16);
    const bool fits_one_axis = (!dx && uint64_t(h) * 2 >= sh) || (!dy && uint64_t(w) * 2 >= sw);
    if (!close && !fits_one_axis) return 0;
    return (1u + (dx & 1u)) * (1u + (dy & 1u));
}

// Pick one geometry for the whole frame using the structure common to both candidates.
// A HUD/overlay patch or an isolated reflection cannot choose a different shift for itself.
inline uint32_t RegisterScreenTiles(const float* data, const float* padded, uint32_t layouts, bool& scaled_matches)
{
    scaled_matches = false;
    if (!layouts || layouts > 4 || !padded) return 0;
    float gains[4]{};
    int supporters[4]{};
    uint32_t scaled_support = 0;
    for (uint32_t tile = 0; tile < kTiles; ++tile) {
        const auto at = tile * kTile * kTile;
        const auto* input = data + BlockOffset(kTileInput) + at;
        const auto* output = data + BlockOffset(kTileOutput) + at;
        float reference[kTile * kTile];
        for (uint32_t i = 0; i < kTile * kTile; ++i) reference[i] = .5f * (input[i] + output[i]);
        const float scaled = EdgeCorrelation(reference, data + BlockOffset(kTileScreen) + at, kTile, kTile);
        if (!std::isfinite(scaled)) continue;
        if (scaled >= .45f) ++scaled_support;
        for (uint32_t i = 0; i < layouts; ++i) {
            const float centered = EdgeCorrelation(reference, padded + i * kTileFloats + at, kTile, kTile);
            if (!std::isfinite(centered)) continue;
            const float gain = centered - scaled;
            gains[i] += std::clamp(gain, -.5f, .5f);
            if (gain > .09f && centered >= .45f) ++supporters[i];
            if (gain < -.09f) --supporters[i];
        }
    }
    float best = .6f;
    uint32_t selected = 0;
    for (uint32_t i = 0; i < layouts; ++i) {
        if (supporters[i] >= 3 && gains[i] > best) {
            best = gains[i];
            selected = i + 1;
        }
    }
    scaled_matches = scaled_support >= 3;
    return selected;
}

struct Params {
    float picture_match = .45f, picture_partial = .2f;      // edge correlation, input vs screen
    float screen_edges = .01f;         // a screen thumbnail with this much edge can be compared
    float tile_energy = .02f;          // a tile needs this much edge (log2 units) to count
    float tile_margin = .02f;          // output and input correlation must differ by this to decide
    float tile_seen = .3f;             // and the better one must reach this, else the screen shows neither
    float point_contrast = .03f;       // a motion point needs this much contrast to count
    float point_moving = .5f;          // pixels: a vector this long is motion
    float point_changed = .08f;        // still error (log2) above which the picture changed there
    float jitter_least = .2f;          // pixels: the jitter must change by this to be judged
    float jitter_margin = .9f;         // a hypothesis wins a point when it is under this share of the next
};

// One sample: what the card (or the reference) measured for one evaluation and its screen.
struct Sample {
    const float* data = nullptr;       // kResultFloats, laid out as above
    bool have_screen = false;          // the presented image was sampled
    bool tiles_comparable = false;     // sampled on the same pixel grid (native or normalized)
    bool have_motion = false;          // the previous evaluation was the frame before, same size
    float jitter_change = 0;           // |j_now - j_before| in pixels
    const float* registered_screen_tiles = nullptr;
};

struct TileVotes { uint8_t input = 0, output = 0, confirmed_input = 0; };

struct SampleScore {
    float picture = std::numeric_limits<float>::quiet_NaN();       // input vs screen
    float picture_output = std::numeric_limits<float>::quiet_NaN(); // output vs screen
    uint32_t tiles = 0, tiles_textured = 0, tiles_output = 0, tiles_input = 0, tiles_neither = 0;
    uint32_t points = 0, textured = 0, moving = 0, explained = 0, changed_still = 0, still_points = 0;
    TileVotes local; // confidently matched registered tiles in this sample only
    uint8_t textured_tiles = 0;
    bool verified_mixed = false; // independently supported input and output in the same registered screen
    uint32_t light_compatible = 0, ambiguous_still = 0; // brightness-only fits and inconclusive residuals
    uint32_t half_better = 0, twice_better = 0; // moving points a scaled vector predicts clearly better
    double err_still = 0, err_vector = 0;   // sums over moving textured points
    uint32_t jitter_points = 0, jitter_follows = 0, jitter_reverse = 0, jitter_none = 0;
    uint32_t placement = kFollows;           // the jitter placement the motion was judged with
};

inline SampleScore Score(const Sample& s, const Params& p = Params())
{
    SampleScore r;
    const float* d = s.data;
    if (!d) return r;
    if (s.have_screen) {
        r.picture = EdgeCorrelation(d + BlockOffset(kThumbInput), d + BlockOffset(kThumbScreen), kThumbW, kThumbH);
        r.picture_output = EdgeCorrelation(d + BlockOffset(kThumbOutput), d + BlockOffset(kThumbScreen), kThumbW, kThumbH);
        // An input without edges under a screen with edges (a flat or smooth layer) looks nothing like it.
        const float screen_edges = EdgeEnergy(d + BlockOffset(kThumbScreen), kThumbW, kThumbH);
        if (!std::isfinite(r.picture) && screen_edges >= p.screen_edges) r.picture = 0;
        if (!std::isfinite(r.picture_output) && screen_edges >= p.screen_edges) r.picture_output = 0;
    }
    if (s.have_screen && s.tiles_comparable) {
        for (uint32_t k = 0; k < kTiles; ++k) {
            const uint32_t at = k * kTile * kTile;
            const float* ti = d + BlockOffset(kTileInput) + at;
            const float* to = d + BlockOffset(kTileOutput) + at;
            const float* ts = (s.registered_screen_tiles ? s.registered_screen_tiles : d + BlockOffset(kTileScreen)) + at;
            ++r.tiles;
            if (EdgeEnergy(to, kTile, kTile) < p.tile_energy) continue;
            ++r.tiles_textured;
            r.textured_tiles |= uint8_t(1u << k);
            const float co = EdgeCorrelation(to, ts, kTile, kTile), ci = EdgeCorrelation(ti, ts, kTile, kTile);
            if (!std::isfinite(co) || !std::isfinite(ci) || (std::max)(co, ci) < p.tile_seen) { ++r.tiles_neither; continue; }
            if (co - ci >= p.tile_margin) ++r.tiles_output;
            else if (ci - co >= p.tile_margin) ++r.tiles_input;
            if (co >= .85f && co - ci >= p.tile_margin) r.local.output |= uint8_t(1u << k);
            if (ci >= .85f && ci - co >= p.tile_margin) r.local.input |= uint8_t(1u << k);
        }
    }
    if (s.have_motion) {
        // The jitter first: which of the three placements explains most explained points. The
        // motion is then judged with that one placement for every point, so a jitter guess can
        // never make up for a wrong vector point by point.
        const bool judge_jitter = s.jitter_change >= p.jitter_least;
        const float* pts = d + BlockOffset(kPoints);
        uint32_t eligible = 0;
        for (uint32_t i = 0; i < kGridW * kGridH; ++i) {
            const float* o = pts + i * kPointValues;
            if (o[kStill] < 0 || o[kContrast] < p.point_contrast || !judge_jitter) continue;
            ++eligible;
            const float f = o[kFollows], v = o[kReverse], n = o[kVector];
            // Only points the vector explains vote: where nothing explains the change (a missing or
            // wrong vector), a jitter placement that happens to be nearer the truth proves nothing.
            if ((std::min)({f, v, n}) >= .6f * o[kStill]) continue;
            if (f < p.jitter_margin * (std::min)(v, n)) ++r.jitter_follows;
            else if (v < p.jitter_margin * (std::min)(f, n)) ++r.jitter_reverse;
            else if (n < p.jitter_margin * (std::min)(f, v)) ++r.jitter_none;
            ++r.jitter_points;
        }
        // A placement other than the one DLSS was told needs a tenth of the textured points behind it.
        PointValue placed = kFollows;
        const bool enough = (r.jitter_follows + r.jitter_reverse + r.jitter_none) * 10 >= eligible;
        if (judge_jitter && enough && r.jitter_none > r.jitter_follows && r.jitter_none >= r.jitter_reverse) placed = kVector;
        else if (judge_jitter && enough && r.jitter_reverse > r.jitter_follows && r.jitter_reverse > r.jitter_none) placed = kReverse;
        r.placement = placed;
        for (uint32_t i = 0; i < kGridW * kGridH; ++i) {
            const float* o = pts + i * kPointValues;
            if (o[kStill] < 0) continue;
            ++r.points;
            if (o[kContrast] < p.point_contrast) continue;
            ++r.textured;
            const float e = o[placed];
            if (o[kLength] >= p.point_moving) {
                ++r.moving;
                r.err_still += o[kStill]; r.err_vector += e;
                if (e < .8f * o[kStill]) ++r.explained;
                const float best_unscaled = (std::min)({o[kFollows], o[kReverse], o[kVector]});
                if (o[kHalf] >= 0 && o[kHalf] < .7f * best_unscaled) ++r.half_better;
                if (o[kTwice] >= 0 && o[kTwice] < .7f * best_unscaled) ++r.twice_better;
            } else {
                ++r.still_points;
                // Changed, and the jitter does not account for it.
                if (o[kStill] > p.point_changed && e > .8f * o[kStill]) {
                    const float residual = o[kStillStructure];
                    if (residual >= 0 && residual <= (std::min)(p.point_changed * .5f, .25f * o[kStill]))
                        ++r.light_compatible;
                    else if (residual > p.point_changed) ++r.changed_still;
                    else ++r.ambiguous_still;
                }
            }
        }
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// Relocate empty probes using the thumbnails already read back. Choose shared scene and
// screen detail in each of eight regions. Never use which candidate won to choose a location.
inline TileLocations FindTexturedTiles(const float* data, bool distinguish = false, TileLocations loc = {})
{
    const float* input = data + BlockOffset(kThumbInput);
    const float* output = data + BlockOffset(kThumbOutput);
    const float* screen = data + BlockOffset(kThumbScreen);
    auto texture = [](const float* q) {
        return std::fabs(q[1] - q[-1]) + std::fabs(q[kThumbW] - q[-int(kThumbW)]);
    };
    for (uint32_t k = 0; k < kTiles; ++k) {
        if (distinguish && !((k + k / 4) & 1u)) continue;
        const uint32_t x0 = k % 4 * (kThumbW / 4), y0 = k / 4 * (kThumbH / 2);
        float best = 0;
        for (uint32_t y = y0 + 3; y + 3 < y0 + kThumbH / 2; y += 2)
            for (uint32_t x = x0 + 3; x + 3 < x0 + kThumbW / 4; x += 2) {
                const uint32_t centre = y * kThumbW + x;
                float strength = 0;
                for (int dy : {-1, 1}) for (int dx : {-1, 1}) {
                    const int i = int(centre) + dy * int(kThumbW) + dx;
                    const float a = texture(input + i), b = texture(screen + i);
                    if (!std::isfinite(a) || !std::isfinite(b)) continue;
                    // Where equally textured inputs make attribution ambiguous,
                    // spend half the probes on regions which can tell them apart.
                    // Select symmetrically from the two source images, without
                    // consulting which one won the comparison at the screen.
                    const float weight = distinguish ? std::fabs(a - texture(output + i)) : 1.f;
                    strength += (std::min)(a, b) * weight;
                }
                if (strength > best) { best = strength; loc.cell[k] = centre + 1; }
            }
    }
    return loc;
}

// A window of samples (a few seconds) and its verdicts, as the log prints them.
struct Window {
    uint32_t samples = 0, screens = 0, motions = 0;
    uint32_t textured_frames = 0, output_frames = 0, input_frames = 0, neither_frames = 0, mixed_frames = 0;
    double picture_sum = 0, picture_output_sum = 0; uint32_t picture_n = 0;
    float picture_min = 2, picture_max = -2;
    SampleScore total;
    void Add(const SampleScore& s)
    {
        ++samples;
        if (std::isfinite(s.picture)) {
            ++screens; ++picture_n;
            picture_sum += s.picture;
            picture_output_sum += std::isfinite(s.picture_output) ? s.picture_output : s.picture;
            picture_min = (std::min)(picture_min, s.picture); picture_max = (std::max)(picture_max, s.picture);
        }
        if (s.points) ++motions;
        total.tiles += s.tiles; total.tiles_textured += s.tiles_textured; total.tiles_output += s.tiles_output;
        total.tiles_input += s.tiles_input; total.tiles_neither += s.tiles_neither;
        if (s.verified_mixed) ++mixed_frames;
        if (s.tiles_textured >= 4) {
            ++textured_frames;
            const uint32_t decided = s.tiles_output + s.tiles_input;
            if (s.tiles_neither * 2 > s.tiles_textured) ++neither_frames;
            else if (decided >= 4 && decided * 2 >= s.tiles_textured) {
                if (!s.verified_mixed && s.tiles_output >= 2 && s.tiles_input >= 2) ++mixed_frames;
                if (s.tiles_output * 4 >= decided * 3) ++output_frames;
                else if (s.tiles_output < 2 && s.tiles_input * 4 >= decided * 3) ++input_frames;
            }
        }
        total.points += s.points; total.textured += s.textured; total.moving += s.moving; total.explained += s.explained;
        total.changed_still += s.changed_still; total.still_points += s.still_points;
        total.light_compatible += s.light_compatible; total.ambiguous_still += s.ambiguous_still;
        total.half_better += s.half_better; total.twice_better += s.twice_better;
        total.err_still += s.err_still; total.err_vector += s.err_vector;
        total.jitter_points += s.jitter_points; total.jitter_follows += s.jitter_follows;
        total.jitter_reverse += s.jitter_reverse; total.jitter_none += s.jitter_none;
    }
    float Picture() const { return picture_n ? float(picture_sum / picture_n) : std::numeric_limits<float>::quiet_NaN(); }
};

enum class Verdict : uint32_t { NotMeasured, Pass, Partial, Fail };
inline const char* VerdictName(Verdict v)
{
    return v == Verdict::Pass ? "pass" : v == Verdict::Partial ? "partial" : v == Verdict::Fail ? "fail" : "not_measured";
}
struct Verdicts {
    Verdict picture = Verdict::NotMeasured, result = Verdict::NotMeasured, motion = Verdict::NotMeasured, jitter = Verdict::NotMeasured;
    const char* picture_why = "no screen sampled";
    const char* result_why = "no screen sampled";
    const char* motion_why = "no pair of consecutive frames";
    const char* jitter_why = "no pair of consecutive frames";
};
inline Verdicts Judge(const Window& w, const Params& p = Params())
{
    Verdicts v;
    if (w.picture_n) {
        const float s = w.Picture();
        if (s >= p.picture_match) { v.picture = Verdict::Pass; v.picture_why = "the image DLSS ran on has the screen's edges"; }
        else if (s >= p.picture_partial) { v.picture = Verdict::Partial; v.picture_why = "some of the screen's edges: post effects, HUD or a partly matching layer"; }
        else { v.picture = Verdict::Fail; v.picture_why = "the image DLSS ran on does not look like the screen"; }
    }
    const auto& t = w.total;
    if (t.tiles) {
        const uint32_t decided = w.output_frames + w.input_frames;
        if (!w.textured_frames) v.result_why = "too few textured tiles per frame";
        else if (w.neither_frames * 2 > w.textured_frames) { v.result = Verdict::Fail; v.result_why = "the screen shows neither DLSS's input nor its output at the tiles"; }
        else if (w.output_frames && w.input_frames) { v.result = Verdict::Partial; v.result_why = "some sampled screens favor the output and others the input"; }
        else if (!decided || decided * 2 < w.textured_frames) { v.result = Verdict::Partial; v.result_why = "too few sampled frames distinguish DLSS input and output"; }
        else if (w.output_frames * 4 >= decided * 3) { v.result = Verdict::Pass; v.result_why = "the screen shows DLSS's output"; }
        else if (w.input_frames * 4 >= decided * 3) { v.result = Verdict::Fail; v.result_why = "the screen shows the image before DLSS, not its output"; }
        else { v.result = Verdict::Partial; v.result_why = "the tiles disagree on which image reached the screen"; }
    } else if (w.screens) v.result_why = "input, output and screen do not share the same extent";
    if (t.points) {
        const uint32_t judged_still = t.still_points;
        if (t.textured < 64) v.motion_why = "too few textured points";
        else if (t.moving * 50 < t.textured) {
            if (judged_still && t.changed_still * 2 > judged_still) { v.motion = Verdict::Fail; v.motion_why = "local patterns changed where the vectors say still"; }
            else if (t.light_compatible > judged_still / 2) v.motion_why = "brightness changed while local patterns held";
            else { v.motion_why = "too few moving vectors to judge motion across the scene"; }
        } else {
            const double ratio = t.err_still > 0 ? t.err_vector / t.err_still : 1.0;
            const double share = double(t.explained) / t.moving;
            if (t.twice_better * 2 >= t.moving) { v.motion = Verdict::Fail; v.motion_why = "the vectors are about half the real motion"; }
            else if (t.half_better * 2 >= t.moving) { v.motion = Verdict::Fail; v.motion_why = "the vectors are about twice the real motion"; }
            else if (ratio <= .6 && share >= .6) { v.motion = Verdict::Pass; v.motion_why = "the vectors predict the frame"; }
            else if (ratio <= .9) { v.motion = Verdict::Partial; v.motion_why = "the vectors predict part of the frame"; }
            else { v.motion = Verdict::Fail; v.motion_why = "the vectors do not predict the frame better than standing still"; }
        }
    }
    if (t.jitter_points && v.motion == Verdict::Fail) {
        v.jitter_why = "not judged while the vectors are wrong";
    } else if (t.jitter_points) {
        const uint32_t decided = t.jitter_follows + t.jitter_reverse + t.jitter_none;
        if (decided < 32) { v.jitter_why = "too few points where the jitter shows"; }
        else if (t.jitter_follows * 2 > decided) { v.jitter = Verdict::Pass; v.jitter_why = "the picture shifts by the jitter DLSS was told"; }
        else if (t.jitter_reverse * 2 > decided) { v.jitter = Verdict::Fail; v.jitter_why = "the picture shifts by the jitter in the opposite sense"; }
        else if (t.jitter_none * 2 > decided) { v.jitter = Verdict::Fail; v.jitter_why = "the picture does not shift with the jitter"; }
        else { v.jitter = Verdict::Partial; v.jitter_why = "no clear answer"; }
    } else if (t.points) v.jitter_why = "the jitter barely changed between the pairs";
    return v;
}

// The log line (event `referee`), one per window.
inline int Format(char* out, size_t n, const Window& w, const Verdicts& v)
{
    const auto& t = w.total;
    const double ratio = t.err_still > 0 ? t.err_vector / t.err_still : -1.0;
    return std::snprintf(out, n,
        "samples=%u screens=%u pairs=%u "
        "picture{verdict=%s edges=%.3f min=%.3f max=%.3f output_edges=%.3f why=\"%s\"} "
        "result{verdict=%s tiles=%u textured=%u output=%u input=%u neither=%u frame_votes=%u/%u output_frames=%u input_frames=%u neither_frames=%u why=\"%s\"} "
        "motion{verdict=%s points=%u textured=%u moving=%u explained=%u error_ratio=%.3f half_better=%u twice_better=%u changed_where_still=%u of=%u light_compatible=%u ambiguous_still=%u why=\"%s\"} "
        "jitter{verdict=%s points=%u follows=%u reverse=%u none=%u why=\"%s\"}",
        w.samples, w.screens, w.motions,
        VerdictName(v.picture), double(w.Picture()), w.picture_n ? double(w.picture_min) : 0.0, w.picture_n ? double(w.picture_max) : 0.0,
        w.picture_n ? w.picture_output_sum / w.picture_n : 0.0, v.picture_why,
        VerdictName(v.result), t.tiles, t.tiles_textured, t.tiles_output, t.tiles_input, t.tiles_neither,
        w.output_frames + w.input_frames + w.neither_frames, w.textured_frames,
        w.output_frames, w.input_frames, w.neither_frames, v.result_why,
        VerdictName(v.motion), t.points, t.textured, t.moving, t.explained, ratio, t.half_better, t.twice_better, t.changed_still, t.still_points, t.light_compatible, t.ambiguous_still, v.motion_why,
        VerdictName(v.jitter), t.jitter_points, t.jitter_follows, t.jitter_reverse, t.jitter_none, v.jitter_why);
}

} // namespace na::referee
