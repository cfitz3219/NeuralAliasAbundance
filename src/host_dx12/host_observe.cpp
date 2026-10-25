// H1 observation host: framing, loss accounting, epochs and status â€” the
// contract frozen as ABI 0.1 (host_observe.h). Producers (D3D12 hooks) attach
// via host_observe_internal.hpp; this file contains no D3D12 code.
//
// Transport (A24, corrected A26 per Route B's A24-TRANSPORT-REVIEW):
//  - Ordinary events: per-producer-thread SPSC byte rings (lock-free).
//  - Snapshot boundary events (EPOCH_BEGIN / INVENTORY_COMPLETE): a
//    serialized CONTROL LANE. Each control entry records every ring's head
//    at emit time; the worker drains rings only up to those marks before
//    delivering the entry. Hence everything published before the boundary
//    (including a helper thread's inventory event) is delivered before it,
//    and nothing published after EPOCH_BEGIN is delivered before it. (R1)
//  - Producer quiescence: emits are counted in/out; stop() waits for the
//    count to reach zero before the final drain, and start() publishes
//    `started` only after all state is reset. (R2)
//  - Status: one spinlock serializes every status writer (validity, drop
//    counters, epoch) around the seqlock, and the eligibility check for
//    validity promotion happens inside the same critical section. (R3)
#include "host_dx12/host_observe.h"

#include "host_dx12/host_observe_internal.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

using na::observe_internal::SnapshotFn;

constexpr uint32_t kDefaultBudgetMiB = 32;
constexpr uint32_t kMaxBatchBytes = 64 * 1024;
constexpr uint32_t kRings = 32;               // claimed thread ids per session
constexpr size_t kMinRingBytes = 2 * kMaxBatchBytes; // one max event must fit

struct Ring {
    std::atomic<DWORD> owner{0};
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    std::vector<uint8_t> buf;
};

struct ControlEntry {
    std::vector<uint8_t> bytes;   // one event, epoch stamped
    uint64_t heads[kRings];       // ring heads at emit time (drain barrier)
};

struct HostState {
    std::mutex mutex;                 // lifecycle + worker wake
    std::condition_variable cv;
    std::vector<uint8_t> queue;       // worker-private staging
    Ring rings[kRings];
    size_t ring_bytes = 0;
    std::atomic<uint64_t> pending_bytes{0};
    std::mutex control_m;             // snapshot lane (lifecycle threads only)
    std::deque<ControlEntry> control;
    std::atomic<uint32_t> control_count{0};
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> active_producers{0}; // R2 quiescence counter
    // T2: lifecycle generation. Bumped at every start and stop; a producer
    // records it before admission and bails after counting itself in if it
    // changed, so an admission attempt from an old session can never land in
    // a new one.
    std::atomic<uint64_t> generation{0};
    na_event_sink sink = nullptr;
    void* user = nullptr;
    std::thread worker;
    DWORD worker_tid = 0;

    std::atomic<na_session_id> session{0}; // T3: published under status_lock
    std::atomic<uint64_t> epoch{0};
    std::atomic<uint32_t> inventory_invalid{1};
    std::atomic<uint64_t> foundational_dropped{0};
    std::atomic<uint64_t> detail_dropped{0};
    std::atomic<uint64_t> batches_delivered{0};
    std::atomic<uint64_t> group_dropped[16];
    std::atomic<uint32_t> pending_loss_mark{0};
    std::atomic<uint32_t> pending_loss_foundational{0};

    // R3: status writers are serialized by this spinlock; readers use the
    // seqlock. Every mutation of epoch/validity/drop counters goes through
    // status_lock()/status_unlock().
    std::atomic<bool> status_busy{false};
    std::atomic<uint64_t> status_seq{0};
    // T4 (rev2): producers never lock, but a reader must not accept a
    // snapshot while a drop's multi-field update is in flight. Producers
    // count themselves in/out around the update (two wait-free atomics);
    // readers retry while the count is non-zero or the sequence moved.
    std::atomic<uint32_t> drops_in_progress{0};

    std::atomic<uint64_t> pending_valid_epoch{0};
    std::atomic<uint64_t> pending_valid_drops{0};

