// The relative-camera rule (providers/camera/relative_camera.hpp): the witness,
// the layout, and learning the layout from what draws bind. The vote itself is
// covered through the DX11 names in native11_split_camera_test.cpp.
// Synthetic data only; saved game runs are not committed.
#include "test_support.hpp"
#include "host_dx12/adapters/wwii/scene_camera.hpp"
#include "providers/camera/relative_camera.hpp"

#include <cstring>
#include <map>

using namespace na::camera;

namespace {

constexpr uint32_t kW = 3440, kH = 1440;

// A reversed-depth, camera-relative view-projection turned `yaw` radians (position removed).
Matrix Relative(double yaw, double scale = 1.5, double aspect = double(kW) / kH, std::array<double, 3> pos = {0, 0, 0})
{
    const double c = std::cos(yaw), s = std::sin(yaw);
    const double rows[3][3]{{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
    Matrix view{};
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
void Store(std::vector<uint8_t>& block, size_t offset, const Matrix& m, Layout layout = Layout::RowMajor)
{
    for (size_t r = 0; r < 4; ++r)
        for (size_t col = 0; col < 4; ++col) {
            const float v = static_cast<float>(m[r * 4 + col]);
            std::memcpy(block.data() + offset + (layout == Layout::RowMajor ? r * 4 + col : col * 4 + r) * 4, &v, 4);
        }
}
std::vector<uint8_t> Block(size_t size, size_t offset, const Matrix& m, Layout layout = Layout::RowMajor)
{
    std::vector<uint8_t> block(size, 0);
    Store(block, offset, m, layout);
    return block;
}
// An unrotated object's world transform at `offset`: a pure translation.
void Translation(std::vector<uint8_t>& block, size_t offset, WitnessForm form, float x, float y, float z)
{
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (form == WitnessForm::Row4x4) { m[12] = x; m[13] = y; m[14] = z; }
    else { m[3] = x; m[7] = y; m[11] = z; }
    std::memcpy(block.data() + offset, m.data(), WitnessBytes(form));
}
// The per-object range of the game the vote was first written for: this frame's transform at 0,
// eight other floats, last frame's at 96.
std::vector<uint8_t> World160(float x, float y = -8018.f, float z = 1746.f)
{
    std::vector<uint8_t> block(160, 0);
    Translation(block, 0, WitnessForm::Column4x4, x, y, z);
    const float other[8]{0.5f, 0.25f, 3.f, 1.f, 7.f, 0.f, 2.f, 9.f};
    std::memcpy(block.data() + 64, other, sizeof other);
    Translation(block, 96, WitnessForm::Column4x4, x, y, z);
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
bool SameLayout(const RelativeLayout& a, const RelativeLayout& b)
{
    return a.world_binding == b.world_binding && a.witnesses == b.witnesses &&
           a.witness[0] == b.witness[0] && (a.witnesses < 2 || a.witness[1] == b.witness[1]) &&
           a.projection_binding == b.projection_binding && a.projection_size == b.projection_size &&
           a.projection_offset == b.projection_offset && a.projection_layout == b.projection_layout &&
           a.reversed == b.reversed;
}

} // namespace

int main()
{
    return test::run({
        {"an unrotated object's transform witnesses the camera position, in each storage form", [] {
            for (const WitnessForm form : {WitnessForm::Column4x4, WitnessForm::Row4x4, WitnessForm::Column3x4}) {
                std::vector<uint8_t> block(96, 0);
                Translation(block, 16, form, -11659.f, 8018.f, -1746.f);
                double origin[3]{};
                CHECK(ReadWitness(block.data(), block.size(), {16, form}, origin));
                CHECK(origin[0] == 11659.0 && origin[1] == -8018.0 && origin[2] == 1746.0);
                CHECK(!ReadWitness(block.data(), 16 + WitnessBytes(form) - 4, {16, form}, origin)); // range too short
            }
            std::vector<uint8_t> rotated(64, 0);
            const std::array<float, 16> turn{0, -1, 0, 5, 1, 0, 0, 6, 0, 0, 1, 7, 0, 0, 0, 1};
            std::memcpy(rotated.data(), turn.data(), 64);
            double origin[3]{};
            CHECK(!ReadWitness(rotated.data(), 64, {0, WitnessForm::Column4x4}, origin)); // a rotated object witnesses nothing
        }},
        {"the places of a per-object range are found by content", [] {
            WitnessPlace places[2]{};
            const auto world = World160(-11659.f);
            CHECK(FindWitnessPlaces(world.data(), world.size(), places) == 2);
            CHECK(places[0] == WitnessPlace{0, WitnessForm::Column4x4} && places[1] == WitnessPlace{96, WitnessForm::Column4x4});
            std::vector<uint8_t> rows(96, 0);
            Translation(rows, 16, WitnessForm::Row4x4, 40.f, 0.f, -3.f);
            CHECK(FindWitnessPlaces(rows.data(), rows.size(), places) == 1);
            CHECK(places[0] == WitnessPlace{16, WitnessForm::Row4x4});
            std::vector<uint8_t> three(48, 0);
            Translation(three, 0, WitnessForm::Column3x4, 40.f, 0.f, -3.f);
            CHECK(FindWitnessPlaces(three.data(), three.size(), places) == 1 && places[0].form == WitnessForm::Column3x4);
            std::vector<uint8_t> identity(64, 0);
            Translation(identity, 0, WitnessForm::Column4x4, 0.f, 0.f, 0.f);
            CHECK(FindWitnessPlaces(identity.data(), identity.size(), places) == 0); // a texture matrix, not a witness
            std::vector<uint8_t> bones(1024, 0);
            Translation(bones, 0, WitnessForm::Column4x4, 40.f, 0.f, -3.f);
            CHECK(FindWitnessPlaces(bones.data(), bones.size(), places) == 0); // too large to be searched per draw
        }},
        {"a camera-relative scene projection is found by shape, in either storage", [] {
            RelativeLayout found;
            found.projection_offset = 99;
            found.projection_layout = Layout::ColumnMajor;
            found.reversed = false;
            const auto small = Block(64, 0, Relative(0.3));
            CHECK(FindRelativeProjections(small.data(), small.size(), kW, kH, found) == 1);
            CHECK(found.projection_offset == 0 && found.projection_layout == Layout::RowMajor && found.reversed);
            const auto column = Block(256, 64, Relative(0.3), Layout::ColumnMajor);
            CHECK(FindRelativeProjections(column.data(), column.size(), kW, kH, found) == 1);
            CHECK(found.projection_offset == 64 && found.projection_layout == Layout::ColumnMajor);
            const auto straight = Block(64, 0, Relative(0.0)); // looking straight down an axis: a plain projection
            CHECK(FindRelativeProjections(straight.data(), straight.size(), kW, kH, found) == 1);
            CHECK(FindRelativeProjections(small.data(), small.size(), 1920, 1200, found) == 0); // another picture
            const auto placed = Block(64, 0, Relative(0.3, 1.5, double(kW) / kH, {500, 60, 20}));
            CHECK(FindRelativeProjections(placed.data(), placed.size(), kW, kH, found) == 0); // carries its position
        }},
        {"a block with this frame's and last frame's projection counts two; copies count one", [] {
            RelativeLayout found;
            auto packed = Block(1280, 96, Relative(0.29), Layout::ColumnMajor);
            Store(packed, 320, Relative(0.30), Layout::ColumnMajor);
            CHECK(FindRelativeProjections(packed.data(), packed.size(), kW, kH, found) == 2);
            CHECK(found.projection_offset == 96);
            auto copies = Block(1280, 208, Relative(0.30), Layout::ColumnMajor);
            Store(copies, 320, Relative(0.30), Layout::ColumnMajor);
            CHECK(FindRelativeProjections(copies.data(), copies.size(), kW, kH, found) == 1);
            CHECK(found.projection_offset == 208 && found.projection_copies == 1 && found.projection_copy[0] == 320);
        }},
        {"places that were equal only while the camera stood still give no vote once they differ", [] {
            // Learned at rest: 96 and 320 hold the same matrix, so they look like copies.
            auto block = Block(1280, 96, Relative(0.30), Layout::ColumnMajor);
            Store(block, 320, Relative(0.30), Layout::ColumnMajor);
            const auto world = World160(-11659.f);
            RelativeLearner learner;
            CHECK(learner.FindProjection({Bound(block, 3)}, kW, kH));
            for (int draw = 0; draw < 20; ++draw) learner.NoteDraw(0, world.data(), world.size());
            RelativeLayout layout;
            CHECK(learner.Finish(20, layout) && layout.projection_copies == 1);
            RelativeKey key{};
            CHECK(MakeRelativeKey(layout, world.data(), world.size(), block.data(), block.size(), key));
            // The camera turns: 320 moves on, 96 turns out to be last frame's. Which is the camera
            // is no longer certain from this block, so the draw is not a vote.
            Store(block, 320, Relative(0.31), Layout::ColumnMajor);
            CHECK(!MakeRelativeKey(layout, world.data(), world.size(), block.data(), block.size(), key));
            RelativeLearner again;
            CHECK(!again.FindProjection({Bound(block, 3)}, kW, kH) && again.ambiguous() && !again.armed());
        }},
        {"learning finds, from the draws alone, the places the seed lists for its game", [] {
            // Ranked as the scene binds them: the per-object range, a packed block with two
            // projections, and a 64-byte block with one.
            const auto world = World160(-11659.f), prop = World160(7.f);
            auto packed = Block(1280, 96, Relative(0.29), Layout::ColumnMajor);
            Store(packed, 320, Relative(0.30), Layout::ColumnMajor);
            const auto projection = Block(64, 0, Relative(0.30));
            RelativeLearner learner;
            CHECK(learner.FindProjection({Bound(world, 0), Bound(packed, 3), Bound(projection, 1)}, kW, kH));
            CHECK(learner.armed() && learner.ambiguous()); // armed, having passed over the packed block
            CHECK(learner.projection_binding() == 1 && learner.projection_size() == 64); // the block with no doubt in it
            for (int draw = 0; draw < 40; ++draw) learner.NoteDraw(0, world.data(), world.size());
            for (int draw = 0; draw < 5; ++draw) learner.NoteDraw(0, prop.data(), prop.size());
            std::vector<uint8_t> skinned(160, 0); // a rotated object: no place in it
            const std::array<float, 16> turn{0, -1, 0, 5, 1, 0, 0, 6, 0, 0, 1, 7, 0, 0, 0, 1};
            std::memcpy(skinned.data(), turn.data(), 64);
            for (int draw = 0; draw < 15; ++draw) learner.NoteDraw(0, skinned.data(), skinned.size());
            RelativeLayout learned;
            CHECK(learner.Finish(60, learned));
            CHECK(!learner.armed());
            CHECK(SameLayout(learned, na::wwii::RelativeSeedLayout()));
            CHECK(learned.world_size == 0 && learned.projection_copies == 0); // any length under that binding
            // The seed knows one length; a learned layout also takes the 176-byte form of the same
            // range (seen in a saved run), because each draw is checked by content.
            std::vector<uint8_t> longer(176, 0);
            std::memcpy(longer.data(), world.data(), world.size());
            RelativeKey key{};
            CHECK(!MakeRelativeKey(na::wwii::RelativeSeedLayout(), longer.data(), longer.size(), projection.data(), projection.size(), key));
            CHECK(MakeRelativeKey(learned, longer.data(), longer.size(), projection.data(), projection.size(), key));
            CHECK(key.second[0] == 11659.0);
        }},
        {"another game's shape: one row-form transform in slot 2, a column-stored projection in a pixel block", [] {
            constexpr uint64_t kPixelSlot2 = 16;
            const Matrix vp = Relative(0.7);
            std::vector<uint8_t> view(256, 0);
            Store(view, 64, vp, Layout::ColumnMajor);
            const auto object = [](float x, float y, float z) {
                std::vector<uint8_t> block(96, 0);
                Translation(block, 16, WitnessForm::Row4x4, x, y, z);
                return block;
            };
            const auto chunk = object(-300.f, -20.f, 45.f), crate = object(-290.f, -20.f, 41.f);
            RelativeLearner learner;
            CHECK(learner.FindProjection({Bound(chunk, 2), Bound(view, kPixelSlot2)}, kW, kH));
            for (int draw = 0; draw < 30; ++draw) learner.NoteDraw(2, chunk.data(), chunk.size());
            learner.NoteDraw(2, crate.data(), crate.size());
            RelativeLayout layout;
            CHECK(learner.Finish(100, layout));
            CHECK(layout.world_binding == 2 && layout.witnesses == 1 &&
                  layout.witness[0] == WitnessPlace{16, WitnessForm::Row4x4});
            CHECK(layout.projection_binding == kPixelSlot2 && layout.projection_size == 256 &&
                  layout.projection_offset == 64 && layout.projection_layout == Layout::ColumnMajor);
            // The learned layout then carries the ordinary vote.
            std::map<RelativeKey, uint32_t> votes;
            RelativeKey key{};
            CHECK(MakeRelativeKey(layout, chunk.data(), chunk.size(), view.data(), view.size(), key));
            votes[key] += 30;
            CHECK(MakeRelativeKey(layout, crate.data(), crate.size(), view.data(), view.size(), key));
            votes[key] += 1;
            auto best = votes.cend();
            Matrix chosen{};
            CHECK(ChooseRelativeCamera(layout, votes, [](uint32_t draws) { return draws; }, kW, kH, 100, best, chosen) ==
                  RelativeVerdict::Chosen);
            CHECK(best->first.second[0] == 300.0 && best->first.second[1] == 20.0 && best->first.second[2] == -45.0);
            NEAR(chosen[0], Relative(0.7)[0], 1e-6);
            // A projection range of another size under that binding is another shader's block: no vote.
            std::vector<uint8_t> wider(272, 0);
            std::memcpy(wider.data(), view.data(), view.size());
            CHECK(!MakeRelativeKey(layout, chunk.data(), chunk.size(), wider.data(), wider.size(), key));
        }},
        {"too few witnessing draws, or no projection at all, learns nothing", [] {
            const auto world = World160(-11659.f);
            const auto projection = Block(64, 0, Relative(0.30));
            RelativeLearner learner;
            RelativeLayout layout;
            CHECK(!learner.Finish(10, layout)); // never armed
            CHECK(learner.FindProjection({Bound(projection, 1)}, kW, kH));
            for (int draw = 0; draw < 10; ++draw) learner.NoteDraw(0, world.data(), world.size());
            CHECK(!learner.Finish(81, layout)); // 10 of 81 scene draws is under an eighth
            CHECK(learner.FindProjection({Bound(projection, 1)}, kW, kH));
            learner.NoteDraw(0, world.data(), world.size());
            CHECK(!learner.Finish(1, layout)); // a single draw is no agreement
            const std::vector<uint8_t> nothing(256, 0);
            CHECK(!learner.FindProjection({Bound(nothing, 1), Bound(world, 0)}, kW, kH) && !learner.armed());
        }},
        {"a block with several different projections is never learned from", [] {
            auto packed = Block(1280, 96, Relative(0.29), Layout::ColumnMajor);
            Store(packed, 320, Relative(0.30), Layout::ColumnMajor);
            const auto clear = Block(64, 0, Relative(0.30));
            RelativeLearner learner;
            CHECK(!learner.FindProjection({Bound(packed, 3)}, kW, kH));
            CHECK(learner.ambiguous() && !learner.armed());
            CHECK(learner.FindProjection({Bound(packed, 3), Bound(clear, 1)}, kW, kH)); // a clear block further down is taken
            CHECK(learner.projection_binding() == 1 && learner.ambiguous());
        }},
    });
}
