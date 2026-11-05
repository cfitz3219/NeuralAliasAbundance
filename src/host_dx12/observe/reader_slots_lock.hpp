#pragma once
// A reader-writer lock whose shared side does not write a shared cache line.
//
// Why (SE5 trace of f207b0a, 2026-11-05): about 30 game threads take the producers' registry
// lock g_m SHARED on every descriptor copy. Exclusive holders were almost absent (11 ms/s
// contended in the whole process), yet the shared acquire/release spun for ~3.7 s of CPU per
// second: every shared acquire of an SRW lock is an atomic write to the same lock word, and
// that word bounced between cores.
//
// Here each reader increments a counter on its own cache line (threads are spread over
// kSlots slots), then checks the writer flag. A writer takes `gate` exclusively (writers stay
// serialised), raises the flag, and waits until every slot is zero (spin, then park until
// the reader that empties the slot wakes it). Seq_cst on both sides
// (reader: increment then load flag; writer: store flag then load slots) means either the
// reader sees the flag and backs off, or the writer sees the reader's count and waits.
// A reader that backs off parks on `gate` (shared) until the writer is done, then retries.
//
// Same contract as std::shared_mutex for the way g_m is used: no recursion, no upgrade, no
// try_lock. Constant-initialised and trivially destructible apart from the std::shared_mutex
// (the same type g_m was), so it stays usable during process exit exactly as before.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace na::observe {

class ReaderSlotsLock {
public:
    static constexpr uint32_t kSlots = 64;

    void lock_shared()
    {
        std::atomic<uint32_t>& c = slots_[my_slot()].count;
        for (;;) {
            c.fetch_add(1, std::memory_order_seq_cst);
            if (!writer_.load(std::memory_order_seq_cst)) return;
            if (c.fetch_sub(1, std::memory_order_seq_cst) == 1) wake_writer();
            gate_.lock_shared(); // blocks while a writer holds the gate
            gate_.unlock_shared();
        }
    }
    void unlock_shared()
    {
        // Seq_cst decrement then flag load, against the writer's flag store then count load:
        // either this reader sees the waiting writer and wakes it, or the writer sees zero.
        if (slots_[my_slot()].count.fetch_sub(1, std::memory_order_seq_cst) == 1 && writer_.load(std::memory_order_seq_cst))
            wake_writer();
    }

    void lock()
    {
        gate_.lock();
        writer_.store(true, std::memory_order_seq_cst);
        for (uint32_t i = 0; i < kSlots; ++i) {
            std::atomic<uint32_t>& c = slots_[i].count;
            for (uint32_t spins = 0; spins < 128 && c.load(std::memory_order_seq_cst) != 0; ++spins) {}
            if (c.load(std::memory_order_seq_cst) == 0) continue;
            // A reader still inside after the spin was most likely preempted there: park (as
            // an SRW writer does), so its core is free for it to run and leave. The reader
            // that drains the slot wakes us; it decrements before taking the waiter mutex,
            // so checking the count under that mutex cannot miss the wake-up. The timeout is
            // only a safety net.
            Waiter& w = waiter();
            std::unique_lock<std::mutex> g(w.m);
            while (c.load(std::memory_order_seq_cst) != 0) w.cv.wait_for(g, std::chrono::milliseconds(1));
        }
    }
    void unlock()
    {
        writer_.store(false, std::memory_order_release);
        gate_.unlock();
    }

    // Diagnostics/tests: readers currently inside (racy snapshot).
    uint32_t readers() const
    {
        uint32_t n = 0;
        for (const auto& s : slots_) n += s.count.load(std::memory_order_relaxed);
        return n;
    }

private:
    // 64 bytes per slot: whatever the array's start, each counter sits on its own cache line
    // (explicit padding rather than alignas, which MSVC /WX reports as C4324).
    struct Slot {
        std::atomic<uint32_t> count{0};
        char pad[64 - sizeof(std::atomic<uint32_t>)]{};
    };
    static_assert(sizeof(Slot) == 64, "one cache line per reader slot");
    static uint32_t my_slot()
    {
        // Threads are numbered on first use; the slot is fixed for the thread's lifetime, so
        // unlock_shared always decrements the slot lock_shared incremented.
        static std::atomic<uint32_t> next{0};
        thread_local uint32_t slot = next.fetch_add(1, std::memory_order_relaxed) % kSlots;
        return slot;
    }

    struct Waiter {
        std::mutex m;
        std::condition_variable cv;
    };
    static Waiter& waiter()
    {
        static Waiter& w = *new Waiter(); // never destroyed: g_m is used during process exit
        return w;
    }
    static void wake_writer()
    {
        Waiter& w = waiter();
        { std::lock_guard<std::mutex> g(w.m); }
        w.cv.notify_all();
    }

    Slot slots_[kSlots];
    std::atomic<bool> writer_{false};
    std::shared_mutex gate_;
};

} // namespace na::observe
