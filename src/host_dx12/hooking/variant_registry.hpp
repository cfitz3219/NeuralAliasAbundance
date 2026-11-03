// A48/A49: implementation-variant registry for COM vtable observation
// (COLLABORATION.md B-005/B-006 Â§3 as amended; corrections per
// A48-REGISTRY-REVIEW).
//
// Problem: a D3D12 command list starts on the runtime's generic vtable and,
// after its first successful Reset, is re-pointed at ANOTHER table: on WARP
// and on NVIDIA hardware that table is allocated per list on the heap, and
// its entries for binding/draw methods live in the user-mode driver.
// Function-level hooks on the first implementation therefore stop seeing
// the list, and a table ADDRESS is not an identity (it is per object and can
// be freed). The concrete functions inside the tables ARE shared, so this
// registry is keyed by (kind, slot, concrete target function).
//
// Model: per KIND of interface (command lists and devices are separate
// registries: their slot numbers would otherwise collide), an observer
// registers, per vtable SLOT, a POOL of thunks (one detour per possible
// implementation variant, each with its own original-pointer storage). The
// first time a target is seen IN THAT SLOT it is assigned the next free
// thunk of the slot and hooked through the shared installation transaction;
// a target shared by several tables in the same slot reuses its thunk. A
// target already hooked for a DIFFERENT slot is not treated as covered
// (signature/observer mismatch): it is reported Incomplete, never Complete.
//
// Coverage of an object = every registered slot of its CURRENT table holds
// a target hooked for that slot. It is CURRENT target coverage only; the
// adapter keeps a recording generation gapped until a fully observed
// Reset-through-Close sequence.
//
// Discovery: (a) warm-up â€” host-owned lists driven through the states that
// swap tables; (b) late â€” a hot path (the host's ExecuteCommandLists
// detour) calls NoteObject on each submitted list: an unknown object's slot
// TARGETS are snapshotted (bounded, immutable copy; the table itself is
// never dereferenced later) into a lock-free queue for the discovery
// thread. Overflow is counted and the object stays Unknown. Producers do
// not scan the queue (a slot can be reused under them); duplicate requests
// are coalesced by the single consumer, whose ensure step is idempotent.
//
// Forwarding: a thunk opens a CallScope(receiver, slot); `outermost` is
// false only when an enclosing scope on the SAME thread has the SAME
// receiver AND slot. Nesting deeper than the bounded stack is reported as
// `ambiguous` (and counted), never read out of bounds.
//
// Bundles are NOT discovered here (they never reach ExecuteCommandLists);
// Stats reports bundle coverage as unsupported.
#pragma once

#include <cstdint>

struct ID3D12Device;