    SnapshotFn snapshot;
    std::atomic<bool> force_foundational_overflow{false};
};

HostState g_host;

void status_lock()
{
    while (g_host.status_busy.exchange(true, std::memory_order_acquire)) YieldProcessor();
    g_host.status_seq.fetch_add(1, std::memory_order_acq_rel); // odd: writing
}
void status_unlock()
{
    g_host.status_seq.fetch_add(1, std::memory_order_acq_rel); // even: stable
    g_host.status_busy.store(false, std::memory_order_release);
}

void append_loss_mark(std::vector<uint8_t>& out, uint32_t group, uint32_t foundational)
{
    na_ev_loss_mark ev{};
    ev.hdr.size_bytes = sizeof(ev);
    ev.hdr.kind = NA_EV_LOSS_MARK;
    ev.hdr.epoch = g_host.epoch.load(std::memory_order_acquire);
    ev.hdr.producer_thread = GetCurrentThreadId();
    ev.hdr.flags = foundational ? NA_EVF_FOUNDATIONAL : 0;
    ev.group = group;
    ev.foundational = foundational;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&ev);
    out.insert(out.end(), p, p + sizeof(ev));
}

void deliver_batch(std::vector<uint8_t>& payload, uint32_t flags, na_epoch batch_epoch)
{
    na_event_batch hdr{};
    hdr.header_bytes = sizeof(na_event_batch);
    hdr.abi_major = NA_OBSERVE_ABI_MAJOR;
    hdr.abi_minor = NA_OBSERVE_ABI_MINOR;
    hdr.session = g_host.session.load(std::memory_order_acquire);
    hdr.epoch = batch_epoch;
    hdr.payload_bytes = static_cast<uint32_t>(payload.size());
    uint32_t count = 0;
    for (size_t off = 0; off + sizeof(na_event_header) <= payload.size();) {
        const auto* eh = reinterpret_cast<const na_event_header*>(payload.data() + off);
        off += eh->size_bytes;
        ++count;
    }
    hdr.event_count = count;
    hdr.batch_flags = flags;

    std::vector<uint8_t> batch(sizeof(hdr) + payload.size());
    std::memcpy(batch.data(), &hdr, sizeof(hdr));
    if (!payload.empty()) std::memcpy(batch.data() + sizeof(hdr), payload.data(), payload.size());
    g_host.sink(reinterpret_cast<const na_event_batch*>(batch.data()), g_host.user);
    g_host.batches_delivered.fetch_add(1, std::memory_order_relaxed);

    // H1-R1 + R3: after DELIVERY, promote validity iff this batch carried the
    // pending epoch's INVENTORY_COMPLETE(complete=1); the eligibility check
    // (epoch still current, no foundational loss since queueing) and the
    // store happen atomically under the status lock so loss/resync cannot
    // interleave between check and store.
    uint64_t pend = g_host.pending_valid_epoch.load(std::memory_order_acquire);
    if (pend != 0 && pend == batch_epoch) {
        bool saw_complete = false;
        for (size_t off = 0; off + sizeof(na_event_header) <= payload.size();) {
            const auto* eh = reinterpret_cast<const na_event_header*>(payload.data() + off);
            if (eh->kind == NA_EV_INVENTORY_COMPLETE && eh->size_bytes >= sizeof(na_ev_inventory_complete)) {
                const auto* ic = reinterpret_cast<const na_ev_inventory_complete*>(eh);
                if (ic->epoch == pend && ic->complete == 1) saw_complete = true;
            }
            off += eh->size_bytes;
        }
        if (saw_complete) {
            status_lock();
            uint64_t expect_drops = g_host.pending_valid_drops.load(std::memory_order_acquire);
            if (g_host.pending_valid_epoch.load(std::memory_order_acquire) == pend &&
                g_host.epoch.load(std::memory_order_acquire) == pend &&
                g_host.foundational_dropped.load(std::memory_order_acquire) == expect_drops) {
                g_host.inventory_invalid.store(0, std::memory_order_release);
                g_host.pending_valid_epoch.store(0, std::memory_order_release);
                // T4: drops are lock-free (producer path must not block), so a
                // foundational drop may land between the check and the store.
                // Drops always store invalid=1 AFTER incrementing; re-checking
                // the counter after our store closes every interleaving.
                if (g_host.foundational_dropped.load(std::memory_order_acquire) != expect_drops)
                    g_host.inventory_invalid.store(1, std::memory_order_release);
            }
            status_unlock();
        }
    }
}

