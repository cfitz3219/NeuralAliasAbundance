#pragma once

#include "core/referee_coverage.hpp"
#include "core/referee_recovery.hpp"

#include <cstdio>
#include <sstream>
#include <string>

namespace na::referee {

// VARIANT: referee/launch-memory
// What the Referee's stage recovery learned in earlier launches of the same
// game with the same runtime: for each API and each stage where flicker first
// appears, how often each control was kept on the screen, ruled out as
// harmful, or left without a verdict.
//
// These are hints, never verdicts. They only change the ORDER in which the
// live trial tries the controls: one that was kept before goes first, one that
// only did harm goes last. Every control is still proved on the screen by the
// same trial before it stays, and a control that did harm before is still
// tried when nothing else is left. A missing, damaged or stale record leaves
// discovery exactly as it is without one.
//
// Nothing process-local is stored: no resource, scene, placement or reader
// identity. The record is discarded as a whole when the game's executable or
// the runtime changed (the two identity strings below), or the schema did.
class Memory {
public:
    // Schema 2 (2026-11-27): also the coverage trial's classes (core/referee_coverage.hpp), as "c" rows.
    static constexpr unsigned schema = 2;
    static constexpr unsigned apis = 2, stages = 3, tools = 5;
    struct Entry {
        uint32_t kept = 0, harm = 0, inconclusive = 0;
        float reduction = -1.f; // best verified reduction on the screen while kept
    };

    void Clear() { *this = {}; }
    bool Empty() const
    {
        for (const auto& e : entries_) if (e.kept || e.harm || e.inconclusive) return false;
        for (const auto& e : coverage_) if (e.kept || e.harm) return false;
        return true;
    }
    // VARIANT: referee/coverage-trial: a class of draws left off the jitter was kept on the sample, or put back.
    bool NoteCoverage(bool dx12, unsigned cls, bool kept)
    {
        if (cls >= kLeftClasses) return false;
        auto& e = coverage_[(dx12 ? 1u : 0u) * kLeftClasses + cls];
        constexpr uint32_t limit = 1000000;
        if (kept) { if (e.kept < limit) ++e.kept; }
        else if (e.harm < limit) ++e.harm;
        return true;
    }
    // Classes put back and never kept in earlier launches: tried after the others (hints, never verdicts).
    uint32_t CoverageAvoided(bool dx12) const
    {
        uint32_t bits = 0;
        for (unsigned c = 0; c < kLeftClasses; ++c) {
            const auto& e = coverage_[(dx12 ? 1u : 0u) * kLeftClasses + c];
            if (e.harm && !e.kept) bits |= 1u << c;
        }
        return bits;
    }
    // Returns true when the record changed and should be saved.
    bool Note(bool dx12, FlickerStage stage, RecoveryTool tool, TrialOutcome outcome, float reduction = -1.f)
    {
        auto* e = Find(dx12, stage, tool);
        if (!e) return false;
        constexpr uint32_t limit = 1000000;
        switch (outcome) {
        case TrialOutcome::Improved:
            if (e->kept < limit) ++e->kept;
            if (reduction > e->reduction && reduction <= 1.f) e->reduction = reduction;
            return true;
        case TrialOutcome::Harm:
            if (e->harm < limit) ++e->harm;
            return true;
        case TrialOutcome::Insufficient:
        case TrialOutcome::Unavailable:
        case TrialOutcome::Slower:
            if (e->inconclusive < limit) ++e->inconclusive;
            return true;
        default:
            return false;
        }
    }
    const Entry* Get(bool dx12, FlickerStage stage, RecoveryTool tool) const
    {
        return const_cast<Memory*>(this)->Find(dx12, stage, tool);
    }
    // Controls kept at least once and kept at least as often as they did harm.
    uint8_t Preferred(bool dx12, FlickerStage stage) const { return Select(dx12, stage, true); }
    // Controls that did harm and were never kept.
    uint8_t Avoided(bool dx12, FlickerStage stage) const { return Select(dx12, stage, false); }

