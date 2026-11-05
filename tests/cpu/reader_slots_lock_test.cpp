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
#include <vector>

using na::observe::ReaderSlotsLock;

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
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            stop.store(true);
            for (auto& t : ts) t.join();
            std::printf("reads %llu writes %llu torn %llu overlap %llu\n", (unsigned long long)reads.load(),
                        (unsigned long long)writes.load(), (unsigned long long)torn.load(), (unsigned long long)overlap.load());
            CHECK(torn.load() == 0);
            CHECK(overlap.load() == 0);
            CHECK(reads.load() > 1000);
            CHECK(writes.load() > 10); // writers are not starved by 70 readers
            CHECK(a == b && a == writes.load());
            CHECK(m.readers() == 0);
        }},
    });
}
