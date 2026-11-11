// The split-camera vote (native11/split_camera.hpp) replayed without D3D11:
// draws are fed as the two bound constant ranges they used, exactly as the live
// provider feeds them. Synthetic data only; saved game runs are not committed.
#include "test_support.hpp"
#include "host_dx12/native11/split_camera.hpp"

#include <map>

using namespace na::native11;

namespace {

constexpr uint32_t kW = 3440, kH = 1440;

// A camera-relative, reversed-depth scene projection for the test extent.
std::vector<uint8_t> Projection(double scale = 1.0)
{
    const na::camera::Matrix m{double(kH) / kW * scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, 0, 4, 0, 0, 1, 0};
    std::vector<uint8_t> bytes(64);
    for (size_t i = 0; i < 16; ++i) {
        const float v = static_cast<float>(m[i]);
        std::memcpy(bytes.data() + i * 4, &v, 4);
    }
    return bytes;
}
// A per-object block: this frame's and last frame's world transform, both pure translations.
std::vector<uint8_t> World(float x, float y = -8018.f, float z = 1746.f, float previous_x_step = 0.f)
{
    std::vector<uint8_t> bytes(160, 0);
    for (const size_t offset : {size_t(0), size_t(96)}) {
        const float tx = offset ? x + previous_x_step : x;
        const std::array<float, 16> world{1, 0, 0, tx, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1};
        std::memcpy(bytes.data() + offset, world.data(), 64);
    }
    return bytes;
}

// The provider's bookkeeping, reduced to what the rule needs.
struct Frame {
    std::map<SplitKey, uint32_t> votes;
    uint64_t scene_draws = 0;
    void Draw(const std::vector<uint8_t>* world, const std::vector<uint8_t>* projection, uint32_t times = 1,
              uint32_t world_first = 0, uint32_t world_count = 0, uint32_t projection_first = 0,
              uint32_t projection_count = 0)
    {
        scene_draws += times;
        if (!world || !projection) return;
        SplitKey key{};
        if (!MakeSplitKey(world->data(), world->size(), world_first, world_count, projection->data(),
                          projection->size(), projection_first, projection_count, key)) return;
        votes[key] += times;
    }
    SplitVerdict Choose(std::array<double, 3>* origin = nullptr, uint32_t w = kW, uint32_t h = kH) const
    {
        auto best = votes.cend();
        na::camera::Matrix vp{};
        const auto verdict = ChooseSplitCamera(votes, [](uint32_t draws) { return draws; }, w, h, scene_draws, best, vp);
        if (origin && best != votes.cend()) *origin = {best->first.second[0], best->first.second[1], best->first.second[2]};
        return verdict;
    }
};

} // namespace

