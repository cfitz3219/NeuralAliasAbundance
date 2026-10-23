#pragma once
#include "host_dx12/adapters/tlou1/apply_request.hpp"

namespace na::tlou1 {
// Init-thread only, after the shared hooking core is initialized. Exact exe pin;
// installed hooks persist for process lifetime. Does nothing in other games.
void InitializeApply();
struct ApplyStatus {
    bool available = false;
    Stage stage = Stage::Idle;
    uint64_t request_id = 0, frame_calls = 0, predicate_calls = 0, triggers = 0;
    uint64_t resource_releases = 0, resource_release_failures = 0;
    uint32_t trigger_thread = 0;
    Choice choice{};
    char reason[160]{};
};
bool RequestApply(Choice choice);
ApplyStatus GetApplyStatus();
} // namespace na::tlou1
