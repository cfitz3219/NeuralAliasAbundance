// DX11 bridge implementation. See dx11_bridge.hpp and docs/DX11-BRIDGE.md.
//
// Transport rebuilt from the recovered Alien bridge (neural_bridge.cpp /
// neural_transport.hpp): NT-handle shared textures and shared D3D11 fences on
// the game's adapter, a shared-memory frame record, and a hidden helper
// process. Keyed mutexes are not used (they fail on current drivers, as
// DLSS5-Feeder's notes also record). Widened here for upscaling (render size
// smaller than output), GPU-side waits, and repeat-last-frame on a miss.
//
// Review fixes (2026-11-04, Far Cry 5 / New Dawn as the example):
//   R1 own context state via SwapDeviceContextState instead of a deferred
//      context: works on SINGLETHREADED devices, and wraps in-process DLSS so
//      NGX's own state changes cannot leak into the game. One swap per frame.
//   R2 inputs without shader access (back buffers, depth buffers) are copied
//      into readable textures; typeless/depth formats get the right view;
//      multisampled colour is resolved.
//   R3 sRGB views: if the input view decodes sRGB and the target does not (or
//      the other way round), the result is re-encoded so it is not washed out
//      or too dark.
//   R4 resize / Alt-Tab / device loss: NotReady + NeedsRestart() instead of
//      drawing an old-size picture; a removed device fails cleanly.
//   R5 one lock around Start/Stop/Process (the header's mutex).
//   R6 default data folder under %LOCALAPPDATA% (game folders are often read-only).
//   R7 deferred contexts refused per call; the game's own D3D11 multithread
//      lock is held while the bridge works, when the game turned it on.
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
#include <filesystem>

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

// Full-screen triangle (same winding as the tested Alien blit). Pixel shaders
// copy texels 1:1 by pixel coordinate, so a render-size viewport copies exactly
// the render region. ps_copy passes every channel (an R32F target keeps red =
// depth, RG16F keeps red/green = motion). The output shaders force alpha to 1
// and optionally convert between sRGB-encoded and linear (R3).
constexpr char kShaders[] = R"(
Texture2D<float4> source : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0); }
float4 ps_copy(float4 p : SV_Position) : SV_Target { return source.Load(int3(int2(p.xy), 0)); }
float4 ps_opaque(float4 p : SV_Position) : SV_Target { return float4(source.Load(int3(int2(p.xy), 0)).rgb, 1.0); }
float3 to_srgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float3 to_linear(float3 c) { c = saturate(c); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 ps_encode(float4 p : SV_Position) : SV_Target { return float4(to_srgb(source.Load(int3(int2(p.xy), 0)).rgb), 1.0); }
float4 ps_decode(float4 p : SV_Position) : SV_Target { return float4(to_linear(source.Load(int3(int2(p.xy), 0)).rgb), 1.0); }
)";

enum class Out { Copy, Opaque, Encode, Decode };

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    Handle shared; // NT handle (helper backend only)
};

bool IsSrgb(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
           f == DXGI_FORMAT_BC1_UNORM_SRGB || f == DXGI_FORMAT_BC2_UNORM_SRGB || f == DXGI_FORMAT_BC3_UNORM_SRGB ||
           f == DXGI_FORMAT_BC7_UNORM_SRGB;
}

// For a texture we copy into (R2): the storage format CopyResource accepts
// and the format of a view that reads its first channel(s).
bool ReadableFormats(DXGI_FORMAT src, DXGI_FORMAT* storage, DXGI_FORMAT* view)
{
    switch (src) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        *storage = DXGI_FORMAT_R24G8_TYPELESS; *view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT:
        *storage = DXGI_FORMAT_R32_TYPELESS; *view = DXGI_FORMAT_R32_FLOAT; return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        *storage = DXGI_FORMAT_R32G8X24_TYPELESS; *view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM:
        *storage = DXGI_FORMAT_R16_TYPELESS; *view = DXGI_FORMAT_R16_UNORM; return true;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: *storage = src; *view = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: *storage = src; *view = DXGI_FORMAT_B8G8R8A8_UNORM; return true;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: *storage = src; *view = DXGI_FORMAT_B8G8R8X8_UNORM; return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: *storage = src; *view = DXGI_FORMAT_R10G10B10A2_UNORM; return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: *storage = src; *view = DXGI_FORMAT_R16G16B16A16_FLOAT; return true;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: *storage = src; *view = DXGI_FORMAT_R32G32B32A32_FLOAT; return true;
    case DXGI_FORMAT_R16G16_TYPELESS: *storage = src; *view = DXGI_FORMAT_R16G16_FLOAT; return true;
    case DXGI_FORMAT_R32G32_TYPELESS: *storage = src; *view = DXGI_FORMAT_R32G32_FLOAT; return true;
    case DXGI_FORMAT_UNKNOWN: return false;
    default: *storage = src; *view = src; return true; // typed formats keep their own view
    }
}

