#pragma once
#include <cstdint>

namespace na::tlou1 {
enum class Stage : uint32_t { Idle, Queued, Entered, Rebuilding, Confirmed, Failed, Cancelled };
struct Choice {
    uint32_t mode = 0, width = 0, height = 0;
    int32_t preset = -1;
};
inline bool Same(const Choice& a, const Choice& b) {
    return a.mode == b.mode && a.width == b.width && a.height == b.height && a.preset == b.preset;
}
// Protected by the adapter mutex. No game pointer survives an invocation.
struct ApplyRequest {
    Stage stage = Stage::Idle;
    uint64_t id = 0, deadline = 0;
    Choice choice{};
    bool Busy() const { return stage == Stage::Queued || stage == Stage::Entered || stage == Stage::Rebuilding; }
    bool Arm(uint64_t request_id, uint64_t now, Choice requested) {
        if (Busy() || !request_id || requested.mode > 3 || !requested.width || !requested.height) return false;
        id = request_id; choice = requested; deadline = now + 15000; stage = Stage::Queued; return true;
    }
    void Tick(uint64_t now, bool enabled, Choice current) {
        if (!Busy()) return;
        if (!enabled || !Same(choice, current)) stage = Stage::Cancelled;
        else if (now >= deadline) stage = Stage::Failed;
    }
    bool Enter(uint64_t now, bool enabled, Choice current) {
        Tick(now, enabled, current);
        if (stage != Stage::Queued) return false;
        stage = Stage::Entered; return true;
    }
    bool Trigger(uint64_t request_id, bool same_context, bool known_caller) {
        if (request_id != id || stage != Stage::Entered || !same_context || !known_caller) return false;
        stage = Stage::Rebuilding; return true;
    }
    void Leave(uint64_t request_id) {
        // Some frame invocations skip the settings branch. No rebuild happened;
        // keep this same request queued without extending its original deadline.
        if (id == request_id && stage == Stage::Entered) stage = Stage::Queued;
    }
    void Observe(uint64_t request_id, uint32_t confirmation_state) {
        if (id != request_id || stage != Stage::Rebuilding) return;
        if (confirmation_state == 3) stage = Stage::Confirmed;
        else if (confirmation_state == 4) stage = Stage::Failed;
    }
};
} // namespace na::tlou1
