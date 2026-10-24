// C9b (A2-R03 second part): asynchronous resource content samples on a real
// device (WARP on CI). Checks:
//   - an armed sample is copied at the end of the first submission whose
//     recorded transitions establish the subresource's state; READY only after
//     the host fence completed; exact clear colour / depth value; queue,
//     ticket and the state it transitioned from are reported;
//   - a subrect is copied with its own extent and row pitch;
//   - a submission without state evidence leaves the request ARMED; release
//     then cancels it; an unknown resource is refused;
//   - the game's resource is left in the state it was in (the next game
//     barrier from that state executes cleanly on the GPU);
//   - READY data expires when not released in time.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/host_observe_samples.h"
#include "host_dx12/observe/producers.hpp"

#include <cstring>

namespace {
void __cdecl Sink(const na_event_batch*, void*) {}

na_sample_info Poll(uint64_t token)
{
    na_sample_info i{};
    i.size_bytes = sizeof(i);
    if (na_observe_sample_poll(token, &i) != NA_OK) i.status = 0;
    return i;
}

na_sample_info WaitReady(uint64_t token)
{
    na_sample_info i{};
    for (int k = 0; k < 500; ++k) {
        i = Poll(token);
        if (i.status != NA_SAMPLE_ARMED && i.status != NA_SAMPLE_IN_FLIGHT) break;
        Sleep(10);
    }
    return i;
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("observe_sample_test");
    ID3D12Device* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);

