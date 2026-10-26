#include "host_dx12/hooking/variant_registry.hpp"

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "host_dx12/hooking/vtable_hook.hpp"

using Microsoft::WRL::ComPtr;

namespace na::hooking::variants {
namespace {

struct Slot {
    uint32_t slot = 0;
    uint32_t thunk_count = 0;
    void* const* thunks = nullptr;
    void** originals = nullptr;
    void* assigned_target[kMaxThunksPerSlot] = {}; // thunk i -> concrete target it hooks (for THIS slot)
    bool dead[kMaxThunksPerSlot] = {};             // install failed once: never reused
};
struct Table {
    void* table = nullptr;
    Coverage coverage = Coverage::Unknown;
    uint32_t slots_hooked = 0, slots_missing = 0;
    char reason[64] = "";
    char module[64] = "";
};
// One immutable snapshot of an object's slot targets (queue payload).
struct Snapshot {
    void* targets[kMaxSlots];
    uint32_t count;
};

constexpr uint32_t kMaxPairs = kMaxSlots * kMaxThunksPerSlot;

struct Registry {
    std::mutex mutex;                // install side
    Slot slots[kMaxSlots];
    uint32_t slot_count = 0;
    Table tables[kMaxTables];
    uint32_t table_count = 0;
    bool discovery_started = false;
    // Lock-free published view: (slot index, target) pairs hooked / dead, and the slot list.
    std::atomic<uint32_t> pub_slots[kMaxSlots];
    std::atomic<uint32_t> pub_slot_count{0};
    std::atomic<void*> pub_target[kMaxPairs];
    std::atomic<uint32_t> pub_target_slot[kMaxPairs];
    std::atomic<uint32_t> pub_count{0};
    std::atomic<void*> dead_target[kMaxPairs];
    std::atomic<uint32_t> dead_target_slot[kMaxPairs];
    std::atomic<uint32_t> dead_count{0};
    // Bounded queue of snapshots (multi-producer claim by CAS, single consumer).
    Snapshot queue[kQueueCapacity];
    std::atomic<uint32_t> queue_ready[kQueueCapacity]; // 1 when the snapshot is fully written
    std::atomic<uint32_t> queue_write{0}, queue_read{0};
    std::atomic<uint32_t> queue_pushes{0}, queue_overflow{0}, queue_drained{0};
    std::atomic<uint32_t> targets_hooked{0}, targets_shared{0}, cross_slot{0}, thunks_exhausted{0}, installs_failed{0};
};
Registry g_reg[kKinds];
std::atomic<uint32_t> g_scope_overflow{0};

// Discovery thread (shared by both kinds).
HANDLE g_queue_event = nullptr;
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_admission_closed{false};
std::atomic<uint32_t> g_producers_active{0};        // A50: NoteObject producers between admission and write
std::atomic<HANDLE> g_pause_producer_for_test{nullptr};
std::atomic<void (*)(void*)> g_idle{nullptr};
std::atomic<void*> g_idle_user{nullptr};

Registry& R(Kind k) { return g_reg[static_cast<uint32_t>(k) < kKinds ? static_cast<uint32_t>(k) : 0]; }

void module_of(void* address, char* out, size_t cap)
{
    HMODULE m = nullptr;
    out[0] = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &m) && m) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* base = path;
        for (const wchar_t* c = path; *c; ++c) if (*c == L'\\' || *c == L'/') base = c + 1;
        WideCharToMultiByte(CP_UTF8, 0, base, -1, out, static_cast<int>(cap), nullptr, nullptr);
    }
}

