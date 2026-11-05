// The registry lock (g_m) of the D3D12 producers: readers on per-thread slots, writers
// exclusive. Readers must never see a half-done write, writers must never overlap readers or
// each other, and a released lock must have no reader left counted.
#include "test_support.hpp"
#include "host_dx12/observe/reader_slots_lock.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <vector>

using na::observe::ReaderSlotsLock;

namespace {
// Writer wait and reader throughput against std::shared_mutex (SRW on Windows, the lock g_m
// was), with readers that spend most of their time outside the lock like the hooks do.
// Printed for comparison on real machines; only progress is checked (CI timing is noisy).
template <class L>
void writer_latency(const char* name, int readers, bool must_progress)
{
    static L m;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::vector<double> waits;
    std::vector<std::thread> ts;
    for (int t = 0; t < readers; ++t)
        ts.emplace_back([&] {
            uint64_t n = 0;
            volatile uint32_t sink = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                { std::shared_lock<L> r(m); sink = sink + 1; ++n; }
                for (int i = 0; i < 400; ++i) sink = sink + static_cast<uint32_t>(i); // hook work outside g_m
            }
            reads.fetch_add(n);
        });
    std::thread w([&] {
        while (!stop.load()) {
            const auto t0 = std::chrono::steady_clock::now();
            { std::unique_lock<L> x(m); }
            waits.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    stop.store(true);
    for (auto& t : ts) t.join();
    w.join();
    std::sort(waits.begin(), waits.end());
    const auto q = [&](double f) { return waits.empty() ? 0.0 : waits[std::min(waits.size() - 1, static_cast<size_t>(f * waits.size()))]; };
    std::printf("%-13s readers %2d: reads %.1fM, writer wait p50 %.1f us p99 %.1f us max %.1f us (%zu writes)\n", name, readers,
                reads.load() / 1e6, q(0.5), q(0.99), waits.empty() ? 0.0 : waits.back(), waits.size());
    if (must_progress) CHECK(!waits.empty()); // the SRW baseline may starve its writer (CI: 0 writes at 30 readers)
}
} // namespace

int main() {
    return test::run({
        {"single thread: shared, exclusive, shared again", [] {
            static ReaderSlotsLock m;
            { std::shared_lock<ReaderSlotsLock> r(m); CHECK(m.readers() == 1); }
            CHECK(m.readers() == 0);
            { std::unique_lock<ReaderSlotsLock> w(m); CHECK(m.readers() == 0); }
            { std::shared_lock<ReaderSlotsLock> r(m, std::defer_lock); r.lock(); CHECK(m.readers() == 1); r.unlock(); }
            CHECK(m.readers() == 0);
        }},
        {"readers never see a torn write; writers exclude readers and each other", [] {
            static ReaderSlotsLock m;
            static uint64_t a = 0, b = 0; // written only under the exclusive lock, never atomic
            std::atomic<bool> stop{false};
            std::atomic<uint64_t> torn{0}, reads{0}, overlap{0}, writes{0};
            std::atomic<int> inside_writers{0}, inside_readers{0};
            std::vector<std::thread> ts;
            for (int t = 0; t < 70; ++t) // more threads than slots: shared slots must still count right
                ts.emplace_back([&] {
                    while (!stop.load()) {
                        std::shared_lock<ReaderSlotsLock> r(m);
                        inside_readers.fetch_add(1);
                        if (inside_writers.load()) overlap.fetch_add(1);
                        const uint64_t x = a, y = b;
                        if (x != y) torn.fetch_add(1);
                        reads.fetch_add(1);
                        inside_readers.fetch_sub(1);
                        r.unlock();
                        // Real readers (descriptor hooks) spend most of their time outside g_m;
                        // a 100% in-lock loop on an oversubscribed CI runner only measures how
                        // long the scheduler takes to run every preempted reader once.
                        std::this_thread::yield();
                    }
                });
            for (int t = 0; t < 3; ++t)
                ts.emplace_back([&] {
                    while (!stop.load()) {
                        std::unique_lock<ReaderSlotsLock> w(m);
                        if (inside_writers.fetch_add(1) != 0 || inside_readers.load() != 0) overlap.fetch_add(1);
                        ++a;
                        std::this_thread::yield();
                        ++b;
                        writes.fetch_add(1);
                        inside_writers.fetch_sub(1);
                        w.unlock();
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                });
            const auto t0 = std::chrono::steady_clock::now();
            while (writes.load() < 2000 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stop.store(true);
            for (auto& t : ts) t.join();
            std::printf("reads %llu writes %llu torn %llu overlap %llu\n", (unsigned long long)reads.load(),
                        (unsigned long long)writes.load(), (unsigned long long)torn.load(), (unsigned long long)overlap.load());
            CHECK(torn.load() == 0);
            CHECK(overlap.load() == 0);
            CHECK(reads.load() > 1000);
            CHECK(writes.load() >= 200); // writers are not starved by 70 readers (CI: 2 cores, 73 threads)
            CHECK(a == b && a == writes.load());
            CHECK(m.readers() == 0);
        }},
        {"writer wait vs std::shared_mutex (printed)", [] {
            const int hw = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
            for (int r : {hw - 1, hw, 8, 30}) {
                writer_latency<std::shared_mutex>("shared_mutex", r, false);
                writer_latency<ReaderSlotsLock>("reader slots", r, true);
            }
        }},
    });
}
