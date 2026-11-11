// The shared rules that admit a camera with no inverse beside it
// (providers/camera/camera_admission.hpp). The DX12 host and the native DX11
// provider both call these, so one test covers both. Synthetic data only.
#include "test_support.hpp"
#include "providers/camera/camera_admission.hpp"

#include <cstring>

using namespace na::camera;

namespace {

constexpr uint32_t kW = 3440, kH = 1440;

// A reversed-depth perspective camera at `pos`, turned `yaw` radians about the
// up axis. Column-vector convention (clip = M * p), as FindMatrices normalises.
Matrix Camera(double yaw, std::array<double, 3> pos = {0, 0, 0}, double scale = 1.5, double aspect = double(kW) / kH)
{
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double right[3]{c, 0, -s}, up[3]{0, 1, 0}, forward[3]{s, 0, c};
    Matrix view{};
    const double* rows[3]{right, up, forward};
    for (int r = 0; r < 3; ++r) {
        double t = 0;
        for (int j = 0; j < 3; ++j) { view[r * 4 + j] = rows[r][j]; t -= rows[r][j] * pos[j]; }
        view[r * 4 + 3] = t;
    }
    view[15] = 1;
    const Matrix proj{scale / aspect, 0, 0, 0, 0, scale, 0, 0, 0, 0, 0, 0.1, 0, 0, 1, 0};
    Matrix m{};
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k) m[r * 4 + col] += proj[r * 4 + k] * view[k * 4 + col];
    return m;
}
void Store(std::vector<uint8_t>& block, size_t offset, const Matrix& m)
{
    for (size_t i = 0; i < 16; ++i) {
        const float v = static_cast<float>(m[i]);
        std::memcpy(block.data() + offset + i * 4, &v, 4);
    }
}
// What the provider would re-read from the block: the matrix at float precision.
Matrix Stored(const Matrix& m)
{
    std::vector<uint8_t> bytes(64);
    Store(bytes, 0, m);
    return LoadMatrix(bytes, 0, Layout::RowMajor);
}
std::vector<uint8_t> Block(size_t size, size_t offset, const Matrix& m)
{
    std::vector<uint8_t> block(size, 0);
    Store(block, offset, m);
    return block;
}
AdmissionBlock Bound(const std::vector<uint8_t>& bytes, uint64_t binding)
{
    AdmissionBlock b;
    b.slot.binding = binding;
    b.slot.size = bytes.size();
    b.bytes = bytes.data();
    b.size = bytes.size();
    return b;
}

} // namespace

