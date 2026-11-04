// DX11 bridge implementation. See dx11_bridge.hpp.
//
// Transport rebuilt from the recovered Alien bridge (neural_bridge.cpp /
// neural_transport.hpp): NT-handle shared textures and shared D3D11 fences on
// the game's adapter, a shared-memory frame record, and a hidden helper
// process. Keyed mutexes are not used (they fail on current drivers, as
// DLSS5-Feeder's notes also record). Widened here for upscaling (render size
// smaller than output), GPU-side waits, and repeat-last-frame on a miss.
#include "bridge/dx11_bridge.hpp"

#include "bridge/dx11_dlss_inproc.hpp"
#include "bridge/win_shared.hpp"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace na::bridge {

using Microsoft::WRL::ComPtr;

const char* BackendName(Backend b)
{
    switch (b) {
    case Backend::Auto: return "auto";
    case Backend::InProcess: return "in-process DX11";
    case Backend::Helper: return "DX12 helper";
    }
    return "unknown";
}

const char* ResultName(Result r)
{
    switch (r) {
    case Result::Ok: return "ok";
    case Result::Repeated: return "repeated last frame";
    case Result::NotReady: return "not ready";
    case Result::Failed: return "failed";
    }
    return "unknown";
}

namespace {

// Full-screen triangle (same winding as the tested Alien blit); `ps_copy` copies texels 1:1 (pixel coordinates), so a
// render-size viewport copies exactly the render region. Every channel is
// passed on: an R32F target keeps red (depth), RG16F keeps red/green (motion).
constexpr char kShaders[] = R"(
Texture2D<float4> source : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0); }
float4 ps_copy(float4 p : SV_Position) : SV_Target { return source.Load(int3(int2(p.xy), 0)); }
float4 ps_opaque(float4 p : SV_Position) : SV_Target { return float4(source.Load(int3(int2(p.xy), 0)).rgb, 1.0); }
)";

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    Handle shared; // NT handle (helper backend only)
};

bool TextureSize(ID3D11View* view, UINT* w, UINT* h, UINT* samples)
{
    ComPtr<ID3D11Resource> r;
    view->GetResource(&r);
    ComPtr<ID3D11Texture2D> t;
    if (!r || FAILED(r.As(&t))) return false;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    *w = d.Width;
    *h = d.Height;
    *samples = d.SampleDesc.Count;
    return true;
}

} // namespace

struct Dx11Bridge::Impl {
    Config cfg;
    Backend active = Backend::Auto;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> deferred;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_copy, ps_opaque;
    Texture color, depth, motion, output, last_good;
    bool have_last_good = false;
    uint32_t mv_w = 0, mv_h = 0;

    // Helper backend.
    std::wstring stem;
    ComPtr<ID3D11Fence> input_fence, output_fence;
    Handle input_fence_handle, output_fence_handle;
    Handle record_event, submitted_event, stop_event, fence_event;
    SharedMemory memory;
    PROCESS_INFORMATION child{};
    uint64_t startup_tick = 0;
    uint64_t waited_value = 0;    // highest output-fence value the game's GPU already waits on
    bool pending = false;         // a submitted frame whose helper work was not yet seen queued
    uint64_t pending_value = 0;

    // In-process backend.
    Dx11Dlss dlss;

    uint32_t sequence = 0;
    bool failed = false;
    char reason[192] = {};
    double wait_ms = 0;
    uint64_t frames = 0, repeats = 0;

    ~Impl() { stop_helper(); dlss.Shutdown(); }

    const char* set_reason(const char* r)
    {
        CopyReason(reason, r);
        return reason;
    }
    const char* hr_reason(const char* what, HRESULT hr)
    {
        std::snprintf(reason, sizeof(reason), "%s (0x%08X).", what, static_cast<unsigned>(hr));
        return reason;
    }
    Result fail(const char* r)
    {
        set_reason(r);
        failed = true;
        return Result::Failed;
    }

