// The captures folder's budget (core/capture_retention.hpp): oldest folders go first until the
// total fits, the run in progress is never touched, 0 turns the rule off (Cort 2026-11-17).
#include "test_support.hpp"
#include "core/capture_retention.hpp"
#include <memory>
#include <vector>

using namespace na::capture::retention;

namespace {
constexpr uint64_t GB = 1ull << 30;
Entry E(uint64_t gb, uint64_t created, bool current = false)
{
    Entry e;
    e.bytes = gb * GB;
    e.created = created;
    e.current_run = current;
    return e;
}
}

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    };

    { // under the budget: nothing goes
        const Entry e[] = {E(10, 1), E(20, 2), E(5, 3)};
        bool r[3];
        const Plan p = Decide(e, 3, 60 * GB, r);
        check(p.removed == 0 && !r[0] && !r[1] && !r[2] && p.total_after == 35 * GB && !p.over_after, "under the budget nothing is removed");
    }
    { // over: oldest first, and only as many as needed, whatever order they are listed in
        const Entry e[] = {E(30, 5), E(30, 1), E(30, 3), E(30, 2)};
        bool r[4];
        const Plan p = Decide(e, 4, 60 * GB, r);
        check(p.removed == 2 && r[1] && r[3] && !r[0] && !r[2], "the two oldest go, the two newest stay");
        check(p.total_after == 60 * GB && p.removed_bytes == 60 * GB && !p.over_after, "the total lands on the budget");
    }
    { // the run in progress is never removed, even when it is the oldest and the largest
        const Entry e[] = {E(50, 1, true), E(20, 2), E(20, 3), E(40, 4, true)};
        bool r[4];
        const Plan p = Decide(e, 4, 60 * GB, r);
        check(!r[0] && !r[3], "folders of the run in progress stay");
        check(r[1] && r[2] && p.removed == 2, "everything else goes when the run alone is over the budget");
        check(p.over_after && p.total_after == 90 * GB && p.protected_count == 2 && p.protected_bytes == 90 * GB,
              "still over, and the report says the current run is why");
    }
    { // 0 = off
        const Entry e[] = {E(500, 1), E(300, 2)};
        bool r[2];
        const Plan p = Decide(e, 2, 0, r);
        check(p.removed == 0 && !r[0] && !r[1] && !p.over_after, "a budget of 0 removes nothing");
    }
    { // the shape found on the PC: hundreds of old folders, a recent run, tonight's run
        std::vector<Entry> e;
        for (uint64_t i = 0; i < 900; ++i) e.push_back(E(1, i));       // old
        for (uint64_t i = 0; i < 16; ++i) e.push_back(E(2, 1000 + i)); // the run before (32 GB)
        for (uint64_t i = 0; i < 4; ++i) e.push_back(E(2, 2000 + i, true)); // the run in progress (8 GB)
        const std::unique_ptr<bool[]> flags(new bool[e.size()]);
        bool* r = flags.get();
        const Plan p = Decide(e.data(), e.size(), 60 * GB, r);
        bool previous_run_kept = true, current_kept = true;
        for (size_t i = 900; i < 916; ++i) previous_run_kept = previous_run_kept && !r[i];
        for (size_t i = 916; i < 920; ++i) current_kept = current_kept && !r[i];
        check(p.total_before == 940 * GB && p.total_after == 60 * GB && p.removed == 880, "940 GB comes down to 60 GB");
        check(previous_run_kept && current_kept, "the last two runs are what is left");
    }
    { // nothing to measure
        const Plan p = Decide(nullptr, 0, 60 * GB, nullptr);
        check(p.total_before == 0 && p.removed == 0 && !p.over_after, "an empty folder is fine");
    }
    // the low-disk guard
    check(DiskTooFull(8 * GB, 20 * GB), "8 GB free is under a 20 GB floor");
    check(!DiskTooFull(84 * GB, 20 * GB), "84 GB free is not");
    check(!DiskTooFull(0, 0), "a floor of 0 is no guard");

    std::printf("capture_retention: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
