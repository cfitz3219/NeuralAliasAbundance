// reconstructed 2026-10 — upload validity, lifecycle, ranges and concurrency.
#include "test_support.hpp"
#include "core/upload_cache.hpp"
#include <array>
#include <atomic>
#include <thread>
using dx11_enhancement::UploadCache;
int main() { return test::run({
    {"uninitialized bytes and partial uploads", [] {
        UploadCache c; c.create(1, 8); std::array<unsigned char, 8> out{};
        CHECK(!c.read(1, 0, out.data(), out.size())); const unsigned char in[]{3,4,5};
        c.update(1, 2, 3, in); CHECK(c.read(1, 2, out.data(), 3)); CHECK(out[0] == 3 && out[2] == 5);
        CHECK(!c.read(1, 1, out.data(), 4)); CHECK(!c.read(1, UINT64_MAX, out.data(), 1));
    }},
    {"copy before unmap and discard invalidation", [] {
        unsigned char data[]{1,2,3,4}, out[4]{}; UploadCache c; c.create(1, 4, data);
        c.map(1, 1, 2, data + 1, false); data[1] = 9;
        CHECK(!c.read(1, 0, out, 4)); c.unmap(1); data[1] = 0;
        CHECK(c.read(1, 0, out, 4)); CHECK(out[0] == 1 && out[1] == 9 && out[3] == 4);
        c.map(1, 1, 2, data + 1, true); c.unmap(1);
        CHECK(!c.read(1, 0, out, 4)); CHECK(c.read(1, 1, out, 2));
    }},
    {"bad writes invalidate, whole-tail sentinel and identity reuse", [] {
        unsigned char data[]{8,7,6,5}, out[4]{}; UploadCache c; c.create(1, 4, data);
        c.update(1, 4, 1, data); CHECK(!c.read(1, 0, out, 1));
        c.update(1, 0, UINT64_MAX, data); CHECK(c.read(1, 0, out, 4));
        c.update(1, UINT64_MAX, 1, data); CHECK(!c.read(1, 0, out, 1));
        c.create(1, 4, data); c.invalidate(1); CHECK(!c.read(1, 0, out, 1));
        c.erase(1); CHECK(!c.contains(1)); c.create(1, 4); CHECK(!c.read(1, 0, out, 1));
        c.create(0, 4); c.create(2, 65537); CHECK(!c.contains(0) && !c.contains(2));
        CHECK(!c.read(1, 0, nullptr, 1));
    }},
    {"same-entry reads never contain a torn upload", [] {
        std::array<unsigned char, 64> first{}, second{}, out{};
        first.fill(0x11); second.fill(0x22);
        UploadCache c; c.create(1, first.size(), first.data());
        std::atomic<bool> started{false}, stop{false};
        std::thread writer([&] {
            unsigned iteration = 0;
            do {
                const auto& pattern = (iteration++ % 2 == 0) ? second : first;
                c.update(1, 0, pattern.size(), pattern.data());
                started.store(true);
                std::this_thread::yield();
            } while (!stop.load());
        });
        while (!started.load()) std::this_thread::yield();
        unsigned rejected = 0, mixed = 0;
        for (unsigned i = 0; i < 20000; ++i) {
            if (!c.read(1, 0, out.data(), out.size())) ++rejected;
            else if (out != first && out != second) ++mixed;
            if (i % 64 == 0) std::this_thread::yield();
        }
        stop.store(true); writer.join();
        // Check after joining so a failed assertion cannot strand a live thread.
        CHECK(rejected == 0); CHECK(mixed == 0);
    }},
    {"bounded entries and isolated concurrent writers", [] {
        UploadCache c; for (uint64_t id = 1; id <= 2049; ++id) c.create(id, 4);
        CHECK(c.contains(2048) && !c.contains(2049)); c.erase(1); c.create(2049, 4); CHECK(c.contains(2049));
        std::thread a([&] { uint32_t v = 42; for (int i=0; i<1000; ++i) c.update(2, 0, 4, &v); });
        std::thread b([&] { uint32_t v = 73; for (int i=0; i<1000; ++i) c.update(3, 0, 4, &v); });
        a.join(); b.join(); uint32_t out = 0;
        CHECK(c.read(2, 0, &out, 4) && out == 42); CHECK(c.read(3, 0, &out, 4) && out == 73);
    }},
}); }
