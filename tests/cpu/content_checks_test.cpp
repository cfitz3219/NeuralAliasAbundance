#include "test_support.hpp"
#include "providers/classifier/content_checks.hpp"
using namespace na::classifier;
static SampleIdentity key() { return {{1,2,3,4,5,6},7,8,0,1,1,true,true,true}; }
static Matrix projection() { return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; }
static CompositionWitness witness() { return {key(), {{{0.4,0.5,0.6}}}, {{{0.1,0.1,0.1}}}, {0.5}}; }
int main() { return test::run({
    {"depth checks independent world position and camera", [] {
        CHECK(CheckDepth(key(),key(),{0.5},{{{0,0,0.5}}},projection(),0,0,false).result == ContentResult::Consistent);
        CHECK(CheckDepth(key(),key(),{0.8},{{{0,0,0.5}}},projection(),0,0,false).result == ContentResult::Contradicted);
        auto p = projection(); p[3] = 0.1;
        CHECK(CheckDepth(key(),key(),{0.5},{{{0,0,0.5}}},p,0,0,false).result == ContentResult::Unavailable);
    }},
    {"depth supports explicit reverse convention without guessing", [] {
        CHECK(CheckDepth(key(),key(),{0.2},{{{0,0,0.8}}},projection(),0,0,true).result == ContentResult::Consistent);
        CHECK(CheckDepth(key(),key(),{0.2},{{{0,0,0.8}}},projection(),0,0,false).result == ContentResult::Contradicted);
    }},
    {"incomplete stale and malformed samples fail closed", [] {
        auto k = key(); k.completed = false;
        CHECK(CheckDepth(k,key(),{0.5},{{{0,0,0.5}}},projection(),0,0,false).result == ContentResult::Unavailable);
        k = key(); ++k.version;
        CHECK(CheckDepth(k,key(),{0.5},{{{0,0,0.5}}},projection(),0,0,false).result == ContentResult::Unavailable);
        CHECK(CheckDepth(key(),key(),{},{{{0,0,0.5}}},projection(),0,0,false).result == ContentResult::Unavailable);
        CHECK(CheckDepth(key(),key(),{std::nan("")},{{{0,0,0.5}}},projection(),0,0,false).result == ContentResult::Unavailable);
    }},
    {"color reconstructs source-over evidence and detects HUD contamination", [] {
        CHECK(CheckColor(key(),{{{0.6,0.8,1.0}}},{witness()}).result == ContentResult::Consistent);
        CHECK(CheckColor(key(),{{{0.4,0.5,0.6}}},{witness()}).result == ContentResult::Contradicted);
    }},
    {"opaque HUD leaves unknowns until another witness exposes them", [] {
        auto w = witness(); w.alpha[0] = 1.0;
        const auto r = CheckColor(key(),{{{0.6,0.8,1.0}}},{w});
        CHECK(r.result == ContentResult::Unavailable && r.unresolved == 1 && r.checked == 0);
        CHECK(CheckColor(key(),{{{0.6,0.8,1.0}}},{w,witness()}).result == ContentResult::Consistent);
    }},
    {"stale or malformed witness does not get ignored", [] {
        auto w = witness(); ++w.source.scope.epoch;
        CHECK(CheckColor(key(),{{{0.6,0.8,1.0}}},{witness(),w}).result == ContentResult::Unavailable);
        w = witness(); w.alpha[0] = -1;
        CHECK(CheckColor(key(),{{{0.6,0.8,1.0}}},{w}).result == ContentResult::Unavailable);
        w = witness(); w.composite[0][0] = std::nan("");
        CHECK(CheckColor(key(),{{{0.6,0.8,1.0}}},{w}).result == ContentResult::Unavailable);
        CHECK(CheckColor(key(),{{{0.6,0.8,1.0}}},{witness()},1.0).result == ContentResult::Unavailable);
    }}
}); }