bool pair_published(const std::atomic<void*>* targets, const std::atomic<uint32_t>* slots_of, const std::atomic<uint32_t>& count,
                    uint32_t slot_index, void* target)
{
    uint32_t n = count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
        if (targets[i].load(std::memory_order_acquire) == target && slots_of[i].load(std::memory_order_acquire) == slot_index)
            return true;
    return false;
}
void publish_pair_locked(Registry& r, uint32_t slot_index, void* target, bool dead)
{
    std::atomic<void*>* targets = dead ? r.dead_target : r.pub_target;
    std::atomic<uint32_t>* slots_of = dead ? r.dead_target_slot : r.pub_target_slot;
    std::atomic<uint32_t>& count = dead ? r.dead_count : r.pub_count;
    if (pair_published(targets, slots_of, count, slot_index, target)) return;
    uint32_t n = count.load(std::memory_order_relaxed);
    if (n >= kMaxPairs) return;
    targets[n].store(target, std::memory_order_relaxed);
    slots_of[n].store(slot_index, std::memory_order_relaxed);
    count.store(n + 1, std::memory_order_release);
}

// Coverage of a target snapshot (registration order) by the published pairs.
Coverage coverage_of_targets(Registry& r, const void* const* targets, uint32_t count)
{
    uint32_t n = r.pub_slot_count.load(std::memory_order_acquire);
    if (!n || count < n) return Coverage::Unknown;
    bool incomplete = false;
    for (uint32_t i = 0; i < n; ++i) {
        void* t = const_cast<void*>(targets[i]);
        if (pair_published(r.pub_target, r.pub_target_slot, r.pub_count, i, t)) continue;
        if (pair_published(r.dead_target, r.dead_target_slot, r.dead_count, i, t)) { incomplete = true; continue; }
        return Coverage::Unknown;
    }
    return incomplete ? Coverage::Incomplete : Coverage::Complete;
}
// Reads an object's current slot targets (object must be valid now).
uint32_t snapshot_of(Registry& r, void* object, void** out)
{
    void** table = *static_cast<void***>(object);
    uint32_t n = r.pub_slot_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) out[i] = table[r.pub_slots[i].load(std::memory_order_acquire)];
    return n;
}

int thunk_for_target(const Slot& s, void* target)
{
    for (uint32_t i = 0; i < s.thunk_count; ++i)
        if (s.assigned_target[i] == target) return static_cast<int>(i);
    return -1;
}
// Is `target` hooked (live or dead) for some OTHER slot of this registry?
bool hooked_elsewhere(Registry& r, uint32_t slot_index, void* target)
{
    for (uint32_t k = 0; k < r.slot_count; ++k) {
        if (k == slot_index) continue;
        if (thunk_for_target(r.slots[k], target) >= 0) return true;
    }
    return false;
}

