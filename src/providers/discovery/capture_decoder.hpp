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

