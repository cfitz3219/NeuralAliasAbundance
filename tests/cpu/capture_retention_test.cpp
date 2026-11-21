// Which capture folders go (core/capture_retention.hpp; Cort 2026-11-17): when captures of ten
// builds are present the five oldest builds' folders go, the run in progress is never touched,
// 0 turns the rule off; and no capture onto a nearly full drive.
#include "test_support.hpp"
#include "core/capture_retention.hpp"
#include <memory>
#include <vector>

using namespace na::capture::retention;

namespace {
constexpr uint64_t GB = 1ull << 30;
Entry E(uint32_t build, uint64_t created, uint64_t gb = 2, bool current = false)
{
    Entry e;
    e.bytes = gb * GB;
    e.created = created;
    e.build = build;
    e.current_run = current;
    return e;
}
// `builds` builds, numbered 1.., each with `per` folders, build b made at times b*100..
std::vector<Entry> Builds(uint32_t builds, uint32_t per)
{
    std::vector<Entry> e;
    for (uint32_t b = 1; b <= builds; ++b)
        for (uint32_t i = 0; i < per; ++i) e.push_back(E(b, b * 100ull + i));
    return e;
}
}

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
    };
    auto run = [](const std::vector<Entry>& e, uint32_t trigger, uint32_t keep, std::vector<char>* gone) {
        const std::unique_ptr<bool[]> flags(new bool[e.size() + 1]);
        const Plan p = Decide(e.data(), e.size(), trigger, keep, flags.get());
        gone->assign(e.size(), 0);
        for (size_t i = 0; i < e.size(); ++i) (*gone)[i] = flags[i] ? 1 : 0;
        return p;
    };
    std::vector<char> gone;

    { // nine builds: nothing goes, however large they are
        std::vector<Entry> e = Builds(9, 20);
        const Plan p = run(e, 10, 5, &gone);
        check(!p.triggered && p.removed == 0 && p.builds_after == 9, "nine builds: nothing is removed");
    }
    { // the tenth build arrives: builds 1 to 5 go, 6 to 10 stay
        std::vector<Entry> e = Builds(10, 3);
        const Plan p = run(e, 10, 5, &gone);
        bool old_gone = true, new_kept = true;
        for (size_t i = 0; i < e.size(); ++i) {
            if (e[i].build <= 5) old_gone = old_gone && gone[i];
            else new_kept = new_kept && !gone[i];
        }
        check(p.triggered && old_gone && new_kept, "ten builds: the five oldest builds go, the five newest stay");
        check(p.removed == 15 && p.removed_builds == 5 && p.builds_after == 5 && p.removed_bytes == 30 * GB, "the count and the bytes are reported");
    }
    { // every five builds: after the cut, five more builds bring the next cut
        std::vector<Entry> e = Builds(10, 2);
        run(e, 10, 5, &gone);
        std::vector<Entry> left;
        for (size_t i = 0; i < e.size(); ++i) if (!gone[i]) left.push_back(e[i]);
        for (uint32_t b = 11; b <= 14; ++b) left.push_back(E(b, b * 100ull));
        Plan p = run(left, 10, 5, &gone);
        check(!p.triggered && p.builds_before == 9, "four more builds: nothing yet");
        left.push_back(E(15, 1500));
        p = run(left, 10, 5, &gone);
        bool right = true;
        for (size_t i = 0; i < left.size(); ++i) right = right && (gone[i] != 0) == (left[i].build <= 10);
        check(p.triggered && right && p.builds_after == 5, "the fifth more: builds 6 to 10 go, 11 to 15 stay");
    }
    { // a build is as new as its newest folder: an old build used again today stays
        std::vector<Entry> e = Builds(10, 2);
        e.push_back(E(1, 5000)); // build 1 was run again after all the others
        const Plan p = run(e, 10, 5, &gone);
        bool one_kept = true, six_gone = true;
        for (size_t i = 0; i < e.size(); ++i) {
            if (e[i].build == 1) one_kept = one_kept && !gone[i];
            if (e[i].build == 6) six_gone = six_gone && gone[i];
        }
        check(p.triggered && one_kept && six_gone, "a build run again today is among the newest five");
    }
    { // the run in progress, and its build, are never removed even when its folders look old
        std::vector<Entry> e = Builds(10, 2);
        e.push_back(E(2, 1, 2, true)); // this process runs build 2; clock looks ancient
        const Plan p = run(e, 10, 5, &gone);
        bool two_kept = true;
        for (size_t i = 0; i < e.size(); ++i)
            if (e[i].build == 2) two_kept = two_kept && !gone[i];
        check(p.triggered && two_kept, "the build that is running keeps all its folders");
        check(p.builds_after == 5, "and it is one of the five that stay");
    }
    { // folders with no recorded build are one build together
        std::vector<Entry> e = Builds(9, 2);
        for (uint64_t i = 0; i < 40; ++i) e.push_back(E(0, i)); // older than everything
        const Plan p = run(e, 10, 5, &gone);
        bool unrecorded_gone = true;
        for (size_t i = 0; i < e.size(); ++i)
            if (e[i].build == 0) unrecorded_gone = unrecorded_gone && gone[i];
        check(p.triggered && unrecorded_gone && p.builds_after == 5, "unrecorded folders count as one old build");
    }
    { // 0 = off; and nothing to measure
        std::vector<Entry> e = Builds(30, 2);
        Plan p = run(e, 0, 5, &gone);
        check(!p.triggered && p.removed == 0, "a trigger of 0 removes nothing");
        p = Decide(nullptr, 0, 10, 5, nullptr);
        check(p.total_before == 0 && p.removed == 0 && !p.triggered, "an empty folder is fine");
    }
    // the low-disk guard
    check(DiskTooFull(8 * GB, 20 * GB), "8 GB free is under a 20 GB floor");
    check(!DiskTooFull(84 * GB, 20 * GB), "84 GB free is not");
    check(!DiskTooFull(0, 0), "a floor of 0 is no guard");

    std::printf("capture_retention: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
