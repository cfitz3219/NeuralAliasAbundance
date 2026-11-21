// The 64-bit companion: opens the textures and fences the 32-bit in-game half shares, and runs the
// same DLSS backend the 64-bit games use. It owns no window, reads no game memory, and ends when
// the game ends. See protocol.hpp.
#include "channel.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace na::dlaa_remote {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto kCommon = D3D12_RESOURCE_STATE_COMMON;
constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

struct Slot {
    ComPtr<ID3D12Resource> color, depth, motion, output;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    uint32_t width = 0, height = 0;
    uint64_t last = 0; // the `done` value of this slot's latest submission
};
struct State {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> ready, done;
    Slot slots[kSlots];
    bool copy = false;
    uint64_t signalled = 0;
    uint32_t stale_every = 0; // tests only (Hello::stale_every_for_tests)
    uint64_t frames_answered = 0;
};

Reply Fail(HRESULT hr, const char* why)
{
    Reply r{};
    r.hresult = hr;
    strncpy_s(r.reason, why, _TRUNCATE);
    return r;
}

template <class T> HRESULT OpenAndClose(ID3D12Device* device, uint64_t handle, ComPtr<T>& out)
{
    // The in-game half duplicated this handle into this process for us; once opened (or refused)
    // it has no further use here.
    const HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
    const HRESULT hr = h ? device->OpenSharedHandle(h, IID_PPV_ARGS(out.ReleaseAndGetAddressOf())) : E_HANDLE;
    if (h) CloseHandle(h);
    return hr;
}

// Blocks until this slot's previous work has finished on the GPU, so its allocator and textures
// can be reused. The in-game half only reuses a slot it has seen retire, so this rarely waits.
bool Idle(State& s, const Slot& slot)
{
    if (!s.done || s.done->GetCompletedValue() >= slot.last) return true;
    const HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!e) return false;
    const bool ok = SUCCEEDED(s.done->SetEventOnCompletion(slot.last, e)) && WaitForSingleObject(e, 2000) == WAIT_OBJECT_0;
    CloseHandle(e);
    return ok;
}

Reply OnHello(State& s, const Hello& h)
{
    if (s.device) return Fail(E_UNEXPECTED, "The companion was started twice.");
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Fail(hr, "The companion could not reach the graphics system.");
    const LUID luid{h.adapter_luid_low, h.adapter_luid_high};
    hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) return Fail(hr, "The companion could not find the graphics card the game is using.");
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.device));
    if (FAILED(hr)) return Fail(hr, "The companion could not start DirectX 12 on the game's graphics card.");
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s.device->CreateCommandQueue(&q, IID_PPV_ARGS(&s.queue));
    if (FAILED(hr)) return Fail(hr, "The companion could not create its command queue.");
    hr = OpenAndClose(s.device.Get(), h.ready_fence, s.ready);
    const HRESULT hr2 = OpenAndClose(s.device.Get(), h.done_fence, s.done);
    if (FAILED(hr) || FAILED(hr2)) return Fail(FAILED(hr) ? hr : hr2, "The companion could not open the shared frame fences.");
    s.copy = h.copy_for_tests != 0;
    s.stale_every = h.stale_every_for_tests;
    Reply r{};
    r.ok = 1;
    if (s.copy) {
        r.available = 1;
        strncpy_s(r.reason, "Copy mode for tests: no DLSS is run.", _TRUNCATE);
        return r;
    }
    wchar_t path[260]{};
    for (size_t i = 0; i < 259 && h.data_path[i]; ++i) path[i] = static_cast<wchar_t>(h.data_path[i]);
    const bool on = dlss::EnsureOn(s.device.Get(), path);
    const auto caps = dlss::GetCapabilities();
    r.available = on && caps.available ? 1u : 0u;
    if (!r.available) strncpy_s(r.reason, caps.reason[0] ? caps.reason : "DLSS could not start in the companion.", _TRUNCATE);
    return r;
}

Reply OnBind(State& s, const Bind& b)
{
    if (!s.device) return Fail(E_UNEXPECTED, "Textures arrived before the companion was started.");
    if (b.slot >= kSlots || !b.width || !b.height) return Fail(E_INVALIDARG, "A frame slot outside the agreed range.");
    Slot& slot = s.slots[b.slot];
    if (!Idle(s, slot)) return Fail(E_FAIL, "The previous frame in this slot never finished.");
    HRESULT first = S_OK;
    const auto open = [&](uint64_t handle, ComPtr<ID3D12Resource>& out) {
        const HRESULT hr = OpenAndClose(s.device.Get(), handle, out);
        if (FAILED(hr) && SUCCEEDED(first)) first = hr;
    };
    open(b.color, slot.color); open(b.depth, slot.depth); open(b.motion, slot.motion); open(b.output, slot.output);
    if (FAILED(first)) { slot = Slot{}; return Fail(first, "The companion could not open a shared frame texture."); }
    if (!slot.allocator) {
        HRESULT hr = s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
        if (SUCCEEDED(hr)) hr = s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list));
        if (SUCCEEDED(hr)) hr = slot.list->Close();
        if (FAILED(hr)) { slot = Slot{}; return Fail(hr, "The companion could not create its command list."); }
    }
    slot.width = b.width;
    slot.height = b.height;
    Reply r{};
    r.ok = 1;
    return r;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

