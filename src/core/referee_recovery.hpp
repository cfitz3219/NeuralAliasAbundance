#pragma once

#include "core/referee_trial.hpp"
#include <tuple>

namespace na::referee {

// VARIANT: referee/stage-recovery
// Bits describe something the recorded frame actually used. Each alternative
// changes one contributor and keeps the user's saved options intact.
enum class RecoveryTool : uint8_t { None = 0, Bloom = 1, AmbientHistory = 2, CameraResidual = 4, ScreenFit = 8, Sharpen = 16 };
constexpr uint8_t Bit(RecoveryTool tool) { return static_cast<uint8_t>(tool); }
inline const char* ToolName(RecoveryTool tool)
{
    switch (tool) {
    case RecoveryTool::Bloom: return "reduce_bloom";
    case RecoveryTool::AmbientHistory: return "current_frame_ambient_shading";
    case RecoveryTool::CameraResidual: return "camera_without_residual";
    case RecoveryTool::ScreenFit: return "directly_measured_motion";
    case RecoveryTool::Sharpen: return "reduce_sharpening";
    default: return "observe";
    }
}
inline RecoveryTool NextTool(FlickerStage stage, uint8_t available)
{
    using T = RecoveryTool;
    const auto has = [available](T t) { return (available & Bit(t)) != 0; };
    switch (stage) {
    case FlickerStage::Input:
        if (has(T::Bloom)) return T::Bloom;
        if (has(T::AmbientHistory)) return T::AmbientHistory;
        break;
    case FlickerStage::Reconstruction:
        if (has(T::CameraResidual)) return T::CameraResidual;
        if (has(T::ScreenFit)) return T::ScreenFit;
        break;
    case FlickerStage::Composition:
        if (has(T::Sharpen)) return T::Sharpen;
        break;
    default: break;
    }
    return T::None;
}

class StageRecovery {
public:
    using Tool = RecoveryTool;
    struct Scene {
        uint64_t color = 0, depth = 0;
        uint32_t width = 0, height = 0, color_sub = 0, depth_sub = 0;
        uint32_t color_slice = 0, depth_slice = 0, x = 0, y = 0;
        bool inline_route = false;
        uint64_t reader = 0, generation = 0, selection = 0;
        auto Key() const {
            return std::tie(color, depth, width, height, color_sub, depth_sub, color_slice, depth_slice,
                            x, y, inline_route, reader, generation, selection);
        }
        bool operator==(const Scene& other) const { return Key() == other.Key(); }
    };
    explicit StageRecovery(bool dx12 = false) : domain_(dx12 ? (uint64_t{1} << 59) : (uint64_t{1} << 60)) {}
    bool Matches(const Scene& scene) const { return watch_ && scene == scene_; }
    void Clear()
    {
        if (!watch_) return;
        for (auto& e : entries_) e.used = 0;
        last_ = 0;
        scene_ = {};
        Observe();
        watch_ = 0;
    }
    // Scene resources belong to one device and presentation lifetime. The
    // owner clears the ledger on resize, device destruction or user opt-out.
    void SetScene(Scene scene)
    {
        if (Matches(scene)) return;
        scene_ = scene;
        Observe();
    }
    void Suspend() { if (Busy() || previous_.ordinal) Observe(); }
    bool Busy() const { return tool_ != Tool::None; }
    Tool ToolInUse() const { return trial_.UsesCandidate() && Busy() ? tool_ : Tool::None; }
    uint64_t Configuration() const { return Busy() ? entries_[entry_].example.configuration : 0; }
    uint64_t Ticket(uint64_t frame) const { return Busy() ? trial_.Ticket(frame) : watch_; }
    // High bit separates this mailbox from the placement-trial namespace.
    void Tick(uint64_t now)
    {
        if (!Busy()) return;
        trial_.Tick(now, true);
        if (trial_.Current() == RouteTrial::Phase::Rejected) Finish(now, false, trial_.Reason());
        else if (trial_.Current() == RouteTrial::Phase::Kept && now - last_witness_ >= RouteTrial::maximum_ms)
            Finish(now, false, "no recent comparable screen evidence");
    }
    void Add(uint64_t ticket, const TrialSample& s, uint64_t next_frame, uint64_t now)
    {
        if (!ticket || ticket != Ticket(s.ordinal) || !s.ordinal || s.ordinal <= last_ || s.broad_light ||
            s.ordinal >= next_frame || next_frame - s.ordinal > 240 ||
            (s.observed_at && now > s.observed_at && now - s.observed_at > 4000)) return;
        last_ = s.ordinal;
        if (Busy()) {
            auto& e = entries_[entry_];
            if (!SameSetup(e.example, s)) { Finish(now, false, "display or evaluation settings changed"); return; }
            const uint8_t need = Bit(tool_);
            if (s.comparable && !SameView(e.example, s)) { Finish(now, false, "the view changed"); return; }
            lost_view_ = s.comparable || !trial_.UsesCandidate() ? 0 : lost_view_ + 1;
            if (lost_view_ >= 2) { Finish(now, false, "two screen pairs no longer match the view"); return; }
            // Check execution on both accepted frames of each pair. A
            // candidate cannot earn credit for an absent or inoperative pass.
            if (!((trial_.UsesCandidate() ? s.applied : s.tools) & need)) {
                Finish(now, false, "the selected control did not run on both frames");
                return;
            }
            const bool candidate = trial_.UsesCandidate();
            if (tool_ == Tool::ScreenFit || tool_ == Tool::CameraResidual) {
                if (!(s.motion_error >= 0)) return;
                if (candidate && s.motion_error > original_motion_ * 1.30f + .025f) {
                    if (++motion_losses_ >= 2) { Finish(now, true, "motion predicted the image less accurately"); return; }
                } else motion_losses_ = 0;
            }
            if (!candidate && (!s.delivered || !s.comparable)) return;
            if (!candidate && s.origin.stage != FlickerStage::Unknown &&
                (s.origin.stage != e.example.origin.stage || s.origin.region != e.example.origin.region)) return;
            trial_.Add(ticket, s, next_frame);
            if (s.comparable && s.delivered) last_witness_ = now;
            if (trial_.Current() == RouteTrial::Phase::Rejected) Finish(now, trial_.RuledOut(), trial_.Reason());
            return;
        }
        if (!s.comparable || !s.delivered || s.origin.stage == FlickerStage::Unknown ||
            s.origin.region >= FlickerTransition::count) return;
        // An earlier stage that moves strongly in the opposite direction is
        // ambiguous (e.g. a nonlinear composite). It cannot authorize a tool.
        const unsigned first = unsigned(s.origin.stage) - 1;
        if (first >= s.origin.stops.size()) return;
        const float upstream_floor = s.origin.recovery_floor > 0.f ? s.origin.recovery_floor * (.085f / .12f) : .085f;
        for (unsigned i = 0; i < first; ++i) if (std::fabs(s.origin.stops[i]) >= upstream_floor) return;
        const bool reversed = previous_.ordinal && s.ordinal - previous_.ordinal <= 240 &&
            now - previous_at_ < RouteTrial::maximum_ms && SameSetup(previous_, s) && SameView(previous_, s) &&
            s.origin.region == previous_.origin.region && ReversesAtSameRegion(previous_, s);
        uint8_t available = s.tools & previous_.tools;
        if (!(s.motion_error >= 0 && previous_.motion_error >= 0))
            available &= uint8_t(~(Bit(Tool::CameraResidual) | Bit(Tool::ScreenFit)));
        previous_ = s;
        previous_at_ = now;
        if (!reversed) return;
        auto& e = Entry(s);
        auto eligible = uint8_t(available & ~e.ruled_out);
        for (unsigned i = 0; i < e.after.size(); ++i)
            if (now < e.after[i]) eligible &= uint8_t(~(1u << i));
        tool_ = NextTool(s.origin.stage, eligible & ~e.attempted);
        if (tool_ == Tool::None) tool_ = NextTool(s.origin.stage, eligible);
        if (!Busy()) {
            if (!e.reported || e.last_available != available) {
                ++exhausted_;
                e.reported = true;
                e.last_available = available;
                why_ = NextTool(s.origin.stage, available) == Tool::None ? "no submitted control matches the measured stage" :
                    NextTool(s.origin.stage, uint8_t(available & ~e.ruled_out)) == Tool::None ?
                    "the available controls have been ruled out here" : "waiting before repeating an inconclusive comparison";
            }
            return;
        }
        e.reported = false;
        e.attempted |= Bit(tool_);
        last_witness_ = now;
        original_motion_ = s.motion_error;
        lost_view_ = motion_losses_ = 0;
        trial_.StartAt(Serial(), next_frame, now, s);
    }
    Tool Selected() const { return tool_; }
    const RouteTrial& Comparison() const { return trial_; }
    uint8_t Excluded() const { return entries_[entry_].ruled_out; }
    FlickerStage Origin() const { return entries_[entry_].example.origin.stage; }
    unsigned Region() const { return entries_[entry_].example.origin.region; }
    uint64_t Exhausted() const { return exhausted_; }
    const char* Reason() const { return Busy() ? trial_.Reason() : why_; }

private:
    static bool ReversesAtSameRegion(const TrialSample& a, const TrialSample& b)
    {
        if (!(a.origin.recovery_floor > 0.f && b.origin.recovery_floor > 0.f)) return Reverses(a.origin, b.origin);
        const auto region = b.origin.region;
        const float earlier = std::fabs(a.origin.stops[2]), later = std::fabs(b.origin.stops[2]);
        return a.origin.stage == b.origin.stage && (std::min)(earlier, later) * 3 >= (std::max)(earlier, later) &&
               TrialDirection(a.change[region], b.change[region]) < 0;
    }
    struct EntryData {
        Scene scene{};
        TrialSample example;
        uint64_t used = 0;
        uint8_t ruled_out = 0, attempted = 0;
        bool reported = false;
        uint8_t last_available = 0;
        std::array<uint64_t, 5> after{};
    };
    static bool SameSetup(const TrialSample& a, const TrialSample& b)
    {
        return a.configuration == b.configuration && a.width == b.width && a.height == b.height && a.layout == b.layout;
    }
    static bool SameView(const TrialSample& a, const TrialSample& b)
    {
        float difference = 0;
        for (unsigned i = 0; i < a.image.size(); ++i) difference += std::fabs(a.image[i] - b.image[i]);
        return difference <= a.image.size() * .10f;
    }
    EntryData& Entry(const TrialSample& s)
    {
        auto found = std::find_if(entries_.begin(), entries_.end(), [&](const EntryData& e) {
            return e.used && e.scene == scene_ && SameSetup(e.example, s) && SameView(e.example, s) &&
                   e.example.origin.stage == s.origin.stage && e.example.origin.region == s.origin.region &&
                   std::fabs(e.example.image[s.origin.region] - s.image[s.origin.region]) < .18f &&
                   TrialDirection(e.example.change[s.origin.region], s.change[s.origin.region]) != 0;
        });
        if (found == entries_.end()) {
            found = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) { return a.used < b.used; });
            *found = {};
            found->scene = scene_;
            found->example = s;
        }
        entry_ = unsigned(found - entries_.begin());
        found->used = ++clock_;
        return *found;
    }
    uint64_t Serial() { return domain_ | ++serial_; }
    void Observe()
    {
        tool_ = Tool::None;
        previous_.ordinal = 0;
        watch_ = Serial() << 3;
    }
    void Finish(uint64_t now, bool ruled_out, const char* reason)
    {
        auto& e = entries_[entry_];
        if (ruled_out) e.ruled_out |= Bit(tool_);
        else for (unsigned i = 0; i < e.after.size(); ++i)
            if (Bit(tool_) == (1u << i)) e.after[i] = now + 30000;
        why_ = reason;
        Observe();
    }
    Scene scene_{};
    RouteTrial trial_;
    std::array<EntryData, 8> entries_{};
    unsigned entry_ = 0;
    unsigned lost_view_ = 0, motion_losses_ = 0;
    uint64_t serial_ = 0, clock_ = 0, watch_ = 0, last_ = 0, last_witness_ = 0, previous_at_ = 0;
    uint64_t domain_, exhausted_ = 0;
    float original_motion_ = -1;
    const char* why_ = "";
    TrialSample previous_;
    Tool tool_ = Tool::None;
};
} // namespace na::referee
