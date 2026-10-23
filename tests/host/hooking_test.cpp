// Reconstructed 2026-10 (C3). Mirrors WORKLOG-ROUTE-A A5/A45/A46/A47:
// hooks ID3D12CommandQueue::ExecuteCommandLists (vtable slot 10) on a real
// device, proves the detour observes real submissions with correct args and
// that GPU fences still complete through the hook, that one hook covers two
// queues sharing a vtable, idempotent re-hook, install transactions
// (prepare/commit/abandon), DetourScope depth, unhook restores passthrough,
// and shutdown leaves zero hooks.
#include "test_support.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"

#include <atomic>

namespace {
constexpr uint32_t kSlotExecute = 10; // ID3D12CommandQueue::ExecuteCommandLists
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecuteFn g_original = nullptr;
std::atomic<uint32_t> g_calls{0};
std::atomic<UINT> g_last_count{0};
std::atomic<ID3D12CommandQueue*> g_last_queue{nullptr};
std::atomic<int> g_depth_seen{-1};

void STDMETHODCALLTYPE DetourExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    na::hooking::DetourScope scope;
    g_depth_seen.store(scope.depth);
    g_calls.fetch_add(1);
    g_last_count.store(n);
    g_last_queue.store(q);
    g_original(q, n, lists);
}
ExecuteFn g_original2 = nullptr;
void STDMETHODCALLTYPE DetourExecute2(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) { g_original2(q, n, lists); }

bool submit_and_wait(test::Device& d, test::List& l)
{
    l.list->Close();
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    l.Reset();
    return true;
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("hooking_test");
    test::List l;
    REQUIRE(l.Create(d.device.Get()));
    REQUIRE(na::hooking::Initialize());
    CHECK(na::hooking::InstalledCount() == 0);

    // 1. Hook slot 10 through the queue's vtable; original published before enable.
    REQUIRE(na::hooking::HookVTableSlot(d.queue.Get(), kSlotExecute, reinterpret_cast<void*>(&DetourExecute),
                                        reinterpret_cast<void**>(&g_original)));
    CHECK(g_original != nullptr);
    CHECK(na::hooking::InstalledCount() == 1);

    // 2. A real submission is observed with the right args and the GPU still finishes it.
    submit_and_wait(d, l);
    CHECK(g_calls.load() == 1);
    CHECK(g_last_count.load() == 1);
    CHECK(g_last_queue.load() == d.queue.Get());
    CHECK(g_depth_seen.load() == 0);

    // 3. A second queue of the same type shares the implementation: observed by the same hook.
    {
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        test::ComPtr<ID3D12CommandQueue> q2;
        REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q, IID_PPV_ARGS(&q2))));
        l.list->Close();
        ID3D12CommandList* lists[] = {l.list.Get()};
        q2->ExecuteCommandLists(1, lists);
        test::ComPtr<ID3D12Fence> f;
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))));
        uint64_t fv = 0;
        test::SignalAndWait(q2.Get(), f.Get(), fv, d.event, d.device.Get(), "second queue idle"); // ends the test on failure
        CHECK(f->GetCompletedValue() == 1);
        l.Reset();
        CHECK(g_calls.load() == 2);
        CHECK(g_last_queue.load() == q2.Get());
    }

    // 4. Idempotent re-hook: same detour returns the same trampoline, no double patch.
    {
        ExecuteFn again = nullptr;
        CHECK(na::hooking::HookVTableSlot(d.queue.Get(), kSlotExecute, reinterpret_cast<void*>(&DetourExecute),
                                          reinterpret_cast<void**>(&again)));
        CHECK(again == g_original);
        CHECK(na::hooking::InstalledCount() == 1);
        // A different detour for an already-hooked target is refused, not chained.
        ExecuteFn other = nullptr;
        CHECK(!na::hooking::HookVTableSlot(d.queue.Get(), kSlotExecute, reinterpret_cast<void*>(&DetourExecute2),
                                           reinterpret_cast<void**>(&other)));
        CHECK(na::hooking::InstalledCount() == 1);
    }

    // 5. OriginalFor finds the trampoline for the concrete target function.
    {
        void** vtable = *reinterpret_cast<void***>(d.queue.Get());
        // The slot now holds the patched function; OriginalFor is keyed by target address.
        void* target = vtable[kSlotExecute];
        CHECK(na::hooking::OriginalFor(target) == reinterpret_cast<void*>(g_original));
    }

    // 6. Transaction: prepare a second slot (Signal = slot 14), abandon → original nulled, nothing installed.
    {
        using SignalFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
        static SignalFn s_orig = nullptr;
        struct Local { static HRESULT STDMETHODCALLTYPE Detour(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) { return s_orig(q, f, v); } };
        REQUIRE(na::hooking::BeginInstall());
        CHECK(na::hooking::PrepareVTableSlot(d.queue.Get(), 14, reinterpret_cast<void*>(&Local::Detour), reinterpret_cast<void**>(&s_orig)));
        CHECK(s_orig != nullptr); // published at prepare time
        na::hooking::AbandonInstall();
        CHECK(s_orig == nullptr); // abandon nulls every publication
        CHECK(na::hooking::InstalledCount() == 1);
        // Commit path: prepare again, commit, the hook is live, fences still work.
        REQUIRE(na::hooking::BeginInstall());
        CHECK(na::hooking::PrepareVTableSlot(d.queue.Get(), 14, reinterpret_cast<void*>(&Local::Detour), reinterpret_cast<void**>(&s_orig)));
        CHECK(na::hooking::CommitInstall());
        CHECK(na::hooking::InstalledCount() == 2);
        d.WaitIdle(); // goes through the Signal detour
        void** vtable = *reinterpret_cast<void***>(d.queue.Get());
        CHECK(na::hooking::Unhook(vtable[14]));
        CHECK(na::hooking::InstalledCount() == 1);
    }

    // 7. Unhook restores unobserved passthrough.
    {
        void** vtable = *reinterpret_cast<void***>(d.queue.Get());
        uint32_t before = g_calls.load();
        CHECK(na::hooking::Unhook(vtable[kSlotExecute]));
        CHECK(na::hooking::InstalledCount() == 0);
        submit_and_wait(d, l);
        CHECK(g_calls.load() == before);
        CHECK(!na::hooking::Unhook(vtable[kSlotExecute])); // never-hooked / already removed → false
    }

    na::hooking::Shutdown();
    CHECK(na::hooking::InstalledCount() == 0);
    CHECK(na::hooking::DeadHookCount() == 0);
    return test::finish("hooking_test");
}