    void stop_helper()
    {
        if (stop_event) SetEvent(stop_event.get());
        if (child.hProcess) {
            if (WaitForSingleObject(child.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 99);
            CloseHandle(child.hProcess);
        }
        if (child.hThread) CloseHandle(child.hThread);
        child = PROCESS_INFORMATION{};
    }

    const char* make_texture(Texture& t, UINT w, UINT h, DXGI_FORMAT f, UINT bind, bool share, Object name)
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        d.MiscFlags = share ? (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE) : 0;
        HRESULT hr = device->CreateTexture2D(&d, nullptr, &t.texture);
        if (FAILED(hr)) return hr_reason("The bridge could not create a texture", hr);
        if (bind & D3D11_BIND_RENDER_TARGET) {
            hr = device->CreateRenderTargetView(t.texture.Get(), nullptr, &t.rtv);
            if (FAILED(hr)) return hr_reason("The bridge could not create a render view", hr);
        }
        if (bind & D3D11_BIND_SHADER_RESOURCE) {
            hr = device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv);
            if (FAILED(hr)) return hr_reason("The bridge could not create a shader view", hr);
        }
        if (share) {
            ComPtr<IDXGIResource1> r;
            hr = t.texture.As(&r);
            if (FAILED(hr)) return hr_reason("This DX11 device cannot share textures", hr);
            HANDLE h = nullptr;
            hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                       ObjectName(stem, name).c_str(), &h);
            if (FAILED(hr)) return hr_reason("The bridge could not share a texture with the helper", hr);
            t.shared.reset(h);
        }
        return nullptr;
    }

    const char* make_fence(ComPtr<ID3D11Fence>& f, Handle& h, Object name)
    {
        HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f));
        if (FAILED(hr)) return hr_reason("This DX11 device cannot create shared fences (needs Windows 10 and D3D11.4)", hr);
        HANDLE raw = nullptr;
        hr = f->CreateSharedHandle(nullptr, GENERIC_ALL, ObjectName(stem, name).c_str(), &raw);
        if (FAILED(hr)) return hr_reason("The bridge could not share a fence with the helper", hr);
        h.reset(raw);
        return nullptr;
    }

    const char* make_resources(bool share)
    {
        const SessionLimits& l = cfg.limits;
        const UINT rt_srv = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (const char* r = make_texture(color, l.max_render_w, l.max_render_h, DXGI_FORMAT_R16G16B16A16_FLOAT, rt_srv, share, Object::Color)) return r;
        if (const char* r = make_texture(depth, l.max_render_w, l.max_render_h, DXGI_FORMAT_R32_FLOAT, rt_srv, share, Object::Depth)) return r;
        if (const char* r = make_texture(motion, mv_w, mv_h, DXGI_FORMAT_R16G16_FLOAT, rt_srv, share, Object::Motion)) return r;
        const UINT out_bind = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | (cfg.copy_test ? D3D11_BIND_RENDER_TARGET : 0u);
        if (const char* r = make_texture(output, l.output_w, l.output_h, DXGI_FORMAT_R16G16B16A16_FLOAT, out_bind, share, Object::Output)) return r;
        if (const char* r = make_texture(last_good, l.output_w, l.output_h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, false, Object::Output)) return r;
        return nullptr;
    }

    void release_resources()
    {
        color = Texture{};
        depth = Texture{};
        motion = Texture{};
        output = Texture{};
        last_good = Texture{};
        have_last_good = false;
        input_fence.Reset();
        output_fence.Reset();
        input_fence_handle.reset();
        output_fence_handle.reset();
        memory.close();
    }

    const char* make_shaders()
    {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "na_bridge", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (FAILED(hr)) return hr_reason("The bridge's copy shader did not compile", hr);
        hr = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr)) return hr_reason("The bridge's copy shader could not be created", hr);
        for (int i = 0; i < 2; ++i) {
            code.Reset();
            errors.Reset();
            hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "na_bridge", nullptr, nullptr, i ? "ps_opaque" : "ps_copy", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
            if (FAILED(hr)) return hr_reason("The bridge's copy shader did not compile", hr);
            hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, i ? &ps_opaque : &ps_copy);
            if (FAILED(hr)) return hr_reason("The bridge's copy shader could not be created", hr);
        }
        hr = device->CreateDeferredContext(0, &deferred);
        if (FAILED(hr)) return hr_reason("The bridge could not create its drawing context", hr);
        return nullptr;
    }

    // Draws `source` 1:1 into `target` over a w x h viewport, on the deferred
    // context, then replays it on the game's context with its state restored.
    void blit(ID3D11DeviceContext* immediate, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, UINT w, UINT h, bool opaque)
    {
        deferred->ClearState();
        const D3D11_VIEWPORT vp{0.f, 0.f, float(w), float(h), 0.f, 1.f};
        deferred->RSSetViewports(1, &vp);
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deferred->VSSetShader(vs.Get(), nullptr, 0);
        deferred->PSSetShader(opaque ? ps_opaque.Get() : ps_copy.Get(), nullptr, 0);
        deferred->PSSetShaderResources(0, 1, &source);
        deferred->OMSetRenderTargets(1, &target, nullptr);
        deferred->Draw(3, 0);
        ComPtr<ID3D11CommandList> list;
        if (SUCCEEDED(deferred->FinishCommandList(FALSE, &list))) immediate->ExecuteCommandList(list.Get(), TRUE);
    }

    const char* check_input(ID3D11ShaderResourceView* v, uint32_t need_w, uint32_t need_h)
    {
        if (!v) return "A bridge input (colour, depth or motion) is missing.";
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        v->GetDesc(&d);
        if (d.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return "A bridge input is not a plain 2D texture view.";
        UINT w = 0, h = 0, s = 0;
        if (!TextureSize(v, &w, &h, &s)) return "A bridge input is not a 2D texture.";
        if (s != 1) return "A bridge input is multisampled; resolve it first.";
        if (w < need_w || h < need_h) return "A bridge input is smaller than this frame's render size.";
        return nullptr;
    }

    const char* start_helper()
    {
        stem = SessionStem(GetCurrentProcessId(), GetTickCount64());
        if (const char* r = memory.create(ObjectName(stem, Object::Block))) return set_reason(r);
        *memory.block() = SharedBlock{};
        record_event = CreateNamedEvent(ObjectName(stem, Object::RecordEvent), false);
        submitted_event = CreateNamedEvent(ObjectName(stem, Object::SubmittedEvent), false);
        stop_event = CreateNamedEvent(ObjectName(stem, Object::StopEvent), true);
        fence_event = Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!record_event || !submitted_event || !stop_event || !fence_event) return set_reason("The bridge could not create its events.");
        if (const char* r = make_resources(true)) return r;
        if (const char* r = make_fence(input_fence, input_fence_handle, Object::InputFence)) return r;
        if (const char* r = make_fence(output_fence, output_fence_handle, Object::OutputFence)) return r;

        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC info{};
        if (FAILED(device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&info)))
            return set_reason("The bridge could not identify the game's graphics adapter.");

        const SessionLimits& l = cfg.limits;
        std::wstring data = cfg.data_path;
        while (!data.empty() && (data.back() == L'\\' || data.back() == L'/')) data.pop_back(); // a trailing \ would escape the quote
        if (data.empty()) data = L".";
        std::wstring cmd = L"\"" + cfg.helper_path + L"\" " + stem + L" " + std::to_wstring(GetCurrentProcessId()) + L" " +
                           std::to_wstring(info.AdapterLuid.LowPart) + L" " + std::to_wstring(info.AdapterLuid.HighPart) + L" " +
                           std::to_wstring(l.max_render_w) + L" " + std::to_wstring(l.max_render_h) + L" " +
                           std::to_wstring(l.output_w) + L" " + std::to_wstring(l.output_h) + L" " +
                           std::to_wstring(mv_w) + L" " + std::to_wstring(mv_h) + L" \"" + data + L"\"" +
                           (cfg.copy_test ? L" --copy-test" : L"");
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (!CreateProcessW(cfg.helper_path.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &child))
            return set_reason("The bridge could not start its DX12 helper (na_bridge_helper64.exe missing?).");
        startup_tick = GetTickCount64();
        return nullptr;
    }

    // Helper backend: is the helper alive and ready? Failed latches.
    Result helper_health()
    {
        if (!child.hProcess) return fail("The DX12 helper is not running.");
        const HelperStatus& st = memory.block()->status;
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_TIMEOUT) {
            char why[192];
            std::snprintf(why, sizeof(why), "The DX12 helper stopped: %s", st.reason[0] ? st.reason : "no reason given");
            return fail(why);
        }
        const auto state = static_cast<HelperState>(st.state);
        if (state == HelperState::Failed || state == HelperState::Stopped) {
            char why[192];
            std::snprintf(why, sizeof(why), "The DX12 helper gave up: %s", st.reason[0] ? st.reason : "no reason given");
            return fail(why);
        }
        if (state != HelperState::Ready) {
            if (GetTickCount64() - startup_tick > cfg.startup_deadline_ms) return fail("The DX12 helper did not start in time.");
            return Result::NotReady;
        }
        return Result::Ok;
    }

    uint64_t helper_submitted() const
    {
        return static_cast<uint64_t>(InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&memory.block()->status.last_submitted), 0, 0));
    }

    Result repeat_last(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target)
    {
        if (!have_last_good) return Result::NotReady;
        blit(immediate, last_good.srv.Get(), target, cfg.limits.output_w, cfg.limits.output_h, true);
        ++repeats;
        return Result::Repeated;
    }

    Result deliver(ID3D11DeviceContext* immediate, ID3D11RenderTargetView* target)
    {
        blit(immediate, output.srv.Get(), target, cfg.limits.output_w, cfg.limits.output_h, true);
        immediate->CopyResource(last_good.texture.Get(), output.texture.Get());
        have_last_good = true;
        ++frames;
        return Result::Ok;
    }

    void gpu_wait_output(ID3D11DeviceContext4* ctx4, uint64_t value)
    {
        if (value > waited_value) {
            ctx4->Wait(output_fence.Get(), value);
            waited_value = value;
        }
    }

    Result process_helper(ID3D11DeviceContext* immediate, const Inputs& in, ID3D11RenderTargetView* target, FrameRecord& f)
    {
        const Result health = helper_health();
        if (health != Result::Ok) return health;
        ComPtr<ID3D11DeviceContext4> ctx4;
        if (FAILED(immediate->QueryInterface(IID_PPV_ARGS(&ctx4)))) return fail("This DX11 context cannot wait on fences (needs D3D11.4).");

        // A late frame: until the helper says its work is queued, nothing may
        // be overwritten. Show the last good result meanwhile.
        if (pending) {
            if (helper_submitted() < pending_value) return repeat_last(immediate, target);
            pending = false;
        }
        // The helper must be done with the previous frame's inputs/outputs
        // before we overwrite them: a GPU-side wait, never a CPU stall.
        gpu_wait_output(ctx4.Get(), helper_submitted());

        blit(immediate, in.color, color.rtv.Get(), f.render_w, f.render_h, false);
        blit(immediate, in.depth, depth.rtv.Get(), f.render_w, f.render_h, false);
        const bool display_mv = (f.flags & kMvDisplayRes) != 0;
        blit(immediate, in.motion, motion.rtv.Get(), display_mv ? f.output_w : f.render_w, display_mv ? f.output_h : f.render_h, false);

        const uint64_t value = uint64_t(sequence) + 1;
        f.sequence = sequence;
        std::memcpy(&memory.block()->frame, &f, sizeof(f));
        MemoryBarrier();
        ctx4->Signal(input_fence.Get(), value);
        immediate->Flush();
        ++sequence;
        SetEvent(record_event.get());

        const auto t0 = std::chrono::steady_clock::now();
        const auto waited = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        if (cfg.wait == WaitMode::Gpu) {
            // Wait (CPU, short) only until the helper has QUEUED its work.
            while (helper_submitted() < value) {
                const double left = double(cfg.submit_deadline_ms) - waited();
                if (left <= 0) break;
                HANDLE hs[2] = {submitted_event.get(), child.hProcess};
                const DWORD w = WaitForMultipleObjects(2, hs, FALSE, static_cast<DWORD>(left) + 1);
                if (w == WAIT_OBJECT_0 + 1) { wait_ms = waited(); return helper_health(); }
                if (w != WAIT_OBJECT_0) break;
            }
            wait_ms = waited();
            if (helper_submitted() < value) {
                pending = true;
                pending_value = value;
                return repeat_last(immediate, target);
            }
            gpu_wait_output(ctx4.Get(), value);
            return deliver(immediate, target);
        }
        // CpuDeadline: wait for the finished result.
        if (output_fence->GetCompletedValue() < value) {
            if (FAILED(output_fence->SetEventOnCompletion(value, fence_event.get()))) return fail("The bridge could not wait for the helper.");
            HANDLE hs[2] = {fence_event.get(), child.hProcess};
            const DWORD w = WaitForMultipleObjects(2, hs, FALSE, cfg.output_deadline_ms);
            wait_ms = waited();
            if (w == WAIT_OBJECT_0 + 1) return helper_health();
            if (w != WAIT_OBJECT_0) {
                pending = true;
                pending_value = value;
                return repeat_last(immediate, target);
            }
        } else {
            wait_ms = waited();
        }
        if (output_fence->GetCompletedValue() == UINT64_MAX) return fail("The graphics device was removed.");
        waited_value = value > waited_value ? value : waited_value;
        return deliver(immediate, target);
    }

    Result process_inproc(ID3D11DeviceContext* immediate, const Inputs& in, ID3D11RenderTargetView* target, FrameRecord& f)
    {
        blit(immediate, in.color, color.rtv.Get(), f.render_w, f.render_h, false);
        blit(immediate, in.depth, depth.rtv.Get(), f.render_w, f.render_h, false);
        const bool display_mv = (f.flags & kMvDisplayRes) != 0;
        blit(immediate, in.motion, motion.rtv.Get(), display_mv ? f.output_w : f.render_w, display_mv ? f.output_h : f.render_h, false);
        f.sequence = sequence++;
        InProcResources r;
        r.color = color.texture.Get();
        r.depth = depth.texture.Get();
        r.motion = motion.texture.Get();
        r.output = output.texture.Get();
        if (const char* why = dlss.Evaluate(immediate, r, f, cfg.limits)) {
            set_reason(why);
            return repeat_last(immediate, target);
        }
        wait_ms = 0;
        return deliver(immediate, target);
    }
};

