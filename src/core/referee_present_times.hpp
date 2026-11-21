#pragma once

#include <array>
#include <cstdint>

namespace na::referee {

// When each presented frame reached its presentation endpoint, by swap chain
// and present ordinal. Both native APIs stamp through the one GPU sampler.
// A frame whose presentation is acknowledged later is stamped only by its own
// accepted acknowledgement, so a duplicate, unknown or refused result cannot
// time a frame. A lookup that does not match exactly returns 0, and a receipt
// without a time takes no part in frame pacing.
class PresentTimes {
public:
    void Stamp(const void* chain, uint64_t ordinal, uint64_t at_us)
    {
        if (chain && ordinal && at_us) slots_[ordinal % slots_.size()] = {chain, ordinal, at_us};
    }
    // Returns whether the frame may be stamped now: only when it does not wait
    // for an acknowledgement.
    bool Presented(const void* chain, uint64_t ordinal, bool awaits_result, uint64_t at_us)
    {
        if (awaits_result) return false;
        Stamp(chain, ordinal, at_us);
        return true;
    }
    // `matched` is whether the result belongs to a present still waiting for it.
    bool Acknowledged(const void* chain, uint64_t ordinal, bool matched, bool accepted, uint64_t at_us)
    {
        if (!matched || !accepted) return false;
        Stamp(chain, ordinal, at_us);
        return true;
    }
    uint64_t At(const void* chain, uint64_t ordinal) const
    {
        const auto& slot = slots_[ordinal % slots_.size()];
        return chain && slot.chain == chain && slot.ordinal == ordinal ? slot.at_us : 0;
    }

private:
    struct Slot { const void* chain = nullptr; uint64_t ordinal = 0, at_us = 0; };
    std::array<Slot, 64> slots_{};
};

} // namespace na::referee