Coverage ensure_locked(Registry& r, const void* const* targets_in, uint32_t count, void* table_for_info)
{
    if (count < r.slot_count) return Coverage::Unknown;
    Table scratch;
    Table* v = &scratch;
    if (table_for_info) {
        int found = -1;
        for (uint32_t i = 0; i < r.table_count; ++i) if (r.tables[i].table == table_for_info) { found = static_cast<int>(i); break; }
        if (found < 0 && r.table_count < kMaxTables) { found = static_cast<int>(r.table_count++); r.tables[found] = Table{}; r.tables[found].table = table_for_info; }
        if (found >= 0) v = &r.tables[found];
    }
    struct Plan { uint32_t slot; int thunk; void* target; };
    Plan plans[kMaxSlots];
    uint32_t plan_count = 0, hooked = 0, missing = 0;
    v->reason[0] = 0;
    for (uint32_t si = 0; si < r.slot_count; ++si) {
        Slot& s = r.slots[si];
        void* target = const_cast<void*>(targets_in[si]);
        if (si == 0) module_of(target, v->module, sizeof(v->module));
        int existing = thunk_for_target(s, target);
        if (existing >= 0) {
            if (s.dead[existing]) { ++missing; lstrcpynA(v->reason, "target dead (earlier install failed)", sizeof(v->reason)); continue; }
            ++hooked;
            if (v->coverage == Coverage::Unknown) r.targets_shared.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (hooked_elsewhere(r, si, target)) {
            // One detour per target (core rule): a target already owned by another
            // slot's observer cannot be hooked again with this slot's thunk. Never
            // report it covered.
            ++missing;
            r.cross_slot.fetch_add(1, std::memory_order_relaxed);
            publish_pair_locked(r, si, target, true);
            lstrcpynA(v->reason, "target already hooked for another slot", sizeof(v->reason));
            continue;
        }
        int free_thunk = -1;
        for (uint32_t i = 0; i < s.thunk_count; ++i)
            if (!s.assigned_target[i] && !s.dead[i]) { free_thunk = static_cast<int>(i); break; }
        if (free_thunk < 0) {
            ++missing;
            r.thunks_exhausted.fetch_add(1, std::memory_order_relaxed);
            publish_pair_locked(r, si, target, true);
            lstrcpynA(v->reason, "thunk pool exhausted for a slot", sizeof(v->reason));
            continue;
        }
        plans[plan_count++] = {si, free_thunk, target};
    }
    if (plan_count) {
        bool ok = na::hooking::BeginInstall();
        for (uint32_t i = 0; ok && i < plan_count; ++i) {
            Slot& s = r.slots[plans[i].slot];
            ok = na::hooking::PrepareHook(plans[i].target, s.thunks[plans[i].thunk], &s.originals[plans[i].thunk]);
            if (!ok) lstrcpynA(v->reason, "prepare failed for a target", sizeof(v->reason));
        }
        if (ok) ok = na::hooking::CommitInstall();
        else na::hooking::AbandonInstall();
        if (ok) {
            for (uint32_t i = 0; i < plan_count; ++i) {
                r.slots[plans[i].slot].assigned_target[plans[i].thunk] = plans[i].target;
                publish_pair_locked(r, plans[i].slot, plans[i].target, false);
            }
            hooked += plan_count;
            r.targets_hooked.fetch_add(plan_count, std::memory_order_relaxed);
        } else {
            for (uint32_t i = 0; i < plan_count; ++i) {
                Slot& s = r.slots[plans[i].slot];
                if (s.originals[plans[i].thunk]) { s.assigned_target[plans[i].thunk] = plans[i].target; s.dead[plans[i].thunk] = true; }
                publish_pair_locked(r, plans[i].slot, plans[i].target, true);
            }
            missing += plan_count;
            r.installs_failed.fetch_add(1, std::memory_order_relaxed);
            if (!v->reason[0]) lstrcpynA(v->reason, "install transaction failed", sizeof(v->reason));
        }
    }
    v->slots_hooked = hooked;
    v->slots_missing = missing;
    v->coverage = (missing == 0 && hooked == r.slot_count) ? Coverage::Complete : Coverage::Incomplete;
    return v->coverage;
}

uint32_t drain(Registry& r)
{
    uint32_t drained = 0;
    for (;;) {
        uint32_t rd = r.queue_read.load(std::memory_order_acquire);
        if (rd == r.queue_write.load(std::memory_order_acquire)) break;
        uint32_t i = rd % kQueueCapacity;
        // A claimed slot whose snapshot is not yet written: wait briefly, never for ever.
        int spins = 0;
        while (!r.queue_ready[i].load(std::memory_order_acquire)) {
            if (++spins > 1000) { r.queue_drained.fetch_add(drained, std::memory_order_relaxed); return drained; }
            Sleep(0);
        }
        Snapshot snap = r.queue[i];
        r.queue_ready[i].store(0, std::memory_order_release);
        r.queue_read.store(rd + 1, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            r.discovery_started = true;
            ensure_locked(r, snap.targets, snap.count, nullptr);
        }
        ++drained;
    }
    r.queue_drained.fetch_add(drained, std::memory_order_relaxed);
    return drained;
}

DWORD WINAPI DiscoveryThread(LPVOID)
{
    while (!g_stop.load(std::memory_order_acquire)) {
        WaitForSingleObject(g_queue_event, 250);
        DrainQueueNow();
        if (auto idle = g_idle.load(std::memory_order_acquire)) idle(g_idle_user.load(std::memory_order_relaxed));
    }
    return 0;
}

struct Frame { const void* receiver; uint32_t slot; };
thread_local Frame t_stack[kScopeDepth];
thread_local uint32_t t_depth = 0;

} // namespace

