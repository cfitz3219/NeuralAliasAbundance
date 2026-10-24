// C8a: H1 producers against a real D3D12 device (WARP on CI, so no GPU needed).
// Drives every hooked creation/use path and decodes the delivered event stream
// with the frozen ABI header only:
//   - snapshot replay of objects created after installation but before start,
//     PREEXISTING identity for a queue created before installation;
//   - resource / heap / descriptor-heap / list / fence / queue creations with
//     their parameters, placed-resource heap linkage;
//   - descriptor writes resolved to (heap, slot) with versions, a CBV resolved
//     through the GPU VA table, a view of an unobserved resource marked
//     UNKNOWN (never invented), CopyDescriptorsSimple versions;
//   - queue execute with list refs and tickets, signal/wait provenance;
//   - OBJECT_RETIRED on final release (private-data sentinel).
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/observe/producers.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
std::mutex g_m;
std::vector<std::vector<uint8_t>> g_events;

void __cdecl Sink(const na_event_batch* b, void*)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(b) + b->header_bytes;
    uint32_t used = 0;
    std::lock_guard<std::mutex> lock(g_m);
    while (used < b->payload_bytes) {
        const auto* h = reinterpret_cast<const na_event_header*>(p + used);
        if (h->size_bytes < sizeof(na_event_header)) break;
        g_events.emplace_back(p + used, p + used + h->size_bytes);
        used += h->size_bytes;
    }
}

template <typename E>
std::vector<E> All(uint32_t kind)
{
    std::vector<E> out;
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& e : g_events) {
        const auto* h = reinterpret_cast<const na_event_header*>(e.data());
        if (h->kind != kind) continue;
        E v{};
        std::memcpy(&v, e.data(), std::min(sizeof(E), e.size()));
        out.push_back(v);
    }
    return out;
}

uint32_t CountKind(uint32_t kind)
{
    std::lock_guard<std::mutex> lock(g_m);
    uint32_t n = 0;
    for (const auto& e : g_events) n += reinterpret_cast<const na_event_header*>(e.data())->kind == kind ? 1u : 0u;
    return n;
}
} // namespace

