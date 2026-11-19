#pragma once
#include "host_dx12/native11/screen_check.hpp"

namespace na::native11 {

// VARIANT: point/native11-rotating-lighting
// Lives under the provider lock. The ordered pair is a resource role; both
// actual reads must independently reproduce the marker in the display.
struct LitScene {
    ScreenCheck screen;
    Id members[2]{}, depth = 0;
    unsigned observations[2]{}, consecutive_proven = 0;
    uint64_t proofs[2]{}, generation = 0, observed_write = 0;
    uint64_t incarnations[2]{}, depth_incarnation = 0;
    // Active extent, typed colour/depth views, depth mip and array slice.
    using View = std::array<uint32_t, 6>;
    View view{};
    View written[2]{};
    Id observed_member = 0;
    bool later_writes_first = false, steer = false, observed_proven = false;
    bool incomplete = false;

    Id Peer(Id member) const {
        if (member && member == members[0]) return members[1];
        if (member && member == members[1]) return members[0];
        return 0;
    }
    bool Learned() const { return observations[0] >= 3 && observations[1] >= 3; }
    Id Predicted(const FrameModel& model) const {
        if (!Learned() || incomplete) return 0;
        const Id first = model.FirstLitWriter(members[0], members[1]);
        return later_writes_first ? first : Peer(first);
    }
    void Clear() {
        const uint64_t next = generation + 1;
        *this = {};
        generation = next;
    }
    bool ObserveView(unsigned member, uint64_t color_id, uint64_t depth_id, View current) {
        if (!color_id || !depth_id || written[member] != current) return false;
        if ((incarnations[member] && incarnations[member] != color_id) ||
            (depth_incarnation && (depth_incarnation != depth_id || !(view == current)))) {
            Clear();
            return false;
        }
        incarnations[member] = color_id;
        depth_incarnation = depth_id;
        view = current;
        return true;
    }
    void LostObservation() {
        incomplete = true;
        consecutive_proven = 0;
        steer = false;
        screen.CancelChallenge();
    }
    void Finish(const FrameModel::LitOrder& order) {
        written[0] = written[1] = {};
        if (incomplete) {
            incomplete = false;
            observed_member = observed_write = 0;
            observed_proven = false;
            return;
        }
        if (!order.late || !order.first) {
            if (depth) Clear();
            return;
        }
        const bool first = order.first == order.late;
        if (depth != order.depth || Peer(order.late) != order.early || first != later_writes_first) {
            Clear();
            members[0] = order.late;
            members[1] = order.early;
            depth = order.depth;
            later_writes_first = first;
            screen.IndependentReaders();
        }
        const unsigned index = order.late == members[1];
        if (observations[index] < 3) ++observations[index];
        // An unseen or contradicted reader gives the very next frame back to
        // ordinary delivery. Observe it again before giving it priority.
        if (!observed_proven || observed_member != order.late || !observed_write) {
            proofs[index] = 0;
            consecutive_proven = 0;
        } else if (consecutive_proven < 3) ++consecutive_proven;
        steer = proofs[0] && proofs[1] && consecutive_proven >= 3;
        observed_member = observed_write = 0;
        observed_proven = false;
    }
    bool Revoke(uint64_t proof, uint64_t frame) {
        if (!proof || !screen.RejectProof(proof, frame)) return false;
        for (auto& p : proofs) if (p == proof) p = 0;
        consecutive_proven = 0;
        steer = false;
        return true;
    }
};

} // namespace na::native11