// Worker only. Drains each ring up to `limits[i]` (or its current head when
// limits is null) into the private staging buffer.
void drain_rings(const uint64_t* limits)
{
    for (uint32_t i = 0; i < kRings; ++i) {
        Ring& r = g_host.rings[i];
        uint64_t head = r.head.load(std::memory_order_acquire);
        if (limits && limits[i] < head) head = limits[i];
        uint64_t tail = r.tail.load(std::memory_order_relaxed);
        if (head <= tail) continue;
        size_t n = static_cast<size_t>(head - tail);
        size_t cap = r.buf.size();
        size_t pos = static_cast<size_t>(tail % cap);
        size_t first = std::min(n, cap - pos);
        g_host.queue.insert(g_host.queue.end(), r.buf.data() + pos, r.buf.data() + pos + first);
        if (first < n) g_host.queue.insert(g_host.queue.end(), r.buf.data(), r.buf.data() + (n - first));
        r.tail.store(head, std::memory_order_release);
        g_host.pending_bytes.fetch_sub(n, std::memory_order_acq_rel);
    }
}

// Worker only. Common ordering protocol with emit_control (T1): ring-head
// snapshots â€” both the barrier stored in a control entry and the limit used
// for a "free" drain â€” are taken while holding control_m. So any control
// entry published after the worker's snapshot has barriers >= that snapshot,
// and events its publisher wrote after it lie beyond the snapshot too: a free
// drain can never consume events that belong behind a not-yet-dequeued
// boundary.
void gather()
{
    for (;;) {
        ControlEntry entry;
        uint64_t free_limit[kRings];
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_host.control_m);
            if (!g_host.control.empty()) {
                entry = std::move(g_host.control.front());
                g_host.control.pop_front();
                g_host.control_count.fetch_sub(1, std::memory_order_acq_rel);
                have = true;
            } else {
                for (uint32_t i = 0; i < kRings; ++i)
                    free_limit[i] = g_host.rings[i].head.load(std::memory_order_acquire);
            }
        }
        if (!have) {
            drain_rings(free_limit);
            break;
        }
        drain_rings(entry.heads);                       // everything before the boundary
        g_host.queue.insert(g_host.queue.end(), entry.bytes.begin(), entry.bytes.end());
    }
}

void worker_main()
{
    g_host.worker_tid = GetCurrentThreadId();
    std::unique_lock<std::mutex> lock(g_host.mutex);
    for (;;) {
        g_host.cv.wait_for(lock, std::chrono::milliseconds(50), [] {
            return g_host.stopping.load(std::memory_order_acquire) ||
                   g_host.pending_bytes.load(std::memory_order_acquire) != 0 ||
                   g_host.control_count.load(std::memory_order_acquire) != 0 ||
                   g_host.pending_loss_mark.load(std::memory_order_acquire) != 0;
        });
        bool final_pass = g_host.stopping.load(std::memory_order_acquire);
        if (final_pass) {
            // R2: no producer may still be inside emit when we take the final
            // view of the rings. Producers are bounded (a memcpy), so this is
            // short; it is a lifecycle wait, never a hook-path wait.
            while (g_host.active_producers.load(std::memory_order_acquire) != 0) YieldProcessor();
        }
        gather();
        do {
            std::vector<uint8_t> payload;
            na_epoch batch_epoch = g_host.epoch.load(std::memory_order_acquire);
            size_t take = 0;
            bool first = true;
            while (take < g_host.queue.size()) {
                const auto* eh = reinterpret_cast<const na_event_header*>(g_host.queue.data() + take);
                if (first) {
                    batch_epoch = eh->epoch;
                    first = false;
                } else {
                    if (eh->epoch != batch_epoch) break;
                    if (take + eh->size_bytes > kMaxBatchBytes) break;
                }
                take += eh->size_bytes;
            }
            if (batch_epoch == g_host.epoch.load(std::memory_order_acquire)) {
                uint32_t marks = g_host.pending_loss_mark.exchange(0, std::memory_order_acq_rel);
                uint32_t fmarks = g_host.pending_loss_foundational.exchange(0, std::memory_order_acq_rel);
                std::vector<uint8_t> mark_bytes;
                for (uint32_t g = 0; g < 16; ++g)
                    if (marks & (1u << g)) append_loss_mark(mark_bytes, g, (fmarks >> g) & 1u);
                payload.insert(payload.end(), mark_bytes.begin(), mark_bytes.end());
            }
            payload.insert(payload.end(), g_host.queue.begin(), g_host.queue.begin() + take);
            g_host.queue.erase(g_host.queue.begin(), g_host.queue.begin() + take);
            if (final_pass) gather();
            bool more = !g_host.queue.empty();
            uint32_t flags = (final_pass && !more) ? NA_BATCHF_FINAL : 0;
            if (!payload.empty() || flags) {
                lock.unlock();
                deliver_batch(payload, flags, batch_epoch);
                lock.lock();
            }
            if (!more) break;
        } while (true);
        if (final_pass) return;
    }
}

