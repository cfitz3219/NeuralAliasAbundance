// P4-0D consumer anchor (insertion_trace.hpp): learn the first actual consumer after
// the final scene write in one recording, match it in a later recording.
// CPU-only cases from Astra's matrix (docs/POSTENTRY-PSO-PREPARATION.md):
//   variable mid-scene readers, variable final producer, shared consumer PSO,
//   copy-only consumer, barrier sentinel never a consumer, mode change.
#include "host_dx12/j2/insertion_trace.hpp"
#include <cstdio>
#include <vector>

namespace {
using namespace na::j2;
constexpr uint64_t C = 1;                       // scene colour
constexpr uint64_t kScene = 100, kScene2 = 101; // geometry PSOs
constexpr uint64_t kWater = 300, kTonemap = 400, kBloom = 500, kScope = 600;
constexpr uint64_t kRoot = 77;
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } }

InsertionEvent write(uint64_t pso) { return {C, 0, pso, true, Op::DrawWrite, 0}; }
InsertionEvent barrier() { return {C, 0, UINT64_MAX, false, Op::Barrier, 0}; }
InsertionEvent read(uint64_t pso, uint64_t dst = 9) { return {C, dst, pso, false, Op::GraphicsRead, kRoot}; }
InsertionEvent copy(uint64_t dst) { return {C, dst, 0, false, Op::CopyRead, 0}; }

// Replays a recording event by event, as the live trigger sees it: returns the number
// of matches and whether the first one was the post entry (no later colour write).
int replay(const ConsumerSignature& sig, InsertionTrace& t, const std::vector<InsertionEvent>& events, bool* only_at_post)
{
    int matches = 0;
    bool late_write = false;
    t.Reset();
    for (const auto& e : events) {
        t.Add(e);
        if (matches && e.write) late_write = true;
        if (!e.write && !MatchConsumer(sig, t.Live(C))) ++matches;
    }
    *only_at_post = matches >= 1 && !late_write;
    return matches;
}
std::vector<InsertionEvent> ser_events(int water, uint64_t final_producer = kScene, uint64_t post = kTonemap)
{
    std::vector<InsertionEvent> v{write(kScene)};
    for (int i = 0; i < water; ++i) { v.push_back(barrier()); v.push_back(read(kWater)); v.push_back(write(kScene)); }
    v.push_back(write(final_producer));
    v.push_back(barrier());
    v.push_back(read(post));
    v.push_back(read(kBloom, 10));
    return v;
}
} // namespace

