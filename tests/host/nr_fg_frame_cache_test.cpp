#include "test_support.hpp"
#include "host_dx12/nr/fg_frame_cache.hpp"

int main() {
    using Cache=na::nr::FgFrameCache;
    using Action=Cache::Action;
    Cache cache;
    CHECK(cache.Acquire(std::nullopt).action==Action::MissingIdentity);
    auto zero=cache.Acquire(0);
    CHECK(zero.action==Action::Record);
    CHECK(cache.Acquire(0).action==Action::Pending);
    CHECK(!cache.Submitted(zero.ticket));
    // First recording is not a GPU submission. A Reset may discard it and
    // re-record the same real frame without losing NR for that frame.
    CHECK(!cache.WasSubmitted(zero.ticket));
    CHECK(!cache.RetireCompleted(zero.ticket));
    CHECK(cache.AbandonUnsubmitted(zero.ticket));
    auto retry=cache.Acquire(0);
    CHECK(retry.action==Action::Record);
    CHECK(!cache.Submitted(zero.ticket)); // recycled slot, stale ticket
    CHECK(cache.Recorded(retry.ticket));
    CHECK(cache.Acquire(0).action==Action::ReuseRecorded);
    CHECK(cache.Submitted(retry.ticket));
    CHECK(!cache.AbandonUnsubmitted(retry.ticket));
    // 4x MFG: repeated callbacks use exactly one cached real-frame output.
    for(int index=1;index<=3;++index) {
        auto repeated=cache.Acquire(0);
        CHECK(repeated.action==Action::ReuseSubmitted);
        CHECK(repeated.ticket.slot==retry.ticket.slot && repeated.ticket.generation==retry.ticket.generation);
    }
    auto next=cache.Acquire(1); CHECK(next.action==Action::Record);
    CHECK(cache.Recorded(next.ticket));
    CHECK(cache.Submitted(next.ticket));
    CHECK(cache.RetireCompleted(next.ticket));
    CHECK(cache.Acquire(1).action==Action::Expired); // cannot enhance it twice
    CHECK(cache.Acquire(0).action==Action::ReuseSubmitted); // delayed older retained frame
    CHECK(cache.RetireCompleted(retry.ticket));
    CHECK(cache.Acquire(0).action==Action::Expired);
    std::array<Cache::Ticket,Cache::Capacity> busy{};
    for(uint32_t i=0;i<Cache::Capacity;++i) {
        auto d=cache.Acquire(i+2); CHECK(d.action==Action::Record); busy[i]=d.ticket;
        CHECK(cache.Recorded(d.ticket));
        CHECK(cache.Submitted(d.ticket));
    }
    CHECK(cache.Acquire(100).action==Action::Full); // cannot overwrite busy GPU output
    CHECK(cache.Acquire(3).action==Action::ReuseSubmitted); // full pool still serves MFG repeat
    CHECK(cache.RetireCompleted(busy[0]));
    auto replacement=cache.Acquire(100); CHECK(replacement.action==Action::Record);
    CHECK(!cache.RetireCompleted(busy[0])); // stale completion cannot free replacement
    CHECK(cache.AbandonUnsubmitted(replacement.ticket));
    CHECK(cache.Acquire(100).action==Action::Record);
    Cache recreated_feature;
    CHECK(recreated_feature.Acquire(0).action==Action::Record); // new native handle epoch
    return test::finish("nr_fg_frame_cache_test");
}