Ring* thread_ring()
{
    thread_local Ring* ring = nullptr;
    thread_local uint64_t ring_session = 0;
    uint64_t session = g_host.session.load(std::memory_order_acquire);
    if (ring && ring_session == session) return ring;
    ring = nullptr;
    DWORD tid = GetCurrentThreadId();
    for (Ring& r : g_host.rings) {
        DWORD expect = 0;
        if (r.owner.load(std::memory_order_acquire) == tid ||
            r.owner.compare_exchange_strong(expect, tid, std::memory_order_acq_rel)) {
            ring = &r;
            ring_session = session;
            break;
        }
    }
    return ring;
}

// T4: the overload path must stay nonblocking (it runs inside game hooks).
// No lock: counters are atomics; the seqlock is bumped by +2 (even -> even)
// so a reader that straddled the change retries, and a lifecycle writer's
// odd phase is preserved.
void record_drop(uint32_t group, bool foundational)
{
    g_host.drops_in_progress.fetch_add(1, std::memory_order_acq_rel);
    struct Done { ~Done() { g_host.drops_in_progress.fetch_sub(1, std::memory_order_acq_rel); } } done;
    if (group < 16) g_host.group_dropped[group].fetch_add(1, std::memory_order_seq_cst);
    if (foundational) {
        g_host.foundational_dropped.fetch_add(1, std::memory_order_seq_cst);
        g_host.inventory_invalid.store(1, std::memory_order_seq_cst); // after the increment
        g_host.pending_loss_foundational.fetch_or(1u << (group & 15), std::memory_order_acq_rel);
    } else {
        g_host.detail_dropped.fetch_add(1, std::memory_order_seq_cst);
    }
    g_host.status_seq.fetch_add(2, std::memory_order_seq_cst);
    g_host.pending_loss_mark.fetch_or(1u << (group & 15), std::memory_order_acq_rel);
    g_host.cv.notify_one();
}