int main()
{
    return test::run({
        {"a camera that keeps its place for four scene frames starts the camera", [] {
            StartWithoutInverse start;
            const StartSlot slot{3, 0, 1280, 320};
            for (uint64_t frame = 10; frame < 13; ++frame) CHECK(!start.Judge(slot, Camera(0.3 + 0.01 * double(frame)), frame));
            CHECK(start.Judge(slot, Camera(0.43), 13));
            CHECK(start.streak() == StartWithoutInverse::kFrames);
            CHECK(start.Judge(slot, Camera(0.43), 13));                  // a retry of the same frame keeps the verdict
            CHECK(!start.Judge(StartSlot{3, 0, 1280, 384}, Camera(0.43), 13)); // ... for that place only
            CHECK(start.streak() == StartWithoutInverse::kFrames);       // and does not restart the streak
        }},
        {"another place, a long gap or a different camera restarts the streak", [] {
            const StartSlot slot{3, 0, 1280, 320};
            const auto run = [&](StartWithoutInverse& s, uint64_t from) {
                bool last = false;
                for (uint64_t f = from; f < from + 3; ++f) last = s.Judge(slot, Camera(0.3), f);
                return last;
            };
            StartWithoutInverse a;
            CHECK(!run(a, 1));
            CHECK(!a.Judge(StartSlot{4, 0, 1280, 320}, Camera(0.3), 4)); // the block moved to another slot
            CHECK(a.streak() == 1);
            StartWithoutInverse b;
            CHECK(!run(b, 1));
            CHECK(b.Judge(slot, Camera(0.3), 5)); // one menu frame between is tolerated
            StartWithoutInverse c;
            CHECK(!run(c, 1));
            CHECK(!c.Judge(slot, Camera(0.3), 6)); // two are not
            CHECK(c.streak() == 1);
            StartWithoutInverse d;
            CHECK(!run(d, 1));
            CHECK(!d.Judge(slot, Camera(1.5), 4)); // turned 69 degrees in one frame: another camera
            CHECK(d.streak() == 1);
            StartWithoutInverse e;
            CHECK(!run(e, 1));
            CHECK(!e.Judge(slot, Camera(0.3, {0, 0, 0}, 3.0), 4)); // another lens
        }},
        {"continuing the last main camera is tighter than starting one", [] {
            VpGeometry last{};
            CHECK(DecomposeViewProjection(Camera(0.3, {100, 20, -50}), last));
            CHECK(ContinuesCamera(last, Camera(0.35, {104, 20, -50})));
            CHECK(!ContinuesCamera(last, Camera(0.6, {100, 20, -50})));   // 17 degrees
            CHECK(!ContinuesCamera(last, Camera(0.3, {150, 20, -50})));   // 50 units away
            CHECK(!ContinuesCamera(last, Camera(0.3, {100, 20, -50}, 3))); // zoomed to twice the lens
            CHECK(StartContinues(Camera(0.3, {100, 20, -50}), Camera(0.6, {130, 20, -50}))); // the start rule allows more
        }},
        {"a matrix with the camera position removed is camera-relative", [] {
            CHECK(IsCameraRelative(Stored(Camera(0.3))));
            CHECK(!IsCameraRelative(Stored(Camera(0.3, {11659, -8018, 1746}))));
            CHECK(MatchesSceneAspect(Camera(0.3), kW, kH));
            CHECK(!MatchesSceneAspect(Camera(0.3), 1920, 1200));
            CHECK(!MatchesSceneAspect(Camera(0.3, {0, 0, 0}, 1.5, 1.0), kW, kH)); // a square shadow camera
        }},
        {"one frame's blocks: the first camera-shaped matrix starts after four frames", [] {
            NoInverseCamera rule;
            const std::vector<uint8_t> per_object(160, 0);
            for (uint64_t frame = 1; frame <= 4; ++frame) {
                const auto camera = Block(1280, 320, Camera(0.3 + 0.01 * double(frame), {500, 60, 20}));
                const std::vector<AdmissionBlock> blocks{Bound(per_object, 0), Bound(camera, 3)};
                const Admission a = rule.Admit(blocks, kW, kH, frame);
                CHECK(a.candidates == 1 && a.streak == frame);
                if (frame < 4) {
                    CHECK(a.how == Admitted::None && a.why == NotAdmitted::Waiting);
                    continue;
                }
                CHECK(a.how == Admitted::Started && a.why == NotAdmitted::Admitted);
                CHECK(a.block == 1 && a.candidate.byte_offset == 320 && !a.relative);
                VpGeometry g{};
                CHECK(DecomposeViewProjection(a.candidate.matrix, g));
                NEAR(g.position[0], 500, 0.01);
            }
        }},
        {"a shadow camera's aspect, or a matrix with its inverse beside it, is no candidate here", [] {
            NoInverseCamera rule;
            const auto shadow = Block(256, 64, Camera(0.3, {5, 5, 5}, 1.5, 1.0));
            auto paired = Block(256, 0, Camera(0.3, {5, 5, 5}));
            Matrix inverse{};
            CHECK(Invert(Stored(Camera(0.3, {5, 5, 5})), inverse));
            Store(paired, 64, inverse);
            for (uint64_t frame = 1; frame <= 6; ++frame) {
                const Admission a = rule.Admit({Bound(shadow, 2), Bound(paired, 3)}, kW, kH, frame);
                CHECK(a.how == Admitted::None && a.why == NotAdmitted::NoCandidate && a.candidates == 0);
            }
        }},
        {"a lone matrix that continues the last camera is admitted at once, from any block", [] {
            NoInverseCamera rule;
            rule.Remember(Camera(0.3, {500, 60, 20}), true); // found by its inverse pair last frame
            const auto other = Block(256, 64, Camera(1.2, {900, 60, 20}));
            const auto same = Block(256, 128, Camera(0.31, {501, 60, 20}));
            const Admission a = rule.Admit({Bound(other, 2), Bound(same, 5)}, kW, kH, 7);
            CHECK(a.how == Admitted::Continued && a.block == 1 && a.candidate.byte_offset == 128 && a.candidates == 2);
        }},
        {"a game that uploads inverse pairs never starts a camera from a lone matrix", [] {
            NoInverseCamera rule;
            rule.Remember(Camera(0.3, {500, 60, 20}), true);
            const auto other = Block(256, 64, Camera(1.2, {900, 60, 20})); // a reflection or light camera
            for (uint64_t frame = 1; frame <= 8; ++frame) {
                const Admission a = rule.Admit({Bound(other, 2)}, kW, kH, frame);
                CHECK(a.how == Admitted::None && a.why == NotAdmitted::InverseGame && a.streak == 0);
            }
        }},
        {"a game without inverse pairs can start again after its camera jumps", [] {
            NoInverseCamera rule;
            rule.Remember(Camera(0.3, {500, 60, 20}), false); // admitted by the start rule earlier (a menu scene)
            const auto gameplay = Block(256, 64, Camera(1.2, {9000, 60, 20}));
            for (uint64_t frame = 1; frame <= 4; ++frame) {
                const Admission a = rule.Admit({Bound(gameplay, 2)}, kW, kH, frame);
                CHECK((a.how == Admitted::Started) == (frame == 4));
            }
        }},
        {"a camera-relative matrix starts a camera only while the game has shown no position", [] {
            NoInverseCamera rule;
            const auto relative = Block(64, 0, Camera(0.3));
            Admission a;
            for (uint64_t frame = 1; frame <= 4; ++frame) a = rule.Admit({Bound(relative, 1)}, kW, kH, frame);
            CHECK(a.how == Admitted::Started && a.relative);
            rule.PositionWitnessed(); // from now on the camera is the matrix AND its witnessed position
            for (uint64_t frame = 5; frame <= 12; ++frame) {
                a = rule.Admit({Bound(relative, 1)}, kW, kH, frame);
                CHECK(a.how == Admitted::None && a.why == NotAdmitted::PositionGame);
            }
        }},
        {"empty and short blocks are skipped, and only the first twelve are read", [] {
            NoInverseCamera rule;
            std::vector<AdmissionBlock> blocks(NoInverseCamera::kMaxBlocks);
            const std::vector<uint8_t> tiny(48, 0);
            blocks[1] = Bound(tiny, 1);
            const auto camera = Block(256, 64, Camera(0.3, {5, 5, 5}));
            blocks.push_back(Bound(camera, 9)); // thirteenth
            CHECK(rule.Admit(blocks, kW, kH, 1).candidates == 0);
            blocks[2] = Bound(camera, 9);
            CHECK(rule.Admit(blocks, kW, kH, 2).candidates == 1);
        }},
    });
}
