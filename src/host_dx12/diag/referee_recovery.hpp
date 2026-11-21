#pragma once
#include "core/referee_recovery.hpp"
#include "core/referee_placement.hpp"
#include "core/referee_memory.hpp"
#include "core/anisotropic_auto.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/settings.hpp"

#include <windows.h>
#include <cwctype>
#include <functional>
#include <mutex>
#include <string>

namespace na::referee {
inline uint64_t EvaluationConfiguration(uint32_t preset, uint32_t cf, uint32_t df, bool hdr, bool reversed,
                                        int camera, bool measured, bool jitter, bool flow, uint32_t debug_jitter)
{
    const auto& set = settings::Get();
    const uint32_t configuration[] = {
        preset, uint32_t(cf), uint32_t(df), uint32_t(hdr), uint32_t(reversed),
        uint32_t(camera), uint32_t(measured), uint32_t(jitter), debug_jitter,
        set.j2_draw_replay.load(), set.j2_replay_movers.load(), set.j2_replay_budget.load(),
        uint32_t(set.j2_viewport_jitter.load()), uint32_t(set.j2_jitter_shift.load()), set.j2_jitter_r2.load(), set.j2_sharpness.load(),
        uint32_t(settings::DlaaMipBiasX100(set)), uint32_t(set.af_mode.load()), af::Global().Current(),
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

// One line per trial transition, identical on both APIs: what the phase that
// just ended established, how many screen receipts it was offered, how many
// it could use and which gate refused the rest. `owner` names the caller.
inline void EmitTrialEvidence(const char* owner, uint64_t frame, const RouteTrial& trial)
{
    const auto& evidence = trial.Evidence();
    char refused[256] = "none";
    size_t at = 0;
    for (unsigned i = 0; i < evidence.refused.size(); ++i) {
        if (!evidence.refused[i] || at >= sizeof(refused) - 1) continue;
        const int wrote = std::snprintf(refused + at, sizeof(refused) - at, "%s%s:%u", at ? "," : "",
                                        TrialGateName(static_cast<TrialGate>(i)), evidence.refused[i]);
        if (wrote <= 0) break;
        at = (std::min)(sizeof(refused) - 1, at + size_t(wrote));
    }
    const auto decisive = evidence.Decisive();
    eventlog::Emit("referee_evidence",
        "owner=%s frame=%llu phase=%s now=%s outcome=%s receipts=%u used=%u decisive=%s retries=%u refused=%s frame_ms=%.2f/%.2f/%.2f",
        owner, (unsigned long long)frame, trial.EvidencePhase(), trial.Name(), TrialOutcomeName(trial.Outcome()),
        evidence.receipts, evidence.Used(),
        !evidence.receipts ? "no_receipt" : decisive == TrialGate::Count ? "none" : TrialGateName(decisive),
        trial.InconclusiveAttempts(), refused, trial.FrameMs(RouteTrial::Phase::Baseline),
        trial.FrameMs(RouteTrial::Phase::Candidate), trial.FrameMs(RouteTrial::Phase::ConfirmBaseline));
}

// VARIANT: referee/launch-memory (core/referee_memory.hpp)
// One small text file per game under the user's NeuralAlias folder. It is
// read once per launch and rewritten when a stage-recovery attempt ends, which
// happens a few times a session. Off until the host asks for it at start-up
// (Enable), so tests and tools never read or write it. Both API adapters go
// through AdvancePlacement below, so they share this one store and behave alike.
namespace memory {
struct Store {
    std::mutex m;
    bool enabled = false, dirty = false, saving = false, save_failed = false;
    bool (*submit)(std::function<void()>) = nullptr;
    Memory record;
    std::wstring path;
    std::string game, runtime;
};
inline Store& State()
{
    static Store& store = *new Store; // process lifetime: nothing to destroy at exit
    return store;
}
// Size and last-write time of a file: changes when the game or the runtime is updated.
inline std::string FileIdentity(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!path[0] || !GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return {};
    char text[64];
    std::snprintf(text, sizeof text, "%08lx%08lx-%08lx%08lx", data.nFileSizeHigh, data.nFileSizeLow,
                  data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
    return text;
}
inline void LoadLocked(Store& store)
{
    std::string text;
    const HANDLE file = CreateFileW(store.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char bytes[8193];
        DWORD got = 0;
        if (ReadFile(file, bytes, sizeof bytes, &got, nullptr)) text.assign(bytes, got);
        CloseHandle(file);
    }
    const auto result = store.record.Decode(text, store.game, store.runtime);
    constexpr const char* names[] = {"loaded", "missing", "invalid", "stale"};
    eventlog::Emit("referee_memory", "action=load result=%s why=%s", names[static_cast<unsigned>(result)],
        result == Memory::Load::Loaded ? "hints from an earlier launch of this game and runtime order the live attempts" :
        result == Memory::Load::Missing ? "first launch for this game: nothing remembered, discovery runs as usual" :
        result == Memory::Load::Stale ? "the game, the runtime or the record format changed: old hints discarded" :
                                        "the record could not be read: ignored, discovery runs as usual");
}
// Called once from host start-up, never from a presentation path: the record
// is read here and only the in-memory copy is used afterwards. `submit` is the
// host's existing background worker; every write to disk happens there.
inline void Enable(bool (*submit)(std::function<void()>))
{
    auto& store = State();
    std::lock_guard<std::mutex> lock(store.m);
    if (store.enabled) return;
    wchar_t game[MAX_PATH]{}, runtime[MAX_PATH]{};
    GetModuleFileNameW(nullptr, game, MAX_PATH);
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&FileIdentity), &self))
        GetModuleFileNameW(self, runtime, MAX_PATH);
    store.game = FileIdentity(game);
    store.runtime = FileIdentity(runtime);
    const wchar_t* user = settings::UserIniPath();
    if (store.game.empty() || store.runtime.empty() || !user || !user[0]) return;
    // The file is named by a hash of the game's full path: no path is stored inside it.
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t* c = game; *c; ++c) hash = (hash ^ uint64_t(towlower(*c))) * 1099511628211ull;
    std::wstring folder(user);
    const auto slash = folder.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    wchar_t name[64];
    swprintf_s(name, L"referee-memory-%016llx.txt", static_cast<unsigned long long>(hash));
    store.path = folder.substr(0, slash + 1) + name;
    store.submit = submit;
    LoadLocked(store);
    store.enabled = true;
}
// Runs on the background worker. Writes the newest record; notes that arrive
// while it writes are picked up by the same job. A failed write keeps the
// counts in memory and is tried again after a later note.
inline void SaveJob()
{
    auto& store = State();
    for (;;) {
        std::string text;
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(store.m);
            if (!store.dirty) { store.saving = false; return; }
            text = store.record.Encode(store.game, store.runtime);
            path = store.path;
            store.dirty = false;
        }
        bool ok = text.empty();
        if (!ok) {
            // One temporary file per process: two running copies of a game never share it.
            wchar_t suffix[32];
            swprintf_s(suffix, L".%lu.tmp", static_cast<unsigned long>(GetCurrentProcessId()));
            const std::wstring temporary = path + suffix;
            const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size();
                CloseHandle(file);
                ok = ok && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
                if (!ok) DeleteFileW(temporary.c_str());
            }
        }
        std::lock_guard<std::mutex> lock(store.m);
        if (ok == store.save_failed)
            eventlog::Emit("referee_memory", "action=save result=%s why=%s", ok ? "saved" : "failed",
                ok ? "the record reached disk again" :
                     "the record could not be written: this launch keeps its counts in memory and tries again after the next result");
        store.save_failed = !ok;
        if (!ok) { store.saving = false; return; }
    }
}
// Presentation paths only ask the worker; they never touch the disk.
inline void ScheduleLocked(Store& store)
{
    if (!store.dirty || store.saving || store.save_failed || !store.submit) return;
    store.saving = true;
    if (!store.submit(&SaveJob)) store.saving = false;
}
// Give this launch's recovery the remembered order for each stage.
inline void Apply(StageRecovery& recovery)
{
    auto& store = State();
    std::lock_guard<std::mutex> lock(store.m);
    if (!store.enabled) return;
    ScheduleLocked(store); // a save the worker could not take earlier
    for (auto stage : {FlickerStage::Input, FlickerStage::Reconstruction, FlickerStage::Composition})
        recovery.SetHints(stage, store.record.Preferred(recovery.Dx12(), stage), store.record.Avoided(recovery.Dx12(), stage));
}
// Record what a finished attempt established on the screen.
inline void Collect(StageRecovery& recovery, uint64_t frame)
{
    StageRecovery::Result result;
    if (!recovery.TakeResult(result)) return;
    auto& store = State();
    std::lock_guard<std::mutex> lock(store.m);
    if (!store.enabled) return;
    if (!store.record.Note(recovery.Dx12(), result.stage, result.tool, result.outcome, result.reduction)) return;
    store.dirty = true;
    store.save_failed = false; // a new result is the next attempt to write
    ScheduleLocked(store);
    eventlog::Emit("referee_memory", "action=note frame=%llu api=%s first_stage=%s tool=%s outcome=%s reduction=%.4f tried_first_from_memory=%u",
                   (unsigned long long)frame, recovery.Dx12() ? "dx12" : "dx11", StageName(result.stage), ToolName(result.tool),
                   TrialOutcomeName(result.outcome), result.reduction, unsigned(result.hinted));
}
} // namespace memory