// Lock-free producer path. R2/T2: counted in/out and generation-checked so
// stop() can quiesce and a stale admission cannot land in a later session.
bool emit_ring(const void* bytes, uint32_t group)
{
    const auto* hdr = static_cast<const na_event_header*>(bytes);
    bool foundational = (hdr->flags & NA_EVF_FOUNDATIONAL) != 0;
    const uint64_t gen = g_host.generation.load(std::memory_order_acquire);
    if (!g_host.started.load(std::memory_order_acquire)) return false;
    g_host.active_producers.fetch_add(1, std::memory_order_acq_rel);
    struct Leave { ~Leave() { g_host.active_producers.fetch_sub(1, std::memory_order_acq_rel); } } leave;
    if (g_host.generation.load(std::memory_order_acquire) != gen ||
        !g_host.started.load(std::memory_order_acquire) ||
        g_host.stopping.load(std::memory_order_acquire))
        return false; // lifecycle moved under us: not admitted
    bool malformed = hdr->size_bytes < sizeof(na_event_header) || (hdr->size_bytes % 8) != 0 ||
                     hdr->size_bytes > kMaxBatchBytes;
    Ring* ring = malformed ? nullptr : thread_ring();
    bool overflow = malformed || !ring;
    if (ring && !overflow) {
        uint64_t head = ring->head.load(std::memory_order_relaxed);
        uint64_t tail = ring->tail.load(std::memory_order_acquire);
        if (ring->buf.size() - (head - tail) < hdr->size_bytes) overflow = true;
    }
    if (foundational && g_host.force_foundational_overflow.load(std::memory_order_acquire)) overflow = true;
    if (overflow) {
        record_drop(group, foundational);
        return false;
    }
    const size_t size = hdr->size_bytes;
    const size_t cap = ring->buf.size();
    uint64_t head = ring->head.load(std::memory_order_relaxed);
    na_event_header stamped = *hdr;
    stamped.epoch = g_host.epoch.load(std::memory_order_acquire);
    auto put = [&](const uint8_t* src, size_t n, uint64_t at) {
        size_t pos = static_cast<size_t>(at % cap);
        size_t first = std::min(n, cap - pos);
        std::memcpy(ring->buf.data() + pos, src, first);
        if (first < n) std::memcpy(ring->buf.data(), src + first, n - first);
    };
    put(reinterpret_cast<const uint8_t*>(&stamped), sizeof(stamped), head);
    put(static_cast<const uint8_t*>(bytes) + sizeof(stamped), size - sizeof(stamped), head + sizeof(stamped));
    g_host.pending_bytes.fetch_add(size, std::memory_order_acq_rel); // before publish: no underflow
    ring->head.store(head + size, std::memory_order_release);
    g_host.cv.notify_one();
    return true;
}

// Snapshot-lane emit (lifecycle threads only): records the drain barrier.
bool emit_control(const void* bytes)
{
    const auto* hdr = static_cast<const na_event_header*>(bytes);
    if (!g_host.started.load(std::memory_order_acquire) || g_host.stopping.load(std::memory_order_acquire))
        return false;
    // The lane is bounded and never overflows in practice, but a lost
    // boundary must remain simulable (H1-R2) and accounted like any
    // foundational loss.
    if (g_host.force_foundational_overflow.load(std::memory_order_acquire)) {
        record_drop(NA_GROUP_SESSION, true);
        return false;
    }
    ControlEntry e;
    e.bytes.assign(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + hdr->size_bytes);
    reinterpret_cast<na_event_header*>(e.bytes.data())->epoch = g_host.epoch.load(std::memory_order_acquire);
    {
        // T1: barrier snapshot and publication under the same lock the worker
        // uses for its own snapshot (see gather()). Bounded lane: overflow is
        // a foundational loss, never unbounded growth.
        std::lock_guard<std::mutex> lock(g_host.control_m);
        if (g_host.control.size() >= 1024) {
            record_drop(NA_GROUP_SESSION, true);
            return false;
        }
        for (uint32_t i = 0; i < kRings; ++i) e.heads[i] = g_host.rings[i].head.load(std::memory_order_acquire);
        g_host.control.push_back(std::move(e));
        g_host.control_count.fetch_add(1, std::memory_order_acq_rel);
    }
    g_host.cv.notify_one();
    return true;
}

bool run_snapshot()
{
    uint64_t drops_before = g_host.foundational_dropped.load(std::memory_order_acquire);
    bool complete = true;

    na_ev_epoch_begin begin{};
    begin.hdr.size_bytes = sizeof(begin);
    begin.hdr.kind = NA_EV_EPOCH_BEGIN;
    begin.hdr.flags = NA_EVF_FOUNDATIONAL;
    begin.hdr.producer_thread = GetCurrentThreadId();
    begin.session = g_host.session.load(std::memory_order_acquire);
    begin.epoch = g_host.epoch.load(std::memory_order_acquire);
    if (!emit_control(&begin)) complete = false;

    if (g_host.snapshot && !g_host.snapshot()) complete = false;
    uint64_t drops_after = g_host.foundational_dropped.load(std::memory_order_acquire);
    if (drops_after != drops_before) complete = false;

    na_ev_inventory_complete done{};
    done.hdr.size_bytes = sizeof(done);
    done.hdr.kind = NA_EV_INVENTORY_COMPLETE;
    done.hdr.flags = NA_EVF_FOUNDATIONAL;
    done.hdr.producer_thread = GetCurrentThreadId();
    done.epoch = begin.epoch;
    done.complete = complete ? 1u : 0u;

    if (complete) {
        status_lock();
        g_host.pending_valid_drops.store(drops_after, std::memory_order_release);
        g_host.pending_valid_epoch.store(begin.epoch, std::memory_order_release);
        status_unlock();
    }
    if (!emit_control(&done)) complete = false;
    if (!complete) {
        status_lock();
        g_host.pending_valid_epoch.store(0, std::memory_order_release);
        g_host.inventory_invalid.store(1, std::memory_order_release);
        status_unlock();
    }
    g_host.cv.notify_one();
    return complete;
}

} // namespace

