// na_bridge_helper64.exe: the DX12 end of the DX11 bridge.
//
// Started hidden by Dx11Bridge (Helper backend). Opens the game's shared
// textures, fences and frame record on the same GPU, and for every frame:
// waits (GPU-side) for the game's inputs, runs DLSS through our existing
// game-agnostic DX12 backend (na::dlss), signals the output fence, and tells
// the game the work is queued. Rebuilt from the recovered Alien helper
// (recovered/dx11-enhancement/src/core/neural_helper.cpp).
//
// Usage: na_bridge_helper64 <stem> <parent pid> <luid low> <luid high>
//        <max render w> <max render h> <output w> <output h> <motion w> <motion h>
//        <data folder> [--copy-test]
// --copy-test copies colour to output instead of DLSS (transport test on any GPU).
#include "bridge/frame_record.hpp"
#include "bridge/win_shared.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace na::bridge;

namespace {

struct Session {
    std::wstring stem;
    DWORD parent_pid = 0;
    LUID luid{};
    SessionLimits limits{};
    uint32_t mv_w = 0, mv_h = 0;
    std::wstring data;
    bool copy_test = false;
};

SharedMemory g_memory;

void Report(HelperState state, const char* reason, int32_t ngx = 0)
{
    SharedBlock* b = g_memory.block();
    if (!b) return;
    if (reason) CopyReason(b->status.reason, reason);
    b->status.ngx_result = ngx;
    MemoryBarrier();
    b->status.state = static_cast<uint32_t>(state);
}

bool ParseU32(const wchar_t* s, uint32_t* out)
{
    wchar_t* end = nullptr;
    const unsigned long v = std::wcstoul(s, &end, 10);
    if (!end || *end) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

bool Parse(int argc, wchar_t** argv, Session* s)
{
    if (argc < 12) return false;
    s->stem = argv[1];
    uint32_t v[9] = {};
    for (int i = 0; i < 9; ++i)
        if (!ParseU32(argv[2 + i], &v[i])) return false;
    s->parent_pid = v[0];
    s->luid.LowPart = v[1];
    s->luid.HighPart = static_cast<LONG>(v[2]);
    s->limits = SessionLimits{v[3], v[4], v[5], v[6]};
    s->mv_w = v[7];
    s->mv_h = v[8];
    s->data = argv[11];
    for (int i = 12; i < argc; ++i)
        if (std::wcscmp(argv[i], L"--copy-test") == 0) s->copy_test = true;
    return s->stem.rfind(L"Local\\NABridge-", 0) == 0;
}

template <class T>
bool OpenShared(ID3D12Device* device, const std::wstring& name, ComPtr<T>& out) // by reference: ComPtr overloads operator&
{
    HANDLE h = nullptr;
    if (FAILED(device->OpenSharedHandleByName(name.c_str(), GENERIC_ALL, &h)) || !h) return false;
    const HRESULT hr = device->OpenSharedHandle(h, IID_PPV_ARGS(out.ReleaseAndGetAddressOf()));
    CloseHandle(h);
    return SUCCEEDED(hr);
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* const* res, int n, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b[4] = {};
    for (int i = 0; i < n; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = res[i];
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = from;
        b[i].Transition.StateAfter = to;
    }
    list->ResourceBarrier(static_cast<UINT>(n), b);
}

int Run(const Session& s)
{
    if (g_memory.open(ObjectName(s.stem, Object::Block))) return 3; // no block: nobody to report to
    Handle record = OpenNamedEvent(ObjectName(s.stem, Object::RecordEvent));
    Handle submitted = OpenNamedEvent(ObjectName(s.stem, Object::SubmittedEvent));
    Handle stop = OpenNamedEvent(ObjectName(s.stem, Object::StopEvent));
    Handle parent(OpenProcess(SYNCHRONIZE, FALSE, s.parent_pid));
    if (!record || !submitted || !stop || !parent) { Report(HelperState::Failed, "The helper could not open the bridge's events."); return 4; }
    if (const char* r = ValidateLimits(s.limits)) { Report(HelperState::Failed, r); return 5; }

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(s.luid, IID_PPV_ARGS(&adapter)))) {
        Report(HelperState::Failed, "The helper could not find the game's graphics adapter.");
        return 6;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        Report(HelperState::Failed, "The helper could not create a DX12 device on the game's GPU.");
        return 7;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    constexpr int kRing = 3;
    ComPtr<ID3D12CommandAllocator> alloc[kRing];
    uint64_t alloc_value[kRing] = {};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> own;
    Handle own_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    bool ok = SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&own))) && own_event;
    for (int i = 0; ok && i < kRing; ++i)
        ok = SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i])));
    ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list))) &&
         SUCCEEDED(list->Close());
    if (!ok) { Report(HelperState::Failed, "The helper could not create its DX12 queue."); return 8; }

    ComPtr<ID3D12Resource> color, depth, motion, output;
    ComPtr<ID3D12Fence> input_fence, output_fence;
    if (!OpenShared(device.Get(), ObjectName(s.stem, Object::Color), color) ||
        !OpenShared(device.Get(), ObjectName(s.stem, Object::Depth), depth) ||
        !OpenShared(device.Get(), ObjectName(s.stem, Object::Motion), motion) ||
        !OpenShared(device.Get(), ObjectName(s.stem, Object::Output), output) ||
        !OpenShared(device.Get(), ObjectName(s.stem, Object::InputFence), input_fence) ||
        !OpenShared(device.Get(), ObjectName(s.stem, Object::OutputFence), output_fence)) {
        Report(HelperState::Failed, "The helper could not open the game's shared textures or fences.");
        return 9;
    }
    const D3D12_RESOURCE_DESC cd = color->GetDesc(), od = output->GetDesc(), md = motion->GetDesc();
    if (cd.Width < s.limits.max_render_w || cd.Height < s.limits.max_render_h || od.Width != s.limits.output_w ||
        od.Height != s.limits.output_h || md.Width < s.mv_w || md.Height < s.mv_h) {
        Report(HelperState::Failed, "The shared textures do not match the sizes the helper was given.");
        return 10;
    }

    if (!s.copy_test) {
        if (!na::dlss::InitOwn(device.Get(), s.data.c_str())) {
            const na::dlss::Capabilities caps = na::dlss::GetCapabilities();
            Report(HelperState::Failed, caps.reason[0] ? caps.reason : "DLSS did not start in the helper.", caps.feature_init_result);
            return 11;
        }
        const na::dlss::Capabilities caps = na::dlss::GetCapabilities();
        if (!caps.available) {
            Report(HelperState::Failed, caps.reason[0] ? caps.reason : "DLSS Super Resolution is not available on this GPU.", caps.feature_init_result);
            na::dlss::Shutdown();
            return 12;
        }
        g_memory.block()->status.dlss_available = 1;
    }
    Report(HelperState::Ready, "");

    uint32_t expected = 0;
    uint64_t own_value = 0;
    int slot = 0;
    int code = 0;
    for (;;) {
        HANDLE hs[3] = {stop.get(), parent.get(), record.get()};
        const DWORD w = WaitForMultipleObjects(3, hs, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 + 2) break; // stop requested, game gone, or a wait error
        // One record per event; the game never writes the next one until this
        // one is reported queued.
        FrameRecord f;
        MemoryBarrier();
        std::memcpy(&f, &g_memory.block()->frame, sizeof(f));
        if (f.sequence != expected) continue; // stale wake-up for a record already handled
        if (const char* why = Validate(f, s.limits, expected)) { Report(HelperState::Failed, why); code = 13; break; }

        // Reuse an allocator only once the GPU finished with it (helper-side wait).
        // Resetting an allocator the GPU still uses would remove the device, so
        // never reset on a timeout: the game stopped feeding its fence (hung,
        // paused on a modal dialog for too long, or its device was lost), and
        // the bridge shows its last good frame in the meantime.
        if (own->GetCompletedValue() < alloc_value[slot]) {
            own->SetEventOnCompletion(alloc_value[slot], own_event.get());
            HANDLE ws[3] = {own_event.get(), stop.get(), parent.get()};
            const DWORD aw = WaitForMultipleObjects(3, ws, FALSE, 10000);
            if (aw != WAIT_OBJECT_0) {
                if (aw == WAIT_TIMEOUT) {
                    Report(HelperState::Failed, "The helper's earlier frame never finished on the GPU (the game stopped signalling its inputs).");
                    code = 16;
                }
                break;
            }
        }
        if (FAILED(device->GetDeviceRemovedReason())) {
            Report(HelperState::Failed, "The helper's DX12 device was lost (driver reset or GPU removed).");
            code = 17;
            break;
        }
        alloc[slot]->Reset();
        list->Reset(alloc[slot].Get(), nullptr);

        if (s.copy_test) {
            ID3D12Resource* in[1] = {color.Get()};
            ID3D12Resource* outr[1] = {output.Get()};
            Barrier(list.Get(), in, 1, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(list.Get(), outr, 1, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = output.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = color.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, 0, 0, f.render_w, f.render_h, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
            Barrier(list.Get(), in, 1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(list.Get(), outr, 1, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        } else {
            ID3D12Resource* in[3] = {color.Get(), depth.Get(), motion.Get()};
            ID3D12Resource* outr[1] = {output.Get()};
            Barrier(list.Get(), in, 3, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(list.Get(), outr, 1, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            na::dlss::EvalDesc d;
            d.color = color.Get();
            d.depth = depth.Get();
            d.motion = motion.Get();
            d.output = output.Get();
            d.render_w = f.render_w;
            d.render_h = f.render_h;
            d.max_render_w = s.limits.max_render_w;
            d.max_render_h = s.limits.max_render_h;
            d.out_w = f.output_w;
            d.out_h = f.output_h;
            d.jitter_x = f.jitter_x;
            d.jitter_y = f.jitter_y;
            d.mv_scale_x = f.mv_scale_x;
            d.mv_scale_y = f.mv_scale_y;
            d.pre_exposure = f.pre_exposure;
            d.exposure_scale = f.exposure_scale;
            d.view_id = 1;
            d.quality = static_cast<na::dlss::Quality>(f.mode);
            d.preset = f.preset;
            d.reset = (f.flags & kReset) != 0;
            d.hdr = (f.flags & kHdr) != 0;
            d.depth_inverted = (f.flags & kDepthInverted) != 0;
            d.mv_jittered = (f.flags & kMvJittered) != 0;
            d.mv_display_res = (f.flags & kMvDisplayRes) != 0;
            d.auto_exposure = (f.flags & kAutoExposure) != 0;
            if (!na::dlss::Evaluate(list.Get(), d)) {
                const na::dlss::Stats st = na::dlss::GetStats();
                Report(HelperState::Failed, st.reason[0] ? st.reason : "DLSS refused this frame.", st.last_ngx_result);
                list->Close();
                code = 14;
                break;
            }
            Barrier(list.Get(), in, 3, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(list.Get(), outr, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
        if (FAILED(list->Close())) { Report(HelperState::Failed, "The helper could not record this frame."); code = 15; break; }

        const uint64_t value = uint64_t(f.sequence) + 1;
        queue->Wait(input_fence.Get(), value); // GPU waits for the game's inputs
        ID3D12CommandList* lists[1] = {list.Get()};
        if (!s.copy_test) na::dlss::BeforeExecute(queue.Get(), 1, lists);
        queue->ExecuteCommandLists(1, lists);
        if (!s.copy_test) na::dlss::OnExecuted(queue.Get(), 1, lists);
        queue->Signal(output_fence.Get(), value);
        queue->Signal(own.Get(), ++own_value);
        alloc_value[slot] = own_value;
        slot = (slot + 1) % kRing;
        ++expected;

        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_memory.block()->status.last_submitted), static_cast<LONG64>(value));
        SetEvent(submitted.get());
    }

    // Drain our own work before releasing anything DLSS used.
    if (own_value && own->GetCompletedValue() < own_value) {
        own->SetEventOnCompletion(own_value, own_event.get());
        WaitForSingleObject(own_event.get(), 2000);
    }
    if (!s.copy_test) na::dlss::Shutdown();
    if (code == 0) Report(HelperState::Stopped, "The helper was asked to stop.");
    return code;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Session s;
    const bool parsed = argv && Parse(argc, argv, &s);
    if (argv) LocalFree(argv);
    if (!parsed) return 2;
    return Run(s);
}
