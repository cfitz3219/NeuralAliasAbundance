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
// C9B-REVIEW-4: a worker that cannot start refuses the request (nothing armed,
//   no worker), and the next request starts it.
// Gate 3 producer qualification (previously source-reviewed only):
//   R3 interleaving: while the host is between the game's evidence submission
//      and its copy, another thread's submission on the same queue is blocked;
//   signal failure: copy submitted but the fence signal refused -> FAILED /
//      SUBMIT_FAILED, entry quarantined (kept) even after release;
//   packed depth: D24S8 / D32S8X24 copy plane 0 (depth) only, reported in the
//      plane's own format, with the cleared depth value;
//   C10 execution-time constant data: an upload-buffer range bound by the
//      submission is copied on the GPU timeline after it; when the CPU rewrites
//      the range between submission and execution, the sample holds the version
//      the GPU read (not the submission-time CPU bytes); an unbound range stays
//      armed;
//   R8: two threads each holding their queue's submission lock re-enter the
//      other queue: no deadlock, and since each re-entry began inside the
//      other queue's copy window, neither sample is copied (both stay ARMED);
//      a nested operation after a copy was submitted leaves it READY.
// Argon Gate 3 (G3-03/06/07):
//   G3-03: a texture and a shader-visible heap created BEFORE the producers
//      were installed are registered on first sight (barrier, SetDescriptorHeaps)
//      and the texture can be sampled;
//   G3-06: a barrier batch larger than the 64-entry wire event still provides
//      evidence for its 71st transition; overflowing a recording's evidence
//      store still leaves requests armed; a state set in an EARLIER submission
//      on the same queue, or a texture's creation state, is used when the
//      sampled submission has no barrier; a resource also transitioned on a
//      second queue is never sampled from carried state;
//   G3-07: host warm-up objects are not registered as game objects; a
//      released quarantined entry no longer consumes one of the 8 leases.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/host_observe_samples.h"
#include "host_dx12/observe/producers.hpp"

#include <atomic>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <vector>

