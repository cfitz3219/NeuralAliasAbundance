#pragma once
#include <cstdint>
#include <mutex>
namespace na::observe {
// One short CPU critical section per hook boundary. Never held across a D3D12
// call, event emission, or GPU wait. End finalizes flags before publication.
class QueueOccurrences {
public:
    struct Token { uint64_t ticket{}, generation{}; bool overlapped{}; };
    Token Begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool overlap = active_ != 0;
        if (overlap) ++generation_;
        ++active_;
        return {++ticket_, generation_, overlap};
    }
    // The ticket of the most recent occurrence to begin on this queue. If it is
    // still a given submission's ticket, no other operation has started on the
    // queue since that submission began (host copies create no occurrence).
    uint64_t LastTicket() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ticket_;
    }
    bool End(const Token& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool ambiguous = t.overlapped || t.generation != generation_;
        --active_;
        return ambiguous;
    }
private:
    std::mutex mutex_;
    uint64_t ticket_{},generation_{},active_{};
};
}