int main()
{
    return test::run({
        {"the world block most scene draws share gives the camera origin", [] {
            const auto vp = Projection(), world = World(11659.f), prop = World(7.f);
            Frame f;
            f.Draw(&world, &vp, 39);
            f.Draw(&prop, &vp, 2);
            f.Draw(nullptr, nullptr, 20); // draws without the pair still count as scene draws
            std::array<double, 3> origin{};
            CHECK(f.Choose(&origin) == SplitVerdict::Chosen);
            CHECK(origin[0] == -11659.0 && origin[1] == 8018.0 && origin[2] == -1746.0);
        }},
        {"no draw with both ranges: no votes", [] {
            Frame f;
            f.Draw(nullptr, nullptr, 100);
            CHECK(f.Choose() == SplitVerdict::NoVotes);
        }},
        {"a small object drawn once is not a world origin", [] {
            const auto vp = Projection(), world = World(5.f);
            Frame f;
            f.Draw(&world, &vp, 1);
            CHECK(f.Choose() == SplitVerdict::FewDraws);
        }},
        {"two origins with equal draws: no majority, no camera", [] {
            const auto vp = Projection(), a = World(11659.f), b = World(7.f);
            Frame f;
            f.Draw(&a, &vp, 20);
            f.Draw(&b, &vp, 20);
            CHECK(f.Choose() == SplitVerdict::NoMajority);
            f.Draw(&a, &vp, 1); // one more draw settles it
            CHECK(f.Choose() == SplitVerdict::Chosen);
        }},
        {"the winner must cover an eighth of the scene's draws", [] {
            const auto vp = Projection(), world = World(11659.f);
            Frame f;
            f.Draw(&world, &vp, 10);
            f.Draw(nullptr, nullptr, 71); // 81 scene draws: 10 * 8 = 80 < 81
            CHECK(f.Choose() == SplitVerdict::SmallShare);
            Frame g;
            g.Draw(&world, &vp, 10);
            g.Draw(nullptr, nullptr, 70); // 80 scene draws: exactly an eighth passes
            CHECK(g.Choose() == SplitVerdict::Chosen);
        }},
        {"a projection for another extent does not qualify, and does not count toward the majority", [] {
            const auto vp = Projection(), world = World(11659.f), other = World(7.f);
            Frame f;
            f.Draw(&world, &vp, 30);
            CHECK(f.Choose(nullptr, 1920, 1200) == SplitVerdict::NoProjection); // 16:10 against a 21:9 lens
            // A shadow pass shares the world blocks with its own, non-scene matrix.
            std::vector<uint8_t> shadow(64, 0);
            const std::array<float, 16> ortho{0.01f, 0, 0, 0, 0, 0.01f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0, 1};
            std::memcpy(shadow.data(), ortho.data(), 64);
            f.Draw(&other, &shadow, 200);
            // 200 draws with another origin outnumber the scene's 30, but their matrix is not a
            // scene projection, so they are no rival: 30 of 30 qualified, and 30 * 8 >= 230.
            CHECK(f.Choose() == SplitVerdict::Chosen);
        }},
        {"the eighth is measured against all scene draws, shadow-like passes included", [] {
            const auto vp = Projection(), world = World(11659.f);
            std::vector<uint8_t> shadow(64, 0);
            Frame f;
            f.Draw(&world, &vp, 30);
            f.Draw(&world, &shadow, 300);
            CHECK(f.Choose() == SplitVerdict::SmallShare);
        }},
        {"only a 64-byte projection range and a 160-byte world range make a vote", [] {
            const auto vp = Projection(), world = World(11659.f);
            std::vector<uint8_t> wide_vp(128, 0), ring(160 * 3, 0), big_vp(64 * 4, 0);
            std::memcpy(wide_vp.data(), vp.data(), 64);
            std::memcpy(ring.data() + 160, world.data(), 160);
            std::memcpy(big_vp.data() + 64 * 2, vp.data(), 64);
            SplitKey key{};
            CHECK(!MakeSplitKey(world.data(), world.size(), 0, 0, wide_vp.data(), wide_vp.size(), 0, 0, key));
            CHECK(MakeSplitKey(world.data(), world.size(), 0, 0, wide_vp.data(), wide_vp.size(), 0, 4, key));
            CHECK(!MakeSplitKey(ring.data(), ring.size(), 0, 0, vp.data(), vp.size(), 0, 0, key));
            // Offset binds into shared rings: constants 10..19 of the world ring, 8..11 of the projection ring.
            SplitKey ringed{}, plain{};
            CHECK(MakeSplitKey(ring.data(), ring.size(), 10, 10, big_vp.data(), big_vp.size(), 8, 4, ringed));
            CHECK(MakeSplitKey(world.data(), world.size(), 0, 0, vp.data(), vp.size(), 0, 0, plain));
            CHECK(ringed == plain); // the same evidence through a different binding is the same vote
            CHECK(!MakeSplitKey(world.data(), world.size(), 10, 0, vp.data(), vp.size(), 0, 0, key)); // start past the end
            CHECK(!MakeSplitKey(world.data(), world.size(), 0, 0, vp.data(), vp.size(), 4, 0, key));
        }},
        {"a world block that is not two pure translations, or sits at the origin, is not a witness", [] {
            const auto vp = Projection();
            auto rotated = World(11659.f);
            const float s = 0.5f;
            std::memcpy(rotated.data() + 4, &s, 4); // an off-diagonal term in this frame's transform
            auto previous_rotated = World(11659.f);
            std::memcpy(previous_rotated.data() + 96 + 4, &s, 4);
            const auto zero = World(0.f, 0.f, 0.f);
            SplitKey key{};
            CHECK(!MakeSplitKey(rotated.data(), rotated.size(), 0, 0, vp.data(), vp.size(), 0, 0, key));
            CHECK(!MakeSplitKey(previous_rotated.data(), previous_rotated.size(), 0, 0, vp.data(), vp.size(), 0, 0, key));
            CHECK(!MakeSplitKey(zero.data(), zero.size(), 0, 0, vp.data(), vp.size(), 0, 0, key));
        }},
        {"a moving camera: last frame's origin is part of the vote's identity", [] {
            const auto vp = Projection();
            const auto moving = World(11659.f, -8018.f, 1746.f, 2.f), still = World(11659.f);
            Frame f;
            f.Draw(&moving, &vp, 25);
            f.Draw(&still, &vp, 5);
            CHECK(f.votes.size() == 2);
            CHECK(f.Choose() == SplitVerdict::Chosen);
        }},
        {"every verdict has a log name", [] {
            for (auto v : {SplitVerdict::Chosen, SplitVerdict::NoVotes, SplitVerdict::NoProjection,
                           SplitVerdict::FewDraws, SplitVerdict::NoMajority, SplitVerdict::SmallShare})
                CHECK(std::string(SplitVerdictName(v)) != "?");
        }},
    });
}
