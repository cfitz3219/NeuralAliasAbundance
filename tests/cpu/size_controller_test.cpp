// Render-small changeover controller (COMMS 4bd474c I1.2, Astra a066d76): pure state, no Windows.
// Fake renderers: (a) follows the request, (b) ignores it, (c) rebuilds buffers but keeps a native
// scene, plus supersede / late events / UNKNOWN samples / contradiction resets / native DLAA.
#include "test_support.hpp"
#include "host_dx12/native_present/size_controller.hpp"
using namespace na::native_present::ctl;
namespace {
const Extent kNative{3440, 1440}, kQuality{2293, 960}, kBalanced{2000, 837};
// One present: the scene sample, then the tick.
Action present(Extent scene, uint64_t& t, uint64_t step = 16) { Observe(scene); t += step; return Tick(t); }
}
int main() {
    return test::run({
        {"same-size display mode change re-asks once and requires new scene confirmation", [] {
            uint64_t t=1000;
            Reset(kNative,kQuality,.667f);
            CHECK(Choose(kQuality,.667f,t).kind==ActionKind::None);
            CHECK(Choose(kQuality,.667f,t,true).kind==ActionKind::Ask);
            Asked(80,t);
            CHECK(Same(Get().buffers,kQuality) && Same(Get().applied,kQuality));
            CHECK(Get().matches==0 && Get().phase==Phase::Allocated);
            CHECK(present({},t).kind==ActionKind::None);
            CHECK(present(kQuality,t).kind==ActionKind::None);
            CHECK(present(kNative,t).kind==ActionKind::None); // contradiction clears partial confirmation
            CHECK(present(kQuality,t).kind==ActionKind::None);
            CHECK(present(kQuality,t).kind==ActionKind::None);
            CHECK(present(kQuality,t).kind==ActionKind::Commit);
            CHECK(Choose(kQuality,.667f,t).kind==ActionKind::None); // no request storm
        }},
        {"native boot: the game follows Quality, the mode applies only after 3 fresh scene samples", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            const Action a = Choose(kQuality, 0.667f, t);
            CHECK(a.kind == ActionKind::Ask && Same(a.size, kQuality));
            Asked(7, t);
            CHECK(Same(AllocationSize(), kQuality));
            CHECK(present(kNative, t).kind == ActionKind::None); // before the rebuild: native samples do not count
            Allocated(kQuality, t);
            CHECK(Same(Get().buffers, kQuality) && Same(Get().applied, kNative)); // real buffers immediately; mode not yet
            CHECK(present(kQuality, t).kind == ActionKind::None);
            CHECK(present({}, t).kind == ActionKind::None);       // UNKNOWN neither counts nor resets
            CHECK(present(kQuality, t).kind == ActionKind::None);
            const Action c = present(kQuality, t);
            CHECK(c.kind == ActionKind::Commit && Same(c.size, kQuality));
            CHECK(Same(Get().applied, kQuality) && Get().phase == Phase::Idle);
            CHECK(Choose(kQuality, 0.667f, t).kind == ActionKind::None); // nothing more to ask
        }},
        {"a known different sample between matches resets the count (match, other, match, match does not commit)", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            Choose(kQuality, 0.667f, t); Asked(1, t); Allocated(kQuality, t);
            present(kQuality, t); present(kNative, t); present(kQuality, t);
            CHECK(present(kQuality, t).kind == ActionKind::None);
            CHECK(present(kQuality, t).kind == ActionKind::Commit);
        }},
        {"one sample per present: several Observe calls in one present count once", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            Choose(kQuality, 0.667f, t); Asked(1, t); Allocated(kQuality, t);
            Observe(kQuality); Observe(kQuality); Observe(kQuality);
            t += 16; CHECK(Tick(t).kind == ActionKind::None);
            CHECK(Get().matches == 1);
        }},
        {"a game that ignores the request: after the timeout the advertisement rolls back, native DLAA stays", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            Choose(kQuality, 0.667f, t); Asked(2, t);
            Action a{};
            for (int i = 0; i < 400 && a.kind == ActionKind::None; ++i) a = present(kNative, t);
            CHECK(a.kind == ActionKind::Ignored && Same(a.size, kNative));
            CHECK(Get().phase == Phase::Idle && Same(Get().applied, kNative) && Get().last == Outcome::Ignored);
            CHECK(Same(AllocationSize(), kNative));
        }},
        {"buffers rebuilt but the scene stayed native: the previous layout is asked back, then confirmed", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            Choose(kQuality, 0.667f, t); Asked(3, t); Allocated(kQuality, t);
            Action a{};
            for (int i = 0; i < 100 && a.kind == ActionKind::None; ++i) a = present(kNative, t);
            CHECK(a.kind == ActionKind::Restore && Same(a.size, kNative));
            CHECK(Get().last == Outcome::Contradicted && Same(Get().applied, kNative));
            Asked(4, t); Allocated(kNative, t);
            present(kNative, t); present(kNative, t);
            const Action c = present(kNative, t);
            CHECK(c.kind == ActionKind::Commit && Same(c.size, kNative) && c.scale == 1.f);
        }},
        {"rebuilt then silent (no scene samples): the timeout after the allocation also restores", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            Choose(kQuality, 0.667f, t); Asked(5, t); Allocated(kQuality, t);
            Action a{};
            for (int i = 0; i < 400 && a.kind == ActionKind::None; ++i) a = present({}, t);
            CHECK(a.kind == ActionKind::Restore && Same(a.size, kNative) && Get().last == Outcome::Restored);
        }},
        {"Quality -> Balanced -> native DLAA: each supersedes, the last one wins, no stale commit", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            Choose(kQuality, 0.667f, t); Asked(10, t); Allocated(kQuality, t);
            present(kQuality, t);
            const Action b = Choose(kBalanced, 0.58f, t);
            CHECK(b.kind == ActionKind::Ask && Same(b.size, kBalanced) && Get().superseded >= 1);
            Asked(11, t);
            present(kQuality, t); present(kQuality, t); present(kQuality, t); // the abandoned size never commits
            CHECK(Same(Get().applied, kNative));
            const Action n = Choose(kNative, 1.f, t); // DLAA (upscaling Off): back to native
            CHECK(n.kind == ActionKind::Ask && Same(n.size, kNative));
            Asked(12, t);
            Allocated(kBalanced, t);                 // a late rebuild for the superseded request
            CHECK(Get().phase == Phase::Asked);      // does not satisfy the native target
            Allocated(kNative, t);
            present(kNative, t); present(kNative, t);
            const Action c = present(kNative, t);
            CHECK(c.kind == ActionKind::Commit && Same(c.size, kNative) && c.scale == 1.f);
        }},
        {"posting the ask failed: nothing stays pending", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            Choose(kQuality, 0.667f, t); Asked(0, t);
            CHECK(Get().phase == Phase::Idle && Same(AllocationSize(), kNative));
        }},
        {"native DLAA when already native needs no ask", [] {
            uint64_t t = 1000;
            Reset(kNative, kNative, 1.f);
            CHECK(Choose(kNative, 1.f, t).kind == ActionKind::None);
        }},
    });
}
