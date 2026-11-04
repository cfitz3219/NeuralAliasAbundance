#include "bridge/frame_record.hpp"

#include <cmath>

namespace na::bridge {

const char* ModeName(Mode m)
{
    switch (m) {
    case Mode::Performance: return "Performance";
    case Mode::Balanced: return "Balanced";
    case Mode::Quality: return "Quality";
    case Mode::UltraPerformance: return "Ultra Performance";
    case Mode::UltraQuality: return "Ultra Quality";
    case Mode::DLAA: return "DLAA";
    }
    return "unknown";
}

Mode ModeForScale(uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h)
{
    if (!render_w || !render_h || !out_w || !out_h) return Mode::DLAA;
    if (render_w >= out_w && render_h >= out_h) return Mode::DLAA;
    // Average per-axis scale, compared with DLSS's nominal factors.
    const double scale = 0.5 * (double(out_w) / render_w + double(out_h) / render_h);
    struct Entry { Mode mode; double factor; };
    const Entry table[] = {
        {Mode::UltraQuality, 1.3}, {Mode::Quality, 1.5}, {Mode::Balanced, 1.7241},
        {Mode::Performance, 2.0}, {Mode::UltraPerformance, 3.0},
    };
    Mode best = Mode::Quality;
    double best_d = 1e9;
    for (const Entry& e : table) {
        const double d = std::fabs(scale - e.factor);
        if (d < best_d) { best_d = d; best = e.mode; }
    }
    return best;
}

const char* ValidateLimits(const SessionLimits& l)
{
    if (!l.max_render_w || !l.max_render_h || !l.output_w || !l.output_h)
        return "The bridge was started with a zero size.";
    if (l.max_render_w > kMaxExtent || l.max_render_h > kMaxExtent || l.output_w > kMaxExtent || l.output_h > kMaxExtent)
        return "The bridge was started with a size larger than 8192 pixels.";
    if (l.max_render_w > l.output_w || l.max_render_h > l.output_h)
        return "The render size is larger than the output size.";
    return nullptr;
}

const char* Validate(const FrameRecord& f, const SessionLimits& l, uint32_t expected_sequence)
{
    if (const char* r = ValidateLimits(l)) return r;
    if (f.version != kRecordVersion) return "The frame record is from a different bridge version.";
    if (f.sequence != expected_sequence) return "The frame record is out of order.";
    if (!f.render_w || !f.render_h) return "The frame has no render size.";
    if (f.render_w > l.max_render_w || f.render_h > l.max_render_h)
        return "The frame's render size is larger than the bridge was started with.";
    if (f.output_w != l.output_w || f.output_h != l.output_h)
        return "The frame's output size differs from the bridge's output size.";
    if (!std::isfinite(f.jitter_x) || !std::isfinite(f.jitter_y) || std::fabs(f.jitter_x) > 1.f || std::fabs(f.jitter_y) > 1.f)
        return "The frame's jitter is not a sub-pixel offset.";
    if (!std::isfinite(f.mv_scale_x) || !std::isfinite(f.mv_scale_y) || f.mv_scale_x == 0.f || f.mv_scale_y == 0.f)
        return "The frame's motion scale is zero or not a number.";
    if (!std::isfinite(f.pre_exposure) || f.pre_exposure <= 0.f || !std::isfinite(f.exposure_scale) || f.exposure_scale <= 0.f)
        return "The frame's exposure is zero, negative or not a number.";
    if (!std::isfinite(f.frame_ms) || f.frame_ms <= 0.f || f.frame_ms > 1000.f)
        return "The frame time is outside 0-1000 ms.";
    if (f.mode > static_cast<uint32_t>(Mode::DLAA)) return "The frame asks for an unknown DLSS mode.";
    if (f.flags & ~static_cast<uint32_t>(kKnownFlags)) return "The frame record carries unknown flags.";
    const bool same_size = f.render_w == f.output_w && f.render_h == f.output_h;
    if (static_cast<Mode>(f.mode) == Mode::DLAA && !same_size)
        return "DLAA was asked for, but the render size is smaller than the output.";
    return nullptr;
}

} // namespace na::bridge
