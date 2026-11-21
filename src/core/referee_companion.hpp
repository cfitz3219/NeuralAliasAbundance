#pragma once

#include <cstdint>

namespace na::referee {

// A 32-bit game cannot load DLSS, so its frame is handed to a 64-bit companion
// program (dlaa_remote/protocol.hpp). The Referee may use a picture of such a
// frame only when three things are the same frame: the inputs the game half
// prepared (`asked`), the result the companion says it produced (`answered`)
// and, later, the present the screen accepted (the existing present result in
// capture/referee_gpu.cpp, keyed by `ordinal`). This is the first two.
struct CompanionHandoff {
    uint64_t ordinal = 0;                // present ordinal the inputs belong to
    uint64_t asked = 0, answered = 0;    // frame value sent, frame value the reply names
    uint32_t asked_slot = 0, answered_slot = 0;
    bool replied = false, ran = false;   // a reply arrived; the companion says DLSS ran
};

enum class CompanionBinding : uint8_t { Bound, NoReply, Stale, Refused };

constexpr CompanionBinding Judge(const CompanionHandoff& h)
{
    if (!h.asked || !h.replied) return CompanionBinding::NoReply;
    if (h.answered != h.asked || h.answered_slot != h.asked_slot) return CompanionBinding::Stale;
    return h.ran ? CompanionBinding::Bound : CompanionBinding::Refused;
}

constexpr const char* CompanionBindingName(CompanionBinding binding)
{
    switch (binding) {
    case CompanionBinding::Bound: return "bound";
    case CompanionBinding::NoReply: return "no_reply";
    case CompanionBinding::Stale: return "stale";
    default: return "refused";
    }
}

constexpr const char* CompanionBindingWhy(CompanionBinding binding)
{
    switch (binding) {
    case CompanionBinding::Bound: return "the companion's result names the frame that was sent: its picture may be judged";
    case CompanionBinding::NoReply: return "no reply arrived for this frame: its picture is not judged";
    case CompanionBinding::Stale: return "the reply names a different frame or slot: its picture is not judged";
    default: return "the companion reports that DLSS did not run for this frame: its picture is not judged";
    }
}

} // namespace na::referee
