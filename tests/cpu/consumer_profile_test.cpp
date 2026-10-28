// J2 placement design (docs/PLACEMENT-DESIGN-RESPONSE.md): role classes, frame
// labelling and the learned consumer profile. Cases from the design and Astra's review:
//   SER-like alpha (water reads + post in the scene list), beta (no water), gamma (post in
//   a later list); shader variants of one pass; copy-then-clear exit; a class shared by a
//   mid-scene reader and the post entry (producer qualification); contradiction demotion.
#include "host_dx12/j2/consumer_profile.hpp"
#include <cstdio>
#include <vector>

namespace {
using namespace na::j2;
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } }
constexpr uint32_t W = 3440, H = 1440;
constexpr uint64_t kRootWater = 11, kRootPost = 22, kGeo = 100, kGeo2 = 101;

ColorEvent write(bool depth = true, uint64_t pso = kGeo, Op op = Op::DrawWrite)
{
    ColorEvent c; c.write = true; c.op = op; c.with_depth = depth; c.raw.pipeline = pso; return c;
}
ColorEvent consumer(uint64_t root, uint64_t pso, uint32_t w, uint32_t h, uint32_t fmt, uint64_t producer, Op op = Op::GraphicsRead)
{
    ColorEvent c; c.op = op; c.producer = producer;
    c.raw.op = op; c.raw.root = root; c.raw.pipeline = pso; c.raw.dst_w = w; c.raw.dst_h = h; c.raw.dst_fmt = fmt; c.raw.color_srvs = 1;
    return c;
}
ColorEvent water(uint64_t pso = 300) { return consumer(kRootWater, pso, W / 2, H / 2, 10, kGeo); }
ColorEvent tonemap(uint64_t pso, uint64_t producer = kGeo) { return consumer(kRootPost, pso, W, H, 24, producer); }
ColorEvent bloom() { return consumer(33, 500, W / 4, H / 4, 10, kGeo); }

std::vector<ColorEvent> alpha(uint64_t post_pso) { return {write(), water(), write(), water(), write(), tonemap(post_pso), bloom()}; }
std::vector<ColorEvent> beta(uint64_t post_pso) { return {write(), write(), tonemap(post_pso), bloom()}; }
std::vector<ColorEvent> gamma(uint64_t post_pso) { return {write(), write(), /* next list */ tonemap(post_pso), bloom()}; }

void learn(ConsumerProfile& p, const std::vector<ColorEvent>& seq, uint64_t frame)
{
    std::vector<Labelled> l;
    LabelFrame(seq, W, H, l);
    for (const auto& x : l) p.Observe(x, frame);
}
ClassKey cls_of(const ColorEvent& c) { return MakeClass(c.raw, W, H); }
} // namespace