bool Register(Kind kind, const SlotSpec* specs, uint32_t count)
{
    Registry& r = R(kind);
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.discovery_started || !specs || count == 0 || r.slot_count + count > kMaxSlots) return false;
    // Validate the WHOLE input before touching anything (A49 fix 1).
    for (uint32_t i = 0; i < count; ++i) {
        const SlotSpec& sp = specs[i];
        if (!sp.thunks || !sp.originals || sp.thunk_count == 0 || sp.thunk_count > kMaxThunksPerSlot) return false;
        for (uint32_t k = 0; k < r.slot_count; ++k) if (r.slots[k].slot == sp.slot) return false;
        for (uint32_t j = 0; j < i; ++j) if (specs[j].slot == sp.slot) return false; // duplicate within the call
    }
    for (uint32_t i = 0; i < count; ++i) {
        r.pub_slots[r.slot_count].store(specs[i].slot, std::memory_order_release);
        Slot& s = r.slots[r.slot_count++];
        s.slot = specs[i].slot;
        s.thunk_count = specs[i].thunk_count;
        s.thunks = specs[i].thunks;
        s.originals = specs[i].originals;
        for (uint32_t t = 0; t < kMaxThunksPerSlot; ++t) { s.assigned_target[t] = nullptr; s.dead[t] = false; }
        for (uint32_t t = 0; t < s.thunk_count; ++t) s.originals[t] = nullptr;
    }
    r.pub_slot_count.store(r.slot_count, std::memory_order_release);
    return true;
}

bool Clear()
{
    if (!StopDiscoveryThread(5000)) return false; // never clear under a live worker or an admitted producer
    for (Registry& r : g_reg) {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.slot_count = 0;
        r.table_count = 0;
        r.discovery_started = false;
        r.pub_slot_count.store(0);
        r.pub_count.store(0);
        r.dead_count.store(0);
        r.queue_write.store(0); r.queue_read.store(0);
        for (uint32_t i = 0; i < kQueueCapacity; ++i) r.queue_ready[i].store(0);
        r.targets_hooked.store(0); r.targets_shared.store(0); r.cross_slot.store(0); r.thunks_exhausted.store(0); r.installs_failed.store(0);
        r.queue_pushes.store(0); r.queue_overflow.store(0); r.queue_drained.store(0);
    }
    g_scope_overflow.store(0);
    g_admission_closed.store(false); // reopened only after proven quiescence above
    return true;
}

Coverage EnsureTargets(Kind kind, const void* const* targets, uint32_t count)
{
    Registry& r = R(kind);
    std::lock_guard<std::mutex> lock(r.mutex);
    r.discovery_started = true;
    return ensure_locked(r, targets, count, nullptr);
}

Coverage EnsureObject(Kind kind, void* object)
{
    if (!object) return Coverage::Unknown;
    Registry& r = R(kind);
    void* snap[kMaxSlots];
    uint32_t n = snapshot_of(r, object, snap);
    std::lock_guard<std::mutex> lock(r.mutex);
    r.discovery_started = true;
    return ensure_locked(r, snap, n, *static_cast<void**>(object));
}

