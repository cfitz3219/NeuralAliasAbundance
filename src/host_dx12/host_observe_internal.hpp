// Internal producer/testing interface for the H1 observation host.
// Consumers use ONLY host_observe.h; this header is for the host's own
// producers (hooks) and its CPU tests.
//
// Transport (A24): per-producer-thread SPSC byte rings, claimed once per
// thread by CAS, drained by the single worker. Producers never lock, allocate
// or wait. Up to 32 producer threads; further threads drop with loss
// accounting. Semantics (framing, loss, epochs, status) are unchanged from
// the frozen contract and covered by the same tests.
#pragma once

#include <cstdint>
#include <functional>

#include "host_dx12/host_observe.h"

namespace na::observe_internal {

// Queues one fully-formed event (starting with na_event_header; size_bytes
// set; epoch field is OVERWRITTEN by the host with the current epoch).
// Foundational routing follows NA_EVF_FOUNDATIONAL. `group` selects the loss
// counter on drop. Returns false when the event was dropped (loss recorded).
bool Emit(const void* event_bytes, uint32_t group);

// True while a consumer is started: producers skip building events otherwise.
bool Started();
// Current session and epoch (0, 0 when stopped): sample leases are scoped to them.
void CurrentScope(na_session_id* session, na_epoch* epoch);

// Snapshot provider used by start()/resync(): must Emit() the inventory
// baseline (foundational events) and return true when the baseline is
// coherent and complete. Absent provider => empty snapshot, complete.
using SnapshotFn = std::function<bool()>;
void SetSnapshotProvider(SnapshotFn fn);

// Test hook: forces the next foundational Emit to be treated as overflow.
void TestForceFoundationalOverflow(bool enable);

} // namespace na::observe_internal