// Holds the device's own lock while the game has D3D11 multithread protection on (R7).
struct MultithreadLock {
    Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
    bool held = false;
    explicit MultithreadLock(ID3D11DeviceContext* ctx)
    {
        if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&mt))) && mt->GetMultithreadProtected()) {
            mt->Enter();
            held = true;
        }
    }
    ~MultithreadLock()
    {
        if (held) mt->Leave();
    }
    MultithreadLock(const MultithreadLock&) = delete;
    MultithreadLock& operator=(const MultithreadLock&) = delete;
};

bool Desc2D(ID3D11Resource* r, D3D11_TEXTURE2D_DESC* d)
{
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim{};
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(d);
    return true;
}

// A cached readable copy of one game texture (R2).
struct Readable {
    ComPtr<ID3D11Texture2D> texture;  // copy target (same shape as the source)
    ComPtr<ID3D11Texture2D> resolved; // single-sample, for multisampled colour
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC source{};
    bool valid = false;
};

} // namespace

struct Dx11Bridge::Impl {
    Config cfg;
    Backend active = Backend::Auto;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3DDeviceContextState> our_state;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps[4]; // indexed by Out
    Texture color, depth, motion, output, last_good;
    Readable readable[3]; // colour, depth, motion copies
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
    bool failed = false, needs_restart = false;
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
    Result restart(const char* r)
    {
        set_reason(r);
        needs_restart = true;
        return Result::NotReady;
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

    // R1: everything the bridge does on the game's context happens between
    // these two calls. The game's state is swapped out (not copied) and comes
    // back exactly as it was.
    struct Scope {
        ID3D11DeviceContext1* ctx;
        ID3DDeviceContextState* ours;
        ComPtr<ID3DDeviceContextState> saved;
        Scope(ID3D11DeviceContext1* c, ID3DDeviceContextState* s) : ctx(c), ours(s) { ctx->SwapDeviceContextState(ours, &saved); }
        ~Scope()
        {
            ctx->ClearState(); // drop our bindings so nothing of ours stays referenced
            ctx->SwapDeviceContextState(saved.Get(), nullptr);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

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
            HANDLE raw = nullptr;
            hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                       ObjectName(stem, name).c_str(), &raw);
            if (FAILED(hr)) return hr_reason("The bridge could not share a texture with the helper", hr);
            t.shared.reset(raw);
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
        if (const char* r = make_texture(output, l.output_w, l.output_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                         D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, share, Object::Output)) return r;
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

    const char* make_pipeline()
    {
        HRESULT hr = device.As(&device1);
        if (FAILED(hr)) return hr_reason("This DX11 device is older than D3D11.1", hr);
        // R1: our own context state; must carry SINGLETHREADED when the game's device does.
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0u;
        const D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
        if (fl < D3D_FEATURE_LEVEL_11_0) return set_reason("The game's DX11 device runs below feature level 11_0.");
        hr = device1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &our_state);
        if (FAILED(hr)) return hr_reason("The bridge could not create its own drawing state", hr);

        ComPtr<ID3DBlob> code, errors;
        hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "na_bridge", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        if (FAILED(hr)) return hr_reason("The bridge's copy shader did not compile", hr);
        hr = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr)) return hr_reason("The bridge's copy shader could not be created", hr);
        const char* entry[4] = {"ps_copy", "ps_opaque", "ps_encode", "ps_decode"};
        for (int i = 0; i < 4; ++i) {
            code.Reset();
            errors.Reset();
            hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "na_bridge", nullptr, nullptr, entry[i], "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
            if (FAILED(hr)) return hr_reason("The bridge's copy shader did not compile", hr);
            hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps[i]);
            if (FAILED(hr)) return hr_reason("The bridge's copy shader could not be created", hr);
        }
        return nullptr;
    }

    // Draws `source` 1:1 into `target` over a w x h viewport. Only inside a Scope.
    void blit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, UINT w, UINT h, Out mode)
    {
        const D3D11_VIEWPORT vp{0.f, 0.f, float(w), float(h), 0.f, 1.f};
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none); // never bound as input and output at once
        ctx->OMSetRenderTargets(1, &target, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps[static_cast<int>(mode)].Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &source);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // R2: a readable view of one input. Uses the caller's view when given,
    // else copies the texture into a cached readable texture. Only inside a Scope.
    // `color` allows multisampled sources (resolved); others must be single-sample.
    const char* readable_view(ID3D11DeviceContext* ctx, int slot, ID3D11ShaderResourceView* view, ID3D11Resource* tex,
                              uint32_t need_w, uint32_t need_h, bool is_color, ID3D11ShaderResourceView** out)
    {
        if (view) {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            view->GetDesc(&vd);
            if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return "A bridge input view is not a plain 2D texture view (multisampled views must be given as the texture).";
            ComPtr<ID3D11Resource> r;
            view->GetResource(&r);
            D3D11_TEXTURE2D_DESC d{};
            if (!Desc2D(r.Get(), &d)) return "A bridge input is not a 2D texture.";
            if (d.Width < need_w || d.Height < need_h) return "A bridge input is smaller than this frame's render size.";
            *out = view;
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC d{};
        if (!Desc2D(tex, &d)) return "A bridge input (colour, depth or motion) is missing or not a 2D texture.";
        if (d.Width < need_w || d.Height < need_h) return "A bridge input is smaller than this frame's render size.";
        if (d.SampleDesc.Count > 1 && !is_color) return "Depth or motion is multisampled; the bridge cannot resolve it.";
        Readable& rd = readable[slot];
        const bool same = rd.valid && std::memcmp(&rd.source, &d, sizeof(d)) == 0;
        if (!same) {
            rd = Readable{};
            DXGI_FORMAT storage{}, vf{};
            if (!ReadableFormats(d.Format, &storage, &vf)) return "A bridge input has an unknown format.";
            D3D11_TEXTURE2D_DESC c = d;
            c.Format = storage;
            c.Usage = D3D11_USAGE_DEFAULT;
            c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            c.CPUAccessFlags = 0;
            c.MiscFlags = 0;
            HRESULT hr = device->CreateTexture2D(&c, nullptr, &rd.texture);
            if (FAILED(hr)) return hr_reason("The bridge could not make a readable copy of a game texture", hr);
            ID3D11Texture2D* viewed = rd.texture.Get();
            if (d.SampleDesc.Count > 1) {
                D3D11_TEXTURE2D_DESC s = c;
                s.Format = vf; // resolve needs a typed format
                s.SampleDesc.Count = 1;
                s.SampleDesc.Quality = 0;
                s.MipLevels = 1;
                s.ArraySize = 1;
                hr = device->CreateTexture2D(&s, nullptr, &rd.resolved);
                if (FAILED(hr)) return hr_reason("The bridge could not make a resolve target for multisampled colour", hr);
                viewed = rd.resolved.Get();
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = vf;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MostDetailedMip = 0;
            vd.Texture2D.MipLevels = 1;
            hr = device->CreateShaderResourceView(viewed, &vd, &rd.srv);
            if (FAILED(hr)) return hr_reason("The bridge could not read a copied game texture", hr);
            rd.source = d;
            rd.valid = true;
        }
        ctx->CopyResource(rd.texture.Get(), tex);
        if (rd.resolved) {
            DXGI_FORMAT storage{}, vf{};
            ReadableFormats(d.Format, &storage, &vf);
            ctx->ResolveSubresource(rd.resolved.Get(), 0, rd.texture.Get(), 0, vf);
        }
        *out = rd.srv.Get();
        return nullptr;
    }

    // Which output shader keeps brightness right between input and target (R3).
    static Out output_mode(DXGI_FORMAT input_view, DXGI_FORMAT target_view)
    {
        const bool in_srgb = IsSrgb(input_view), out_srgb = IsSrgb(target_view);
        if (in_srgb == out_srgb) return Out::Opaque;
        // Input view decoded to linear, target stores raw: encode. Input raw
        // (already sRGB-encoded numbers), target encodes on write: decode first.
        return in_srgb ? Out::Encode : Out::Decode;
    }

    const char* start_helper()
    {
        // Unique per session even for a quick restart (resize): the old helper
        // may still hold the old names for a moment while it exits.
        static volatile LONG session_counter = 0;
        const uint64_t unique = (GetTickCount64() << 16) | (uint64_t(InterlockedIncrement(&session_counter)) & 0xffffu);
        stem = SessionStem(GetCurrentProcessId(), unique);
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
        while (!data.empty() && (data.back() == L'\\' || data.back() == L'/')) data.pop_back(); // a trailing backslash would escape the closing quote
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
            return hr_reason("The bridge could not start its DX12 helper (na_bridge_helper64.exe missing or blocked?)", HRESULT_FROM_WIN32(GetLastError()));
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

    Result repeat_last(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, Out mode)
    {
        if (!have_last_good) return Result::NotReady;
        blit(ctx, last_good.srv.Get(), target, cfg.limits.output_w, cfg.limits.output_h, mode);
        ++repeats;
        return Result::Repeated;
    }

    Result deliver(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, Out mode)
    {
        blit(ctx, output.srv.Get(), target, cfg.limits.output_w, cfg.limits.output_h, mode);
        ctx->CopyResource(last_good.texture.Get(), output.texture.Get());
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

    void copy_in(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* views, const FrameRecord& f)
    {
        blit(ctx, views[0], color.rtv.Get(), f.render_w, f.render_h, Out::Copy);
        blit(ctx, views[1], depth.rtv.Get(), f.render_w, f.render_h, Out::Copy);
        const bool display_mv = (f.flags & kMvDisplayRes) != 0;
        if (views[2]) // kCameraMotion: no motion texture, the helper builds it
            blit(ctx, views[2], motion.rtv.Get(), display_mv ? f.output_w : f.render_w, display_mv ? f.output_h : f.render_h, Out::Copy);
    }

    Result process_helper(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* views, ID3D11RenderTargetView* target, FrameRecord& f, Out mode)
    {
        const Result health = helper_health();
        if (health != Result::Ok) return health;
        ComPtr<ID3D11DeviceContext4> ctx4;
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) return fail("This DX11 context cannot wait on fences (needs D3D11.4).");

        // A late frame: until the helper says its work is queued, nothing may
        // be overwritten. Show the last good result meanwhile.
        if (pending) {
            if (helper_submitted() < pending_value) return repeat_last(ctx, target, mode);
            pending = false;
        }
        // The helper must be done with the previous frame's inputs/outputs
        // before we overwrite them: a GPU-side wait, never a CPU stall.
        gpu_wait_output(ctx4.Get(), helper_submitted());
        copy_in(ctx, views, f);

        const uint64_t value = uint64_t(sequence) + 1;
        f.sequence = sequence;
        std::memcpy(&memory.block()->frame, &f, sizeof(f));
        MemoryBarrier();
        ctx4->Signal(input_fence.Get(), value);
        ctx->Flush();
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
                return repeat_last(ctx, target, mode);
            }
            gpu_wait_output(ctx4.Get(), value);
            return deliver(ctx, target, mode);
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
                return repeat_last(ctx, target, mode);
            }
        } else {
            wait_ms = waited();
        }
        if (output_fence->GetCompletedValue() == UINT64_MAX) return fail("The graphics device was removed.");
        waited_value = value > waited_value ? value : waited_value;
        return deliver(ctx, target, mode);
    }

    Result process_inproc(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* views, ID3D11RenderTargetView* target, FrameRecord& f, Out mode)
    {
        if (f.flags & kCameraMotion) return fail("Camera motion is built in the DX12 helper; start the bridge with the helper backend.");
        copy_in(ctx, views, f);
        f.sequence = sequence++;
        InProcResources r;
        r.color = color.texture.Get();
        r.depth = depth.texture.Get();
        r.motion = motion.texture.Get();
        r.output = output.texture.Get();
        // Still inside our own context state (R1): whatever NGX binds stays here.
        if (const char* why = dlss.Evaluate(ctx, r, f, cfg.limits)) {
            set_reason(why);
            return repeat_last(ctx, target, mode);
        }
        wait_ms = 0;
        return deliver(ctx, target, mode);
    }
};