// Whatever happens, `done` reaches the frame's value: the game's graphics queue is waiting on it,
// and a value that never arrives would freeze the game's picture.
Reply OnFrame(State& s, const Frame& f)
{
    const auto release = [&] {
        if (s.queue && s.done && f.value > s.signalled) { s.queue->Signal(s.done.Get(), f.value); s.signalled = f.value; }
    };
    if (!s.device || f.slot >= kSlots || !s.slots[f.slot].list) { release(); return Fail(E_UNEXPECTED, "A frame arrived for a slot with no textures."); }
    Slot& slot = s.slots[f.slot];
    if (!Idle(s, slot) || FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) {
        release();
        return Fail(E_FAIL, "The companion's command list could not be reused.");
    }
    auto* list = slot.list.Get();
    bool evaluated = false;
    if (s.copy) {
        Barrier(list, slot.color.Get(), kCommon, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, slot.output.Get(), kCommon, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(slot.output.Get(), slot.color.Get());
        Barrier(list, slot.color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kCommon);
        Barrier(list, slot.output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kCommon);
        evaluated = true;
    } else {
        Barrier(list, slot.color.Get(), kCommon, kRead);
        Barrier(list, slot.depth.Get(), kCommon, kRead);
        Barrier(list, slot.motion.Get(), kCommon, kRead);
        Barrier(list, slot.output.Get(), kCommon, kWrite);
        dlss::EvalDesc d{};
        d.color = slot.color.Get(); d.depth = slot.depth.Get(); d.motion = slot.motion.Get(); d.output = slot.output.Get();
        d.render_w = d.out_w = slot.width;
        d.render_h = d.out_h = slot.height;
        d.jitter_x = f.jitter_x; d.jitter_y = f.jitter_y;
        d.mv_scale_x = f.mv_scale_x; d.mv_scale_y = f.mv_scale_y;
        d.pre_exposure = f.pre_exposure; d.exposure_scale = f.exposure_scale;
        d.view_id = f.view_id;
        d.quality = static_cast<dlss::Quality>(f.quality);
        d.preset = f.preset;
        d.reset = (f.flags & kReset) != 0;
        d.hdr = (f.flags & kHdr) != 0;
        d.depth_inverted = (f.flags & kDepthInverted) != 0;
        d.mv_jittered = (f.flags & kMvJittered) != 0;
        d.auto_exposure = (f.flags & kAutoExposure) != 0;
        evaluated = dlss::Evaluate(list, d);
        Barrier(list, slot.color.Get(), kRead, kCommon);
        Barrier(list, slot.depth.Get(), kRead, kCommon);
        Barrier(list, slot.motion.Get(), kRead, kCommon);
        Barrier(list, slot.output.Get(), kWrite, kCommon);
    }
    if (FAILED(list->Close())) { release(); return Fail(E_FAIL, "The companion's command list could not close."); }
    // Inputs are complete once the in-game queue has signalled `ready`; no CPU wait.
    s.queue->Wait(s.ready.Get(), f.value);
    ID3D12CommandList* lists[] = {list};
    if (!s.copy) dlss::BeforeExecute(s.queue.Get(), 1, lists);
    s.queue->ExecuteCommandLists(1, lists);
    if (!s.copy) dlss::OnExecuted(s.queue.Get(), 1, lists);
    release();
    slot.last = f.value;
    if (!evaluated) {
        const auto stats = dlss::GetStats();
        return Fail(E_FAIL, stats.reason[0] ? stats.reason : "DLSS refused this frame.");
    }
    Reply r{};
    r.ok = 1;
    return r;
}
} // namespace

// `name` is the channel the in-game half created; `game_pid` is that game's process.
int RunCompanion(const wchar_t* name, DWORD game_pid)
{
    Channel channel;
    if (!channel.Open(name)) return 2;
    const HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, game_pid);
    if (!game) return 3;
    channel.Shared()->companion_pid = GetCurrentProcessId();
    auto* state = new State; // released by process exit: GPU work may still be in flight
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    int code = 0;
    for (;;) {
        Request request{};
        const auto got = channel.Receive(&request, game, INFINITE);
        if (got != Channel::Result::Ok) { code = got == Channel::Result::PeerGone ? 0 : 4; break; }
        QueryPerformanceCounter(&t0);
        Reply reply{};
        bool bye = false;
        switch (request.kind) {
        case Kind::Hello: reply = OnHello(*state, request.hello); break;
        case Kind::Bind: reply = OnBind(*state, request.bind); break;
        case Kind::Frame:
            reply = OnFrame(*state, request.frame);
            reply.value = request.frame.value;
            reply.slot = request.frame.slot;
            // Tests only: answer as if for the frame before, which the in-game half must refuse.
            if (state->stale_every && ++state->frames_answered % state->stale_every == 0) --reply.value;
            break;
        case Kind::Bye: reply.ok = 1; bye = true; break;
        default: reply = Fail(E_NOTIMPL, "A request this companion does not know; the two halves are from different builds."); break;
        }
        QueryPerformanceCounter(&t1);
        reply.micros = static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
        channel.Answer(reply);
        if (bye) break;
    }
    CloseHandle(game);
    return code;
}

} // namespace na::dlaa_remote