    const uint32_t W = 64, H = 32;
    auto color = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = test::CreateTexture(dev, 32, 16, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto idle = test::CreateTexture(dev, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(color && depth && idle);
    test::ComPtr<ID3D12DescriptorHeap> rtvh, dsvh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh))));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh))));
    dev->CreateRenderTargetView(color.Get(), nullptr, rtvh->GetCPUDescriptorHandleForHeapStart());
    dev->CreateDepthStencilView(depth.Get(), nullptr, dsvh->GetCPUDescriptorHandleForHeapStart());

    const na_object_id color_id = na::observe::IdOf(color.Get()), depth_id = na::observe::IdOf(depth.Get()), idle_id = na::observe::IdOf(idle.Get());
    REQUIRE(color_id && depth_id && idle_id);

    // Requests armed BEFORE the submission.
    na_sample_request rq{};
    rq.size_bytes = sizeof(rq);
    rq.resource = color_id;
    uint64_t t_color = 0, t_sub = 0, t_depth = 0, t_idle = 0, t_bad = 0;
    REQUIRE(na_observe_sample_request(&rq, &t_color) == NA_OK && t_color);
    na_sample_request sub = rq;
    sub.x = 8; sub.y = 4; sub.width = 16; sub.height = 8;
    REQUIRE(na_observe_sample_request(&sub, &t_sub) == NA_OK);
    na_sample_request dq = rq;
    dq.resource = depth_id;
    REQUIRE(na_observe_sample_request(&dq, &t_depth) == NA_OK);
    na_sample_request iq = rq;
    iq.resource = idle_id;
    REQUIRE(na_observe_sample_request(&iq, &t_idle) == NA_OK);
    na_sample_request bad = rq;
    bad.resource = 0xDEAD0000ull;
    CHECK(na_observe_sample_request(&bad, &t_bad) == NA_E_ARG && t_bad == 0);
    CHECK(Poll(t_color).status == NA_SAMPLE_ARMED);

    // The game's frame: transitions recorded, clears.
    test::ComPtr<ID3D12CommandAllocator> alloc;
    test::ComPtr<ID3D12GraphicsCommandList> list;
    REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))));
    REQUIRE(SUCCEEDED(list->Close()));
    REQUIRE(SUCCEEDED(list->Reset(alloc.Get(), nullptr)));
    D3D12_RESOURCE_BARRIER b[2] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                   test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE)};
    list->ResourceBarrier(2, b);
    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    list->ClearRenderTargetView(rtvh->GetCPUDescriptorHandleForHeapStart(), clear, 0, nullptr);
    list->ClearDepthStencilView(dsvh->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.375f, 0, 0, nullptr);
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* ls[] = {list.Get()};
    d.queue->ExecuteCommandLists(1, ls);

    const na_sample_info ci = WaitReady(t_color);
    std::printf("color sample: status %u reason %u %ux%u pitch %u state_before 0x%x ticket %llu\n", ci.status, ci.reason, ci.width, ci.height,
                ci.row_pitch, ci.state_before, static_cast<unsigned long long>(ci.queue_ticket));
    CHECK(ci.status == NA_SAMPLE_READY && ci.width == W && ci.height == H && ci.data && ci.row_pitch >= W * 4);
    CHECK(ci.state_before == D3D12_RESOURCE_STATE_RENDER_TARGET && ci.queue == na::observe::IdOf(d.queue.Get()) && ci.queue_ticket != 0);
    if (ci.status == NA_SAMPLE_READY && ci.data) {
        const auto* px = static_cast<const uint8_t*>(ci.data);
        bool all = true;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* p = px + y * ci.row_pitch + x * 4;
                all = all && p[0] >= 63 && p[0] <= 65 && p[1] >= 127 && p[1] <= 129 && p[2] >= 190 && p[2] <= 192 && p[3] == 255;
            }
        CHECK(all);
    }
    const na_sample_info si = WaitReady(t_sub);
    CHECK(si.status == NA_SAMPLE_READY && si.x == 8 && si.y == 4 && si.width == 16 && si.height == 8 && si.data);
    if (si.data) CHECK(static_cast<const uint8_t*>(si.data)[1] >= 127 && static_cast<const uint8_t*>(si.data)[1] <= 129);
    const na_sample_info di = WaitReady(t_depth);
    std::printf("depth sample: status %u %ux%u state_before 0x%x\n", di.status, di.width, di.height, di.state_before);
    CHECK(di.status == NA_SAMPLE_READY && di.state_before == D3D12_RESOURCE_STATE_DEPTH_WRITE && di.data);
    if (di.data) {
        float v = 0;
        std::memcpy(&v, static_cast<const uint8_t*>(di.data) + 5 * di.row_pitch + 7 * 4, 4);
        CHECK(v == 0.375f);
    }
    // No state evidence for `idle` in that submission: still armed. Release cancels.
    CHECK(Poll(t_idle).status == NA_SAMPLE_ARMED);
    CHECK(na_observe_sample_release(t_idle) == NA_OK);
    CHECK(Poll(t_idle).status == 0); // gone
    CHECK(na_observe_sample_release(t_color) == NA_OK && na_observe_sample_release(t_color) == NA_OK); // idempotent
    CHECK(na_observe_sample_release(t_sub) == NA_OK);

    // The game continues from the states it left: RT -> COPY_SOURCE and
    // DEPTH_WRITE -> COMMON must be valid, i.e. the host restored them.
    REQUIRE(SUCCEEDED(alloc->Reset()));
    REQUIRE(SUCCEEDED(list->Reset(alloc.Get(), nullptr)));
    D3D12_RESOURCE_BARRIER b2[2] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                    test::Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON)};
    list->ResourceBarrier(2, b2);
    REQUIRE(SUCCEEDED(list->Close()));
    d.queue->ExecuteCommandLists(1, ls);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);

    // Expiry: READY data not released in time goes away.
    Sleep(60);
    na_sample_request eq = rq;
    eq.expiry_ms = 50;
    eq.resource = color_id;
    uint64_t t_exp = 0;
    REQUIRE(na_observe_sample_request(&eq, &t_exp) == NA_OK);
    REQUIRE(SUCCEEDED(alloc->Reset()));
    REQUIRE(SUCCEEDED(list->Reset(alloc.Get(), nullptr)));
    D3D12_RESOURCE_BARRIER b3 = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b3);
    REQUIRE(SUCCEEDED(list->Close()));
    d.queue->ExecuteCommandLists(1, ls);
    CHECK(WaitReady(t_exp).status == NA_SAMPLE_READY);
    Sleep(120);
    CHECK(Poll(t_exp).status == NA_SAMPLE_EXPIRED);
    na_observe_sample_release(t_exp);
    na_observe_sample_release(t_depth);
    d.WaitIdle();
    REQUIRE(na_observe_stop() == NA_OK);
    return test::finish("observe_sample_test");
}
