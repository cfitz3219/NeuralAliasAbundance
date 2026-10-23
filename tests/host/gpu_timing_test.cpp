// Reconstructed 2026-10 (C3) and extended for C4 / R-001. Exercises the
// in-game DLSS timer (na::gputime) on a real D3D12 device (hardware or WARP):
//   1. inert until a device is set: fake list pointers are never touched;
//   2. a measured copy executes -> exactly one sample with a sane duration;
//   3. R-001 regression: a list whose execution is delayed far beyond the old
//      3-Present heuristic yields NO sample (stale stamps from step 2 sit in
//      the same readback memory) until it actually executes;
//   4. a never-executed list is quarantined after the abandon window and its
//      slot is not reused; other slots keep measuring;
//   5. non-DIRECT lists are counted unsupported, never measured.
#include "test_support.hpp"
#include "host_dx12/ngx_observe/gpu_timing.hpp"

namespace {
void RecordCopyWork(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, ID3D12Resource* src)
{
    for (int i = 0; i < 8; ++i) l->CopyResource(dst, src);
}
void Polls(ID3D12CommandQueue* q, int n) { for (int i = 0; i < n; ++i) na::gputime::Poll(q); }
} // namespace

int main()
{
    // 1. No device yet: Begin/End must not dereference the list pointer.
    na::gputime::Begin(reinterpret_cast<ID3D12GraphicsCommandList*>(0x1234));
    na::gputime::End(reinterpret_cast<ID3D12GraphicsCommandList*>(0x1234));
    CHECK(na::gputime::GetStats().samples == 0);

    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("gpu_timing_test");
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
    d.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    na::gputime::SetDevice(d.device.Get());
    if (!(o3.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT)) {
        test::List l; REQUIRE(l.Create(d.device.Get()));
        na::gputime::Begin(l.list.Get());
        CHECK(na::gputime::GetStats().enabled == 0); // refuses rather than guessing
        std::printf("gpu_timing_test: SKIP rest (device lacks WriteBufferImmediate on DIRECT)\n");
        return test::g_failures ? 1 : test::kSkip;
    }

    const uint64_t kBytes = 8ull << 20;
    auto src = test::CreateBuffer(d.device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    auto dst = test::CreateBuffer(d.device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(src && dst);
    ID3D12CommandQueue* q = d.queue.Get();

    // 2. Measured work executes -> one sample.
    {
        test::List l; REQUIRE(l.Create(d.device.Get()));
        na::gputime::Begin(l.list.Get());
        RecordCopyWork(l.list.Get(), dst.Get(), src.Get());
        na::gputime::End(l.list.Get());
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        q->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        Polls(q, 1);
        const auto s = na::gputime::GetStats();
        CHECK(s.enabled == 1);
        CHECK(s.samples == 1);
        CHECK(s.last_ms > 0.0f && s.last_ms < 5000.0f);
        std::printf("measured copy: %.3f ms\n", static_cast<double>(s.last_ms));
    }

    // 3. Delayed execution: recorded and closed, but not executed for 20
    //    Presents (old code read after 3). No sample may appear.
    {
        test::List l; REQUIRE(l.Create(d.device.Get()));
        na::gputime::Begin(l.list.Get());
        RecordCopyWork(l.list.Get(), dst.Get(), src.Get());
        na::gputime::End(l.list.Get());
        REQUIRE(SUCCEEDED(l.list->Close()));
        Polls(q, 20);
        CHECK(na::gputime::GetStats().samples == 1); // still only step 2's sample
        ID3D12CommandList* lists[] = {l.list.Get()};
        q->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        Polls(q, 1);
        CHECK(na::gputime::GetStats().samples == 2); // counted once it really ran
    }

    // 4. Never executed: quarantined after the abandon window, others keep working.
    {
        test::List abandoned; REQUIRE(abandoned.Create(d.device.Get()));
        na::gputime::Begin(abandoned.list.Get());
        na::gputime::End(abandoned.list.Get());
        abandoned.list->Close(); // deliberately never executed
        Polls(q, 260);
        auto s = na::gputime::GetStats();
        CHECK(s.quarantined == 1);
        CHECK(s.enabled == 1);
        test::List l; REQUIRE(l.Create(d.device.Get()));
        na::gputime::Begin(l.list.Get());
        RecordCopyWork(l.list.Get(), dst.Get(), src.Get());
        na::gputime::End(l.list.Get());
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        q->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        Polls(q, 1);
        s = na::gputime::GetStats();
        CHECK(s.samples == 3);
        CHECK(s.quarantined == 1);
    }

    // 5. Compute list: unsupported, not measured.
    {
        test::List l; REQUIRE(l.Create(d.device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE));
        const uint64_t before = na::gputime::GetStats().unsupported;
        na::gputime::Begin(l.list.Get());
        na::gputime::End(l.list.Get());
        CHECK(na::gputime::GetStats().unsupported == before + 1);
        l.list->Close();
    }

    return test::finish("gpu_timing_test");
}
