// reconstructed 2026-10 — ABI 0.1 wire layout; no simulated host or consumer.
#include "test_support.hpp"
#include "observe_abi.hpp"
#include <cstddef>
#include <cstring>
#include <type_traits>
static_assert(NA_OBSERVE_ABI_MAJOR == 0 && NA_OBSERVE_ABI_MINOR == 2); // 0.2 = C8 (H2) append-only
static_assert(sizeof(na_event_header) == 32 && alignof(na_event_header) == 8);
static_assert(sizeof(na_event_batch) == 40 && offsetof(na_event_batch, payload_bytes) == 24);
static_assert(sizeof(na_list_ref) == 16 && offsetof(na_list_ref, recording_generation) == 8);
static_assert(sizeof(na_observe_status) == 184 && offsetof(na_observe_status, per_group_dropped) == 56);
#define CHECK_EVENT(type, bytes) static_assert(sizeof(type) == bytes && alignof(type) == 8 && offsetof(type, hdr) == 0 && std::is_standard_layout_v<type> && std::is_trivially_copyable_v<type>)
CHECK_EVENT(na_ev_capability_report, 72);
CHECK_EVENT(na_ev_device_created, 56);
CHECK_EVENT(na_ev_object_retired, 48);
CHECK_EVENT(na_ev_resource_created, 112);
CHECK_EVENT(na_ev_resource_heap_created, 64);
CHECK_EVENT(na_ev_descriptor_heap_created, 64);
CHECK_EVENT(na_ev_descriptor_write, 80);
CHECK_EVENT(na_ev_descriptor_copy, 64);
CHECK_EVENT(na_ev_list_created, 72);
CHECK_EVENT(na_ev_list_reset_close, 64);
CHECK_EVENT(na_ev_queue_created, 56);
CHECK_EVENT(na_ev_queue_execute, 64);
CHECK_EVENT(na_ev_order_ambiguity_range, 56);
CHECK_EVENT(na_ev_fence_created, 64);
CHECK_EVENT(na_ev_queue_sync, 88);
CHECK_EVENT(na_ev_swapchain_created, 80);
CHECK_EVENT(na_ev_swapchain_resized, 72);
CHECK_EVENT(na_ev_present, 64);
CHECK_EVENT(na_ev_loss_mark, 40);
CHECK_EVENT(na_ev_epoch_begin, 48);
CHECK_EVENT(na_ev_inventory_complete, 48);
// ABI 0.2 (C8 / H2) additions; 0.1 layouts above are unchanged.
CHECK_EVENT(na_ev_root_signature_created, 80);
CHECK_EVENT(na_ev_pipeline_created, 216);
CHECK_EVENT(na_ev_list_op, 120);
static_assert(sizeof(na_root_param) == 32 && sizeof(na_view_ref) == 40 && sizeof(na_barrier) == 40);
static_assert(offsetof(na_ev_list_op, a) == 72 && offsetof(na_ev_pipeline_created, stage_hash) == 64);
static_assert(NA_EV_ROOT_SIGNATURE_CREATED == 24 && NA_EV_PIPELINE_CREATED == 25 && NA_EV_LIST_OP == 26);
#undef CHECK_EVENT
int main() { return test::run({
    {"little-endian wire and explicit inline-array offset", [] {
        const uint32_t n=0x04030201; unsigned char bytes[4]{}; std::memcpy(bytes,&n,4); CHECK(bytes[0] == 1 && bytes[3] == 4);
        struct Packet { na_ev_queue_execute event; uint64_t extension; na_list_ref refs[2]; } packet{};
        packet.event.hdr.size_bytes=sizeof(packet); packet.event.hdr.kind=NA_EV_QUEUE_EXECUTE;
        packet.event.list_count=2; packet.event.refs_offset=offsetof(Packet,refs); packet.event.ref_stride=sizeof(na_list_ref);
        packet.refs[0]={123,7}; packet.refs[1]={456,8};
        na_list_ref ref{}; const auto* raw=reinterpret_cast<const unsigned char*>(&packet);
        std::memcpy(&ref,raw+packet.event.refs_offset+packet.event.ref_stride,sizeof(ref));
        CHECK(ref.list == 456 && ref.recording_generation == 8);
        CHECK(packet.event.refs_offset > sizeof(na_ev_queue_execute));
    }},
}); }
