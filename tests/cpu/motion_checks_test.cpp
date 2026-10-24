#include "test_support.hpp"
#include "providers/classifier/motion_checks.hpp"
using namespace na::motion_discovery;
static Color signal(double x,double y) { return {x/32,y/24,x*y/(32*24)}; }
struct Pair { Frame previous,current; Motion motion; };
static Pair make(double mx=3,double my=2,double jx=0,double jy=0) {
    Pair p{};
    p.previous={1,2,10,32,24,true,true,false,0,0,{},{}};
    p.current={1,2,11,32,24,true,true,false,jx,jy,{},{}};
    p.motion={1,2,11,5,6,true,true,{}};
    for (unsigned y=0;y<24;++y) for (unsigned x=0;x<32;++x) {
        p.previous.color.push_back(signal(x,y)); p.previous.stable.push_back(1);
        p.current.color.push_back(signal(x+mx-jx,y+my-jy));
        p.current.stable.push_back(x>=5 && x<26 && y>=4 && y<19 ? 1 : 0);
        p.motion.values.push_back({mx,my});
    }
    return p;
}
static Report run(const Pair& p,Convention c={}) { return Evaluate(p.previous,p.current,p.motion,c,0.001); }
int main() { return test::run({
    {"known image warp matches and wrong sign scale and channels fail", [] {
        const auto p=make(); const auto r=run(p);
        CHECK(r.verdict==Verdict::Consistent && r.checked>100); NEAR(r.mean_error,0,1e-12);
        CHECK(run(p,{-1,-1,false}).verdict==Verdict::Contradicted);
        CHECK(run(p,{0.5,0.5,false}).verdict==Verdict::Contradicted);
        auto swapped=p; for (auto& v:swapped.motion.values) std::swap(v[0],v[1]);
        CHECK(run(swapped).verdict==Verdict::Contradicted);
    }},
    {"jitter excluded and included conventions map to same correspondence", [] {
        auto p=make(3,2,0.3,-0.25);
        CHECK(run(p).verdict==Verdict::Consistent);
        CHECK(run(p,{1,1,true}).verdict==Verdict::Contradicted);
        for (auto& v:p.motion.values) { v[0]-=0.3; v[1]+=0.25; }
        CHECK(run(p,{1,1,true}).verdict==Verdict::Consistent);
    }},
    {"static flat and all-invalid frames cannot identify motion semantics", [] {
        auto p=make(0,0); CHECK(run(p).verdict==Verdict::Ambiguous);
        for (auto& c:p.previous.color) c={0.5,0.5,0.5};
        for (auto& c:p.current.color) c={0.5,0.5,0.5};
        for (auto& v:p.motion.values) v={3,2};
        CHECK(run(p).verdict==Verdict::Ambiguous);
        for (auto& v:p.current.stable) v=0;
        CHECK(run(p).verdict==Verdict::Unavailable);
    }},
    {"candidate cannot hide errors by moving off screen", [] {
        auto p=make(); for (auto& v:p.motion.values) v={1000,1000};
        const auto r=run(p); CHECK(r.verdict==Verdict::Contradicted && r.out_of_bounds>0);
    }},
    {"one bad correspondence is not diluted by a large correct image", [] {
        auto p=make(); p.motion.values[8*32+8]={0,0};
        CHECK(run(p).verdict==Verdict::Contradicted);
    }},
    {"cut gaps epochs and uncompleted snapshots require reacquisition", [] {
        auto p=make(); p.current.reset=true; CHECK(run(p).verdict==Verdict::Unavailable);
        p=make(); ++p.current.frame; CHECK(run(p).verdict==Verdict::Unavailable);
        p=make(); ++p.previous.epoch; CHECK(run(p).verdict==Verdict::Unavailable);
        p=make(); p.motion.completed=false; CHECK(run(p).verdict==Verdict::Unavailable);
        p=make(); p.previous.lifetime_valid=false; CHECK(run(p).verdict==Verdict::Unavailable);
    }},
    {"malformed and NaN values cannot be masked out", [] {
        auto p=make(); p.motion.values[0][0]=std::nan(""); CHECK(run(p).verdict==Verdict::Unavailable);
        p=make(); p.motion.values.pop_back(); CHECK(run(p).verdict==Verdict::Unavailable);
        p=make(); p.current.stable[0]=2; CHECK(run(p).verdict==Verdict::Unavailable);
        CHECK(run(make(),{0,1,false}).verdict==Verdict::Unavailable);
    }},
    {"visibility exclusions cannot silently remove most validation pixels", [] {
        auto p=make(); for (auto& v:p.previous.stable) v=0;
        CHECK(run(p).verdict==Verdict::Unavailable);
    }}
}); }
