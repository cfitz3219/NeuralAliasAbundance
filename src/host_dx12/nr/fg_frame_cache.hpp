#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace na::nr {
// One cache per native FG feature incarnation (core + handle + creation).
// The native BackbufferFrameID identifies a real image, not MultiFrameIndex,
// a texture pointer, a Present, or the CPU call count. No inferred identity.
// Externally serialized by the feature owner. Payloads/GPU fences live in the
// owner's matching slots; this class does not claim GPU completion itself.
class FgFrameCache {
public:
    static constexpr uint32_t Capacity = 16;
    struct Ticket { uint32_t slot=Capacity; uint64_t generation=0; };
    // ReuseRecorded is usable only later in the SAME recording, or after its
    // actual submission/order has been established. ReuseSubmitted still
    // requires queue ordering or completed fences before another queue reads.
    enum class Action { Record, Pending, ReuseRecorded, ReuseSubmitted, MissingIdentity, Full, Expired };
    struct Decision { Action action; Ticket ticket; };

    Decision Acquire(std::optional<uint64_t> frame_id) {
        if (!frame_id) return {Action::MissingIdentity,{}};
        // Look for retained older pictures BEFORE checking the retirement
        // watermark: a delayed MFG call may still use a retained slot.
        for (uint32_t i=0;i<Capacity;++i) if (slots_[i].occupied && slots_[i].frame_id==*frame_id)
            return {slots_[i].submitted ? Action::ReuseSubmitted :
                (slots_[i].recorded ? Action::ReuseRecorded : Action::Pending),{i,slots_[i].generation}};
        // Never advance temporal NR twice because a late duplicate arrives
        // after its cached output was retired. Frame IDs are monotonic within
        // a feature incarnation; start a fresh cache on native feature create.
        if (retired_through_ && *frame_id<=*retired_through_) return {Action::Expired,{}};
        for (uint32_t i=0;i<Capacity;++i) if (!slots_[i].occupied) {
            auto& s=slots_[i]; s.occupied=true; s.recorded=false; s.submitted=false; s.frame_id=*frame_id;
            ++s.generation;
            return {Action::Record,{i,s.generation}};
        }
        return {Action::Full,{}};
    }
    bool Recorded(Ticket t) {
        auto* s=Find(t); if (!s || s->submitted) return false;
        s->recorded=true; return true;
    }
    bool Submitted(Ticket t) {
        auto* s=Find(t); if (!s || !s->recorded) return false;
        s->submitted=true; return true;
    }
    // Only for a proven Reset/discard before ANY submission. A failed NGX
    // return alone is not proof that recorded commands will not be submitted.
    bool AbandonUnsubmitted(Ticket t) {
        auto* s=Find(t); if (!s || s->submitted) return false;
        s->occupied=false; return true;
    }
    // Call only after ALL consumers have finished, including repeated MFG
    // calls using the cached image. Completion/queue ordering is the caller's
    // explicit responsibility. A busy slot must never be evicted to make room.
    bool RetireCompleted(Ticket t) {
        auto* s=Find(t); if (!s || !s->submitted) return false;
        if (!retired_through_ || s->frame_id>*retired_through_) retired_through_=s->frame_id;
        s->occupied=false; return true;
    }
    bool WasSubmitted(Ticket t) const {
        return t.slot<Capacity && slots_[t.slot].occupied && slots_[t.slot].generation==t.generation && slots_[t.slot].submitted;
    }
private:
    struct Slot { uint64_t frame_id=0,generation=0; bool occupied=false,recorded=false,submitted=false; };
    Slot* Find(Ticket t) {
        if (t.slot>=Capacity) return nullptr;
        auto& s=slots_[t.slot]; return s.occupied && s.generation==t.generation ? &s : nullptr;
    }
    std::array<Slot,Capacity> slots_{};
    std::optional<uint64_t> retired_through_;
};
} // namespace na::nr