namespace {
void __cdecl Sink(const na_event_batch*, void*) {}

// #2 boundary probe: runs after the game's ExecuteCommandLists returned and
// before servicing. Counts references (the host's must already be there) and
// drops the game's last one.
// R3 interleaving probe: runs on the submitting thread while it holds the
// queue's submission lock, between the game's submission and the host copy.
HANDLE g_b_go = nullptr;
std::atomic<bool> g_b_started{false}, g_b_returned{false}, g_b_blocked_during_copy{false};
void InterleaveProbe()
{
    SetEvent(g_b_go);
    for (int i = 0; i < 1000 && !g_b_started.load(); ++i) Sleep(1);
    Sleep(50); // ample time for an unserialized submission to complete
    g_b_blocked_during_copy.store(g_b_started.load() && !g_b_returned.load());
}

// R8 probe: each submitting thread, holding its own queue's submission lock,
// waits for the other, then performs an observed queue op on the OTHER queue
// (what a driver re-entering another queue does).
thread_local int t_role = 0;
std::atomic<int> g_cross_arrived{0};
std::atomic<int> g_cross_signaled{0};
ID3D12CommandQueue* g_cross_queue[2] = {};
test::ComPtr<ID3D12Fence> g_cross_fence[2];
void CrossProbe()
{
    ++g_cross_arrived;
    for (int i = 0; i < 2000 && g_cross_arrived.load() < 2; ++i) Sleep(1);
    const int other = 1 - t_role;
    g_cross_queue[other]->Signal(g_cross_fence[other].Get(), 1);
    ++g_cross_signaled;
    for (int i = 0; i < 2000 && g_cross_signaled.load() < 2; ++i) Sleep(1);
}

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
    // G3-03: objects the game created before the producers were installed.
    auto pre = test::CreateTexture(dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    test::ComPtr<ID3D12DescriptorHeap> pre_heap;
    {
        D3D12_DESCRIPTOR_HEAP_DESC phd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        REQUIRE(pre && SUCCEEDED(dev->CreateDescriptorHeap(&phd, IID_PPV_ARGS(&pre_heap))));
    }
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    {
        // G3-07: warm-up lists/allocators and map-hook probe buffers (at least
        // ten objects) are host work, not game objects.
        const auto st = na::observe::GetProducerStats();
        std::printf("after install: objects created %llu live %llu\n", static_cast<unsigned long long>(st.objects_created),
                    static_cast<unsigned long long>(st.objects_live));
        CHECK(st.objects_created < 8);
    }
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
    // C9B-REVIEW-4: the worker cannot start -> refused, nothing armed, no worker; a later request retries.
    na::observe::SetSampleWorkerStartFailsForTests(true);
    CHECK(Request(color_id) == 0);
    std::printf("worker start failure: request refused, live %llu, workers %u\n",
                static_cast<unsigned long long>(na::observe::GetProducerStats().samples_live), na::observe::GetProducerStats().sample_workers_started);
    CHECK(na::observe::GetProducerStats().samples_live == 0 && na::observe::GetProducerStats().sample_workers_started == 0);
    na::observe::SetSampleWorkerStartFailsForTests(false);

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
        // (c) G3-06: a batch larger than the 64-entry wire event. The event is
        //     TRUNCATED, but the host records every transition: the colour
        //     transition at index 70 (beyond the wire limit) is evidence.
        std::vector<test::ComPtr<ID3D12Resource>> many;
        for (int i = 0; i < 70; ++i)
            many.push_back(test::CreateTexture(dev, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE));
        const uint64_t t_big = Request(color_id);
        record([&] {
            std::vector<D3D12_RESOURCE_BARRIER> bs;
            for (auto& r : many) bs.push_back(test::Transition(r.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
            bs.push_back(test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
            list->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
        });
        d.queue->ExecuteCommandLists(1, ls);
        const na_sample_info big = WaitDone(t_big);
        std::printf("71-barrier batch: sample status %u state_before 0x%x\n", big.status, big.state_before);
        CHECK(big.status == NA_SAMPLE_READY && big.state_before == D3D12_RESOURCE_STATE_RENDER_TARGET);
        na_observe_sample_release(t_big);
        // (d) overflowing the recording's evidence store (8192 transitions)
        //     -> the evidence is invalid, the request stays armed.
        const uint64_t t_over = Request(color_id);
        record([&] {
            std::vector<D3D12_RESOURCE_BARRIER> bs;
            bs.push_back(test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
            for (int k = 0; k < 4150; ++k) {
                bs.push_back(test::Transition(many[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
                bs.push_back(test::Transition(many[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
            }
            bs.push_back(test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
            list->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
        });
        d.queue->ExecuteCommandLists(1, ls);
        d.WaitIdle();
        Sleep(20);
        CHECK(Poll(t_over).status == NA_SAMPLE_ARMED);
        na_observe_sample_release(t_over);
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

    // ---- G3-03: objects created before installation, registered on first sight ----
    {
        CHECK(na::observe::IdOf(pre.Get()) == 0 && na::observe::IdOf(pre_heap.Get()) == 0);
        record([&] {
            ID3D12DescriptorHeap* hs[] = {pre_heap.Get()};
            list->SetDescriptorHeaps(1, hs);
            D3D12_RESOURCE_BARRIER bb = test::Transition(pre.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &bb);
        });
        const na_object_id pre_id = na::observe::IdOf(pre.Get()), heap_id = na::observe::IdOf(pre_heap.Get());
        const auto st = na::observe::GetProducerStats();
        std::printf("preexisting: texture id %llu heap id %llu (registered resources %llu heaps %llu)\n", static_cast<unsigned long long>(pre_id),
                    static_cast<unsigned long long>(heap_id), static_cast<unsigned long long>(st.preexisting_resources),
                    static_cast<unsigned long long>(st.preexisting_heaps));
        CHECK(pre_id != 0 && heap_id != 0 && st.preexisting_resources >= 1 && st.preexisting_heaps >= 1);
        // The registered texture is sampleable: the recording above (closed,
        // not yet submitted) transitions it to RENDER_TARGET.
        const uint64_t t_pre = Request(pre_id);
        REQUIRE(t_pre);
        d.queue->ExecuteCommandLists(1, ls);
        const na_sample_info pi = WaitDone(t_pre);
        std::printf("preexisting texture sample: status %u state_before 0x%x\n", pi.status, pi.state_before);
        CHECK(pi.status == NA_SAMPLE_READY && pi.state_before == D3D12_RESOURCE_STATE_RENDER_TARGET);
        na_observe_sample_release(t_pre);
        d.WaitIdle();
    }

    // ---- G3-06: state carried from earlier submissions / creation, one queue only ----
    {
        const uint64_t known0 = na::observe::GetProducerStats().sample_known_state;
        // (a) transitioned in an earlier submission; the sampled one has no barrier
        auto kt = test::CreateTexture(dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        // (b) created in a definite state, never transitioned
        auto ct = test::CreateTexture(dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        // (c) transitioned on this queue, then on a second queue
        auto mq = test::CreateTexture(dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        REQUIRE(kt && ct && mq);
        record([&] {
            D3D12_RESOURCE_BARRIER bb[2] = {test::Transition(kt.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                            test::Transition(mq.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
            list->ResourceBarrier(2, bb);
        });
        d.queue->ExecuteCommandLists(1, ls); // no samples armed: only history
        d.WaitIdle();
        {
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            test::ComPtr<ID3D12CommandQueue> q2;
            test::ComPtr<ID3D12CommandAllocator> a2;
            test::ComPtr<ID3D12GraphicsCommandList> l2;
            REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q2))));
            REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a2))));
            REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a2.Get(), nullptr, IID_PPV_ARGS(&l2))));
            REQUIRE(SUCCEEDED(l2->Close()));
            REQUIRE(SUCCEEDED(a2->Reset()));
            REQUIRE(SUCCEEDED(l2->Reset(a2.Get(), nullptr)));
            D3D12_RESOURCE_BARRIER bb = test::Transition(mq.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            l2->ResourceBarrier(1, &bb);
            REQUIRE(SUCCEEDED(l2->Close()));
            ID3D12CommandList* l2s[] = {l2.Get()};
            q2->ExecuteCommandLists(1, l2s);
            test::ComPtr<ID3D12Fence> f;
            REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))));
            REQUIRE(SUCCEEDED(q2->Signal(f.Get(), 1)));
            for (int i = 0; i < 500 && f->GetCompletedValue() < 1; ++i) Sleep(2);
        }
        const uint64_t t_kt = Request(na::observe::IdOf(kt.Get())), t_ct = Request(na::observe::IdOf(ct.Get())),
                       t_mq = Request(na::observe::IdOf(mq.Get()));
        REQUIRE(t_kt && t_ct && t_mq);
        record([&] {}); // a complete submission with no barriers at all
        d.queue->ExecuteCommandLists(1, ls);
        const na_sample_info ki = WaitDone(t_kt), cti = WaitDone(t_ct);
        d.WaitIdle();
        Sleep(20);
        const na_sample_info mi = Poll(t_mq);
        const uint64_t used = na::observe::GetProducerStats().sample_known_state - known0;
        std::printf("known state: earlier submission %u (0x%x), creation state %u (0x%x), second-queue resource %u, carried copies %llu\n", ki.status,
                    ki.state_before, cti.status, cti.state_before, mi.status, static_cast<unsigned long long>(used));
        CHECK(ki.status == NA_SAMPLE_READY && ki.state_before == D3D12_RESOURCE_STATE_RENDER_TARGET);
        CHECK(cti.status == NA_SAMPLE_READY && cti.state_before == D3D12_RESOURCE_STATE_RENDER_TARGET);
        CHECK(mi.status == NA_SAMPLE_ARMED); // transitioned by two queues: never carried
        CHECK(used >= 2);
        for (uint64_t t : {t_kt, t_ct, t_mq}) na_observe_sample_release(t);
        d.WaitIdle();
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
    }

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

    // ---- Gate 3: R3 interleaving (deterministic: B can only finish early if unserialized) ----
    {
        test::ComPtr<ID3D12CommandAllocator> alloc_b;
        test::ComPtr<ID3D12GraphicsCommandList> list_b;
        REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_b))));
        REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_b.Get(), nullptr, IID_PPV_ARGS(&list_b))));
        REQUIRE(SUCCEEDED(list_b->Close()));
        g_b_go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        REQUIRE(g_b_go);
        std::thread b([&] {
            WaitForSingleObject(g_b_go, 5000);
            ID3D12CommandList* lb[] = {list_b.Get()};
            g_b_started.store(true);
            d.queue->ExecuteCommandLists(1, lb); // must wait for the host copy
            g_b_returned.store(true);
        });
        const uint64_t t_il = Request(color_id);
        REQUIRE(t_il);
        record([&] {
            D3D12_RESOURCE_BARRIER bb = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &bb);
            bb = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &bb);
        });
        na::observe::SetSampleAcquireProbeForTests(&InterleaveProbe);
        d.queue->ExecuteCommandLists(1, ls);
        na::observe::SetSampleAcquireProbeForTests(nullptr);
        b.join();
        CloseHandle(g_b_go);
        const na_sample_info ii = WaitDone(t_il);
        std::printf("interleaving: other thread started %d, blocked during copy %d, returned after %d; sample status %u state_before 0x%x\n",
                    g_b_started.load() ? 1 : 0, g_b_blocked_during_copy.load() ? 1 : 0, g_b_returned.load() ? 1 : 0, ii.status, ii.state_before);
        CHECK(g_b_started.load() && g_b_blocked_during_copy.load() && g_b_returned.load());
        CHECK(ii.status == NA_SAMPLE_READY && ii.state_before == D3D12_RESOURCE_STATE_RENDER_TARGET);
        na_observe_sample_release(t_il);
        d.WaitIdle();
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
    }

    // ---- ASTRA-P3-R8: cross-queue re-entry while both queues hold submission locks ----
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        test::ComPtr<ID3D12CommandQueue> q2;
        REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q2))));
        test::ComPtr<ID3D12CommandAllocator> alloc2;
        test::ComPtr<ID3D12GraphicsCommandList> list2;
        REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc2))));
        REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc2.Get(), nullptr, IID_PPV_ARGS(&list2))));
        auto tex2 = test::CreateTexture(dev, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        REQUIRE(tex2);
        REQUIRE(SUCCEEDED(list2->Close()));
        REQUIRE(SUCCEEDED(alloc2->Reset()));
        REQUIRE(SUCCEEDED(list2->Reset(alloc2.Get(), nullptr))); // a recording that begins under observation
        {
            D3D12_RESOURCE_BARRIER bb = test::Transition(tex2.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list2->ResourceBarrier(1, &bb);
            REQUIRE(SUCCEEDED(list2->Close()));
        }
        record([&] {
            D3D12_RESOURCE_BARRIER bb = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &bb);
            bb = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &bb);
        });
        REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_cross_fence[0]))));
        REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_cross_fence[1]))));
        g_cross_queue[0] = d.queue.Get();
        g_cross_queue[1] = q2.Get();
        const uint64_t t1 = Request(color_id), t2 = Request(na::observe::IdOf(tex2.Get()));
        REQUIRE(t1 && t2);
        const uint64_t nested0 = na::observe::GetProducerStats().nested_unlocked;
        na::observe::SetSampleAcquireProbeForTests(&CrossProbe);
        std::atomic<int> done{0};
        std::thread a([&] { t_role = 0; ID3D12CommandList* l1[] = {list.Get()}; d.queue->ExecuteCommandLists(1, l1); ++done; });
        std::thread b([&] { t_role = 1; ID3D12CommandList* l2[] = {list2.Get()}; q2->ExecuteCommandLists(1, l2); ++done; });
        for (int i = 0; i < 1000 && done.load() < 2; ++i) Sleep(10);
        if (done.load() < 2) { // the pre-R8 code deadlocks here: never hang the suite
            std::printf("FATAL cross-queue re-entry deadlock (both submission locks held)\n");
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 1);
        }
        a.join();
        b.join();
        na::observe::SetSampleAcquireProbeForTests(nullptr);
        const na_sample_info c1 = Poll(t1), c2 = Poll(t2);
        const uint64_t nested = na::observe::GetProducerStats().nested_unlocked - nested0;
        std::printf("cross-queue re-entry: both threads finished, nested ops without lock %llu, samples %u/%u\n",
                    static_cast<unsigned long long>(nested), c1.status, c2.status);
        CHECK(g_cross_signaled.load() == 2 && nested >= 2);
        CHECK(c1.status == NA_SAMPLE_ARMED && c2.status == NA_SAMPLE_ARMED);
        na_observe_sample_release(t1);
        na_observe_sample_release(t2);
        test::ComPtr<ID3D12Fence> f;
        REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))));
        REQUIRE(SUCCEEDED(q2->Signal(f.Get(), 1)));
        for (int i = 0; i < 500 && f->GetCompletedValue() < 1; ++i) Sleep(2);
        d.WaitIdle();
        g_cross_fence[0].Reset();
        g_cross_fence[1].Reset();
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
    }

    // ---- packed depth-stencil: plane 0 (depth) copied, stencil untouched ----
    {
        test::ComPtr<ID3D12DescriptorHeap> pdsv;
        D3D12_DESCRIPTOR_HEAP_DESC pd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&pd, IID_PPV_ARGS(&pdsv))));
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const DXGI_FORMAT fmts[2] = {DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
        for (int k = 0; k < 2; ++k) {
            auto ds = test::CreateTexture(dev, 32, 16, fmts[k], D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            REQUIRE(ds);
            const D3D12_CPU_DESCRIPTOR_HANDLE h{pdsv->GetCPUDescriptorHandleForHeapStart().ptr + k * inc};
            dev->CreateDepthStencilView(ds.Get(), nullptr, h);
            const na_object_id dsid = na::observe::IdOf(ds.Get());
            REQUIRE(dsid);
            const uint64_t t_pl = Request(dsid);
            REQUIRE(t_pl);
            record([&] {
                list->ClearDepthStencilView(h, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.375f, 0x5A, 0, nullptr);
                D3D12_RESOURCE_BARRIER bb = test::Transition(ds.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
                list->ResourceBarrier(1, &bb);
            });
            d.queue->ExecuteCommandLists(1, ls);
            const na_sample_info pi = WaitDone(t_pl);
            double v = -1;
            const auto px = pi.status == NA_SAMPLE_READY ? Read(t_pl, pi) : std::vector<uint8_t>{};
            if (px.size() >= 5 * pi.row_pitch + 8 * 4) {
                uint32_t raw = 0;
                std::memcpy(&raw, px.data() + 5 * pi.row_pitch + 7 * 4, 4);
                if (pi.format == DXGI_FORMAT_R32_TYPELESS || pi.format == DXGI_FORMAT_R32_FLOAT || pi.format == DXGI_FORMAT_D32_FLOAT) {
                    float f = 0;
                    std::memcpy(&f, &raw, 4);
                    v = f;
                } else if (pi.format == DXGI_FORMAT_R24G8_TYPELESS || pi.format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
                           pi.format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                    v = double(raw & 0xFFFFFFu) / double(0xFFFFFF);
                }
            }
            std::printf("packed depth %s: status %u reason %u plane format %u %ux%u state_before 0x%x depth %.6f\n", k ? "D32S8X24" : "D24S8", pi.status,
                        pi.reason, pi.format, pi.width, pi.height, pi.state_before, v);
            CHECK(pi.status == NA_SAMPLE_READY && pi.width == 32 && pi.height == 16 && pi.state_before == D3D12_RESOURCE_STATE_DEPTH_READ);
            // Unambiguous plane format (both footprints are R32_TYPELESS in D3D12).
            CHECK(pi.format == static_cast<uint32_t>(k ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS));
            CHECK(v > 0.37499 && v < 0.37501);
            na_observe_sample_release(t_pl);
            d.WaitIdle();
        }
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
    }

    // ---- C10: execution-time constant data (upload buffer range) ----
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor = {0, 0};
        D3D12_ROOT_SIGNATURE_DESC rsd{1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        test::ComPtr<ID3DBlob> blob, err;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
        test::ComPtr<ID3D12RootSignature> rs;
        REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))));
        auto cb = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        REQUIRE(cb);
        float* cpu = nullptr;
        REQUIRE(SUCCEEDED(cb->Map(0, nullptr, reinterpret_cast<void**>(&cpu))));
        for (int i = 0; i < 16; ++i) cpu[64 + i] = float(i + 1); // version A at byte 256 (submission time)
        const na_object_id cbid = na::observe::IdOf(cb.Get());
        REQUIRE(cbid);
        {
            na_sample_request bad{};
            bad.size_bytes = sizeof(bad);
            bad.resource = cbid;
            uint64_t tok = 0;
            CHECK(na_observe_sample_request(&bad, &tok) == NA_E_ARG); // buffer: width (byte count) required
        }
        const uint64_t t_cb = Request(cbid, 0, 256, 0, 64, 1), t_unbound = Request(cbid, 0, 2048, 0, 64, 1);
        REQUIRE(t_cb && t_unbound);
        record([&] {
            list->SetGraphicsRootSignature(rs.Get());
            list->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress() + 256);
        });
        test::ComPtr<ID3D12Fence> hold;
        REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold))));
        REQUIRE(SUCCEEDED(d.queue->Wait(hold.Get(), 1))); // the GPU has not executed anything below yet
        d.queue->ExecuteCommandLists(1, ls);
        for (int i = 0; i < 16; ++i) cpu[64 + i] = float(100 + i); // version B: what the GPU will actually read
        REQUIRE(SUCCEEDED(hold->Signal(1)));
        d.WaitIdle();
        const na_sample_info bi = WaitDone(t_cb);
        const auto bytes = bi.status == NA_SAMPLE_READY ? Read(t_cb, bi) : std::vector<uint8_t>{};
        bool is_b = bytes.size() == 64, is_a = bytes.size() == 64;
        for (int i = 0; i < 16 && bytes.size() == 64; ++i) {
            float f = 0;
            std::memcpy(&f, bytes.data() + i * 4, 4);
            is_b = is_b && f == float(100 + i);
            is_a = is_a && f == float(i + 1);
        }
        std::printf("buffer sample: status %u reason %u offset %u bytes %u format %u state_before 0x%x ticket %llu; execution-time version %s; unbound range status %u\n",
                    bi.status, bi.reason, bi.x, bi.width, bi.format, bi.state_before, static_cast<unsigned long long>(bi.queue_ticket),
                    is_b ? "B (GPU-read)" : (is_a ? "A (submission-time, WRONG)" : "neither"), Poll(t_unbound).status);
        CHECK(bi.status == NA_SAMPLE_READY && bi.x == 256 && bi.width == 64 && bi.format == 0 && bi.state_before == D3D12_RESOURCE_STATE_GENERIC_READ);
        CHECK(bi.queue == na::observe::IdOf(d.queue.Get()) && bi.queue_ticket != 0);
        CHECK(is_b);
        CHECK(Poll(t_unbound).status == NA_SAMPLE_ARMED); // no submission bound that range: no evidence
        na_observe_sample_release(t_cb);
        na_observe_sample_release(t_unbound);
        // A bypassed operation that begins AFTER the copy was submitted is
        // FIFO-ordered after the copy on that queue (as is any application fence
        // signal), so it cannot change the bytes the copy reads: still READY.
        // (An operation inside the window before the copy is the cross-queue
        // case above, which leaves the samples ARMED.)
        test::ComPtr<ID3D12Fence> pending_hold, nested_fence;
        REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pending_hold))));
        REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nested_fence))));
        REQUIRE(SUCCEEDED(d.queue->Wait(pending_hold.Get(), 1)));
        const uint64_t pending = Request(cbid, 0, 256, 0, 64, 1);
        const uint64_t still_armed = Request(cbid, 0, 2048, 0, 64, 1);
        REQUIRE(pending && still_armed);
        d.queue->ExecuteCommandLists(1, ls);
        CHECK(Poll(pending).status == NA_SAMPLE_IN_FLIGHT);
        const auto outer = na::observe::ExecuteBegin(d.queue.Get());
        REQUIRE(outer.submit_locked);
        REQUIRE(SUCCEEDED(d.queue->Signal(nested_fence.Get(), 1)));
        na::observe::ExecuteEnd(outer, d.queue.Get(), 0, nullptr);
        REQUIRE(SUCCEEDED(pending_hold->Signal(1)));
        d.WaitIdle();
        const auto after = WaitDone(pending);
        CHECK(after.status == NA_SAMPLE_READY);
        std::printf("pending camera copy, nested queue signal after the copy was submitted: status %u reason %u\n", after.status, after.reason);
        na_observe_sample_release(pending);
        na_observe_sample_release(still_armed);
        // Wait for this test's deferred GPU ownership to drain before the next
        // case measures its deliberately quarantined entry count.
        for (int i = 0; i < 500 && na::observe::GetProducerStats().samples_live; ++i) Sleep(2);
        CHECK(na::observe::GetProducerStats().samples_live == 0);
        cb->Unmap(0, nullptr);
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
    }

    // ---- Gate 3: signal failure -> quarantine ----
    {
        const uint64_t live0 = na::observe::GetProducerStats().samples_live;
        const uint64_t t_sf = Request(color_id);
        REQUIRE(t_sf);
        record([&] {
            D3D12_RESOURCE_BARRIER bb = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &bb);
            bb = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &bb);
        });
        na::observe::SetSampleSignalFailsForTests(true);
        d.queue->ExecuteCommandLists(1, ls);
        na::observe::SetSampleSignalFailsForTests(false);
        const na_sample_info si2 = Poll(t_sf);
        CHECK(si2.status == NA_SAMPLE_FAILED && si2.reason == NA_SAMPLE_E_SUBMIT_FAILED);
        CHECK(na_observe_sample_release(t_sf) == NA_OK);
        d.WaitIdle();
        Sleep(30); // several worker ticks
        const uint64_t live1 = na::observe::GetProducerStats().samples_live;
        std::printf("signal failure: status %u reason %u, quarantined after release %d (live %llu -> %llu)\n", si2.status, si2.reason,
                    live1 == live0 + 1 ? 1 : 0, static_cast<unsigned long long>(live0), static_cast<unsigned long long>(live1));
        CHECK(live1 == live0 + 1); // never freed without completion proof
        // G3-07: the released quarantined entry no longer holds a lease: all 8
        // remain available to the consumer.
        std::vector<uint64_t> leases;
        for (int i = 0; i < 8; ++i) leases.push_back(Request(color_id));
        int granted = 0;
        for (uint64_t t : leases) granted += t ? 1 : 0;
        std::printf("leases with a quarantined entry live: %d of 8 granted\n", granted);
        CHECK(granted == 8);
        for (uint64_t t : leases) na_observe_sample_release(t);
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
