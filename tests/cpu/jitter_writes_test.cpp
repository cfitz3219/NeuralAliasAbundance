#include "test_support.hpp"
#include "core/temporal_jitter.hpp"
#include "host_dx12/j2/jitter_writes.hpp"
#include <array>
#include <cstring>
#include <vector>

using Matrix = std::array<float, 16>;
static Matrix shift(const Matrix& m, uint32_t frame) {
    std::array<double,16> d{};
    for (size_t i=0;i<16;++i) d[i]=m[i];
    const auto j = neural_alias::jitter_projection(d, neural_alias::temporal_jitter(frame,8),3440,1440);
    Matrix result{};
    for (size_t i=0;i<16;++i) result[i]=static_cast<float>(j[i]);
    return result;
}
int main() { return test::run({
    {"rotating buffers reproduce old drift, exact write ledger prevents accumulation", [] {
        const Matrix original{2,0,0,0, 0,3,0,0, 0,0,.1f,1, 0,0,1,0};
        float worst=0;
        for (uint32_t ring : {2u,3u,4u,6u,8u,16u}) {
        std::vector<Matrix> old(ring,original), fixed=old;
        Matrix last_written{}, last_original{};
        na::j2::JitterWrites ledger;
        float peak=0;
        for (uint32_t frame=1;frame<=480;++frame) {
            const size_t slot=frame%ring;
            auto old_input=old[slot];
            if (old_input==last_written) old_input=last_original;
            old[slot]=shift(old_input,frame); last_original=old_input; last_written=old[slot];
            auto input=fixed[slot];
            // Same upload resource, different offsets: exactly the game-ring case.
            ledger.Normalize(10,slot*256,reinterpret_cast<uint8_t*>(input.data()),64);
            CHECK(input==original);
            const auto output=shift(input,frame);
            CHECK(ledger.Apply(10,slot*256,reinterpret_cast<const uint8_t*>(input.data()),
                reinterpret_cast<const uint8_t*>(output.data()),[&]{ fixed[slot]=output; return true; }));
            CHECK(fixed[slot]==shift(original,frame));
            peak=(std::max)(peak,std::abs((old[slot][6]-fixed[slot][6])*720.0f));
        }
        std::printf("ring=%u old peak drift=%.4f pixels; corrected output equals native+one sample\n",ring,peak);
        worst=(std::max)(worst,peak);
        }
        CHECK(worst>1.0f);
    }},
    {"game rewrites and failed writes are not undone", [] {
        na::j2::JitterWrites ledger;
        std::array<uint8_t,64> native{}, patch{}, game{};
        patch.fill(1); game.fill(2);
        CHECK(!ledger.Apply(1,64,native.data(),patch.data(),[]{return false;}));
        auto read=patch; CHECK(ledger.Normalize(1,64,read.data(),64)==0);
        CHECK(ledger.Apply(1,64,native.data(),patch.data(),[]{return true;}));
        read=game; CHECK(ledger.Normalize(1,64,read.data(),64)==0); CHECK(read==game);
        read=patch; CHECK(ledger.Normalize(2,64,read.data(),64)==0); CHECK(read==patch);
        CHECK(ledger.Normalize(1,64,read.data(),64)==1); CHECK(read==native);
        CHECK(ledger.Apply(1,64,game.data(),patch.data(),[]{return true;}));
        read=patch; CHECK(ledger.Normalize(1,64,read.data(),64)==1); CHECK(read==game);
    }},
}); }
