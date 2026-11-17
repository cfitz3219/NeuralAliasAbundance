#include "test_support.hpp"
#include "core/jitter_domain.hpp"
using namespace na::jitter;
int main() {
    return test::run({
        {"late source evidence does not certify or overwrite a raster sample", [] {
            ColourDomain d; const Source s{10,2293,941,28}; d.Bind(s);
            const Sample written{.3125f,-.351852f};
            CHECK(d.Resolve(written).x==written.x);
            CHECK(!d.Observe(s,{60,100000,95000,1000,1000}));
            CHECK(d.Get()==Convention::Unknown);
            CHECK(d.Observe(s,{120,100000,94000,1000,1000}));
            CHECK(d.Get()==Convention::Unjittered && d.Resolve(written).x==0);
            CHECK(written.x==.3125f); // raster write is retained for calibration/other stages
        }},
        {"early and final colour retain independent jitter conventions", [] {
            ColourDomain early,final; const Source a{10,2293,941,10},b{10,2293,941,28}; early.Bind(a);final.Bind(b);
            early.Observe(a,{60,100000,1000,95000,1000}); CHECK(early.Observe(a,{120,100000,1000,95000,1000}));
            final.Observe(b,{60,100000,95000,1000,1000}); CHECK(final.Observe(b,{120,100000,95000,1000,1000}));
            CHECK(early.Resolve({.25f,.5f}).x==.25f && final.Resolve({.25f,.5f}).x==0);
        }},
        {"source changes reject old asynchronous windows and reacquire", [] {
            ColourDomain d; const Source a{10,2293,941,28},b{11,2293,941,28};d.Bind(a);
            d.Observe(a,{60,100000,95000,1000,1000});d.Bind(b);
            CHECK(!d.Observe(a,{120,100000,95000,1000,1000}));
            CHECK(!d.Observe(b,{121,100000,95000,1000,1000}));
            CHECK(!d.Observe(b,{120,100000,95000,1000,1000})); // late window cannot advance streak
            CHECK(d.Get()==Convention::Unknown);
            CHECK(d.Observe(b,{180,100000,95000,1000,1000}));
            const auto epoch=d.Epoch();CHECK(!d.Observe(b,{240,100000,95000,1000,1000}) && d.Epoch()==epoch);
        }},
        {"ambiguous movement does not oscillate a qualified source", [] {
            ColourDomain d; const Source s{10,2293,941,28};d.Bind(s);
            d.Observe(s,{60,100000,95000,1000,1000});d.Observe(s,{120,100000,95000,1000,1000});
            const auto epoch=d.Epoch();
            CHECK(!d.Observe(s,{180,100000,1000,95000,1000}));
            CHECK(!d.Observe(s,{240,100000,30000,30000,30000}));
            CHECK(!d.Observe(s,{300,100000,1000,95000,1000}));
            CHECK(d.Get()==Convention::Unjittered && d.Epoch()==epoch);
            CHECK(d.Observe(s,{360,100000,1000,95000,1000}));
            CHECK(d.Get()==Convention::Raster && d.Resolve({.25f,.5f}).y==.5f);
        }},
        {"a remembered verdict survives source changes and still yields to new evidence", [] {
            ColourDomain a(7), b(7), other(8);
            const Source s1{10,2293,941,28}, s2{11,2000,820,28}, s3{12,2000,820,10};
            { // memory is off until the host loads its settings: nothing is kept
                ColourDomain x(7), y(7); x.Bind(s1);
                x.Observe(s1,{60,100000,95000,1000,1000}); CHECK(x.Observe(s1,{120,100000,95000,1000,1000}));
                y.Bind(s2); CHECK(y.Get()==Convention::Unknown);
            }
            na::jitter::Remembered().enabled.store(true);
            a.Bind(s1);
            a.Observe(s1,{60,100000,95000,1000,1000}); CHECK(a.Observe(s1,{120,100000,95000,1000,1000}));
            b.Bind(s2); // new selection and size, same stage and format: no relearning
            CHECK(b.Get()==Convention::Unjittered && b.Resolve({.25f,.5f}).x==0);
            other.Bind(s2); CHECK(other.Get()==Convention::Unknown); // other stage: independent
            b.Bind(s3); CHECK(b.Get()==Convention::Unknown); // other format and nothing saved for the stage
            na::jitter::Remembered().Put(7, 0, static_cast<uint32_t>(Convention::Unjittered)); // saved per game
            const Source s4{13,2000,820,24}; b.Bind(s4); CHECK(b.Get()==Convention::Unjittered);
            CHECK(!b.Observe(s4,{60,100000,1000,95000,1000})); // one contrary window is not enough
            CHECK(b.Observe(s4,{120,100000,1000,95000,1000}) && b.Get()==Convention::Raster);
        }},
        {"opposite sign is established separately rather than guessed from any shifted match", [] {
            ColourDomain d;const Source s{1,640,360,28};d.Bind(s);
            d.Observe(s,{60,100000,1000,1000,95000});CHECK(d.Observe(s,{120,100000,1000,1000,95000}));
            CHECK(d.Resolve({.25f,-.5f}).x==-.25f && d.Resolve({.25f,-.5f}).y==.5f);
        }},
    });
}
