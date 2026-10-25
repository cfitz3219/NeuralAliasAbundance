// C9b (A2-R03 second part) on a real device (WARP on CI), including the
// adversarial cases from C9B-REVIEW-1:
//   happy path: exact colour / depth, subrect extent, provenance; the caller
//     reads into ITS OWN buffer (R5: no borrowed pointers);
//   R1: release twice while the copy is stalled on the GPU -> the entry is
//     kept until completion, then freed by the worker;
//   R2: the game drops its last reference while the host copy is stalled ->
//     the host's reference keeps the source alive; correct data;
//   R4: truncated barrier batch, BEGIN_ONLY split barrier and a last state of
//     COMMON leave requests ARMED (no stale StateBefore);
//   R6: subresource out of range and a misaligned BC subrect are refused
//     before anything reaches the GPU;
//   scope: stop() ends armed leases (SESSION_ENDED); requests need a session;
//   the game's states are restored; READY data expires when not released.
// C9B-REVIEW-2:
//   #1 one worker for the whole process however many requests; the process
//      exits cleanly with live entries and the worker running;
//   #2 the source reference exists BEFORE the game's submission returns: at
//      that boundary the game drops its last reference and the copy still
//      succeeds;
//   #3 with the worker paused (no maintenance tick to mask it): READY data is
//      refused right after stop(), and a request armed in an earlier session
//      is never serviced by a submission in the next one;
//   #4 typeless depth (R32_TYPELESS + ALLOW_DEPTH_STENCIL): partial refused,
//      whole subresource copied.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/host_observe_samples.h"
#include "host_dx12/observe/producers.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void __cdecl Sink(const na_event_batch*, void*) {}

// #2 boundary probe: runs after the game's ExecuteCommandLists returned and
// before servicing. Counts references (the host's must already be there) and
// drops the game's last one.
test::ComPtr<ID3D12Resource> g_victim;
ULONG g_refs_at_boundary = 0;
bool g_dropped = false;
void AcquireProbe()
{
    if (!g_victim) return;
    g_victim->AddRef();
    g_refs_at_boundary = g_victim->Release();
    g_victim.Reset();
    g_dropped = true;
}

