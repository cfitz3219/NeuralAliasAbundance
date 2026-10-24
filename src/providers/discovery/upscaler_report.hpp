// A4: observation report, not permission to run a reconstruction backend.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace na::discovery {
enum class Upscaler : uint32_t { Dlss, Fsr, Xess };
enum class Detection { Unobserved, ModuleOnly, FailedCalls, InjectorOnly, SourceUnknown, Created, Evaluated };
struct UpscalerEvidence {
    bool observer_available = false;
    bool module_seen = false;
    // Successful SR operations only, classified by the producer's origin evidence.
    // FG/NR/RR operations and generic API evaluations must not enter these counters.
    uint64_t game_creates = 0, game_evaluates = 0;
    uint64_t unknown_creates = 0, unknown_evaluates = 0;
    uint64_t injector_creates = 0, injector_evaluates = 0;
    uint64_t failed_game_calls = 0, failed_unknown_calls = 0;
};
using UpscalerSnapshot = std::array<UpscalerEvidence, 3>;
struct UpscalerRow {
    Upscaler provider;
    Detection detection;
    bool game_observed;
    const char* name;
    const char* badge;
    std::string reason;
};
struct UpscalerReport {
    std::array<UpscalerRow, 3> rows;
    uint32_t game_routes = 0; // bitset; multiple observed integrations remain visible
    std::string summary;
};

inline UpscalerReport DescribeUpscalers(const UpscalerSnapshot& snapshot)
{
    UpscalerReport report{};
    constexpr const char* names[] = {"DLSS", "FSR", "XeSS"};
    bool unknown_source = false;
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto& e = snapshot[i];
        auto& row = report.rows[i];
        row.provider = static_cast<Upscaler>(i);
        row.name = names[i];
        row.game_observed = e.game_creates != 0 || e.game_evaluates != 0;
        if (e.game_evaluates) {
            row.detection = Detection::Evaluated; row.badge = "GAME CALLS";
            row.reason = "Successful game SR evaluation observed.";
        } else if (e.game_creates) {
            row.detection = Detection::Created; row.badge = "GAME CREATED";
            row.reason = "Successful game SR creation observed; evaluation not established.";
        } else if (e.unknown_creates || e.unknown_evaluates) {
            row.detection = Detection::SourceUnknown; row.badge = "SOURCE UNKNOWN";
            row.reason = "Successful SR calls observed; game versus injector source is unverified.";
            unknown_source = true;
        } else if (e.injector_creates || e.injector_evaluates) {
            row.detection = Detection::InjectorOnly; row.badge = "INJECTOR";
            row.reason = "Only injector-origin SR calls observed; game integration is unconfirmed.";
        } else if (e.failed_game_calls || e.failed_unknown_calls) {
            row.detection = Detection::FailedCalls; row.badge = "CALL FAILED";
            row.reason = "An SR call failed; no successful game integration has been observed.";
        } else if (e.module_seen) {
            row.detection = Detection::ModuleOnly; row.badge = "LIBRARY ONLY";
            row.reason = "Library presence observed; no successful SR calls established.";
        } else {
            row.detection = Detection::Unobserved; row.badge = e.observer_available ? "NOT OBSERVED" : "NOT MONITORED";
            row.reason = e.observer_available ? "No SR calls observed yet; this does not prove absence."
                                              : "Observation is not available yet; game support is unknown.";
        }
        if (row.game_observed) {
            report.game_routes |= 1u << static_cast<unsigned>(i);
            if (!report.summary.empty()) report.summary += " / ";
            report.summary += names[i];
        }
        if (!e.observer_available && row.detection != Detection::Unobserved)
            row.reason += " Historical evidence; the observer is not currently available.";
        if (row.game_observed || row.detection == Detection::SourceUnknown)
            row.reason += " Current-frame inputs and activity are not qualified by this report.";
        if ((e.failed_game_calls || e.failed_unknown_calls) && row.detection != Detection::FailedCalls)
            row.reason += " Failed calls were also observed.";
    }
    if (report.game_routes) report.summary += " game integration observed";
    else if (unknown_source) report.summary = "SR calls observed; game source unconfirmed";
    else report.summary = "No game upscaler confirmed; automatic discovery is the next route";
    return report;
}
} // namespace na::discovery
