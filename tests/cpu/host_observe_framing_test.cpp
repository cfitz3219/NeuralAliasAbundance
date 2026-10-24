// reconstructed 2026-10 — exercises the REAL host transport on Windows; no GPU.
#include "test_support.hpp"
#include "providers/discovery/capture_decoder.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/host_observe_internal.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
using namespace na::observe_internal;
namespace {
na_observe_status status() {
    na_observe_status s{}; s.size_bytes=sizeof(s); CHECK(na_observe_query_status(&s) == NA_OK); return s;
}
template<class F> void eventually(F condition) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!condition()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
struct Capture {
    std::mutex mutex;
    std::condition_variable cv;
    bool park_complete=false, entered=false, release=false;
    std::string error;
    unsigned calls=0, final=0;
    std::vector<na_event_header> events;
    static void __cdecl sink(const na_event_batch* b, void* user) {
        auto& c=*static_cast<Capture*>(user);
        std::unique_lock<std::mutex> lock(c.mutex);
        ++c.calls;
        try {
            CHECK(b->header_bytes >= sizeof(*b)); CHECK(b->abi_major == 0 && b->abi_minor == NA_OBSERVE_ABI_MINOR);
            CHECK(b->session != 0); CHECK(b->payload_bytes % 8 == 0);
            // Allocation extent is guaranteed by the real host callback contract.
            const auto decoded=na::discovery::DecodeBatch(b,static_cast<size_t>(b->header_bytes)+b->payload_bytes);
            CHECK(decoded.valid && decoded.events.size() == b->event_count);
            const auto s=status(); CHECK(s.session == b->session); // status is sink-safe
            const auto* payload=reinterpret_cast<const unsigned char*>(b)+b->header_bytes;
            size_t offset=0; uint32_t count=0; bool complete=false;
            while (offset < b->payload_bytes) {
                CHECK(b->payload_bytes-offset >= sizeof(na_event_header));
                na_event_header e{}; std::memcpy(&e,payload+offset,sizeof(e));
                CHECK(e.size_bytes >= sizeof(e) && e.size_bytes % 8 == 0);
                CHECK(e.size_bytes <= b->payload_bytes-offset); CHECK(e.epoch == b->epoch);
                if (e.kind == NA_EV_INVENTORY_COMPLETE) {
                    CHECK(e.size_bytes >= sizeof(na_ev_inventory_complete));
                    na_ev_inventory_complete value{}; std::memcpy(&value,payload+offset,sizeof(value));
                    CHECK(value.epoch == e.epoch); complete=value.complete == 1;
                }
                c.events.push_back(e); offset+=e.size_bytes; ++count;
            }
            CHECK(offset == b->payload_bytes && count == b->event_count);
            if (b->batch_flags & NA_BATCHF_FINAL) ++c.final;
            if (complete && c.park_complete && !c.release) {
                c.entered=true; c.cv.notify_all();
                c.cv.wait(lock,[&] { return c.release; });
            }
        } catch (const std::exception& e) { c.error=e.what(); }
        c.cv.notify_all();
    }
    void unblock() { std::lock_guard<std::mutex> lock(mutex); release=true; cv.notify_all(); }
    bool saw(uint32_t kind) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& e:events) if (e.kind == kind) return true;
        return false;
    }
    void check() { std::lock_guard<std::mutex> lock(mutex); CHECK(error.empty()); }
};
struct Session {
    Capture capture;
    bool running=false;
    Session() { SetSnapshotProvider({}); TestForceFoundationalOverflow(false); }
    void start() {
        na_observe_config cfg{}; cfg.size_bytes=sizeof(cfg); cfg.metadata_budget_mib=4;
        CHECK(na_observe_start(&cfg,Capture::sink,&capture) == NA_OK); running=true;
    }
    void stop() { capture.unblock(); CHECK(na_observe_stop() == NA_OK); running=false; capture.check(); }
    ~Session() {
        capture.unblock(); if (running) na_observe_stop();
        TestForceFoundationalOverflow(false); SetSnapshotProvider({});
    }
};
na_ev_object_retired event(bool foundational=true) {
    na_ev_object_retired e{}; e.hdr.size_bytes=sizeof(e); e.hdr.kind=NA_EV_OBJECT_RETIRED;
    e.hdr.flags=foundational ? NA_EVF_FOUNDATIONAL : 0;
    e.object=42; e.object_kind=NA_OBJ_RESOURCE; return e;
}
}
int main() { return test::run({
    {"lifecycle, exact batch framing, final delivery and no post-stop callbacks", [] {
        Session s; CHECK(na_observe_stop() == NA_E_STATE); CHECK(na_observe_resync() == NA_E_STATE);
        CHECK(na_observe_start(nullptr,nullptr,nullptr) == NA_E_ARG);
        uint16_t major=99, minor=99; CHECK(na_observe_abi(&major,&minor) == NA_OK && major == 0 && minor == NA_OBSERVE_ABI_MINOR);
        CHECK(na_observe_abi(nullptr,&minor) == NA_E_ARG);
        s.start(); CHECK(na_observe_start(nullptr,Capture::sink,&s.capture) == NA_E_STATE);
        eventually([] { return status().inventory_invalid == 0; });
        auto e=event(); CHECK(Emit(&e,NA_GROUP_RESOURCE));
        eventually([&] { return s.capture.saw(NA_EV_OBJECT_RETIRED); });
        s.stop(); CHECK(s.capture.final == 1); auto calls=s.capture.calls;
        CHECK(!Emit(&e,NA_GROUP_RESOURCE)); CHECK(na_observe_resync() == NA_E_STATE);
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); CHECK(s.capture.calls == calls);
    }},
    {"inventory validity waits for callback completion", [] {
        Session s; s.capture.park_complete=true; s.start();
        {
            std::unique_lock<std::mutex> lock(s.capture.mutex);
            CHECK(s.capture.cv.wait_for(lock,std::chrono::seconds(5),[&] { return s.capture.entered || !s.capture.error.empty(); }));
            CHECK(s.capture.error.empty());
        }
        CHECK(status().inventory_invalid == 1); s.capture.unblock();
        eventually([] { return status().inventory_invalid == 0; }); s.stop();
    }},
    {"foundational loss sticks until a new successful epoch", [] {
        Session s; s.start(); eventually([] { return status().inventory_invalid == 0; }); auto before=status();
        TestForceFoundationalOverflow(true); auto e=event(); CHECK(!Emit(&e,NA_GROUP_RESOURCE));
        auto lost=status(); CHECK(lost.inventory_invalid == 1 && lost.foundational_dropped == before.foundational_dropped+1);
        CHECK(lost.per_group_dropped[NA_GROUP_RESOURCE] == 1);
        TestForceFoundationalOverflow(false); CHECK(Emit(&e,NA_GROUP_RESOURCE));
        eventually([&] { return s.capture.saw(NA_EV_LOSS_MARK); }); CHECK(status().inventory_invalid == 1);
        CHECK(na_observe_resync() == NA_OK);
        eventually([&] { auto now=status(); return now.epoch == before.epoch+1 && now.inventory_invalid == 0; }); s.stop();
    }},
    {"failed snapshot and lost boundary never declare complete inventory", [] {
        Session s; SetSnapshotProvider([] { return false; }); s.start();
        eventually([&] { return s.capture.saw(NA_EV_INVENTORY_COMPLETE); }); CHECK(status().inventory_invalid == 1);
        // Provider changes occur on this lifecycle thread after the previous synchronous snapshot returned.
        SetSnapshotProvider([] { return true; }); TestForceFoundationalOverflow(true);
        CHECK(na_observe_resync() == NA_OK); CHECK(status().inventory_invalid == 1);
        CHECK(status().foundational_dropped >= 1); TestForceFoundationalOverflow(false);
        CHECK(na_observe_resync() == NA_OK); eventually([] { return status().inventory_invalid == 0; }); s.stop();
    }},
    {"malformed detail and foundational events count loss without hanging drain", [] {
        Session s; s.start(); eventually([] { return status().inventory_invalid == 0; });
        auto e=event(false);
        for (uint32_t size : {0u,31u,33u,65544u}) { e.hdr.size_bytes=size; CHECK(!Emit(&e,NA_GROUP_PASS)); }
        auto detail=status(); CHECK(detail.detail_dropped == 4 && detail.inventory_invalid == 0);
        e.hdr.flags=NA_EVF_FOUNDATIONAL; CHECK(!Emit(&e,NA_GROUP_RESOURCE));
        CHECK(status().inventory_invalid == 1); s.stop();
    }},
    {"status capacity, restart identity and sink reentrancy", [] {
        // The callback's state must outlive Session even if a CHECK throws.
        struct Probe { std::atomic<int> stop{99},resync{99}; } probe;
        Session s; s.start(); eventually([] { return status().inventory_invalid == 0; });
        const auto first=status().session;
        alignas(na_observe_status) unsigned char storage[sizeof(na_observe_status)+16];
        std::memset(storage,0xA5,sizeof(storage)); uint32_t capacity=16; std::memcpy(storage,&capacity,sizeof(capacity));
        CHECK(na_observe_query_status(reinterpret_cast<na_observe_status*>(storage)) == NA_OK);
        for (size_t i=16;i<sizeof(storage);++i) CHECK(storage[i] == 0xA5);
        capacity=15; std::memcpy(storage,&capacity,sizeof(capacity));
        CHECK(na_observe_query_status(reinterpret_cast<na_observe_status*>(storage)) == NA_E_ARG);
        CHECK(na_observe_query_status(nullptr) == NA_E_ARG); s.stop();
        auto sink=+[](const na_event_batch*,void* user) {
            auto& p=*static_cast<Probe*>(user); p.stop.store(na_observe_stop()); p.resync.store(na_observe_resync());
        };
        CHECK(na_observe_start(nullptr,sink,&probe) == NA_OK); s.running=true;
        eventually([&] { return probe.stop.load() != 99 && probe.resync.load() != 99; });
        CHECK(probe.stop.load() == NA_E_STATE && probe.resync.load() == NA_E_STATE);
        CHECK(status().session != first && status().epoch == 1); s.stop();
    }},
}); }
