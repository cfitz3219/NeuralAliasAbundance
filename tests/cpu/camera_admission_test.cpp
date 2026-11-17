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

// One 512-byte view block as a game uploaded it (recorded in a run log, 2026-11-12): a view matrix,
// a projection, the view-projection WITH the camera position at byte 176, the same view-projection
// with the position removed at byte 240, and the position itself. Numbers only.
const char kRecordedViewBlock[] =
    "9741ce438f41ce430000000000000000000057450000b444c8689839610b363a00000000000000000000000000000000"
    "2fad783f3688c0b9c32d733ed88016c3efc6d3bbf8e47f3f343ae53c12a47bc3c71673be2f3eebbcc592783f57912144"
    "0000000000000000000000000000803f5f5b813f0000000000000000000000000000000079821a400000000000000000"
    "0000000000000000190080bfbfc275bd0000000000000000000080bf000000000d507b3fb692c2b9b6c1753e491918c3"
    "36a37fbc28721a40d7598a3df3e017c4f616733e5d3eeb3cf69278bf4e9521c4c716733e2f3eeb3cc59278bf579121c4"
    "0d507b3fb692c2b9b6c1753e0000000036a37fbc28721a40d7598a3d00000000f616733e5d3eeb3cf69278bfbfc275bd"
    "c716733e2f3eeb3cc59278bf000000005f5b813f79821a401900803fbfc275bdcffe944328058743542f12c400000000"
    "00000000000020410000000000000000000000000000000000000000000000000000803f000000000000000000000000"
    "000000000000803f00000000000000000000000000000000344883bf200d52be0000000000000000000080bf00000000"
    "fb839bbd118766bd9cda7e3f35bd1a4448c87d3f9008d33dd1caa63dc4058ac37f1ae13dc05f82bfce8449bd00915643"
    "d679db3db83a7ebf167b44bdfa665143fb839bbd118766bd9cda7e3f00000000";
std::vector<uint8_t> Recorded()
{
    std::vector<uint8_t> bytes;
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; kRecordedViewBlock[i] && kRecordedViewBlock[i + 1]; i += 2)
        bytes.push_back(static_cast<uint8_t>(nibble(kRecordedViewBlock[i]) * 16 + nibble(kRecordedViewBlock[i + 1])));
    return bytes;
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
        {"one sighting of an inverse pair does not close the start rule of a game whose camera is a lone matrix", [] {
            NoInverseCamera rule;
            for (uint64_t frame = 1; frame <= 200; ++frame) rule.Remember(Camera(0.3, {500, 60, 20}), false, frame);
            rule.Remember(Camera(0.3, {500, 60, 20}), true, 201); // some other block held a matrix with its inverse, once
            const auto turned = Block(256, 64, Camera(1.2, {500, 60, 20})); // the player turned: no candidate continues
            CHECK(rule.StartClosed(202));
            Admission a = rule.Admit({Bound(turned, 2)}, kW, kH, 202);
            CHECK(a.how == Admitted::None && a.why == NotAdmitted::InverseGame);
            const uint64_t open = 201 + NoInverseCamera::kPairMemory + 1;
            CHECK(!rule.StartClosed(open));
            for (uint64_t frame = open; frame < open + 4; ++frame) a = rule.Admit({Bound(turned, 2)}, kW, kH, frame);
            CHECK(a.how == Admitted::Started);
        }},
        {"a game whose camera is usually found by inverse pairs keeps its start rule closed", [] {
            NoInverseCamera rule;
            for (uint64_t frame = 1; frame <= 200; ++frame) rule.Remember(Camera(0.3, {500, 60, 20}), true, frame);
            for (uint64_t frame = 201; frame <= 220; ++frame) rule.Remember(Camera(0.3, {500, 60, 20}), false, frame);
            const auto other = Block(256, 64, Camera(1.2, {900, 60, 20}));
            for (uint64_t frame = 300; frame <= 308; ++frame) {
                const Admission a = rule.Admit({Bound(other, 2)}, kW, kH, frame);
                CHECK(a.how == Admitted::None && a.why == NotAdmitted::InverseGame);
            }
        }},
        {"a camera with its position continues one without, by direction and lens", [] {
            NoInverseCamera rule;
            rule.Remember(Camera(0.3), false, 1); // camera-relative last frame
            const auto positioned = Block(256, 64, Camera(0.31, {500, 60, 20}));
            const Admission a = rule.Admit({Bound(positioned, 2)}, kW, kH, 2);
            CHECK(a.how == Admitted::Continued && !a.relative);
            NoInverseCamera other;
            other.Remember(Camera(0.3), false, 1);
            const auto elsewhere = Block(256, 64, Camera(1.2, {500, 60, 20})); // another direction is another camera
            CHECK(other.Admit({Bound(elsewhere, 2)}, kW, kH, 2).how != Admitted::Continued);
        }},
        {"a recorded block that holds the camera with and without its position gives the one with it", [] {
            const auto block = Recorded();
            CHECK(block.size() == 512);
            const Matrix relative = LoadMatrix(block, 240, Layout::RowMajor);
            CHECK(IsCameraRelative(relative));
            Candidate twin;
            CHECK(PositionedTwin(block.data(), block.size(), relative, kW, kH, twin));
            CHECK(twin.byte_offset == 176 && !IsCameraRelative(twin.matrix));
            VpGeometry g{};
            CHECK(DecomposeViewProjection(twin.matrix, g));
            CHECK(std::abs(g.position[0] - 298.0) < 0.5 && std::abs(g.position[1] - 270.0) < 0.5 && std::abs(g.position[2] + 584.7) < 0.5);
            NoInverseCamera rule;
            rule.Remember(relative, false, 1); // the position-less copy was the camera so far
            const Admission a = rule.Admit({Bound(block, 2)}, kW, kH, 2);
            CHECK(a.how == Admitted::Continued && a.candidate.byte_offset == 176 && !a.relative);
            NoInverseCamera fresh; // and a fresh start takes the positioned one too
            Admission s;
            for (uint64_t frame = 1; frame <= 4; ++frame) s = fresh.Admit({Bound(block, 2)}, kW, kH, frame);
            CHECK(s.how == Admitted::Started && s.candidate.byte_offset == 176);
        }},
        {"the learned place is the most-drawn range of that shape, wherever it starts this frame", [] {
            // Recorded (run p39980, frame 52464): the world view's block and two first-person
            // blocks of the same size sit side by side at one slot; the world view had 43 draws.
            const uint64_t ring = 7;
            const std::vector<BoundRange> frame{{ring, 2, 21456, 32, 3}, {ring, 2, 21488, 32, 43}, {ring, 2, 21424, 32, 2},
                                                {ring, 0, 29296, 800, 86}, {9, 2, 21456, 32, 500}};
            // Last frame the camera was read at 21456: this frame that start holds the first-person block.
            CHECK(LearnedRange(frame, ring, 2, 21456, 32) == 1);
            CHECK(LearnedRange(frame, ring, 2, 13392, 32) == 1); // a start never seen before
            CHECK(LearnedRange(frame, ring, 3, 21456, 32) == -1); // no range of that shape
            const std::vector<BoundRange> tie{{ring, 2, 100, 32, 5}, {ring, 2, 200, 32, 5}};
            CHECK(LearnedRange(tie, ring, 2, 200, 32) == 1);     // equal draws: the start it had
            CHECK(LearnedRange(tie, ring, 2, 300, 32) == 0);
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