int main()
{
    SetEnvironmentVariableW(L"NEURALALIAS_OBSERVE_TRACE", L"1");
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("observe_producers_test");
    ID3D12Device* dev = d.device.Get();

    // Created BEFORE installation: unknown to the producers.
    auto early = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(early);

    std::printf("checkpoint 0\n"); std::fflush(stdout);
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    const auto st0 = na::observe::GetProducerStats();
    std::printf("hooks installed %u, optional missing %u, shared-target slots [%s]\n", st0.hooks_installed, st0.hooks_missing, st0.shared_target_slots);
    CHECK(st0.hooks_installed >= 20);

    // Created after installation but before start: must arrive in the snapshot.
    test::ComPtr<ID3D12Fence> fence_pre;
    REQUIRE(SUCCEEDED(dev->CreateFence(7, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_pre))));
    const na_object_id fence_pre_id = na::observe::IdOf(fence_pre.Get());
    CHECK(fence_pre_id != 0);

    std::printf("checkpoint 1\n"); std::fflush(stdout);
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);

    std::printf("checkpoint 2\n"); std::fflush(stdout);
    // Resources, heaps.
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 1 << 20;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
    test::ComPtr<ID3D12Heap> heap;
    REQUIRE(SUCCEEDED(dev->CreateHeap(&hd, IID_PPV_ARGS(&heap))));
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 64; td.Height = 32; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    test::ComPtr<ID3D12Resource> placed;
    REQUIRE(SUCCEEDED(dev->CreatePlacedResource(heap.Get(), 0, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&placed))));
    auto cbuf = test::CreateBuffer(dev, 65536, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto rt = test::CreateTexture(dev, 128, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(cbuf && rt);
    const na_object_id placed_id = na::observe::IdOf(placed.Get()), cbuf_id = na::observe::IdOf(cbuf.Get()),
                       rt_id = na::observe::IdOf(rt.Get()), heap_id = na::observe::IdOf(heap.Get());
    CHECK(placed_id && cbuf_id && rt_id && heap_id);
    CHECK(na::observe::IdOf(early.Get()) == 0);
    {
        na_object_id r = 0;
        uint64_t off = 0;
        CHECK(na::observe::ResolveGpuVa(cbuf->GetGPUVirtualAddress() + 300, &r, &off) && r == cbuf_id && off == 300);
        CHECK(!na::observe::ResolveGpuVa(cbuf->GetGPUVirtualAddress() + 65536 + 4096 * 64, &r, &off) || r != cbuf_id);
    }

    std::printf("checkpoint 3\n"); std::fflush(stdout);
    // Descriptor heaps and writes.
    D3D12_DESCRIPTOR_HEAP_DESC dhd{};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dhd.NumDescriptors = 8;
    dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    test::ComPtr<ID3D12DescriptorHeap> srv_heap, staging, rtv_heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&srv_heap))));
    dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&staging))));
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    dhd.NumDescriptors = 2;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&rtv_heap))));
    const uint32_t inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE s0 = staging->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE s3{s0.ptr + 3 * inc};
    dev->CreateShaderResourceView(placed.Get(), nullptr, s3);          // staging slot 3, version 1
    dev->CreateShaderResourceView(placed.Get(), nullptr, s3);          // version 2
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{cbuf->GetGPUVirtualAddress() + 256, 256};
    dev->CreateConstantBufferView(&cbv, D3D12_CPU_DESCRIPTOR_HANDLE{s0.ptr + 4 * inc});
    dev->CreateShaderResourceView(early.Get(), nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{s0.ptr + 5 * inc}); // unobserved resource
    dev->CopyDescriptorsSimple(3, D3D12_CPU_DESCRIPTOR_HANDLE{srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + inc},
                               s3, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    std::printf("checkpoint 4\n"); std::fflush(stdout);
    // Command list + execution on the queue created before installation.
    test::ComPtr<ID3D12CommandAllocator> alloc;
    test::ComPtr<ID3D12GraphicsCommandList> list;
    REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    std::printf("checkpoint 4.0\n"); std::fflush(stdout);
    REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))));
    std::printf("checkpoint 4.1\n"); std::fflush(stdout);
    const float clear[4] = {0, 0, 0, 1};
    list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(), clear, 0, nullptr);
    std::printf("checkpoint 4.2\n"); std::fflush(stdout);
    REQUIRE(SUCCEEDED(list->Close()));
    std::printf("checkpoint 4.3\n"); std::fflush(stdout);
    test::ComPtr<ID3D12Fence> fence;
    REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    std::printf("checkpoint 4.4\n"); std::fflush(stdout);
    ID3D12CommandList* lists[] = {list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    std::printf("checkpoint 4.5\n"); std::fflush(stdout);
    REQUIRE(SUCCEEDED(d.queue->Signal(fence.Get(), 1)));
    std::printf("checkpoint 4.6\n"); std::fflush(stdout);
    REQUIRE(SUCCEEDED(d.queue->Wait(fence.Get(), 1)));
    std::printf("checkpoint 4.7\n"); std::fflush(stdout);
    d.WaitIdle();
    std::printf("checkpoint 4.8\n"); std::fflush(stdout);

    std::printf("checkpoint 5\n"); std::fflush(stdout);
    // Retirement: the last reference goes away.
    placed.Reset();
    REQUIRE(na_observe_stop() == NA_OK);

    std::printf("checkpoint 6\n"); std::fflush(stdout);
    // ---- decode ----
    {
        auto reports = All<na_ev_capability_report>(NA_EV_CAPABILITY_REPORT);
        CHECK(!reports.empty());
        auto fences = All<na_ev_fence_created>(NA_EV_FENCE_CREATED);
        bool pre_in_snapshot = false, post = false;
        for (const auto& f : fences) {
            if (f.fence == fence_pre_id && f.initial_value == 7) pre_in_snapshot = true;
            if (f.fence == na::observe::IdOf(fence.Get())) post = true;
        }
        CHECK(pre_in_snapshot); // replayed by the snapshot
        CHECK(post);
        CHECK(CountKind(NA_EV_INVENTORY_COMPLETE) >= 1);
    }
    {
        auto res = All<na_ev_resource_created>(NA_EV_RESOURCE_CREATED);
        bool placed_ok = false, rt_ok = false, buf_ok = false;
        for (const auto& r : res) {
            if (r.resource == placed_id)
                placed_ok = r.creation_kind == NA_CREATE_PLACED && r.heap == heap_id && r.heap_offset == 0 && r.width == 64 &&
                            r.height == 32 && r.format == DXGI_FORMAT_R8G8B8A8_UNORM && r.heap_type == D3D12_HEAP_TYPE_DEFAULT;
            if (r.resource == rt_id)
                rt_ok = r.creation_kind == NA_CREATE_COMMITTED && r.format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                        (r.resource_flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) && r.initial_state == D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (r.resource == cbuf_id) buf_ok = r.dimension == D3D12_RESOURCE_DIMENSION_BUFFER && r.heap_type == D3D12_HEAP_TYPE_UPLOAD;
        }
        CHECK(placed_ok);
        CHECK(rt_ok);
        CHECK(buf_ok);
        CHECK(!All<na_ev_resource_heap_created>(NA_EV_RESOURCE_HEAP_CREATED).empty());
    }
    const na_object_id staging_id = na::observe::IdOf(staging.Get()), srv_heap_id = na::observe::IdOf(srv_heap.Get());
    {
        auto heaps = All<na_ev_descriptor_heap_created>(NA_EV_DESCRIPTOR_HEAP_CREATED);
        bool vis = false, nonvis = false;
        for (const auto& h : heaps) {
            if (h.heap == srv_heap_id) vis = h.shader_visible == 1 && h.descriptor_count == 8;
            if (h.heap == staging_id) nonvis = h.shader_visible == 0;
        }
        CHECK(vis && nonvis);
        auto writes = All<na_ev_descriptor_write>(NA_EV_DESCRIPTOR_WRITE);
        int srv_v = 0;
        bool cbv_ok = false, unknown_ok = false, rtv_ok = false;
        for (const auto& w : writes) {
            if (w.heap == staging_id && w.slot == 3 && w.descriptor_kind == NA_DESC_SRV && w.resource == placed_id) srv_v = static_cast<int>(w.slot_version);
            if (w.heap == staging_id && w.slot == 4) cbv_ok = w.descriptor_kind == NA_DESC_CBV && w.resource == cbuf_id;
            if (w.heap == staging_id && w.slot == 5)
                unknown_ok = w.resource == NA_UNKNOWN_U64 && (w.hdr.flags & NA_EVF_METHOD_UNOBSERVED);
            if (w.descriptor_kind == NA_DESC_RTV) rtv_ok = w.resource == rt_id && w.slot == 0;
        }
        CHECK(srv_v == 2);
        CHECK(cbv_ok);
        CHECK(unknown_ok);
        CHECK(rtv_ok);
        auto copies = All<na_ev_descriptor_copy>(NA_EV_DESCRIPTOR_COPY);
        bool copy_ok = false;
        for (const auto& c : copies)
            if (c.src_heap == staging_id && c.dst_heap == srv_heap_id && c.src_slot == 3 && c.dst_slot == 1 && c.slot_count == 3) copy_ok = true;
        CHECK(copy_ok);
    }
    {
        const na_object_id list_id = na::observe::IdOf(list.Get());
        auto lc = All<na_ev_list_created>(NA_EV_LIST_CREATED);
        bool list_ok = false;
        for (const auto& l : lc)
            if (l.list == list_id) list_ok = l.initially_open == 1 && l.list_type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
                                             l.initial_allocator == na::observe::IdOf(alloc.Get()) && l.initial_allocator != 0;
        CHECK(list_ok);
        const na_object_id qid = na::observe::IdOf(d.queue.Get());
        CHECK(qid != 0);
        bool pre_queue = false;
        for (const auto& q : All<na_ev_queue_created>(NA_EV_QUEUE_CREATED))
            if (q.queue == qid) pre_queue = (q.hdr.flags & NA_EVF_PREEXISTING) != 0;
        CHECK(pre_queue);
        // Execute: its list ref, inline after the fixed part.
        bool exec_ok = false;
        uint64_t exec_ticket = 0;
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (const auto& e : g_events) {
                const auto* x = reinterpret_cast<const na_ev_queue_execute*>(e.data());
                if (x->hdr.kind != NA_EV_QUEUE_EXECUTE || x->queue != qid || x->list_count != 1) continue;
                const auto* ref = reinterpret_cast<const na_list_ref*>(e.data() + x->refs_offset);
                if (ref->list == list_id && ref->recording_generation == 1) { exec_ok = true; exec_ticket = x->queue_ticket; }
            }
        }
        CHECK(exec_ok);
        auto syncs = std::vector<na_ev_queue_sync>();
        for (auto& s : All<na_ev_queue_sync>(NA_EV_QUEUE_SIGNAL)) syncs.push_back(s);
        for (auto& s : All<na_ev_queue_sync>(NA_EV_QUEUE_WAIT)) syncs.push_back(s);
        bool sig = false, wait = false;
        for (const auto& s : syncs) {
            if (s.queue != qid || s.fence != na::observe::IdOf(fence.Get())) continue;
            if (s.hdr.kind == NA_EV_QUEUE_SIGNAL) sig = s.provenance == NA_FENCE_PROV_OBSERVED_QUEUE_SIGNAL && s.queue_ticket > exec_ticket && s.hresult == 0;
            if (s.hdr.kind == NA_EV_QUEUE_WAIT) wait = s.provenance == NA_FENCE_PROV_UNKNOWN && s.source_queue == 0;
        }
        CHECK(sig);
        CHECK(wait);
    }
    {
        bool retired = false;
        for (const auto& r : All<na_ev_object_retired>(NA_EV_OBJECT_RETIRED))
            if (r.object == placed_id) retired = r.object_kind == NA_OBJ_RESOURCE;
        CHECK(retired);
        CHECK(na::observe::IdOf(placed.Get()) == 0);
    }
    const auto st = na::observe::GetProducerStats();
    std::printf("producers: live %llu created %llu retired %llu writes %llu copies %llu unresolved %llu sentinel-fail %llu executes %llu\n",
                static_cast<unsigned long long>(st.objects_live), static_cast<unsigned long long>(st.objects_created),
                static_cast<unsigned long long>(st.objects_retired), static_cast<unsigned long long>(st.descriptor_writes),
                static_cast<unsigned long long>(st.descriptor_copies), static_cast<unsigned long long>(st.unresolved_handles),
                static_cast<unsigned long long>(st.sentinel_failures), static_cast<unsigned long long>(st.executes));
    CHECK(st.sentinel_failures == 0);
    CHECK(st.unresolved_handles == 0);
    return test::finish("observe_producers_test");
}
