#include "test_support.hpp"
#include "providers/discovery/capture_validity.hpp"
using namespace na::discovery;
namespace {
na_observe_status Status(uint64_t epoch=1) {
    na_observe_status s{}; s.size_bytes=sizeof(s); s.abi_major=NA_OBSERVE_ABI_MAJOR;
    s.session=42; s.epoch=epoch; return s;
}
struct Batch {
    na_event_batch header{}; std::vector<unsigned char> bytes;
    explicit Batch(uint64_t epoch=1) {
        header.header_bytes=sizeof(header); header.abi_major=NA_OBSERVE_ABI_MAJOR;
        header.session=42; header.epoch=epoch; bytes.resize(sizeof(header));
    }
    template<class T> void Add(T e,uint32_t kind) {
        e.hdr.size_bytes=sizeof(T); e.hdr.kind=kind; e.hdr.epoch=header.epoch;
        auto start=bytes.size(); bytes.resize(start+sizeof(e)); std::memcpy(bytes.data()+start,&e,sizeof(e));
        ++header.event_count; header.payload_bytes+=sizeof(e);
    }
    void Send(CaptureValidity& v,na_observe_status s) {
        std::memcpy(bytes.data(),&header,sizeof(header)); v.Consume(bytes.data(),bytes.size(),s);
    }
};
Batch Snapshot(uint64_t epoch=1,uint32_t support=NA_CAP_SUPPORTED) {
    Batch b(epoch); na_ev_epoch_begin begin{}; begin.session=42; begin.epoch=epoch;
    b.Add(begin,NA_EV_EPOCH_BEGIN);
    struct Caps { na_ev_capability_report report; na_cap_entry cap; } caps{};
    caps.report.session=42; caps.report.entry_count=1; caps.report.entries_offset=sizeof(caps.report);
    caps.report.entry_stride=sizeof(caps.cap); caps.cap.group=NA_GROUP_RESOURCE; caps.cap.support=support;
    // Inline packet has header nested in report.
    caps.report.hdr.size_bytes=sizeof(caps); caps.report.hdr.kind=NA_EV_CAPABILITY_REPORT; caps.report.hdr.epoch=epoch;
    auto start=b.bytes.size(); b.bytes.resize(start+sizeof(caps)); std::memcpy(b.bytes.data()+start,&caps,sizeof(caps));
    ++b.header.event_count; b.header.payload_bytes+=sizeof(caps);
    na_ev_inventory_complete end{}; end.epoch=epoch; end.complete=1; b.Add(end,NA_EV_INVENTORY_COMPLETE);
    return b;
}
}
int main() { return test::run({
    {"callback completion and capability scope are mandatory", [] {
        CaptureValidity v; auto s=Status(); s.inventory_invalid=1; Snapshot().Send(v,s);
        CHECK(!v.InventoryConsistent()); s.inventory_invalid=0; v.ObserveStatus(s);
        CHECK(v.CaptureContiguous() && v.Supports(NA_GROUP_RESOURCE)); CHECK(!v.Supports(NA_GROUP_PASS));
        CaptureValidity partial; Snapshot(1,NA_CAP_PARTIAL).Send(partial,s);
        CHECK(partial.InventoryConsistent() && !partial.Supports(NA_GROUP_RESOURCE));
    }},
    {"authoritative unmarked loss sticks until a new epoch", [] {
        CaptureValidity v; auto s=Status(); Snapshot().Send(v,s); CHECK(v.InventoryConsistent());
        ++s.foundational_dropped; v.ObserveStatus(s); CHECK(!v.InventoryConsistent());
        Snapshot().Send(v,s); CHECK(!v.InventoryConsistent());
        s.epoch=2; Snapshot(2).Send(v,s); CHECK(v.InventoryConsistent());
    }},
    {"detail loss revokes history without pretending inventory loss", [] {
        CaptureValidity v; auto s=Status(); Snapshot().Send(v,s); ++s.detail_dropped;
        v.ObserveStatus(s); CHECK(v.InventoryConsistent() && !v.CaptureContiguous());
        v.ObserveStatus(s); CHECK(!v.CaptureContiguous());
    }},
    {"old buffered epoch cannot restore current inventory", [] {
        CaptureValidity v; auto s=Status(2); Snapshot(1).Send(v,s);
        CHECK(v.Epoch()==2 && !v.InventoryConsistent()); Snapshot(2).Send(v,s); CHECK(v.InventoryConsistent());
        Snapshot(1).Send(v,s); CHECK(v.InventoryConsistent());
        v.ObserveStatus(Status(1)); CHECK(!v.InventoryConsistent() && v.Epoch()==2);
    }},
    {"unknown foundational malformed and final batches revoke", [] {
        for (int mode=0;mode<3;++mode) {
            CaptureValidity v; auto s=Status(); Snapshot().Send(v,s); Batch b;
            if(mode==0) { na_ev_loss_mark e{}; e.hdr.flags=NA_EVF_FOUNDATIONAL; b.Add(e,9000); }
            if(mode==1) b.header.event_count=1;
            if(mode==2) b.header.batch_flags=NA_BATCHF_FINAL;
            b.Send(v,s); CHECK(!v.InventoryConsistent());
        }
    }},
    {"retroactive ambiguity invalidates retained execution evidence", [] {
        CaptureValidity v; auto s=Status(); Snapshot().Send(v,s); Batch b;
        na_ev_order_ambiguity_range e{}; e.queue=9; e.first_ticket=1; e.last_ticket=2;
        b.Add(e,NA_EV_ORDER_AMBIGUITY_RANGE); b.Send(v,s);
        CHECK(v.InventoryConsistent() && !v.CaptureContiguous());
    }},
    {"counter rollback and incomplete status cannot manufacture recovery", [] {
        CaptureValidity v; auto s=Status(); s.detail_dropped=2; Snapshot().Send(v,s); CHECK(!v.CaptureContiguous());
        s.detail_dropped=1; v.ObserveStatus(s); CHECK(!v.CaptureContiguous());
        s.size_bytes=16; v.ObserveStatus(s); CHECK(!v.InventoryConsistent());
        s.size_bytes=sizeof(s); v.ObserveStatus(s); CHECK(!v.InventoryConsistent());
    }},
    {"loss racing an epoch change cannot hide in the new baseline", [] {
        CaptureValidity v; auto s=Status(); Snapshot().Send(v,s); CHECK(v.CaptureContiguous());
        s.epoch=2; ++s.detail_dropped; Snapshot(2).Send(v,s); CHECK(!v.CaptureContiguous());
        s.epoch=3; Snapshot(3).Send(v,s); CHECK(v.CaptureContiguous());
    }},
    {"bundle and failed recording evidence cannot stay contiguous", [] {
        for (int mode=0; mode<3; ++mode) {
            CaptureValidity v; auto s=Status(); Snapshot().Send(v,s); CHECK(v.CaptureContiguous());
            Batch b;
            if (mode==0) {
                na_ev_queue_execute e{}; e.queue=9; e.queue_ticket=1; e.contains_bundles=1;
                b.Add(e,NA_EV_QUEUE_EXECUTE);
            } else {
                na_ev_list_reset_close e{}; e.list=8; e.recording_generation=1; e.hresult=-1;
                b.Add(e,mode==1 ? NA_EV_LIST_RESET : NA_EV_LIST_CLOSE);
            }
            b.Send(v,s); CHECK(v.InventoryConsistent() && !v.CaptureContiguous());
        }
    }},
#ifdef NA_SNAP_SYNC_APP_UNKNOWN
    {"provisional H3 cannot be qualified by a later clean execute", [] {
        for (bool reverse : {false,true}) {
            CaptureValidity v; auto s=Status(); Snapshot().Send(v,s); CHECK(v.CaptureContiguous());
            Batch snapshot,execute;
            na_ev_snapshot e{}; e.queue=9; e.queue_ticket=1; e.list=8;
            e.recording_generation=1; e.status=NA_SNAP_NOT_MAPPED; e.sync=NA_SNAP_SYNC_APP_UNKNOWN;
            snapshot.Add(e,NA_EV_SNAPSHOT);
            na_ev_queue_execute q{}; q.queue=9; q.queue_ticket=1;
            execute.Add(q,NA_EV_QUEUE_EXECUTE);
            if (reverse) { execute.Send(v,s); snapshot.Send(v,s); }
            else { snapshot.Send(v,s); execute.Send(v,s); }
            CHECK(v.InventoryConsistent() && !v.CaptureContiguous());
            v.ObserveStatus(s); CHECK(!v.CaptureContiguous());
            s.epoch=2; Snapshot(2).Send(v,s); CHECK(v.CaptureContiguous());
            snapshot.Send(v,s); CHECK(v.CaptureContiguous()); // stale snapshot cannot poison new epoch
        }
    }},
#endif
    {"new session discards old readiness", [] {
        CaptureValidity v; auto s=Status(); Snapshot().Send(v,s); CHECK(v.InventoryConsistent());
        s.session=43; v.ObserveStatus(s); CHECK(!v.InventoryConsistent() && v.Session()==43);
    }},
}); }