// Both API adapters poll the same bounded GPU mailbox after closing a frame.
// Each adapter supplies its own scratch sample under its existing render lock.
template<class Reader, class Accept>
void AdvancePlacement(PlacementTrial& placement, PlacementTrial::Scope scope, bool can_nominate, bool screen_proved,
                      bool candidate_submitted, StageRecovery& recovery, uint64_t frame, uint64_t now,
                      Reader read, Accept accept, TrialSample& sample, bool parity_seen = false)
{
    bool on_kept_route = false;
    // VARIANT: referee/proved-roles-kept
    // Both adapters offer `screen_proved` only for a rotating scene pair whose
    // two roles each reached the screen. With the parity signature also
    // recognized (referee_parity.hpp) the role-following route is kept at
    // once (core/referee_trial.hpp), upgrading a timed trial on the same
    // scope. Without the signature the timed trial below decides as before.
    if (can_nominate && screen_proved && parity_seen && !placement.KeptByStructure() &&
        (!placement.OwnsComparison() || placement.Matches(scope)) &&
        placement.KeepProvedRoles(scope, parity_seen, frame, now)) {
        recovery.Suspend();
        eventlog::Emit("referee_roles", "frame=%llu kept=1 incumbent_samples=%u why=%s", (unsigned long long)frame,
                       placement.IncumbentSamples(), placement.Comparison().Reason());
        return;
    }
    if (placement.OwnsComparison()) {
        if (!placement.Matches(scope)) {
            placement.Invalidate(now);
            recovery.Suspend();
            return;
        }
        on_kept_route = placement.AllowsStageRecovery();
        placement.Tick(now, candidate_submitted);
        if (!on_kept_route || !placement.AllowsStageRecovery()) {
            recovery.Suspend();
            const auto ticket = placement.Ticket(frame);
            if (ticket && read(ticket, sample)) {
                if (accept(sample, true, placement.UsesCandidate())) {
                    if (!placement.UsesCandidate()) placement.ObserveIncumbent(sample);
                    placement.Add(scope, ticket, sample, frame, now);
                } else placement.Refused(TrialGate::Scope);
            }
            return;
        }
    }
    const auto previous = recovery.Selected();
    const auto phase = recovery.Comparison().Current();
    const auto exhausted = recovery.Exhausted();
    const auto ticket = recovery.Ticket(frame);
    memory::Apply(recovery);
    recovery.Tick(now);
    memory::Collect(recovery, frame);
    if (!on_kept_route && !recovery.Busy() && can_nominate && screen_proved && placement.BeginVerified(scope, frame, now)) {
        recovery.Suspend();
        return;
    }
    const bool got = ticket && read(ticket, sample) && ticket == recovery.Ticket(sample.ordinal);
    if (got && on_kept_route) {
        if (!accept(sample, true, true)) return;
        // Maintain the original placement verdict against untouched frames.
        // If a second tool degrades the picture, its own trial restores this
        // route without taking the earlier improvement away.
        if (recovery.ToolInUse() == RecoveryTool::None) {
            placement.Add(scope, placement.Ticket(sample.ordinal), sample, frame, now);
            if (!placement.UsesCandidate()) { recovery.Suspend(); return; }
        }
    }
    if (got && !on_kept_route && !recovery.Busy() && can_nominate && accept(sample, false, false) &&
        placement.Begin(scope, sample, frame, now)) {
        recovery.Suspend();
        return;
    }
    if (got && !on_kept_route && previous == RecoveryTool::None) placement.ObserveIncumbent(sample);
    if (got) recovery.Add(ticket, sample, frame + 1, now);
    memory::Collect(recovery, frame);
    const bool exhausted_now = exhausted != recovery.Exhausted();
    if (exhausted_now || previous != recovery.Selected() ||
        (recovery.Busy() && phase != recovery.Comparison().Current())) {
        const auto& trial = recovery.Comparison();
        const float total = on_kept_route && recovery.Busy() && trial.VerifiedReduction() >= 0.f &&
            recovery.Region() == placement.RecoveryRegion() && placement.RecoveryBaseline() > 0.f
            ? 1.f - trial.CandidateAffectedRegion() / placement.RecoveryBaseline() : -1.f;
        eventlog::Emit("referee_recovery",
            "frame=%llu first_stage=%s region=%u tool=%s phase=%s ruled_out=%u stops=%.3f/%.3f/%.3f reduction=%.4f total=%.4f why=%s",
            (unsigned long long)frame, StageName(recovery.Origin()), recovery.Region(),
            ToolName(recovery.Busy() ? recovery.Selected() : previous),
            exhausted_now ? "no_matching_tool" : recovery.Busy() ? trial.Name() : "restored",
            unsigned(recovery.Excluded()), trial.Baseline(), trial.Candidate(), trial.Confirmation(),
            recovery.Busy() ? trial.VerifiedReduction() : -1.f, total, recovery.Reason());
        if (!exhausted_now) EmitTrialEvidence("recovery", frame, trial);
    }
}
} // namespace na::referee
