// Reconstructed 2026-10 (C3), extended for C4 / R-001 and Gate 1 R-202/R-204.
// Exercises the in-game DLSS timer (na::gputime) on a real D3D12 device
// (hardware or WARP). Submit() plays the host's ExecuteCommandLists detour:
// the original submit, then gputime::OnExecuted on the same queue.
//   1. inert until a device is set: fake list pointers are never touched;
//   2. a measured copy executes -> exactly one sample with a sane duration;
//   3. R-202: the queue is HELD on a fence before the measured list, so the
//      measurement stays in flight while Poll runs; the reused slot still holds
//      step 2's stamps. No sample may appear until the GPU really ran it;
//   4. R-001: a list submitted 20 Presents late yields no sample until then;
//   5. a never-submitted list is quarantined after the abandon window and its
//      slot is not reused; other slots keep measuring;
//   6. a COMPUTE list on a compute queue is measured with that queue's
//      frequency; a bundle is counted unsupported;
//   7. R-204: the checked wait reports a timeout on a fence nobody signals.
#include "test_support.hpp"
#include "host_dx12/ngx_observe/gpu_timing.hpp"

namespace {
void RecordCopyWork(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, ID3D12Resource* src)
{
    for (int i = 0; i < 8; ++i) l->CopyResource(dst, src);
}
void Polls(int n) { for (int i = 0; i < n; ++i) na::gputime::Poll(nullptr); }
void Submit(ID3D12CommandQueue* q, ID3D12GraphicsCommandList* l)
{
    ID3D12CommandList* lists[] = {l};
    q->ExecuteCommandLists(1, lists);
    na::gputime::OnExecuted(q, 1, lists); // what the overlay's execute detour does after the original call
}
bool Measured(test::Device& d, test::List& l, ID3D12Resource* dst, ID3D12Resource* src)
{
    if (!l.Create(d.device.Get())) return false;
    na::gputime::Begin(l.list.Get());
    RecordCopyWork(l.list.Get(), dst, src);
    na::gputime::End(l.list.Get());
    return SUCCEEDED(l.list->Close());
}
} // namespace

int main()
{
    // 1. No device yet: Begin/End must not dereference the list pointer.
    na::gputime::Begin(reinterpret_cast<ID3D12GraphicsCommandList*>(0x1234));
    na::gputime::End(reinterpret_cast<ID3D12GraphicsCommandList*>(0x1234));
    CHECK(na::gputime::GetStats().samples == 0);

    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("gpu_timing_test");
    na::gputime::SetDevice(d.device.Get());
    const uint64_t kBytes = 8ull << 20;
    auto src = test::CreateBuffer(d.device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    auto dst = test::CreateBuffer(d.device.Get(), kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(src && dst);
    ID3D12CommandQueue* q = d.queue.Get();

    // 2. Measured work executes -> one sample.
    {
        test::List l; REQUIRE(Measured(d, l, dst.Get(), src.Get()));
        Submit(q, l.list.Get());
        d.WaitIdle();
        Polls(1);
        const auto s = na::gputime::GetStats();
        CHECK(s.enabled == 1);
        CHECK(s.samples == 1);
        CHECK(s.last_ms > 0.0f && s.last_ms < 5000.0f);
        std::printf("measured copy: %.3f ms\n", static_cast<double>(s.last_ms));
    }

    // 3. R-202: measurement in flight while Poll runs (queue held on a fence).
    {
        test::ComPtr<ID3D12Fence> hold;
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold))));
        REQUIRE(SUCCEEDED(q->Wait(hold.Get(), 1))); // GPU work after this waits for the CPU
        test::List l; REQUIRE(Measured(d, l, dst.Get(), src.Get()));
        Submit(q, l.list.Get());
        Polls(30);
        CHECK(na::gputime::GetStats().samples == 1); // stale stamps from step 2 must not be read
        REQUIRE(SUCCEEDED(hold->Signal(1)));       // release the queue
        d.WaitIdle();
        Polls(1);
        CHECK(na::gputime::GetStats().samples == 2);
    }

    // 4. R-001: closed but submitted 20 Presents late.
    {
        test::List l; REQUIRE(Measured(d, l, dst.Get(), src.Get()));
        Polls(20);
        CHECK(na::gputime::GetStats().samples == 2);
        Submit(q, l.list.Get());
        d.WaitIdle();
        Polls(1);
        CHECK(na::gputime::GetStats().samples == 3);
    }

    // 5. Never submitted: quarantined after the abandon window, others keep working.
    {
        test::List abandoned; REQUIRE(abandoned.Create(d.device.Get()));
        na::gputime::Begin(abandoned.list.Get());
        na::gputime::End(abandoned.list.Get());
        abandoned.list->Close(); // deliberately never executed
        Polls(260);
        auto s = na::gputime::GetStats();
        CHECK(s.quarantined == 1);
        CHECK(s.enabled == 1);
        test::List l; REQUIRE(Measured(d, l, dst.Get(), src.Get()));
        Submit(q, l.list.Get());
        d.WaitIdle();
        Polls(1);
        s = na::gputime::GetStats();
        CHECK(s.samples == 4);
        CHECK(s.quarantined == 1);
    }

    // 6. COMPUTE list on a compute queue: measured with that queue's frequency.
    {
        D3D12_COMMAND_QUEUE_DESC cq{}; cq.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        test::ComPtr<ID3D12CommandQueue> compute;
        REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&cq, IID_PPV_ARGS(&compute))));
        test::List l; REQUIRE(l.Create(d.device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE));
        na::gputime::Begin(l.list.Get());
        RecordCopyWork(l.list.Get(), dst.Get(), src.Get());
        na::gputime::End(l.list.Get());
        REQUIRE(SUCCEEDED(l.list->Close()));
        Submit(compute.Get(), l.list.Get());
        test::SignalAndWait(compute.Get(), d.fence.Get(), d.fence_value, d.event, d.device.Get(), "compute queue idle");
        Polls(1);
        CHECK(na::gputime::GetStats().samples == 5);

        test::List b; REQUIRE(b.Create(d.device.Get(), D3D12_COMMAND_LIST_TYPE_BUNDLE));
        const uint64_t before = na::gputime::GetStats().unsupported;
        na::gputime::Begin(b.list.Get());
        na::gputime::End(b.list.Get());
        CHECK(na::gputime::GetStats().unsupported == before + 1);
        b.list->Close();
    }

    // 7. R-204: a fence nobody signals is reported as a timeout, never as "idle".
    {
        test::ComPtr<ID3D12Fence> never;
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&never))));
        CHECK(test::WaitFence(never.Get(), 1, d.event, 50) == test::WaitResult::Timeout);
        CHECK(test::WaitFence(never.Get(), 0, d.event, 50) == test::WaitResult::Ok);
    }

    return test::finish("gpu_timing_test");
}