na_sample_info Poll(uint64_t token)
{
    na_sample_info i{};
    i.size_bytes = sizeof(i);
    if (na_observe_sample_poll(token, &i) != NA_OK) i.status = 0;
    return i;
}
na_sample_info WaitDone(uint64_t token, int ms = 5000)
{
    na_sample_info i{};
    for (int k = 0; k < ms / 5; ++k) {
        i = Poll(token);
        if (i.status != NA_SAMPLE_ARMED && i.status != NA_SAMPLE_IN_FLIGHT) break;
        Sleep(5);
    }
    return i;
}
std::vector<uint8_t> Read(uint64_t token, const na_sample_info& i)
{
    std::vector<uint8_t> v(static_cast<size_t>(i.data_bytes));
    uint64_t got = 0;
    if (v.empty() || na_observe_sample_read(token, v.data(), v.size(), &got) != NA_OK || got != v.size()) v.clear();
    return v;
}
uint64_t Request(na_object_id res, uint32_t sub = 0, uint32_t x = 0, uint32_t y = 0, uint32_t w = 0, uint32_t h = 0, uint32_t expiry = 0)
{
    na_sample_request r{};
    r.size_bytes = sizeof(r);
    r.resource = res;
    r.subresource = sub;
    r.x = x; r.y = y; r.width = w; r.height = h;
    r.expiry_ms = expiry;
    uint64_t t = 0;
    return na_observe_sample_request(&r, &t) == NA_OK ? t : 0;
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("observe_sample_test");
    ID3D12Device* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    {
        na_sample_request r{};
        r.size_bytes = sizeof(r);
        uint64_t t = 0;
        CHECK(na_observe_sample_request(&r, &t) == NA_E_STATE); // no session yet
    }
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);

    const uint32_t W = 64, H = 32;
    auto color = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = test::CreateTexture(dev, 32, 16, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto bc = test::CreateTexture(dev, 16, 16, DXGI_FORMAT_BC1_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(color && depth && bc);
    test::ComPtr<ID3D12DescriptorHeap> rtvh, dsvh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh))));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh))));
    dev->CreateRenderTargetView(color.Get(), nullptr, rtvh->GetCPUDescriptorHandleForHeapStart());
    dev->CreateDepthStencilView(depth.Get(), nullptr, dsvh->GetCPUDescriptorHandleForHeapStart());
    const na_object_id color_id = na::observe::IdOf(color.Get()), depth_id = na::observe::IdOf(depth.Get()), bc_id = na::observe::IdOf(bc.Get());
    REQUIRE(color_id && depth_id && bc_id);
    CHECK(Request(0xDEAD0000ull) == 0);

    test::ComPtr<ID3D12CommandAllocator> alloc;
    test::ComPtr<ID3D12GraphicsCommandList> list;
    REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))));
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* ls[] = {list.Get()};
    const auto must = [](bool ok, const char* what) { // REQUIRE returns from main; lambdas exit instead
        if (!ok) {
            std::printf("FATAL %s\n", what);
            std::exit(1);
        }
    };
    const auto record = [&](auto&& body) {
        d.WaitIdle();
        must(SUCCEEDED(alloc->Reset()), "allocator reset");
        must(SUCCEEDED(list->Reset(alloc.Get(), nullptr)), "list reset");
        body();
        must(SUCCEEDED(list->Close()), "list close");
    };

    // ---- happy path ----
    const uint64_t t_color = Request(color_id), t_sub = Request(color_id, 0, 8, 4, 16, 8), t_depth = Request(depth_id);
    const uint64_t t_bad_sub = Request(color_id, 5);          // R6: only 1 mip, 1 slice
    const uint64_t t_bc = Request(bc_id, 0, 2, 0, 4, 4);      // R6: BC subrect origin not 4-aligned
    REQUIRE(t_color && t_sub && t_depth && t_bad_sub && t_bc);
    CHECK(Poll(t_color).status == NA_SAMPLE_ARMED);
    record([&] {
        D3D12_RESOURCE_BARRIER b[3] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                       test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE),
                                       test::Transition(bc.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(3, b);
        const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        list->ClearRenderTargetView(rtvh->GetCPUDescriptorHandleForHeapStart(), clear, 0, nullptr);
        list->ClearDepthStencilView(dsvh->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.375f, 0, 0, nullptr);
    });
    d.queue->ExecuteCommandLists(1, ls);
    const na_sample_info ci = WaitDone(t_color);
    std::printf("color sample: status %u reason %u %ux%u pitch %u state_before 0x%x ticket %llu\n", ci.status, ci.reason, ci.width, ci.height,
                ci.row_pitch, ci.state_before, static_cast<unsigned long long>(ci.queue_ticket));
    CHECK(ci.status == NA_SAMPLE_READY && ci.width == W && ci.height == H && ci.row_pitch >= W * 4 && ci.state_before == D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(ci.queue == na::observe::IdOf(d.queue.Get()) && ci.queue_ticket != 0 && ci.session != 0);
    {
        const auto px = Read(t_color, ci);
        bool all = !px.empty();
        for (uint32_t y = 0; all && y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* p = px.data() + y * ci.row_pitch + x * 4;
                all = all && p[0] >= 63 && p[0] <= 65 && p[1] >= 127 && p[1] <= 129 && p[2] >= 190 && p[2] <= 192 && p[3] == 255;
            }
        CHECK(all);
        uint8_t tiny[4];
        uint64_t got = 0;
        CHECK(na_observe_sample_read(t_color, tiny, sizeof(tiny), &got) == NA_E_ARG && got == 0); // too small: refused, nothing written
    }
    const na_sample_info si = WaitDone(t_sub);
    CHECK(si.status == NA_SAMPLE_READY && si.x == 8 && si.y == 4 && si.width == 16 && si.height == 8);
    const na_sample_info di = WaitDone(t_depth);
    std::printf("depth sample: status %u %ux%u state_before 0x%x\n", di.status, di.width, di.height, di.state_before);
    CHECK(di.status == NA_SAMPLE_READY && di.state_before == D3D12_RESOURCE_STATE_DEPTH_WRITE);
    {
        const auto px = Read(t_depth, di);
        float v = 0;
        if (px.size() >= 5 * di.row_pitch + 8 * 4) std::memcpy(&v, px.data() + 5 * di.row_pitch + 7 * 4, 4);
        CHECK(v == 0.375f);
    }
    CHECK(WaitDone(t_bad_sub).status == NA_SAMPLE_FAILED && Poll(t_bad_sub).reason == NA_SAMPLE_E_UNSUPPORTED);
    CHECK(WaitDone(t_bc).status == NA_SAMPLE_FAILED && Poll(t_bc).reason == NA_SAMPLE_E_UNSUPPORTED);
    for (uint64_t t : {t_color, t_sub, t_depth, t_bad_sub, t_bc}) na_observe_sample_release(t);
    CHECK(na_observe_sample_release(t_color) == NA_OK); // idempotent

    // ---- R4: no stale state ----
    {
        // (a) last explicit state COMMON (implicit promotion possible) -> stays armed
        const uint64_t t_common = Request(color_id);
        record([&] {
            D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &b);
        });
        d.queue->ExecuteCommandLists(1, ls);
        d.WaitIdle();
        Sleep(20);
        CHECK(Poll(t_common).status == NA_SAMPLE_ARMED);
        // (b) a split barrier counts only once completed: BEGIN_ONLY then
        //     END_ONLY -> the END state is the evidence.
        record([&] {
            D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &b);
            D3D12_RESOURCE_BARRIER s = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            s.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
            list->ResourceBarrier(1, &s);
            s.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
            list->ResourceBarrier(1, &s);
        });
        d.queue->ExecuteCommandLists(1, ls);
        const na_sample_info sp = WaitDone(t_common);
        CHECK(sp.status == NA_SAMPLE_READY && sp.state_before == D3D12_RESOURCE_STATE_COPY_SOURCE);
        na_observe_sample_release(t_common);
        // (c) truncated barrier batch (more transitions than the host records)
        //     -> the recording's evidence is invalid, the request stays armed.
        std::vector<test::ComPtr<ID3D12Resource>> many;
        for (int i = 0; i < 70; ++i)
            many.push_back(test::CreateTexture(dev, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE));
        const uint64_t t_trunc = Request(color_id);
        record([&] {
            std::vector<D3D12_RESOURCE_BARRIER> bs;
            bs.push_back(test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
            for (auto& r : many) bs.push_back(test::Transition(r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
            list->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
        });
        d.queue->ExecuteCommandLists(1, ls);
        d.WaitIdle();
        Sleep(20);
        CHECK(Poll(t_trunc).status == NA_SAMPLE_ARMED);
        na_observe_sample_release(t_trunc);
        // color is RENDER_TARGET again.
    }

    // ---- R1 + R2: stalled copy, double release, game drops its reference ----
    {
        auto victim = test::CreateTexture(dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        const na_object_id vid = na::observe::IdOf(victim.Get());
        test::ComPtr<ID3D12Fence> hold;
        REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold))));
        const uint64_t t_r1 = Request(color_id), t_r2 = Request(vid);
        REQUIRE(t_r1 && t_r2);
        const uint64_t live_before = na::observe::GetProducerStats().samples_live;
        d.WaitIdle();
        REQUIRE(SUCCEEDED(alloc->Reset()));
        REQUIRE(SUCCEEDED(list->Reset(alloc.Get(), nullptr)));
        D3D12_RESOURCE_BARRIER b[2] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                       test::Transition(victim.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        list->ResourceBarrier(2, b);
        REQUIRE(SUCCEEDED(list->Close()));
        REQUIRE(SUCCEEDED(d.queue->Wait(hold.Get(), 1)));   // stall everything after this
        d.queue->ExecuteCommandLists(1, ls);                 // game work + both host copies queue up behind it
        Sleep(30);
        CHECK(Poll(t_r1).status == NA_SAMPLE_IN_FLIGHT && Poll(t_r2).status == NA_SAMPLE_IN_FLIGHT);
        CHECK(na_observe_sample_release(t_r1) == NA_OK && na_observe_sample_release(t_r1) == NA_OK); // R1: twice
        Sleep(30);
        CHECK(na::observe::GetProducerStats().samples_live == live_before); // still held: the GPU has not run it
        victim.Reset();                                      // R2: the game's last reference goes away
        REQUIRE(SUCCEEDED(hold->Signal(1)));
        d.WaitIdle();
        const na_sample_info r2 = WaitDone(t_r2);
        CHECK(r2.status == NA_SAMPLE_READY && r2.state_before == D3D12_RESOURCE_STATE_COPY_SOURCE);
        na_observe_sample_release(t_r2);
        for (int k = 0; k < 200 && na::observe::GetProducerStats().samples_live != 0; ++k) Sleep(5);
        CHECK(na::observe::GetProducerStats().samples_live == 0); // freed after completion
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
    }

    // ---- states restored; expiry ----
    record([&] {
        D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &b);
    });
    const uint64_t t_exp = Request(color_id, 0, 0, 0, 0, 0, 50);
    d.queue->ExecuteCommandLists(1, ls);
    CHECK(WaitDone(t_exp).status == NA_SAMPLE_READY);
    Sleep(150);
    CHECK(Poll(t_exp).status == NA_SAMPLE_EXPIRED);
    na_observe_sample_release(t_exp);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);

    // ---- C9B-REVIEW-2 #2: source acquired before the game's submission ----
    {
        g_victim = test::CreateTexture(dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        REQUIRE(g_victim);
        const na_object_id vid = na::observe::IdOf(g_victim.Get());
        const uint64_t t_acq = Request(vid);
        REQUIRE(vid && t_acq);
        record([&] {
            D3D12_RESOURCE_BARRIER b = test::Transition(g_victim.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
        });
        na::observe::SetSampleAcquireProbeForTests(&AcquireProbe);
        d.queue->ExecuteCommandLists(1, ls);
        na::observe::SetSampleAcquireProbeForTests(nullptr);
        std::printf("acquire boundary: references at the boundary %lu, game reference dropped %d\n", g_refs_at_boundary, g_dropped ? 1 : 0);
        CHECK(g_dropped && g_refs_at_boundary >= 2); // the host's reference predates the boundary
        const na_sample_info ai = WaitDone(t_acq);
        CHECK(ai.status == NA_SAMPLE_READY && ai.state_before == D3D12_RESOURCE_STATE_COPY_SOURCE);
        na_observe_sample_release(t_acq);
        d.WaitIdle();
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
    }

    // ---- C9B-REVIEW-2 #3: scope at each operation (worker paused) ----
    {
        const auto bounce = [&] { // evidence: RENDER_TARGET; the resource ends where it started
            D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
            b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &b);
        };
        // (a) READY data, then stop(): read refused at once, poll reports the end.
        const uint64_t t_ready = Request(color_id);
        REQUIRE(t_ready);
        record(bounce);
        d.queue->ExecuteCommandLists(1, ls);
        const na_sample_info ri = WaitDone(t_ready);
        CHECK(ri.status == NA_SAMPLE_READY);
        na::observe::SetSampleWorkerPausedForTests(true);
        REQUIRE(na_observe_stop() == NA_OK);
        {
            std::vector<uint8_t> buf(static_cast<size_t>(ri.data_bytes ? ri.data_bytes : 1));
            uint64_t got = 0;
            CHECK(na_observe_sample_read(t_ready, buf.data(), buf.size(), &got) == NA_E_STATE && got == 0);
            const na_sample_info pi = Poll(t_ready);
            std::printf("scope after stop: read refused, poll status %u reason %u\n", pi.status, pi.reason);
            CHECK(pi.status == NA_SAMPLE_FAILED && pi.reason == NA_SAMPLE_E_SESSION_ENDED);
        }
        na_observe_sample_release(t_ready);
        // (b) armed in session N, submission with evidence in session N+1:
        //     never serviced (no host copy, no queue ticket).
        REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
        const uint64_t t_old = Request(color_id);
        REQUIRE(t_old);
        REQUIRE(na_observe_stop() == NA_OK);
        REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
        record(bounce);
        d.queue->ExecuteCommandLists(1, ls);
        d.WaitIdle();
        const na_sample_info oi = Poll(t_old);
        std::printf("scope across restart: status %u reason %u ticket %llu\n", oi.status, oi.reason, static_cast<unsigned long long>(oi.queue_ticket));
        CHECK(oi.status == NA_SAMPLE_FAILED && oi.reason == NA_SAMPLE_E_SESSION_ENDED && oi.queue_ticket == 0);
        na::observe::SetSampleWorkerPausedForTests(false);
        na_observe_sample_release(t_old);
    }

    // ---- C9B-REVIEW-2 #4: typeless depth ----
    {
        auto tdepth = test::CreateTexture(dev, 32, 16, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        REQUIRE(tdepth);
        const na_object_id tid = na::observe::IdOf(tdepth.Get());
        REQUIRE(tid);
        const uint64_t t_part = Request(tid, 0, 0, 0, 8, 8), t_whole = Request(tid);
        REQUIRE(t_part && t_whole);
        record([&] {
            D3D12_RESOURCE_BARRIER b = test::Transition(tdepth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            list->ResourceBarrier(1, &b);
        });
        d.queue->ExecuteCommandLists(1, ls);
        const na_sample_info pi = WaitDone(t_part), wi = WaitDone(t_whole);
        std::printf("typeless depth: partial status %u reason %u, whole status %u %ux%u\n", pi.status, pi.reason, wi.status, wi.width, wi.height);
        CHECK(pi.status == NA_SAMPLE_FAILED && pi.reason == NA_SAMPLE_E_UNSUPPORTED);
        CHECK(wi.status == NA_SAMPLE_READY && wi.width == 32 && wi.height == 16);
        na_observe_sample_release(t_part);
        na_observe_sample_release(t_whole);
        d.WaitIdle();
    }

    // ---- C9B-REVIEW-2 #1: one worker ----
    std::printf("sample workers started: %u\n", na::observe::GetProducerStats().sample_workers_started);
    CHECK(na::observe::GetProducerStats().sample_workers_started == 1);

    // ---- scope: stop ends armed leases ----
    const uint64_t t_scope = Request(color_id);
    REQUIRE(t_scope);
    REQUIRE(na_observe_stop() == NA_OK);
    const na_sample_info sc = WaitDone(t_scope);
    CHECK(sc.status == NA_SAMPLE_FAILED && sc.reason == NA_SAMPLE_E_SESSION_ENDED);
    na_observe_sample_release(t_scope);

    // ---- C9B-REVIEW-2 #1: clean teardown with a live entry and the worker running ----
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
    CHECK(Request(color_id) != 0);
    REQUIRE(na_observe_stop() == NA_OK);
    std::printf("teardown: exiting with %llu live sample(s)\n", static_cast<unsigned long long>(na::observe::GetProducerStats().samples_live));
    return test::finish("observe_sample_test");
}
