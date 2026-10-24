// Conservative worker-side validity gate. No resource selection or GPU ordering proof.
#pragma once
#include "capture_decoder.hpp"
#include <array>
namespace na::discovery {
class CaptureValidity {
public:
    // Serialized worker/control-owner calls only. Status must be a fresh coherent
    // query, including after callback completion when querying readiness.
    void ObserveStatus(const na_observe_status& s) {
        if (s.size_bytes < sizeof(s) || s.abi_major != NA_OBSERVE_ABI_MAJOR ||
            !s.session || s.session == NA_UNKNOWN_U64 || !s.epoch || s.epoch == NA_UNKNOWN_U64) {
            foundational_bad_ = true; status_valid_ = false; return;
        }
        if (s.session != session_ || s.epoch > epoch_) {
            const bool same_session = s.session == session_;
            const auto previous = counters_;
            *this = CaptureValidity{};
            // A loss since the previous query may belong to this new epoch.
            // First attachment with historical losses is also conservatively
            // incomplete until a fresh, observed loss-free resync.
            foundational_bad_ = s.foundational_dropped != (same_session ? previous.foundational_dropped : 0);
            detail_bad_ = s.detail_dropped != (same_session ? previous.detail_dropped : 0);
            for (size_t i=0;i<16;++i)
                if (s.per_group_dropped[i] != (same_session ? previous.per_group_dropped[i] : 0)) detail_bad_ = true;
            session_ = s.session; epoch_ = s.epoch;
            counters_ = s;
        } else if (s.epoch < epoch_) {
            // Never roll current evidence back to an older query.
            status_valid_ = false; return;
        } else {
            if (s.foundational_dropped != counters_.foundational_dropped) foundational_bad_ = true;
            if (s.detail_dropped != counters_.detail_dropped) detail_bad_ = true;
            // Per-group changes without aggregate changes are inconsistent too.
            for (size_t i=0;i<16;++i)
                if (s.per_group_dropped[i] != counters_.per_group_dropped[i]) detail_bad_ = true;
            counters_ = s;
        }
        status_valid_ = true;
        host_invalid_ = s.inventory_invalid != 0;
    }
    void Consume(const void* bytes, size_t size, const na_observe_status& status) {
        ObserveStatus(status);
        if (!status_valid_) return;
        const auto batch = DecodeBatch(bytes,size);
        if (!batch.valid) { foundational_bad_ = true; return; }
        if (batch.header.session != session_ || batch.header.epoch != epoch_) return;
        if (final_) { foundational_bad_ = true; return; }
        if (batch.unknown_foundational) foundational_bad_ = true;
        for (const auto& item : batch.events) {
            const auto& h = item.header;
            const auto* event = batch.bytes.data()+item.offset;
            if (h.flags & (NA_EVF_TRUNCATED | NA_EVF_METHOD_UNOBSERVED)) {
                if (h.flags & NA_EVF_FOUNDATIONAL) foundational_bad_ = true;
                detail_bad_ = true;
            }
            if (h.flags & NA_EVF_ORDER_AMBIGUOUS) detail_bad_ = true;
            if (h.kind == NA_EV_EPOCH_BEGIN) {
                if (begun_ || complete_) foundational_bad_ = true;
                begun_ = true;
            } else if (h.kind == NA_EV_INVENTORY_COMPLETE) {
                na_ev_inventory_complete e{}; std::memcpy(&e,event,sizeof(e));
                if (!begun_ || complete_ || e.complete != 1) foundational_bad_ = true;
                complete_ = e.complete == 1;
            } else if (h.kind == NA_EV_CAPABILITY_REPORT) {
                na_ev_capability_report e{}; std::memcpy(&e,event,sizeof(e));
                if (!begun_ || capabilities_seen_) foundational_bad_ = true;
                capabilities_seen_ = true;
                std::array<bool,NA_GROUP_COUNT_V1> seen{};
                for (uint32_t i=0;i<e.entry_count;++i) {
                    na_cap_entry cap{};
                    std::memcpy(&cap,event+e.entries_offset+static_cast<size_t>(i)*e.entry_stride,sizeof(cap));
                    if (cap.group >= NA_GROUP_COUNT_V1) continue;
                    if (seen[cap.group] || cap.support > NA_CAP_SUPPORTED) foundational_bad_ = true;
                    seen[cap.group] = true;
                    support_[cap.group] = cap.support;
                }
            } else if (h.kind == NA_EV_LOSS_MARK) {
                na_ev_loss_mark e{}; std::memcpy(&e,event,sizeof(e));
                if (e.foundational) foundational_bad_ = true;
                else detail_bad_ = true;
            } else if (h.kind == NA_EV_ORDER_AMBIGUITY_RANGE) {
                // Until per-ticket evidence exists, revoke all retained history.
                detail_bad_ = true;
            }
        }
        if (batch.header.batch_flags & NA_BATCHF_FINAL) final_ = true;
    }
    bool InventoryConsistent() const {
        return status_valid_ && !host_invalid_ && begun_ && complete_ && capabilities_seen_ &&
            !foundational_bad_ && !final_;
    }
    bool CaptureContiguous() const { return InventoryConsistent() && !detail_bad_; }
    bool Supports(uint32_t group) const {
        return InventoryConsistent() && group < support_.size() && support_[group] == NA_CAP_SUPPORTED;
    }
    uint64_t Session() const { return session_; }
    uint64_t Epoch() const { return epoch_; }
private:
    uint64_t session_{},epoch_{};
    na_observe_status counters_{};
    std::array<uint32_t,NA_GROUP_COUNT_V1> support_{};
    bool status_valid_{},host_invalid_{true},begun_{},complete_{},capabilities_seen_{};
    bool foundational_bad_{},detail_bad_{},final_{};
};
} // namespace na::discovery