uint32_t WarmUpCommandLists(ID3D12Device* device, uint32_t type_mask)
{
    if (!device) return 0;
    uint32_t ensured = 0;
    const D3D12_COMMAND_LIST_TYPE types[4] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                              D3D12_COMMAND_LIST_TYPE_COPY, D3D12_COMMAND_LIST_TYPE_BUNDLE};
    for (int t = 0; t < 4; ++t) {
        if (!(type_mask & (1u << t))) continue;
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(types[t], IID_PPV_ARGS(&alloc)))) continue;
        if (FAILED(device->CreateCommandList(0, types[t], alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) continue;
        EnsureObject(Kind::CommandList, list.Get()); ++ensured;
        list->Close();
        if (SUCCEEDED(list->Reset(alloc.Get(), nullptr))) { EnsureObject(Kind::CommandList, list.Get()); ++ensured; list->Close(); }
        if (SUCCEEDED(list->Reset(alloc.Get(), nullptr))) { EnsureObject(Kind::CommandList, list.Get()); ++ensured; list->Close(); }
    }
    return ensured;
}

Coverage CoverageOf(Kind kind, void* object)
{
    if (!object) return Coverage::Unknown;
    Registry& r = R(kind);
    void* snap[kMaxSlots];
    uint32_t n = snapshot_of(r, object, snap);
    return coverage_of_targets(r, snap, n);
}

bool Active(Kind kind) { return R(kind).pub_slot_count.load(std::memory_order_acquire) != 0; }

Coverage NoteObject(Kind kind, void* object)
{
    if (!object) return Coverage::Unknown;
    Registry& r = R(kind);
    if (!r.pub_slot_count.load(std::memory_order_acquire)) return Coverage::Unknown;
    void* snap[kMaxSlots];
    uint32_t n = snapshot_of(r, object, snap);
    Coverage c = coverage_of_targets(r, snap, n);
    if (c != Coverage::Unknown) return c;
    // A50 (R1/R3): no producer-side duplicate scan (a queue slot can be reused
    // under a scanning producer); duplicates are harmlessly coalesced by the
    // consumer (ensure is idempotent). Producer accounting brackets admission
    // and the write, so Stop/Clear can prove no producer still owns a slot.
    struct ProducerGuard { ProducerGuard() { g_producers_active.fetch_add(1, std::memory_order_acq_rel); } ~ProducerGuard() { g_producers_active.fetch_sub(1, std::memory_order_acq_rel); } } guard;
    if (g_admission_closed.load(std::memory_order_acquire)) return Coverage::Unknown;
    if (HANDLE pause = g_pause_producer_for_test.exchange(nullptr, std::memory_order_acq_rel)) WaitForSingleObject(pause, 10000); // test injection only
    for (;;) {
        uint32_t wr = r.queue_write.load(std::memory_order_acquire);
        uint32_t rd = r.queue_read.load(std::memory_order_acquire);
        if (wr - rd >= kQueueCapacity) { r.queue_overflow.fetch_add(1, std::memory_order_relaxed); return Coverage::Unknown; }
        if (r.queue_write.compare_exchange_weak(wr, wr + 1, std::memory_order_acq_rel)) {
            uint32_t k = wr % kQueueCapacity;
            std::memcpy(r.queue[k].targets, snap, n * sizeof(void*));
            r.queue[k].count = n;
            r.queue_ready[k].store(1, std::memory_order_release);
            r.queue_pushes.fetch_add(1, std::memory_order_relaxed);
            if (g_queue_event) SetEvent(g_queue_event);
            return Coverage::Unknown;
        }
    }
}

uint32_t DrainQueueNow()
{
    uint32_t n = 0;
    for (Registry& r : g_reg) n += drain(r);
    return n;
}

