// C9 (H3): constant data bound by a submitted recording is copied at the
// submission boundary from the application's CPU mapping, labelled
// app-sync-unknown, with full provenance (queue ticket, list, generation,
// op, resource, offset, root parameter). Checks on a real device (WARP on CI):
//   - root CBV in a persistently mapped upload buffer -> exact camera matrix;
//   - CBV reached through a descriptor table (root signature range) -> exact;
//   - the snapshot reflects what the CPU wrote BEFORE the submission even if
//     the buffer is rewritten afterwards;
//   - default-heap buffer -> GPU_WRITTEN, no bytes;
//   - upload buffer never mapped -> NOT_MAPPED, no bytes.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/observe/producers.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
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

void Matrix(float* m, float seed)
{
    for (int i = 0; i < 16; ++i) m[i] = seed + static_cast<float>(i) * 0.5f;
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("observe_snapshot_test");
    ID3D12Device* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);

    // Root signature: 0 = root CBV b0, 1 = table with one CBV (b1), 2 = root CBV b2.
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, 0};
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor = {2, 0};
    D3D12_ROOT_SIGNATURE_DESC rsd{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    test::ComPtr<ID3DBlob> blob, err;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
    test::ComPtr<ID3D12RootSignature> rs;
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))));

    auto upload = test::CreateBuffer(dev, 8192, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto unmapped = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpu = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(upload && unmapped && gpu);
    uint8_t* cpu = nullptr;
    REQUIRE(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&cpu)))); // persistent mapping
    float view[16], proj[16];
    Matrix(view, 1.0f);
    Matrix(proj, 100.0f);
    std::memcpy(cpu + 512, view, sizeof(view));
    std::memcpy(cpu + 1024, proj, sizeof(proj));

    test::ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))));
    const uint32_t inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{upload->GetGPUVirtualAddress() + 1024, 256};
    dev->CreateConstantBufferView(&cbv, D3D12_CPU_DESCRIPTOR_HANDLE{heap->GetCPUDescriptorHandleForHeapStart().ptr + 3 * inc});

    test::ComPtr<ID3D12CommandAllocator> alloc;
    test::ComPtr<ID3D12GraphicsCommandList> list;
    REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))));
    REQUIRE(SUCCEEDED(list->Close()));
    REQUIRE(SUCCEEDED(list->Reset(alloc.Get(), nullptr)));
    list->SetGraphicsRootSignature(rs.Get());                                                        // op 1
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);                                                              // op 2
    list->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress() + 512);                // op 3
    list->SetGraphicsRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{heap->GetGPUDescriptorHandleForHeapStart().ptr + 3 * inc}); // op 4
    list->SetGraphicsRootConstantBufferView(2, gpu->GetGPUVirtualAddress());                         // op 5
    list->SetGraphicsRootConstantBufferView(0, unmapped->GetGPUVirtualAddress() + 256);              // op 6
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* ls[] = {list.Get()};
    d.queue->ExecuteCommandLists(1, ls);
    // The application overwrites after submitting: the snapshot must hold the
    // pre-submission contents.
    std::memset(cpu + 512, 0xEE, 64);
    d.WaitIdle();

    // C9-H3-REVIEW-1 #1 regression: an Unmap racing a snapshot copy must wait
    // until the copy has finished with the mapped pointer.
    struct Race {
        ID3D12Resource* buffer;
        std::thread unmapper;
        std::atomic<bool> returned{false};
        HANDLE entered;            // set by Unmap right before it contends for the mapping lock
        bool reached_boundary = false;
        bool returned_during_copy = true;
        bool fired = false;
    } race{upload.Get(), {}, {}, CreateEventW(nullptr, TRUE, FALSE, nullptr), false, true, false};
    REQUIRE(race.entered != nullptr);
    na::observe::SetUnmapBoundaryProbeForTests([](void* user) { SetEvent(static_cast<Race*>(user)->entered); }, &race);
    na::observe::SetSnapshotCopyProbeForTests(
        [](void* user) {
            auto* r = static_cast<Race*>(user);
            if (r->fired) return;
            r->fired = true;
            r->unmapper = std::thread([r] {
                r->buffer->Unmap(0, nullptr);
                r->returned.store(true);
            });
            // Explicit handshake: the unmapper has reached the contested lock.
            r->reached_boundary = WaitForSingleObject(r->entered, 5000) == WAIT_OBJECT_0;
            Sleep(50); // it is now blocked (or, if unsynchronised, would have finished)
            r->returned_during_copy = r->returned.load();
        },
        &race);
    REQUIRE(SUCCEEDED(alloc->Reset()));
    REQUIRE(SUCCEEDED(list->Reset(alloc.Get(), nullptr)));
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootConstantBufferView(0, upload->GetGPUVirtualAddress() + 512);
    REQUIRE(SUCCEEDED(list->Close()));
    d.queue->ExecuteCommandLists(1, ls);
    na::observe::SetSnapshotCopyProbeForTests(nullptr, nullptr);
    if (race.unmapper.joinable()) race.unmapper.join();
    na::observe::SetUnmapBoundaryProbeForTests(nullptr, nullptr);
    CloseHandle(race.entered);
    d.WaitIdle();
    std::printf("unmap race: probe fired %d, boundary reached %d, unmap returned during copy %d, after %d\n", race.fired, race.reached_boundary,
                race.returned_during_copy, race.returned.load());
    CHECK(race.fired);
    CHECK(race.reached_boundary);
    CHECK(!race.returned_during_copy);
    CHECK(race.returned.load());
    REQUIRE(na_observe_stop() == NA_OK);

    const na_object_id upload_id = na::observe::IdOf(upload.Get()), gpu_id = na::observe::IdOf(gpu.Get()),
                       unmapped_id = na::observe::IdOf(unmapped.Get()), list_id = na::observe::IdOf(list.Get());
    CHECK(upload_id && gpu_id && unmapped_id && list_id);
    bool root_ok = false, table_ok = false, gpu_ok = false, unmapped_ok = false;
    uint64_t exec_ticket = 0;
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& e : g_events) {
        const auto* x = reinterpret_cast<const na_ev_queue_execute*>(e.data());
        if (x->hdr.kind == NA_EV_QUEUE_EXECUTE && x->list_count == 1 && !exec_ticket) exec_ticket = x->queue_ticket;
    }
    int snaps = 0;
    for (const auto& e : g_events) {
        const auto* s = reinterpret_cast<const na_ev_snapshot*>(e.data());
        if (s->hdr.kind != NA_EV_SNAPSHOT) continue;
        ++snaps;
        CHECK(s->sync == NA_SNAP_SYNC_APP_UNKNOWN && s->list == list_id);
        if (s->recording_generation == 3) { // the raced submission: copied before the Unmap went through
            CHECK(s->status == NA_SNAP_CAPTURED && s->captured_bytes > 0);
            continue;
        }
        CHECK(s->recording_generation == 2 && s->queue_ticket == exec_ticket);
        const uint8_t* bytes = s->payload_offset ? e.data() + s->payload_offset : nullptr;
        if (s->resource == upload_id && s->binding == NA_SNAP_ROOT_CBV)
            root_ok = s->status == NA_SNAP_CAPTURED && s->offset == 512 && s->param == 0 && s->op_index == 3 && s->captured_bytes >= 64 &&
                      bytes && std::memcmp(bytes, view, 64) == 0;
        if (s->resource == upload_id && s->binding == NA_SNAP_TABLE_CBV)
            table_ok = s->status == NA_SNAP_CAPTURED && s->offset == 1024 && s->param == 1 && s->op_index == 4 && s->requested_bytes == 256 &&
                       s->captured_bytes == 256 && bytes && std::memcmp(bytes, proj, 64) == 0;
        if (s->resource == gpu_id) gpu_ok = s->status == NA_SNAP_GPU_WRITTEN && s->captured_bytes == 0 && s->payload_offset == 0;
        if (s->resource == unmapped_id) unmapped_ok = s->status == NA_SNAP_NOT_MAPPED && s->captured_bytes == 0 && s->offset == 256;
    }
    std::printf("snapshots: %d (root %d table %d gpu-written %d not-mapped %d)\n", snaps, root_ok, table_ok, gpu_ok, unmapped_ok);
    CHECK(snaps == 5); // 4 from the first submission + the raced root CBV
    CHECK(root_ok);
    CHECK(table_ok);
    CHECK(gpu_ok);
    CHECK(unmapped_ok);
    return test::finish("observe_snapshot_test"); // the race already unmapped the buffer
}
