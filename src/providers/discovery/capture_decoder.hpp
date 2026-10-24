// Worker/offline wire validation only. No GPU access or semantic qualification.
#pragma once
#if !defined(_WIN32) && !defined(__cdecl)
#define __cdecl
#define NA_DISCOVERY_UNDEFINE_CDECL
#endif
#include "host_dx12/host_observe.h"
#ifdef NA_DISCOVERY_UNDEFINE_CDECL
#undef __cdecl
#undef NA_DISCOVERY_UNDEFINE_CDECL
#endif
#include <cstddef>
#include <cstring>
#include <vector>

namespace na::discovery {
struct DecodedEvent { na_event_header header{}; size_t offset{}; };
struct DecodedBatch {
    bool valid{};
    bool unknown_foundational{};
    bool incomplete_capture{}; // Structurally valid bytes do not establish complete evidence.
    na_event_batch header{};
    // Owned bytes; offsets never refer into the callback's borrowed allocation.
    std::vector<unsigned char> bytes;
    std::vector<DecodedEvent> events;
};
inline size_t KnownEventSize(uint32_t kind) {
    switch (kind) {
#define NA_SIZE(k,t) case k: return sizeof(t)
    NA_SIZE(NA_EV_CAPABILITY_REPORT, na_ev_capability_report);
    NA_SIZE(NA_EV_RESOURCE_CREATED, na_ev_resource_created);
    NA_SIZE(NA_EV_OBJECT_RETIRED, na_ev_object_retired);
    NA_SIZE(NA_EV_DESCRIPTOR_WRITE, na_ev_descriptor_write);
    NA_SIZE(NA_EV_DESCRIPTOR_COPY, na_ev_descriptor_copy);
    NA_SIZE(NA_EV_RESOURCE_HEAP_CREATED, na_ev_resource_heap_created);
    NA_SIZE(NA_EV_DESCRIPTOR_HEAP_CREATED, na_ev_descriptor_heap_created);
    NA_SIZE(NA_EV_LIST_CREATED, na_ev_list_created);
    NA_SIZE(NA_EV_LIST_RESET, na_ev_list_reset_close);
    NA_SIZE(NA_EV_LIST_CLOSE, na_ev_list_reset_close);
    NA_SIZE(NA_EV_QUEUE_CREATED, na_ev_queue_created);
    NA_SIZE(NA_EV_QUEUE_EXECUTE, na_ev_queue_execute);
    NA_SIZE(NA_EV_QUEUE_SIGNAL, na_ev_queue_sync);
    NA_SIZE(NA_EV_QUEUE_WAIT, na_ev_queue_sync);
    NA_SIZE(NA_EV_FENCE_CREATED, na_ev_fence_created);
    NA_SIZE(NA_EV_SWAPCHAIN_CREATED, na_ev_swapchain_created);
    NA_SIZE(NA_EV_SWAPCHAIN_RESIZED, na_ev_swapchain_resized);
    NA_SIZE(NA_EV_PRESENT, na_ev_present);
    NA_SIZE(NA_EV_LOSS_MARK, na_ev_loss_mark);
    NA_SIZE(NA_EV_EPOCH_BEGIN, na_ev_epoch_begin);
    NA_SIZE(NA_EV_INVENTORY_COMPLETE, na_ev_inventory_complete);
    NA_SIZE(NA_EV_ORDER_AMBIGUITY_RANGE, na_ev_order_ambiguity_range);
    NA_SIZE(NA_EV_DEVICE_CREATED, na_ev_device_created);
#if NA_OBSERVE_ABI_MINOR >= 2
    NA_SIZE(NA_EV_ROOT_SIGNATURE_CREATED, na_ev_root_signature_created);
    NA_SIZE(NA_EV_PIPELINE_CREATED, na_ev_pipeline_created);
    NA_SIZE(NA_EV_LIST_OP, na_ev_list_op);
#endif
#ifdef NA_SNAP_SYNC_APP_UNKNOWN
    NA_SIZE(NA_EV_SNAPSHOT, na_ev_snapshot);
#endif
#undef NA_SIZE
    default: return 0;
    }
}
inline bool WireArrayFits(size_t size, size_t prefix, uint32_t offset,
                          uint32_t stride, uint32_t count, size_t element) {
    if (!count) return true; // No array bytes are accessed.
    if (offset < prefix || offset > size || stride < element)
        return false;
    const size_t available = size - offset;
    return available >= element && static_cast<size_t>(count - 1) <= (available - element) / stride;
}
// readable is supplied by the owner of the input allocation. It is not derived
// from untrusted wire fields. Limits are consumer policy, not ABI restrictions.
inline DecodedBatch DecodeBatch(const void* input, size_t readable) {
    constexpr size_t max_bytes = 4 * 1024 * 1024;
    constexpr size_t max_events = 65536;
    DecodedBatch result{};
    if (!input || readable < sizeof(na_event_batch) || readable > max_bytes) return {};
    const auto* raw = static_cast<const unsigned char*>(input);
    auto& b = result.header;
    std::memcpy(&b, raw, sizeof(b));
    if (b.abi_major != NA_OBSERVE_ABI_MAJOR || b.header_bytes < sizeof(b) || b.header_bytes % 8 ||
        b.header_bytes > readable || b.payload_bytes != readable - b.header_bytes ||
        !b.session || b.session == NA_UNKNOWN_U64 || !b.epoch || b.epoch == NA_UNKNOWN_U64 ||
        b.event_count > max_events || b.event_count > b.payload_bytes / sizeof(na_event_header)) return {};
    size_t position = b.header_bytes;
    for (uint32_t i = 0; i < b.event_count; ++i) {
        na_event_header h{};
        if (readable - position < sizeof(h)) return {};
        std::memcpy(&h, raw + position, sizeof(h));
        const size_t minimum = KnownEventSize(h.kind);
        if (h.size_bytes < sizeof(h) || h.size_bytes % 8 || h.size_bytes > readable - position ||
            h.size_bytes < minimum || h.epoch != b.epoch) return {};
        if (h.kind == NA_EV_QUEUE_EXECUTE) {
            na_ev_queue_execute e{}; std::memcpy(&e, raw + position, sizeof(e));
            if (!WireArrayFits(h.size_bytes, sizeof(e), e.refs_offset, e.ref_stride, e.list_count, sizeof(na_list_ref))) return {};
            if (e.contains_bundles) result.incomplete_capture = true;
            for (uint32_t j=0; j<e.list_count; ++j) {
                na_list_ref ref{};
                std::memcpy(&ref, raw+position+e.refs_offset+static_cast<size_t>(j)*e.ref_stride, sizeof(ref));
                if (!ref.list || ref.list==NA_UNKNOWN_U64 || ref.recording_generation==NA_UNKNOWN_U64)
                    result.incomplete_capture = true;
            }
        } else if (h.kind == NA_EV_LIST_RESET || h.kind == NA_EV_LIST_CLOSE) {
            na_ev_list_reset_close e{}; std::memcpy(&e, raw+position, sizeof(e));
            if (e.hresult < 0 || (e.list_flags & NA_LISTF_RESET_FAILED)) result.incomplete_capture = true;
        } else if (h.kind == NA_EV_CAPABILITY_REPORT) {
            na_ev_capability_report e{}; std::memcpy(&e, raw + position, sizeof(e));
            if (e.session != b.session || !WireArrayFits(h.size_bytes, sizeof(e), e.entries_offset,
                e.entry_stride, e.entry_count, sizeof(na_cap_entry))) return {};
        } else if (h.kind == NA_EV_EPOCH_BEGIN) {
            na_ev_epoch_begin e{}; std::memcpy(&e, raw + position, sizeof(e));
            if (e.session != b.session || e.epoch != b.epoch) return {};
        } else if (h.kind == NA_EV_INVENTORY_COMPLETE) {
            na_ev_inventory_complete e{}; std::memcpy(&e, raw + position, sizeof(e));
            if (e.epoch != b.epoch) return {};
        }
#if NA_OBSERVE_ABI_MINOR >= 2
        if (h.kind == NA_EV_ROOT_SIGNATURE_CREATED) {
            na_ev_root_signature_created e{}; std::memcpy(&e,raw+position,sizeof(e));
            if (!WireArrayFits(h.size_bytes,sizeof(e),e.params_offset,e.param_stride,e.param_count,sizeof(na_root_param))) return {};
            for (uint32_t j=0;j<e.param_count;++j) {
                na_root_param p{}; std::memcpy(&p,raw+position+e.params_offset+static_cast<size_t>(j)*e.param_stride,sizeof(p));
                // ABI0.2 only carries the first range and omits table offsets.
                if (p.param_type == NA_ROOT_TABLE) result.incomplete_capture = true;
            }
        } else if (h.kind == NA_EV_LIST_OP) {
            na_ev_list_op e{}; std::memcpy(&e,raw+position,sizeof(e));
            if (e.payload_bytes && (e.payload_offset < sizeof(e) || e.payload_offset > h.size_bytes ||
                e.payload_bytes > h.size_bytes-e.payload_offset)) return {};
            uint64_t count=0; size_t element=0;
            switch (e.op) {
            case NA_OP_SET_ROOT_CONSTANTS: count=e.a[2];element=4;break;
            case NA_OP_OM_SET_RENDER_TARGETS:
                if(e.a[0]>8 || e.a[1]>1) return {};
                count=e.a[0]+e.a[1];element=sizeof(na_view_ref);break;
            case NA_OP_CLEAR_RTV: case NA_OP_CLEAR_DSV: count=1;element=sizeof(na_view_ref);break;
            case NA_OP_RESOURCE_BARRIER: count=e.a[0];element=sizeof(na_barrier);break;
            case NA_OP_RS_VIEWPORT: case NA_OP_RS_SCISSOR:
                if(e.a[0]>1) result.incomplete_capture=true;
                break;
            case NA_OP_COPY_TEXTURE_REGION:
                result.incomplete_capture=true; // offsets/box/footprint not represented
                break;
            default:
                if(e.op<NA_OP_SET_PIPELINE || e.op>NA_OP_RS_SCISSOR) result.incomplete_capture=true;
            }
            if(element && count) {
                if(e.payload_bytes && (e.payload_stride<element || e.payload_bytes%e.payload_stride)) return {};
                if(h.flags&NA_EVF_TRUNCATED) result.incomplete_capture=true;
                else if(!e.payload_stride || count>e.payload_bytes/e.payload_stride) return {};
            }
        }
#endif
#ifdef NA_SNAP_SYNC_APP_UNKNOWN
        if (h.kind == NA_EV_SNAPSHOT) {
            na_ev_snapshot e{}; std::memcpy(&e,raw+position,sizeof(e));
            if (e.captured_bytes > e.requested_bytes ||
                (e.status != NA_SNAP_CAPTURED && e.captured_bytes)) return {};
            if (e.captured_bytes && (e.payload_offset < sizeof(e) || e.payload_offset > h.size_bytes ||
                e.captured_bytes > h.size_bytes-e.payload_offset)) return {};
            // C9 provides no independent application-write synchronization or
            // GPU completion. Keep these bytes available for offline hypotheses.
            result.incomplete_capture = true;
        }
#endif
        if (!minimum && (h.flags & NA_EVF_FOUNDATIONAL)) result.unknown_foundational = true;
        result.events.push_back({h, position});
        position += h.size_bytes;
    }
    if (position != readable) return {};
    result.bytes.assign(raw, raw + readable);
    result.valid = true;
    return result;
}
} // namespace na::discovery
