// Scene depth publication from accumulated frames (scene_pick.hpp, 2026-11-04).
// Reproduces the SE5 05dc62b pattern: the scene depth is missing from about one
// frame in nine and shadow maps never rank (score 0), so the window holds zeros;
// the published depth must never drop for those frames. Also: a one-frame
// challenger is not a contradiction, a real replacement switches, a retired
// depth is dropped at once, and a long absence finally clears.
#include "test_support.hpp"
#include "providers/discovery/scene_pick.hpp"
using namespace na::discovery;

namespace {
constexpr uint64_t kScene = 13581, kOther = 20733, kShadow = 1808;
// One publication = ~11 frames at 40-44 fps.
void frames(DepthWindow& w, uint64_t depth, int n) { for (int i = 0; i < n; ++i) w.Push(depth); }
}

int main() {
    return test::run({
        {"first pick needs a majority", [] {
            DepthWindow w; DepthPick p;
            frames(w, kScene, 1);
            CHECK(p.Publish(w, true) == PickReason::none);
            CHECK(p.depth == 0);
            frames(w, kScene, 2);
            CHECK(p.Publish(w, true) == PickReason::first);
            CHECK(p.depth == kScene);
        }},
        {"missing scene pass never clears the pick", [] {
            DepthWindow w; DepthPick p;
            frames(w, kScene, 16);
            p.Publish(w, true);
            for (int pub = 0; pub < 400; ++pub) {
                // 11 frames per publication, the scene missing from one in nine; sometimes 2-3 in a row.
                for (int f = 0; f < 11; ++f) w.Push(((pub * 11 + f) % 9 == 0 || (pub % 17 == 0 && f < 3)) ? 0 : kScene);
                p.Publish(w, true);
                CHECK(p.depth == kScene);
            }
        }},
        {"a publication whose snapshot holds only zeros keeps the pick", [] {
            DepthWindow w; DepthPick p;
            frames(w, kScene, 16); p.Publish(w, true);
            frames(w, 0, 11); // a whole publication with no eligible depth (only score-0 shadow maps)
            CHECK(p.Publish(w, true) == PickReason::kept);
            CHECK(p.depth == kScene);
            frames(w, kScene, 11);
            CHECK(p.Publish(w, true) == PickReason::kept);
            CHECK(p.depth == kScene);
        }},
        {"a brief challenger is not a contradiction", [] {
            DepthWindow w; DepthPick p;
            frames(w, kScene, 16); p.Publish(w, true);
            frames(w, kShadow, 3); frames(w, kScene, 8);
            CHECK(p.Publish(w, true) == PickReason::kept);
            CHECK(p.depth == kScene);
            frames(w, kShadow, 12); // one publication where another depth leads
            CHECK(p.Publish(w, true) == PickReason::kept);
            CHECK(p.depth == kScene);
            frames(w, kScene, 16);
            CHECK(p.Publish(w, true) == PickReason::kept);
            CHECK(p.challenger_streak == 0);
        }},
        {"an established replacement switches after two publications", [] {
            DepthWindow w; DepthPick p;
            frames(w, kScene, 16); p.Publish(w, true);
            frames(w, kOther, 16);
            CHECK(p.Publish(w, true) == PickReason::kept);
            CHECK(p.depth == kScene);
            frames(w, kOther, 11);
            CHECK(p.Publish(w, true) == PickReason::replaced);
            CHECK(p.depth == kOther);
        }},
        {"a retired depth is dropped at once and its successor adopted", [] {
            DepthWindow w; DepthPick p;
            frames(w, kScene, 16); p.Publish(w, true);
            frames(w, kOther, 11); // resize: the new depth already leads
            CHECK(p.Publish(w, false) == PickReason::retired);
            CHECK(p.depth == kOther);
            DepthWindow w2; DepthPick q;
            frames(w2, kScene, 16); q.Publish(w2, true);
            frames(w2, 0, 16);
            CHECK(q.Publish(w2, false) == PickReason::retired);
            CHECK(q.depth == 0);
        }},
        {"retired votes neither win nor block a current replacement", [] {
            DepthWindow w; DepthPick p;
            frames(w, kScene, 16); p.Publish(w, true);
            frames(w, 0, 2);
            frames(w, kOther, 7);
            CHECK(p.Publish(w, false) == PickReason::retired);
            CHECK(p.depth == kOther);

            DepthWindow w2; DepthPick q;
            frames(w2, kScene, 16); q.Publish(w2, true);
            frames(w2, kOther, 1);
            CHECK(q.Publish(w2, false) == PickReason::retired);
            CHECK(q.depth == 0);
        }},
        {"a long absence clears after eight publications", [] {
            DepthWindow w; DepthPick p;
            frames(w, kScene, 16); p.Publish(w, true);
            frames(w, 0, 16);
            for (unsigned i = 1; i < DepthPick::kAbsentPublications; ++i) {
                CHECK(p.Publish(w, true) == PickReason::kept);
                CHECK(p.depth == kScene);
            }
            CHECK(p.Publish(w, true) == PickReason::absent);
            CHECK(p.depth == 0);
        }},
    });
}