bool StartDiscoveryThread(void (*idle)(void*), void* user)
{
    if (g_thread) {
        // Started earlier without a consumer (the observation producers start
        // it first under [observe] early_install): adopt this one.
        if (idle && !g_idle.load(std::memory_order_acquire)) {
            g_idle_user.store(user, std::memory_order_relaxed);
            g_idle.store(idle, std::memory_order_release);
        }
        return true;
    }
    g_idle_user.store(user, std::memory_order_relaxed);
    g_idle.store(idle, std::memory_order_release);
    if (!g_queue_event) g_queue_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_queue_event) return false;
    g_stop.store(false, std::memory_order_release);
    g_admission_closed.store(false, std::memory_order_release);
    g_thread = CreateThread(nullptr, 0, &DiscoveryThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

bool StopDiscoveryThread(uint32_t timeout_ms)
{
    // A50 (R3): admission is closed FIRST, whether or not a worker exists;
    // then producers already admitted must finish (bounded wait on this
    // control thread), then the worker must exit. Any timeout returns false
    // and leaves every state in place.
    g_admission_closed.store(true, std::memory_order_release);
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (g_producers_active.load(std::memory_order_acquire)) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
    if (!g_thread) return true;
    g_stop.store(true, std::memory_order_release);
    SetEvent(g_queue_event);
    ULONGLONG now = GetTickCount64();
    DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    if (WaitForSingleObject(g_thread, remaining) != WAIT_OBJECT_0) return false; // worker still active: keep everything
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_idle.store(nullptr, std::memory_order_release);
    g_idle_user.store(nullptr, std::memory_order_relaxed);
    return true;
}

bool ProducersQuiescent() { return g_admission_closed.load(std::memory_order_acquire) && g_producers_active.load(std::memory_order_acquire) == 0; }
void PauseNextProducerForTest(void* event) { g_pause_producer_for_test.store(static_cast<HANDLE>(event), std::memory_order_release); }

bool GetTableInfo(Kind kind, uint32_t index, TableInfo* out)
{
    if (!out) return false;
    Registry& r = R(kind);
    std::lock_guard<std::mutex> lock(r.mutex);
    if (index >= r.table_count) return false;
    const Table& v = r.tables[index];
    out->table = v.table;
    out->coverage = v.coverage;
    out->slots_hooked = v.slots_hooked;
    out->slots_missing = v.slots_missing;
    lstrcpynA(out->reason, v.reason, sizeof(out->reason));
    lstrcpynA(out->module, v.module, sizeof(out->module));
    return true;
}

Stats GetStats(Kind kind)
{
    Registry& r = R(kind);
    Stats s{};
    std::lock_guard<std::mutex> lock(r.mutex);
    s.slots_registered = r.slot_count;
    s.tables_known = r.table_count;
    for (uint32_t i = 0; i < r.table_count; ++i) {
        if (r.tables[i].coverage == Coverage::Complete) ++s.tables_complete; else ++s.tables_incomplete;
    }
    s.targets_hooked = r.targets_hooked.load(std::memory_order_relaxed);
    s.targets_shared = r.targets_shared.load(std::memory_order_relaxed);
    s.cross_slot_rejected = r.cross_slot.load(std::memory_order_relaxed);
    s.thunks_exhausted = r.thunks_exhausted.load(std::memory_order_relaxed);
    s.installs_failed = r.installs_failed.load(std::memory_order_relaxed);
    s.queue_pushes = r.queue_pushes.load(std::memory_order_relaxed);
    s.queue_overflow = r.queue_overflow.load(std::memory_order_relaxed);
    s.queue_drained = r.queue_drained.load(std::memory_order_relaxed);
    s.scope_overflow = g_scope_overflow.load(std::memory_order_relaxed);
    s.bundles_unsupported = 1;
    return s;
}

CallScope::CallScope(const void* receiver, uint32_t slot)
{
    outermost = true;
    ambiguous = false;
    uint32_t scan = t_depth < kScopeDepth ? t_depth : kScopeDepth;
    for (uint32_t i = 0; i < scan; ++i)
        if (t_stack[i].receiver == receiver && t_stack[i].slot == slot) { outermost = false; break; }
    if (t_depth < kScopeDepth) {
        t_stack[t_depth] = {receiver, slot};
    } else {
        ambiguous = true; // deeper frames are not recorded; the answer cannot be proven
        g_scope_overflow.fetch_add(1, std::memory_order_relaxed);
    }
    ++t_depth;
}
CallScope::~CallScope() { if (t_depth) --t_depth; }

} // namespace na::hooking::variants