Dx11Bridge::Dx11Bridge() = default;
Dx11Bridge::~Dx11Bridge() { Stop(); }

bool Dx11Bridge::Started() const { return impl_ != nullptr; }
void Dx11Bridge::Stop() { impl_.reset(); }

const char* Dx11Bridge::Start(ID3D11DeviceContext* immediate, const Config& config)
{
    Stop();
    static thread_local char why[192];
    auto s = std::make_unique<Impl>();
    s->cfg = config;
    if (const char* r = ValidateLimits(config.limits)) { CopyReason(why, r); return why; }
    if (!immediate || immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { CopyReason(why, "The bridge needs the game's immediate context."); return why; }
    immediate->GetDevice(&s->device);
    s->mv_w = config.motion_w ? config.motion_w : config.limits.max_render_w;
    s->mv_h = config.motion_h ? config.motion_h : config.limits.max_render_h;
    if (s->mv_w > config.limits.output_w || s->mv_h > config.limits.output_h) { CopyReason(why, "The motion size is larger than the output."); return why; }
    if (const char* r = s->make_shaders()) { CopyReason(why, r); return why; }

    Backend want = config.backend;
    if (config.copy_test) want = Backend::Helper;
#if !defined(_WIN64)
    if (want == Backend::InProcess) { CopyReason(why, "DLSS cannot run inside a 32-bit game; use the helper."); return why; }
    want = Backend::Helper;
#endif
    if (want == Backend::Auto || want == Backend::InProcess) {
        const char* r = s->dlss.Init(s->device.Get(), config.data_path.c_str());
        if (!r) r = s->make_resources(false);
        if (!r) {
            s->active = Backend::InProcess;
        } else if (want == Backend::InProcess) {
            CopyReason(why, r);
            return why;
        } else {
            s->dlss.Shutdown();
            s->release_resources();
        }
    }
    if (s->active == Backend::Auto) {
        if (FAILED(s->device.As(&s->device5))) { CopyReason(why, "This DX11 device is older than D3D11.4, so it cannot share fences with the helper."); return why; }
        if (config.helper_path.empty()) { CopyReason(why, "No DX12 helper path was given."); return why; }
        if (const char* r = s->start_helper()) { CopyReason(why, r); return why; }
        s->active = Backend::Helper;
    }
    impl_ = std::move(s);
    return nullptr;
}

Result Dx11Bridge::Process(ID3D11DeviceContext* immediate, const Inputs& in, ID3D11RenderTargetView* target, FrameRecord frame)
{
    if (!impl_) return Result::Failed;
    Impl& s = *impl_;
    if (s.failed) return Result::Failed;
    if (!immediate || !target) return s.fail("The bridge was called without a context or target.");
    frame.sequence = s.sequence;
    if (const char* r = Validate(frame, s.cfg.limits, s.sequence)) { s.set_reason(r); return s.repeat_last(immediate, target); }
    const bool display_mv = (frame.flags & kMvDisplayRes) != 0;
    if (display_mv && (s.mv_w != frame.output_w || s.mv_h != frame.output_h)) {
        s.set_reason("Display-resolution motion needs a bridge started with the output size as motion size.");
        return s.repeat_last(immediate, target);
    }
    const char* bad = s.check_input(in.color, frame.render_w, frame.render_h);
    if (!bad) bad = s.check_input(in.depth, frame.render_w, frame.render_h);
    if (!bad) bad = s.check_input(in.motion, display_mv ? frame.output_w : frame.render_w, display_mv ? frame.output_h : frame.render_h);
    if (!bad) {
        UINT w = 0, h = 0, n = 0;
        if (!TextureSize(target, &w, &h, &n) || w != frame.output_w || h != frame.output_h || n != 1)
            bad = "The bridge's target is not a single-sample texture of the output size.";
    }
    if (bad) { s.set_reason(bad); return s.repeat_last(immediate, target); }
    return s.active == Backend::InProcess ? s.process_inproc(immediate, in, target, frame)
                                          : s.process_helper(immediate, in, target, frame);
}

Backend Dx11Bridge::ActiveBackend() const { return impl_ ? impl_->active : Backend::Auto; }
const char* Dx11Bridge::Reason() const { return impl_ ? impl_->reason : ""; }
double Dx11Bridge::LastWaitMs() const { return impl_ ? impl_->wait_ms : 0; }
uint64_t Dx11Bridge::Frames() const { return impl_ ? impl_->frames : 0; }
uint64_t Dx11Bridge::Repeats() const { return impl_ ? impl_->repeats : 0; }

} // namespace na::bridge