namespace na::hooking::variants {

constexpr uint32_t kMaxSlots = 40; // C8b registers 33 command-list slots
constexpr uint32_t kMaxThunksPerSlot = 8;
constexpr uint32_t kMaxTables = 16;      // informational snapshots kept
constexpr uint32_t kQueueCapacity = 16;
constexpr uint32_t kScopeDepth = 32;

// CommandList4: render-pass methods (ID3D12GraphicsCommandList4 slots 68/69). A
// separate kind so (a) objects are only snapshotted through a pointer obtained by
// QueryInterface(ID3D12GraphicsCommandList4) (never reading past a shorter table)
// and (b) base command-list coverage never depends on these optional slots.
enum class Kind : uint32_t { CommandList = 0, Device = 1, CommandList4 = 2, CommandList7 = 3 };
constexpr uint32_t kKinds = 4;

struct SlotSpec {
    uint32_t slot;          // vtable index (e.g. 30 = ID3D12GraphicsCommandList::SetGraphicsRootSignature)
    uint32_t thunk_count;   // <= kMaxThunksPerSlot
    void* const* thunks;    // thunk_count detour functions with the slot's exact signature
    void** originals;       // thunk_count original-pointer slots, one per thunk (written by the registry)
};

enum class Coverage : uint32_t { Unknown = 0, Complete = 1, Incomplete = 2 };

struct Stats {
    uint32_t slots_registered;
    uint32_t tables_known;        // informational snapshots (capped at kMaxTables)
    uint32_t tables_complete;
    uint32_t tables_incomplete;
    uint32_t targets_hooked;      // distinct (slot, target) pairs hooked
    uint32_t targets_shared;      // slot/target pairs met again in another table (thunk reused)
    uint32_t cross_slot_rejected; // a target seen in a slot other than the one it was hooked for (-> Incomplete)
    uint32_t thunks_exhausted;    // (-> Incomplete)
    uint32_t installs_failed;     // (-> Incomplete)
    uint32_t queue_pushes, queue_overflow, queue_drained;
    uint32_t scope_overflow;      // CallScope nesting beyond kScopeDepth (ambiguous)
    uint32_t bundles_unsupported; // always 1
};

// Registration: once per kind, before any discovery for that kind. The
// whole input is validated first (duplicate slots within the call or
// against earlier registrations are rejected atomically, nothing mutated).
// Thunk and original arrays must outlive the registry.
bool Register(Kind kind, const SlotSpec* specs, uint32_t count);
// Tests: forget everything (hooks are removed by na::hooking::Shutdown).
// A50: refuses (false, nothing touched) unless admission is closed, every
// admitted producer has finished and the worker has exited. A51 contract:
// Clear and re-registration additionally REQUIRE hot-callback quiescence
// enforced by the caller (no thread inside NoteObject/CoverageOf, whose
// target snapshot is read before the producer guard, and no thunk in
// flight); they are test/teardown operations, never performed live.
bool Clear();
// Lifetime assumption (documented for the first Anno use): hooked targets
// are code inside the D3D12 runtime and the user-mode driver, which stay
// loaded for the process lifetime; the registry never unloads or re-hooks.

// Install-thread side (blocking). `object` must be valid for the call; its
// current table is read immediately. Never from a render callback.
Coverage EnsureObject(Kind kind, void* object);
// Same, from an immutable target snapshot (one entry per registered slot,
// in registration order).
Coverage EnsureTargets(Kind kind, const void* const* targets, uint32_t count);

// Warm-up for command lists on the given device: DIRECT/COMPUTE/COPY/BUNDLE
// (mask bits 1/2/4/8) lists driven through create -> Close -> Reset -> Close
// -> Reset with EnsureObject after each state. Returns states ensured.
uint32_t WarmUpCommandLists(ID3D12Device* device, uint32_t type_mask = 1 | 2 | 4 | 8);

// Hot path (any thread, lock-free): coverage of the object's CURRENT table.
// NoteObject additionally snapshots the slot targets of an Unknown object
// into the bounded queue (once per distinct snapshot). Returns Unknown when
// nothing is registered for the kind.
Coverage NoteObject(Kind kind, void* object);
Coverage CoverageOf(Kind kind, void* object);
bool Active(Kind kind);

// Discovery thread: drains the queue (EnsureTargets per snapshot) and calls
// `idle` (may be null) after each drain. StopDiscoveryThread closes admission
// and waits for the worker; it returns false and keeps every state if the
// worker has not exited within the timeout (nothing is cleared then).
bool StartDiscoveryThread(void (*idle)(void*), void* user);
// A50 (R3): closes admission first (worker or not), then waits (bounded, on
// the calling control thread) for every already-admitted producer to finish
// its write, then for the worker to exit. Any timeout returns false and
// keeps every state; call again later.
bool StopDiscoveryThread(uint32_t timeout_ms = 5000);
bool ProducersQuiescent(); // admission closed and no producer between admission and write
uint32_t DrainQueueNow(); // synchronous drain on the calling thread (tests)
// Test injection: the next NoteObject producer, after admission, waits on
// `event` (a Windows event handle) before writing its snapshot.
void PauseNextProducerForTest(void* event);

struct TableInfo {
    void* table;
    Coverage coverage;
    uint32_t slots_hooked, slots_missing;
    char reason[64];
    char module[64];        // module of the first registered slot's target
};
bool GetTableInfo(Kind kind, uint32_t index, TableInfo* out);
Stats GetStats(Kind kind);

// Forwarding scope for thunks (thread-local stack, bounded kScopeDepth).
struct CallScope {
    bool outermost;  // false = an enclosing scope on this thread has the same receiver and slot
    bool ambiguous;  // nesting exceeded the bounded stack: the answer above is not proven
    CallScope(const void* receiver, uint32_t slot);
    ~CallScope();
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;
};

} // namespace na::hooking::variants