namespace na::observe_internal {

bool Emit(const void* event_bytes, uint32_t group) { return emit_ring(event_bytes, group); }
void CurrentScope(na_session_id* session, na_epoch* epoch)
{
    const bool on = Started();
    if (session) *session = on ? g_host.session.load(std::memory_order_acquire) : 0;
    if (epoch) *epoch = on ? g_host.epoch.load(std::memory_order_acquire) : 0;
}
bool Started() { return g_host.started.load(std::memory_order_acquire) && !g_host.stopping.load(std::memory_order_acquire); }
void SetSnapshotProvider(SnapshotFn fn) { g_host.snapshot = std::move(fn); }
void TestForceFoundationalOverflow(bool enable)
{
    g_host.force_foundational_overflow.store(enable, std::memory_order_release);
}

} // namespace na::observe_internal

extern "C" {

int __cdecl na_observe_abi(uint16_t* out_major, uint16_t* out_minor)
{
    if (!out_major || !out_minor) return NA_E_ARG;
    *out_major = NA_OBSERVE_ABI_MAJOR;
    *out_minor = NA_OBSERVE_ABI_MINOR;
    return NA_OK;
}

int __cdecl na_observe_start(const na_observe_config* cfg, na_event_sink sink, void* user)
{
    if (!sink) return NA_E_ARG;
    uint32_t budget_mib = kDefaultBudgetMiB;
    if (cfg) {
        if (cfg->size_bytes < 8 || cfg->size_bytes > 4096) return NA_E_ABI;
        for (uint32_t r : cfg->_reserved)
            if (cfg->size_bytes >= sizeof(na_observe_config) && r != 0) return NA_E_ARG;
        if (cfg->metadata_budget_mib) budget_mib = cfg->metadata_budget_mib;
    }
    {
        std::lock_guard<std::mutex> lock(g_host.mutex);
        if (g_host.started.load()) return NA_E_STATE;
        // R2: a previous session's producers are quiesced by stop(); rings
        // are only reallocated here, after that.
        g_host.sink = sink;
        g_host.user = user;
        size_t budget = static_cast<size_t>(budget_mib) * 1024 * 1024;
        // Budget floor: every ring must hold one maximum event; the effective
        // minimum total is kRings * kMinRingBytes (4 MiB), documented.
        g_host.ring_bytes = std::max(kMinRingBytes, budget / kRings);
        for (Ring& r : g_host.rings) {
            r.owner.store(0);
            r.head.store(0);
            r.tail.store(0);
            r.buf.assign(g_host.ring_bytes, 0);
        }
        g_host.pending_bytes.store(0);
        g_host.queue.clear();
        {
            std::lock_guard<std::mutex> cl(g_host.control_m);
            g_host.control.clear();
        }
        g_host.control_count.store(0);
        std::random_device rd;
        status_lock();
        g_host.session.store((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ __rdtsc(),
                             std::memory_order_release); // T3: inside the coherent write
        g_host.epoch.store(1, std::memory_order_release);
        g_host.inventory_invalid.store(1, std::memory_order_release);
        g_host.foundational_dropped.store(0, std::memory_order_relaxed);
        g_host.detail_dropped.store(0, std::memory_order_relaxed);
        g_host.batches_delivered.store(0, std::memory_order_relaxed);
        for (auto& g : g_host.group_dropped) g.store(0, std::memory_order_relaxed);
        g_host.pending_valid_epoch.store(0);
        status_unlock();
        g_host.stopping.store(false, std::memory_order_release);
        g_host.worker = std::thread(&worker_main);
        g_host.generation.fetch_add(1, std::memory_order_acq_rel); // T2: new lifecycle
        g_host.started.store(true, std::memory_order_release); // last: admission opens
    }
    run_snapshot();
    return NA_OK;
}

int __cdecl na_observe_stop(void)
{
    if (GetCurrentThreadId() == g_host.worker_tid) return NA_E_STATE;
    std::thread joiner;
    {
        std::lock_guard<std::mutex> lock(g_host.mutex);
        if (!g_host.started.load()) return NA_E_STATE;
        g_host.stopping.store(true, std::memory_order_release); // closes admission
        g_host.cv.notify_one();
        joiner = std::move(g_host.worker);
    }
    joiner.join(); // worker quiesced producers before its final gather
    std::lock_guard<std::mutex> lock(g_host.mutex);
    g_host.started.store(false, std::memory_order_release);
    g_host.generation.fetch_add(1, std::memory_order_acq_rel); // T2: closes this lifecycle
    g_host.sink = nullptr;
    g_host.worker_tid = 0;
    return NA_OK;
}

int __cdecl na_observe_query_status(na_observe_status* inout)
{
    if (!inout) return NA_E_ARG;
    uint32_t capacity = inout->size_bytes;
    if (capacity < 16) return NA_E_ARG;
    na_observe_status full{};
    for (;;) {
        uint64_t s1 = g_host.status_seq.load(std::memory_order_seq_cst);
        if ((s1 & 1) || g_host.drops_in_progress.load(std::memory_order_seq_cst) != 0) {
            YieldProcessor();
            continue;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        full.abi_major = NA_OBSERVE_ABI_MAJOR;
        full.abi_minor = NA_OBSERVE_ABI_MINOR;
        full.session = g_host.session.load(std::memory_order_acquire);
        full.epoch = g_host.epoch.load(std::memory_order_acquire);
        full.inventory_invalid = g_host.inventory_invalid.load(std::memory_order_acquire);
        full.foundational_dropped = g_host.foundational_dropped.load(std::memory_order_relaxed);
        full.detail_dropped = g_host.detail_dropped.load(std::memory_order_relaxed);
        full.batches_delivered = g_host.batches_delivered.load(std::memory_order_relaxed);
        for (int i = 0; i < 16; ++i)
            full.per_group_dropped[i] = g_host.group_dropped[i].load(std::memory_order_relaxed);
        // Closing check ORDER matters: a producer bumps the sequence and THEN
        // clears in_progress. Reading in_progress first means that if we see
        // it clear, the sequence read that follows is ordered after the
        // producer's bump (seq_cst total order) and must observe it. Reading
        // the sequence first could observe a stale value alongside an already
        // cleared counter â€” a torn snapshot (rev2-T4).
        std::atomic_thread_fence(std::memory_order_seq_cst);
        bool quiet = g_host.drops_in_progress.load(std::memory_order_seq_cst) == 0;
        uint64_t s2 = g_host.status_seq.load(std::memory_order_seq_cst);
        if (quiet && s1 == s2) break;
    }
    uint32_t written = capacity < sizeof(full) ? capacity : sizeof(full);
    full.size_bytes = written;
    std::memcpy(inout, &full, written);
    return NA_OK;
}

int __cdecl na_observe_resync(void)
{
    if (GetCurrentThreadId() == g_host.worker_tid) return NA_E_STATE;
    {
        std::lock_guard<std::mutex> lock(g_host.mutex);
        if (!g_host.started.load() || g_host.stopping.load()) return NA_E_STATE;
        status_lock();
        g_host.epoch.fetch_add(1, std::memory_order_acq_rel);
        g_host.inventory_invalid.store(1, std::memory_order_release);
        g_host.pending_valid_epoch.store(0, std::memory_order_release);
        status_unlock();
    }
    run_snapshot();
    return NA_OK;
}

} // extern "C"