int main()
{
    InsertionTrace t;
    // 1. Variable readers (primary): learn on a frame with 3 refraction reads, then match
    //    measured frames with 3 and 0 reads; the old occurrence anchor differs between them.
    {
        for (const auto& e : ser_events(3)) t.Add(e);
        const auto sig = t.AnalyzeConsumer(C);
        check(sig.mode == ConsumerSignature::UniqueConsumer && sig.consumer_pso == kTonemap && sig.op == Op::GraphicsRead,
              "tonemap learned as unique consumer");
        check(sig.consumer_root == kRoot, "consumer root signature learned");
        for (int water : {3, 0}) {
            bool clean = false;
            check(replay(sig, t, ser_events(water), &clean) == 1 && clean, "one match, at the post entry, with 3 and 0 mid-scene readers");
        }
    }
    // 2. Variable final producer: unique consumer does not depend on the producer.
    {
        t.Reset();
        for (const auto& e : ser_events(2, kScene)) t.Add(e);
        const auto sig = t.AnalyzeConsumer(C);
        bool clean = false;
        check(replay(sig, t, ser_events(2, kScene2), &clean) == 1 && clean, "a different final geometry PSO still matches");
    }
    // 3. Shared consumer PSO mid-scene and post-scene: never unique. With a distinguishing
    //    final producer it becomes producer+consumer; a repeated pair falls back to legacy.
    {
        t.Reset();
        t.Add(write(kScene)); t.Add(read(kTonemap)); t.Add(write(kScene2)); t.Add(read(kTonemap));
        auto sig = t.AnalyzeConsumer(C);
        check(sig.mode == ConsumerSignature::ProducerConsumer && sig.producer_pso == kScene2, "shared PSO, distinct producer");
        t.Reset();
        t.Add(write(kScene)); t.Add(read(kTonemap));                     // live: mid-scene shared use
        check(MatchConsumer(sig, t.Live(C)) != nullptr, "mid-scene shared use rejected by producer");
        t.Add(write(kScene2)); t.Add(read(kTonemap));
        check(MatchConsumer(sig, t.Live(C)) == nullptr, "post use matches with the final producer");
        t.Reset();
        t.Add(write(kScene)); t.Add(read(kTonemap)); t.Add(write(kScene)); t.Add(read(kTonemap));
        sig = t.AnalyzeConsumer(C);
        check(sig.mode == ConsumerSignature::Legacy, "repeated producer+consumer pair is ambiguous");
        check(MatchConsumer(sig, t.Live(C)) != nullptr, "legacy never matches through the consumer path");
    }
    // 4. Copy-only consumer: no PSO anchor; destination identity; repeated copies ambiguous.
    {
        t.Reset();
        t.Add(write(kScene)); t.Add(barrier()); t.Add(copy(42));
        auto sig = t.AnalyzeConsumer(C);
        check(sig.mode == ConsumerSignature::CopyDestination && sig.consumer_pso == 0 && sig.destination == 42, "copy destination anchor");
        t.Reset();
        t.Add(write(kScene)); t.Add(copy(43));
        check(MatchConsumer(sig, t.Live(C)) != nullptr, "other copy destination rejected");
        t.Add(copy(42));
        check(MatchConsumer(sig, t.Live(C)) == nullptr, "same copy destination matches");
        t.Reset();
        t.Add(write(kScene)); t.Add(copy(42)); t.Add(write(kScene)); t.Add(copy(42));
        check(t.AnalyzeConsumer(C).mode == ConsumerSignature::Legacy, "repeated copy destination is ambiguous");
    }
    // 5. Barrier sentinel and PSO 0 never become a consumer anchor.
    {
        t.Reset();
        t.Add(write(kScene)); t.Add(barrier());
        check(t.AnalyzeConsumer(C).mode == ConsumerSignature::None, "barrier-only boundary has no consumer");
        check(!t.Live(C).valid, "a barrier is not a live consumer");
        t.Reset();
        t.Add(write(kScene)); t.Add({C, 9, UINT64_MAX, false, Op::GraphicsRead, 0});
        check(t.AnalyzeConsumer(C).mode == ConsumerSignature::Legacy, "sentinel pipeline is never a unique PSO");
        t.Reset();
        t.Add(write(kScene)); t.Add(read(kTonemap, C));
        check(t.AnalyzeConsumer(C).mode == ConsumerSignature::None, "a read into the colour itself is not a consumer");
    }
    // 6. Mode change (scope): the new post PSO does not match the old signature on the
    //    change frame (an honest miss); the next learned signature matches it.
    {
        t.Reset();
        for (const auto& e : ser_events(1)) t.Add(e);
        const auto old_sig = t.AnalyzeConsumer(C);
        bool clean = false;
        check(replay(old_sig, t, ser_events(1, kScene, kScope), &clean) == 0, "change frame: no false match");
        for (const auto& e : ser_events(1, kScene, kScope)) t.Add(e);
        t.Reset();
        for (const auto& e : ser_events(1, kScene, kScope)) t.Add(e);
        const auto new_sig = t.AnalyzeConsumer(C);
        check(new_sig.consumer_pso == kScope && replay(new_sig, t, ser_events(0, kScene, kScope), &clean) == 1 && clean,
              "next recording matches the new consumer");
    }
    // 7. Legacy events (op unspecified) classify as before.
    {
        t.Reset();
        t.Add({C, 0, kScene, true}); t.Add({C, 0, UINT64_MAX, false}); t.Add({C, 9, kTonemap, false});
        const auto sig = t.AnalyzeConsumer(C);
        check(sig.mode == ConsumerSignature::UniqueConsumer && sig.op == Op::GraphicsRead, "legacy fields classified");
    }
    if (failures) { std::printf("consumer_anchor_test: FAILED (%d)\n", failures); return 1; }
    std::printf("consumer_anchor_test: OK\n");
    return 0;
}