    static bool Identity(const std::string& s)
    {
        return !s.empty() && s.size() <= 128 &&
               std::all_of(s.begin(), s.end(), [](unsigned char c) { return c > 32 && c < 127; });
    }
    std::string Encode(const std::string& game, const std::string& runtime) const
    {
        if (!Identity(game) || !Identity(runtime)) return {};
        std::ostringstream out;
        out << "NA_REFEREE_MEMORY " << schema << '\n' << game << '\n' << runtime << '\n';
        for (unsigned i = 0; i < entries_.size(); ++i) {
            const auto& e = entries_[i];
            if (!e.kept && !e.harm && !e.inconclusive) continue;
            char reduction[32];
            std::snprintf(reduction, sizeof reduction, "%.4f", e.reduction);
            out << i / (stages * tools) << ' ' << i / tools % stages << ' ' << i % tools << ' '
                << e.kept << ' ' << e.harm << ' ' << e.inconclusive << ' ' << reduction << '\n';
        }
        for (unsigned i = 0; i < coverage_.size(); ++i) {
            const auto& e = coverage_[i];
            if (!e.kept && !e.harm) continue;
            out << "c " << i / kLeftClasses << ' ' << i % kLeftClasses << ' ' << e.kept << ' ' << e.harm << '\n';
        }
        return out.str();
    }
    enum class Load { Loaded, Missing, Invalid, Stale };
    Load Decode(const std::string& text, const std::string& game, const std::string& runtime)
    {
        Clear();
        if (text.empty()) return Load::Missing;
        if (text.size() > 8192) return Load::Invalid;
        std::istringstream in(text);
        std::string magic, stored_game, stored_runtime;
        unsigned version = 0;
        in >> magic >> version >> stored_game >> stored_runtime;
        if (!in || magic != "NA_REFEREE_MEMORY" || !Identity(stored_game) || !Identity(stored_runtime)) return Load::Invalid;
        if (version != schema || stored_game != game || stored_runtime != runtime) return Load::Stale;
        Memory read;
        for (unsigned lines = 0;; ++lines) {
            unsigned api = 0, stage = 0, tool = 0;
            Entry e;
            in >> std::ws;
            if (in.peek() == 'c') {
                char tag = 0;
                unsigned capi = 0, cls = 0;
                Entry ce;
                in >> tag >> capi >> cls >> ce.kept >> ce.harm;
                if (!in || capi >= apis || cls >= kLeftClasses) return Load::Invalid;
                auto& slot = read.coverage_[capi * kLeftClasses + cls];
                if (slot.kept || slot.harm) return Load::Invalid;
                slot = ce;
                continue;
            }
            in >> api;
            if (in.eof() && in.fail()) break;
            in >> stage >> tool >> e.kept >> e.harm >> e.inconclusive >> e.reduction;
            if (!in || lines >= apis * stages * tools || api >= apis || stage >= stages || tool >= tools ||
                !(e.reduction >= -1.f && e.reduction <= 1.f)) return Load::Invalid;
            auto& slot = read.entries_[(api * stages + stage) * tools + tool];
            if (slot.kept || slot.harm || slot.inconclusive) return Load::Invalid; // a row may appear once
            slot = e;
        }
        *this = read;
        return Load::Loaded;
    }

private:
    static int ToolIndex(RecoveryTool tool)
    {
        const unsigned bit = Bit(tool);
        for (unsigned i = 0; i < tools; ++i) if (bit == (1u << i)) return int(i);
        return -1;
    }
    Entry* Find(bool dx12, FlickerStage stage, RecoveryTool tool)
    {
        const int t = ToolIndex(tool);
        const unsigned s = unsigned(stage) - 1;
        if (t < 0 || s >= stages) return nullptr;
        return &entries_[((dx12 ? 1u : 0u) * stages + s) * tools + unsigned(t)];
    }
    uint8_t Select(bool dx12, FlickerStage stage, bool preferred) const
    {
        const unsigned s = unsigned(stage) - 1;
        if (s >= stages) return 0;
        uint8_t bits = 0;
        for (unsigned t = 0; t < tools; ++t) {
            const auto& e = entries_[((dx12 ? 1u : 0u) * stages + s) * tools + t];
            if (preferred ? (e.kept && e.kept >= e.harm) : (e.harm && !e.kept)) bits |= uint8_t(1u << t);
        }
        return bits;
    }
    std::array<Entry, apis * stages * tools> entries_{};
    std::array<Entry, apis * kLeftClasses> coverage_{};
};

} // namespace na::referee