int main()
{
    // Labelling.
    {
        std::vector<Labelled> l;
        LabelFrame(alpha(400), W, H, l);
        check(l.size() == 4 && l[0].role == Role::Temporary && l[1].role == Role::Temporary &&
              l[2].role == Role::PostEntry && l[3].role == Role::Later, "alpha: water temporary, tonemap post entry, bloom later");
        LabelFrame(gamma(401), W, H, l);
        check(l.size() == 2 && l[0].role == Role::PostEntry, "gamma: post entry in a later list");
    }
    // Shader variants share a class; the water pass does not.
    check(cls_of(tonemap(400)) == cls_of(tonemap(401)), "variants of the post pass share one class");
    check(cls_of(tonemap(400)) != cls_of(water()), "water reader is a different class");
    check(SizeClass(W / 2, W) == 1 && SizeClass(W / 4, W) == 2 && SizeClass(W, W) == 0 && SizeClass(0, W) == 4, "size classes");

    // SER-like alternation with variants: accepted after 3 post-entry frames, water rejected.
    {
        ConsumerProfile p;
        uint64_t f = 1;
        learn(p, alpha(400), f++);
        check(p.Judge(cls_of(tonemap(401)), kGeo) == ConsumerProfile::Verdict::Unknown, "not accepted after one frame");
        learn(p, gamma(402), f++);
        learn(p, beta(403), f++);
        check(p.Judge(cls_of(tonemap(404)), kGeo) == ConsumerProfile::Verdict::Accepted, "post class accepted across alpha/gamma/beta and variants");
        check(p.Judge(cls_of(water(301)), kGeo) == ConsumerProfile::Verdict::Rejected, "water class rejected");
        check(p.Judge(cls_of(bloom()), kGeo) == ConsumerProfile::Verdict::Unknown, "downstream bloom teaches nothing");
        for (int i = 0; i < 50; ++i) learn(p, (i % 3 == 0) ? alpha(400 + i) : (i % 3 == 1) ? gamma(400 + i) : beta(400 + i), f++);
        check(p.Judge(cls_of(tonemap(999)), kGeo) == ConsumerProfile::Verdict::Accepted, "alternation never un-learns the class");
        check(p.AcceptedCount() == 1 && p.RejectedCount() == 1, "one accepted, one rejected class");
    }
    // Copy-then-clear (Astra #3): the copy is the exit even though the original is written again.
    {
        std::vector<ColorEvent> seq{write(), write(), consumer(0, 0, W, H, 10, kGeo, Op::CopyRead), write(false, 0, Op::ClearWrite)};
        std::vector<Labelled> l;
        LabelFrame(seq, W, H, l);
        check(l.size() == 1 && l[0].role == Role::PostEntry, "copy before a clear/reuse with no later read is the exit");
        // ...but a copy followed by MORE scene content is temporary.
        seq = {write(), consumer(0, 0, W, H, 10, kGeo, Op::CopyRead), write(true), tonemap(400)};
        LabelFrame(seq, W, H, l);
        check(l.size() == 2 && l[0].role == Role::Temporary && l[1].role == Role::PostEntry, "copy followed by scene content is temporary");
        // A reuse write without depth, then the colour read again: temporary.
        seq = {write(), consumer(0, 0, W, H, 10, kGeo, Op::CopyRead), write(false), tonemap(400)};
        LabelFrame(seq, W, H, l);
        check(l[0].role == Role::Temporary, "read again after a non-depth write: temporary");
    }
    // Shared class: the same pass shape reads mid-scene (after geometry A) and post-scene
    // (after geometry B). Ambiguous on its own; accepted only with producer B.
    {
        ConsumerProfile p;
        for (uint64_t f = 1; f <= 4; ++f)
            learn(p, {write(true, kGeo), tonemap(400, kGeo), write(true, kGeo2), tonemap(400, kGeo2)}, f);
        const ClassKey k = cls_of(tonemap(400));
        check(p.Judge(k, kGeo) == ConsumerProfile::Verdict::Ambiguous, "shared class after producer A: ambiguous (rejected live)");
        check(p.Judge(k, kGeo2) == ConsumerProfile::Verdict::AcceptedWithProducer, "shared class after producer B: accepted with producer");
        check(p.AmbiguousCount() == 1, "one ambiguous class");
    }
    // Contradiction demotes an accepted class at once.
    {
        ConsumerProfile p;
        for (uint64_t f = 1; f <= 3; ++f) learn(p, beta(400), f);
        const ClassKey k = cls_of(tonemap(400));
        check(p.Judge(k, kGeo) == ConsumerProfile::Verdict::Accepted, "accepted before the contradiction");
        p.Contradict(k, kGeo, 4);
        check(p.Judge(k, kGeo) != ConsumerProfile::Verdict::Accepted && p.Judge(k, kGeo) != ConsumerProfile::Verdict::AcceptedWithProducer,
              "demoted after a later scene write");
    }
    // Bounded: many distinct classes never grow past the cap.
    {
        ConsumerProfile p;
        for (uint64_t f = 1; f <= 200; ++f) learn(p, {write(), consumer(1000 + f, 1, W, H, 10, kGeo)}, f);
        check(p.Size() <= ConsumerProfile::kMaxEntries, "profile is bounded");
    }
    if (failures) { std::printf("consumer_profile_test: FAILED (%d)\n", failures); return 1; }
    std::printf("consumer_profile_test: OK\n");
    return 0;
}