Dx11Bridge::Dx11Bridge() = default;
Dx11Bridge::~Dx11Bridge() { Stop(); }

bool Dx11Bridge::Started() const
{
    std::lock_guard<std::mutex> g(lock_);
    return impl_ != nullptr;
}

void Dx11Bridge::Stop()
{
    std::lock_guard<std::mutex> g(lock_);
    impl_.reset();
}

const char* Dx11Bridge::Start(ID3D11DeviceContext* immediate, const Config& config)
{
    Stop();
    std::lock_guard<std::mutex> g(lock_);
    static thread_local char why[192];
    auto s = std::make_unique<Impl>();
    s->cfg = config;
    if (const char* r = ValidateLimits(config.limits)) { CopyReason(why, r); return why; }
    if (!immediate || immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { CopyReason(why, "The bridge needs the game's immediate context."); return why; }
    immediate->GetDevice(&s->device);
    if (s->device->GetDeviceRemovedReason() != S_OK) { CopyReason(why, "The game's graphics device was removed."); return why; }
    s->mv_w = config.motion_w ? config.motion_w : config.limits.max_render_w;
    s->mv_h = config.motion_h ? config.motion_h : config.limits.max_render_h;
    if (s->mv_w > config.limits.output_w || s->mv_h > config.limits.output_h) { CopyReason(why, "The motion size is larger than the output."); return why; }
    if (s->cfg.data_path.empty()) { // R6
        wchar_t base[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        s->cfg.data_path = (n && n < MAX_PATH) ? std::wstring(base) + L"\\NeuralAlias\\bridge" : std::wstring(L".");
    }
    std::error_code ec;
    std::filesystem::create_directories(s->cfg.data_path, ec);
    if (const char* r = s->make_pipeline()) { CopyReason(why, r); return why; }

    Backend want = config.backend;
    if (config.copy_test) want = Backend::Helper;
#if !defined(_WIN64)
    if (want == Backend::InProcess) { CopyReason(why, "DLSS cannot run inside a 32-bit game; use the helper."); return why; }
    want = Backend::Helper;
#endif
    if (want == Backend::Auto || want == Backend::InProcess) {
        const char* r = s->dlss.Init(s->device.Get(), s->cfg.data_path.c_str());
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
        if (s->cfg.helper_path.empty()) { CopyReason(why, "No DX12 helper path was given."); return why; }
        if (const char* r = s->start_helper()) { CopyReason(why, r); return why; }
        s->active = Backend::Helper;
    }
    impl_ = std::move(s);
    return nullptr;
}

Result Dx11Bridge::Process(ID3D11DeviceContext* immediate, const Inputs& in, ID3D11RenderTargetView* target, FrameRecord frame)
{
    std::lock_guard<std::mutex> g(lock_);
    if (!impl_) return Result::Failed;
    Impl& s = *impl_;
    if (s.failed) return Result::Failed;
    if (s.needs_restart) return Result::NotReady;
    if (!immediate || !target) return s.fail("The bridge was called without a context or target.");
    // R7: hooks on multithreaded engines (Dunia records on deferred contexts)
    // must call from the immediate context; a deferred one cannot swap state.
    if (immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        s.set_reason("The bridge was called with a deferred context; call it on the immediate context.");
        return Result::NotReady;
    }

    // R4: a different device, a removed device, or a resized target needs a restart.
    ComPtr<ID3D11Device> dev;
    immediate->GetDevice(&dev);
    if (dev.Get() != s.device.Get()) return s.restart("The game is using a different graphics device now; the bridge must restart.");
    if (s.device->GetDeviceRemovedReason() != S_OK) return s.restart("The game's graphics device was removed; the bridge must restart.");
    D3D11_RENDER_TARGET_VIEW_DESC tv{};
    target->GetDesc(&tv);
    if (tv.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || tv.Texture2D.MipSlice != 0)
        return s.fail("The bridge's target must be a plain 2D render view (mip 0).");
    ComPtr<ID3D11Resource> tr;
    target->GetResource(&tr);
    D3D11_TEXTURE2D_DESC td{};
    if (!Desc2D(tr.Get(), &td) || td.SampleDesc.Count != 1) return s.fail("The bridge's target is not a single-sample 2D texture.");
    if (td.Width != s.cfg.limits.output_w || td.Height != s.cfg.limits.output_h || frame.output_w != s.cfg.limits.output_w ||
        frame.output_h != s.cfg.limits.output_h || frame.render_w > s.cfg.limits.max_render_w || frame.render_h > s.cfg.limits.max_render_h)
        return s.restart("The game's size changed (resize, Alt-Tab or a new resolution); the bridge must restart.");

    frame.sequence = s.sequence;
    if (const char* r = Validate(frame, s.cfg.limits, s.sequence)) {
        s.set_reason(r);
        return Result::NotReady;
    }
    const bool display_mv = (frame.flags & kMvDisplayRes) != 0;
    if (display_mv && (s.mv_w != frame.output_w || s.mv_h != frame.output_h)) {
        s.set_reason("Display-resolution motion needs a bridge started with the output size as motion size.");
        return Result::NotReady;
    }

    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(immediate->QueryInterface(IID_PPV_ARGS(&ctx1)))) return s.fail("The game's context is older than D3D11.1.");
    // R7: if the game turned on D3D11's own multithread protection, hold its
    // lock so another game thread cannot use the immediate context mid-bridge.
    MultithreadLock mt(immediate);
    Impl::Scope scope(ctx1.Get(), s.our_state.Get()); // R1: game state swapped out until return

    ID3D11ShaderResourceView* views[3] = {};
    const char* bad = s.readable_view(immediate, 0, in.color, in.color_texture, frame.render_w, frame.render_h, true, &views[0]);
    if (!bad) bad = s.readable_view(immediate, 1, in.depth, in.depth_texture, frame.render_w, frame.render_h, false, &views[1]);
    if (!bad && !(frame.flags & kCameraMotion))
        bad = s.readable_view(immediate, 2, in.motion, in.motion_texture, display_mv ? frame.output_w : frame.render_w,
                              display_mv ? frame.output_h : frame.render_h, false, &views[2]);
    D3D11_SHADER_RESOURCE_VIEW_DESC cv{};
    if (views[0]) views[0]->GetDesc(&cv);
    const Out mode = Impl::output_mode(cv.Format, tv.Format);
    if (bad) { s.set_reason(bad); return s.repeat_last(immediate, target, mode); }
    return s.active == Backend::InProcess ? s.process_inproc(immediate, views, target, frame, mode)
                                          : s.process_helper(immediate, views, target, frame, mode);
}

Backend Dx11Bridge::ActiveBackend() const
{
    std::lock_guard<std::mutex> g(lock_);
    return impl_ ? impl_->active : Backend::Auto;
}
const char* Dx11Bridge::Reason() const
{
    std::lock_guard<std::mutex> g(lock_);
    return impl_ ? impl_->reason : "";
}
bool Dx11Bridge::NeedsRestart() const
{
    std::lock_guard<std::mutex> g(lock_);
    return impl_ && impl_->needs_restart;
}
double Dx11Bridge::LastWaitMs() const
{
    std::lock_guard<std::mutex> g(lock_);
    return impl_ ? impl_->wait_ms : 0;
}
uint64_t Dx11Bridge::Frames() const
{
    std::lock_guard<std::mutex> g(lock_);
    return impl_ ? impl_->frames : 0;
}
uint64_t Dx11Bridge::Repeats() const
{
    std::lock_guard<std::mutex> g(lock_);
    return impl_ ? impl_->repeats : 0;
}
uint32_t Dx11Bridge::HelperMotionFrames() const
{
    std::lock_guard<std::mutex> g(lock_);
    if (!impl_ || impl_->active != Backend::Helper || !impl_->memory.block()) return 0;
    return *reinterpret_cast<volatile uint32_t*>(&impl_->memory.block()->status.motion_frames);
}
FlowStatus Dx11Bridge::HelperFlow() const
{
    std::lock_guard<std::mutex> g(lock_);
    if (!impl_ || impl_->active != Backend::Helper || !impl_->memory.block()) return {};
    return ReadFlowStatus(impl_->memory.block()->status);
}

} // namespace na::bridge
