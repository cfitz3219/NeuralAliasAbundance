#pragma once
#include <cstdint>

namespace diagnostics {
enum class ResetReason { none, first_frame, missing_inputs, disabled, discontinuity,
    layout_change, display_reset, adapter_transition, manual };
inline const char* reset_name(ResetReason reason) {
    switch (reason) {
    case ResetReason::none: return "continuous";
    case ResetReason::first_frame: return "first usable frame";
    case ResetReason::missing_inputs: return "missing candidate inputs";
    case ResetReason::disabled: return "capture disabled";
    case ResetReason::discontinuity: return "frame/time discontinuity";
    case ResetReason::layout_change: return "input layout changed";
    case ResetReason::display_reset: return "display resources reset";
    case ResetReason::adapter_transition: return "adapter transition";
    case ResetReason::manual: return "manual reset";
    }
    return "unknown";
}
struct FrameObservation {
    uint64_t frame=0, time_ms=0;
    uint32_t width=0, height=0, color_format=0, depth_format=0, motion_format=0;
    bool enabled=true, usable=false;
};
struct HistoryDecision {
    bool usable=false, reset=false;
    uint64_t generation=0;
    ResetReason reason=ResetReason::first_frame;
};
// Metadata gate only. The caller must establish actual input validity. A backend
// must consume reset before evaluating this frame; this class owns no GPU history.
class FrameHistory {
public:
    void invalidate(ResetReason reason) {
        if (reason!=ResetReason::none) { pending_=reason; continuous_=false; }
    }
    HistoryDecision observe(const FrameObservation& frame) {
        if (!frame.enabled || !frame.usable || !frame.width || !frame.height) {
            if (continuous_ || pending_==ResetReason::first_frame)
                invalidate(frame.enabled ? ResetReason::missing_inputs : ResetReason::disabled);
            return {false,false,generation_,pending_};
        }
        if (continuous_) {
            if (frame.frame<=previous_.frame || frame.frame-previous_.frame!=1 ||
                frame.time_ms<previous_.time_ms || frame.time_ms-previous_.time_ms>250)
                invalidate(ResetReason::discontinuity);
            else if (frame.width!=previous_.width || frame.height!=previous_.height ||
                frame.color_format!=previous_.color_format || frame.depth_format!=previous_.depth_format ||
                frame.motion_format!=previous_.motion_format) invalidate(ResetReason::layout_change);
        }
        const bool reset=!continuous_;
        const auto reason=reset ? pending_ : ResetReason::none;
        if (reset) ++generation_;
        previous_=frame;continuous_=true;pending_=ResetReason::none;
        return {true,reset,generation_,reason};
    }
private:
    FrameObservation previous_{};
    bool continuous_=false;
    uint64_t generation_=0;
    ResetReason pending_=ResetReason::first_frame;
};
}
