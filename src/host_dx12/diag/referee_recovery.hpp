#pragma once
#include "core/referee_recovery.hpp"
#include "core/referee_placement.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/settings.hpp"

namespace na::referee {
inline uint64_t EvaluationConfiguration(uint32_t preset, uint32_t cf, uint32_t df, bool hdr, bool reversed,
                                        int camera, bool measured, bool jitter, bool flow)
{
    const auto& set = settings::Get();
    const uint32_t configuration[] = {
        preset, uint32_t(cf), uint32_t(df), uint32_t(hdr), uint32_t(reversed),
        uint32_t(camera), uint32_t(measured), uint32_t(jitter),
        set.j2_draw_replay.load(), set.j2_replay_movers.load(), set.j2_replay_budget.load(),
        uint32_t(set.j2_viewport_jitter.load()), uint32_t(set.j2_jitter_shift.load()), set.j2_jitter_r2.load(), set.j2_sharpness.load(),
        uint32_t(settings::DlaaMipBiasX100(set)), set.aniso16.load(),
        set.motion_enabled.load(), settings::kObjectFlowLoosen, uint32_t(flow),
        set.xegtao_enabled.load(), set.xegtao_amount.load(), set.xegtao_quality.load(),
        set.xegtao_radius_x1000.load(), set.xegtao_denoise.load(), set.xegtao_radius_multiplier.load(),
        set.xegtao_falloff.load(), set.xegtao_sample_distribution.load(), set.xegtao_thin_occluder.load(),
        set.xegtao_final_power.load(), set.xegtao_depth_mip_offset.load(), set.xegtao_temporal.load(),
        set.donut_tone.load(), set.donut_bloom.load(), set.donut_curve.load(), set.donut_amount.load(),
        set.donut_bias.load(), set.donut_white.load(), set.donut_speed_up.load(), set.donut_speed_down.load(),
        set.donut_bloom_amount.load(), set.donut_bloom_sigma.load(), set.donut_bloom_threshold.load()
    };
    uint64_t hash = 14695981039346656037ull;
    for (const auto value : configuration) hash = (hash ^ value) * 1099511628211ull;
    return hash;
}

// Both API adapters poll the same bounded GPU mailbox after closing a frame.
// Each adapter supplies its own scratch sample under its existing render lock.
template<class Reader, class Accept>
void AdvancePlacement(PlacementTrial& placement, PlacementTrial::Scope scope, bool can_nominate, bool screen_proved,
                      bool candidate_submitted, StageRecovery& recovery, uint64_t frame, uint64_t now,
                      Reader read, Accept accept, TrialSample& sample)
{
    if (placement.OwnsComparison()) {
        if (!placement.Matches(scope)) {
            placement.Invalidate(now);
            return;
        }
        placement.Tick(now, candidate_submitted);
        const auto ticket = placement.Ticket(frame);
        if (ticket && read(ticket, sample) && accept(sample, true, placement.UsesCandidate()))
            placement.Add(scope, ticket, sample, frame, now);
        return;
    }
    const auto previous = recovery.Selected();
    const auto phase = recovery.Comparison().Current();
    const auto exhausted = recovery.Exhausted();
    const auto ticket = recovery.Ticket(frame);
    recovery.Tick(now);
    if (!recovery.Busy() && can_nominate && screen_proved && placement.BeginVerified(scope, frame, now)) {
        recovery.Suspend();
        return;
    }
    const bool got = ticket && read(ticket, sample) && ticket == recovery.Ticket(sample.ordinal);
    if (got && !recovery.Busy() && can_nominate && accept(sample, false, false) &&
        placement.Begin(scope, sample, frame, now)) {
        recovery.Suspend();
        return;
    }
    if (got) recovery.Add(ticket, sample, frame + 1, now);
    const bool exhausted_now = exhausted != recovery.Exhausted();
    if (exhausted_now || previous != recovery.Selected() ||
        (recovery.Busy() && phase != recovery.Comparison().Current())) {
        const auto& trial = recovery.Comparison();
        eventlog::Emit("referee_recovery",
            "frame=%llu first_stage=%s region=%u tool=%s phase=%s ruled_out=%u stops=%.3f/%.3f/%.3f why=%s",
            (unsigned long long)frame, StageName(recovery.Origin()), recovery.Region(),
            ToolName(recovery.Busy() ? recovery.Selected() : previous),
            exhausted_now ? "no_matching_tool" : recovery.Busy() ? trial.Name() : "restored",
            unsigned(recovery.Excluded()), trial.Baseline(), trial.Candidate(), trial.Confirmation(), recovery.Reason());
    }
}
} // namespace na::referee
