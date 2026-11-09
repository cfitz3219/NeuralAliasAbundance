#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace na::on12 {
namespace {

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                       ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                   const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                                   D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using On12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*, UINT, UINT,
                                           ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);
using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

CreateDeviceFn g_orig_create = nullptr;
CreateDeviceAndSwapChainFn g_orig_create_sc = nullptr;
On12CreateDeviceFn g_on12_create = nullptr;
D3D12CreateDeviceFn g_d3d12_create = nullptr;
LogFn g_log = nullptr;
std::atomic<bool> g_enabled{false}, g_hooked{false};
// [dx11on12] menu_buffer: 1 (default) the menu draws into a canvas texture of ours, which the
// game's own D3D11 context then blends onto the back buffer; 2 the menu draws straight into the
// borrowed back buffer (D3D12 writes to an 11on12 swap chain's presented buffer make the D3D11
// present wait ~2 s on its latency semaphore: probe runs 2026-11-09, modes 1-6); 0 no menu.
std::atomic<int> g_menu_buffer{1};
thread_local int t_inside = 0; // 11on12 may call D3D11CreateDevice itself: inner calls go to the original

struct Entry { IUnknown* identity = nullptr; ID3D12Device* device = nullptr; ID3D12CommandQueue* queue = nullptr; };
std::mutex g_m;
Entry g_entries[8];
uint32_t g_next = 0;

std::atomic<uint32_t> g_requests{0}, g_wrapped{0}, g_native{0}, g_probes{0}, g_with_swapchain{0};
std::atomic<int> g_swapchain_check{-1}; // -1 not run, 0 failed, 1 ok
std::atomic<long> g_last_hr{0};
char g_reason[160] = "Waiting for the game's D3D11 device.";

void note(const char* reason, HRESULT hr = S_OK)
{
    {
        std::lock_guard<std::mutex> lk(g_m);
        std::snprintf(g_reason, sizeof g_reason, "%s", reason);
    }
    g_last_hr.store(static_cast<long>(hr));
    if (g_log) {
        char line[224];
        std::snprintf(line, sizeof line, "dx11on12: %s (hr=0x%08lX)", reason, static_cast<unsigned long>(hr));
        g_log(line);
    }
}

IUnknown* identity_of(IUnknown* p)
{
    if (!p) return nullptr;
    IUnknown* id = nullptr;
    if (FAILED(p->QueryInterface(IID_PPV_ARGS(&id))) || !id) return nullptr;
    id->Release(); // identity pointer only; the registry holds its own reference on the device
    return id;
}

void remember(ID3D11Device* d11, ID3D12Device* d12, ID3D12CommandQueue* q)
{
    std::lock_guard<std::mutex> lk(g_m);
    Entry& e = g_entries[g_next++ % 8];
    if (e.identity) { e.identity->Release(); }
    if (e.device) e.device->Release();
    if (e.queue) e.queue->Release();
    e.identity = identity_of(d11);
    if (e.identity) e.identity->AddRef();
    e.device = d12; d12->AddRef();
    e.queue = q; q->AddRef();
}

// An 11on12 device that cannot own a swap chain would break the game at its
// first CreateSwapChain: check once per process on a hidden window.
bool swapchain_supported(ID3D11Device* device)
{
    const int known = g_swapchain_check.load();
    if (known >= 0) return known == 1;
    bool ok = false;
    HRESULT hr = E_FAIL;
    const wchar_t* cls = L"NeuralAliasOn12Probe";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, cls, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (wnd && SUCCEEDED(hr = device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
        SUCCEEDED(hr = dxgi_device->GetAdapter(&adapter)) && SUCCEEDED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 64; d.Height = 64; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc;
        hr = factory->CreateSwapChainForHwnd(device, wnd, &d, nullptr, nullptr, &sc);
        ok = SUCCEEDED(hr) && sc;
    }
    if (wnd) DestroyWindow(wnd);
    g_swapchain_check.store(ok ? 1 : 0);
    if (!ok) note("an 11on12 device could not create a swap chain on this system; the game keeps native DX11", hr);
    return ok;
}

// Builds the 11on12 device on our own D3D12 device + direct queue. Returns
// false (nothing handed out, nothing kept) on any failure.
bool create_on12(IDXGIAdapter* adapter, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT level_count,
                 ID3D11Device** out_device, D3D_FEATURE_LEVEL* out_level, ID3D11DeviceContext** out_context)
{
    if (!g_on12_create || !g_d3d12_create) { note("D3D11On12 or D3D12 is not available; native DX11"); return false; }
    ComPtr<ID3D12Device> d12;
    HRESULT hr = g_d3d12_create(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    if (FAILED(hr) || !d12) { note("no D3D12 device on the game's adapter; native DX11", hr); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(hr = d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { note("D3D12 queue creation failed; native DX11", hr); return false; }
    IUnknown* queues[] = {queue.Get()};
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL chosen{};
    hr = g_on12_create(d12.Get(), flags, levels, level_count, queues, 1, 0, &d11, &ctx, &chosen);
    if (FAILED(hr) || !d11) { note("D3D11On12CreateDevice refused the game's request; native DX11", hr); return false; }
    if (!swapchain_supported(d11.Get())) return false;
    remember(d11.Get(), d12.Get(), queue.Get());
    if (out_level) *out_level = chosen;
    if (out_context) *out_context = ctx.Detach();
    *out_device = d11.Detach();
    g_wrapped.fetch_add(1);
    char line[128];
    std::snprintf(line, sizeof line, "the game's D3D11 device runs on our D3D12 queue (feature level 0x%X)", static_cast<unsigned>(chosen));
    note(line);
    return true;
}

// Requests this layer answers: a real device on hardware, no debug layer.
bool wanted(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, UINT flags, void* out_device)
{
    if (!g_enabled.load() || t_inside > 0) return false;
    if (!out_device) { g_probes.fetch_add(1); return false; } // feature-level probe
    if (flags & D3D11_CREATE_DEVICE_DEBUG) { note("debug-layer device requested; native DX11"); return false; }
    if (adapter ? type != D3D_DRIVER_TYPE_UNKNOWN : type != D3D_DRIVER_TYPE_HARDWARE) {
        note("software/WARP/reference device requested; native DX11"); return false;
    }
    return true;
}

HRESULT WINAPI DetourCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                  const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk,
                                  ID3D11Device** out_device, D3D_FEATURE_LEVEL* out_level, ID3D11DeviceContext** out_context)
{
    if (wanted(adapter, type, flags, out_device)) {
        g_requests.fetch_add(1);
        ++t_inside;
        const bool ok = create_on12(adapter, flags, levels, level_count, out_device, out_level, out_context);
        --t_inside;
        if (ok) return S_OK;
        g_native.fetch_add(1);
    }
    ++t_inside;
    const HRESULT hr = g_orig_create(adapter, type, sw, flags, levels, level_count, sdk, out_device, out_level, out_context);
    --t_inside;
    return hr;
}

HRESULT WINAPI DetourCreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                              const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk,
                                              const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out_sc,
                                              ID3D11Device** out_device, D3D_FEATURE_LEVEL* out_level,
                                              ID3D11DeviceContext** out_context)
{
    if (wanted(adapter, type, flags, out_device) && (!out_sc || desc)) {
        g_requests.fetch_add(1);
        ++t_inside;
        ID3D11Device* d11 = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL level{};
        bool ok = create_on12(adapter, flags, levels, level_count, &d11, &level, &ctx);
        IDXGISwapChain* sc = nullptr;
        if (ok && out_sc) {
            ComPtr<IDXGIDevice> dxgi_device;
            ComPtr<IDXGIAdapter> a;
            ComPtr<IDXGIFactory> factory;
            HRESULT hr = d11->QueryInterface(IID_PPV_ARGS(&dxgi_device));
            if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&a);
            if (SUCCEEDED(hr)) hr = a->GetParent(IID_PPV_ARGS(&factory));
            DXGI_SWAP_CHAIN_DESC copy = *desc;
            if (SUCCEEDED(hr)) hr = factory->CreateSwapChain(d11, &copy, &sc);
            if (FAILED(hr) || !sc) {
                note("the game's swap chain could not be created on the 11on12 device; native DX11", hr);
                if (ctx) ctx->Release();
                d11->Release();
                ok = false;
            }
        }
        --t_inside;
        if (ok) {
            if (out_sc) { *out_sc = sc; g_with_swapchain.fetch_add(1); }
            *out_device = d11;
            if (out_level) *out_level = level;
            if (out_context) *out_context = ctx; else if (ctx) ctx->Release();
            return S_OK;
        }
        g_native.fetch_add(1);
    }
    ++t_inside;
    const HRESULT hr = g_orig_create_sc(adapter, type, sw, flags, levels, level_count, sdk, desc, out_sc, out_device, out_level, out_context);
    --t_inside;
    return hr;
}

HMODULE system_module(const wchar_t* name)
{
    wchar_t path[MAX_PATH + 32]{};
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    wcscat_s(path, L"\\");
    wcscat_s(path, name);
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

} // namespace

void Start(const wchar_t* ini, LogFn log)
{
    g_log = log;
    if (GetPrivateProfileIntW(L"dx11on12", L"enabled", 0, ini) != 1) {
        std::snprintf(g_reason, sizeof g_reason, "Off ([dx11on12] enabled=0).");
        return;
    }
    HMODULE d3d11 = system_module(L"d3d11.dll");
    HMODULE d3d12 = system_module(L"d3d12.dll");
    if (!d3d11) { note("system d3d11.dll not found; DX11 bridge off"); return; }
    g_on12_create = reinterpret_cast<On12CreateDeviceFn>(GetProcAddress(d3d11, "D3D11On12CreateDevice"));
    g_d3d12_create = d3d12 ? reinterpret_cast<D3D12CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    void* create = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    void* create_sc = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    bool ok = create && create_sc &&
              na::hooking::HookFunction(create, reinterpret_cast<void*>(&DetourCreateDevice), reinterpret_cast<void**>(&g_orig_create)) &&
              na::hooking::HookFunction(create_sc, reinterpret_cast<void*>(&DetourCreateDeviceAndSwapChain), reinterpret_cast<void**>(&g_orig_create_sc));
    if (!ok) { note("could not hook D3D11CreateDevice; DX11 bridge off"); return; }
    g_menu_buffer.store(static_cast<int>(GetPrivateProfileIntW(L"dx11on12", L"menu_buffer", 1, ini)));
    g_hooked.store(true);
    g_enabled.store(true);
    note("D3D11 device creation hooked; the game's DX11 device will run on D3D12");
}

ID3D12CommandQueue* QueueFor(IUnknown* device)
{
    if (!g_hooked.load() || !device) return nullptr;
    IUnknown* id = identity_of(device);
    if (!id) return nullptr;
    std::lock_guard<std::mutex> lk(g_m);
    for (const auto& e : g_entries)
        if (e.identity == id && e.queue) { e.queue->AddRef(); return e.queue; }
    return nullptr;
}

ID3D12Device* DeviceFor(IUnknown* device)
{
    if (!g_hooked.load() || !device) return nullptr;
    IUnknown* id = identity_of(device);
    if (!id) return nullptr;
    std::lock_guard<std::mutex> lk(g_m);
    for (const auto& e : g_entries)
        if (e.identity == id && e.device) { e.device->AddRef(); return e.device; }
    return nullptr;
}

namespace {
struct Token {
    ComPtr<ID3D11On12Device2> device;
    ComPtr<ID3D11Resource> resource;    // what was unwrapped: the canvas, or the back buffer itself
    ComPtr<ID3D11Resource> back_buffer; // set when `resource` is the canvas
    ComPtr<ID3D11Device> d11;
};
std::atomic<uint64_t> g_unwraps{0}, g_unwrap_failures{0}, g_returns{0};
// Time spent inside 11on12's unwrap / return (CPU), to find stalls while the menu draws.
std::atomic<uint64_t> g_unwrap_us{0}, g_unwrap_max_us{0}, g_return_us{0}, g_return_max_us{0};
uint64_t now_us()
{
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1e6 / double(q.QuadPart); }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(double(t.QuadPart) * f);
}
void add_time(std::atomic<uint64_t>& total, std::atomic<uint64_t>& peak, uint64_t us)
{
    total.fetch_add(us);
    uint64_t m = peak.load();
    while (us > m && !peak.compare_exchange_weak(m, us)) {}
}
}

namespace {
// The canvas: a D3D11 texture on the game's 11on12 device that the menu draws into through
// D3D12 (unwrapped for that draw), then a fullscreen triangle on the game's immediate context
// blends it onto the back buffer. Drawing never touches the presented buffer from D3D12.
const char kCompositeSource[] = R"(
Texture2D<float4> canvas : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target { return canvas.Load(int3(pos.xy, 0)); }
)";

struct Canvas {
    ID3D11Device* owner = nullptr; // identity of the 11on12 device (not owned)
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3DDeviceContextState> state;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool broken = false; // composite setup failed on this device: draw straight into the back buffer
};
std::mutex g_canvas_m;
Canvas g_canvas;
std::atomic<uint64_t> g_composites{0}, g_composite_failures{0};

using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out)
{
    static CompileFn fn = [] {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        return m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }();
    return fn && SUCCEEDED(fn(kCompositeSource, sizeof(kCompositeSource) - 1, "NeuralAlias 11on12 menu composite", nullptr, nullptr,
                              entry, target, 0, 0, &out, nullptr));
}

// Pipeline objects, once per device. Caller holds g_canvas_m.
bool ensure_pipeline(ID3D11Device* d11)
{
    if (g_canvas.owner != d11) g_canvas = Canvas{}, g_canvas.owner = d11;
    if (g_canvas.broken) return false;
    if (g_canvas.state) return true;
    ComPtr<ID3DBlob> vsb, psb;
    ComPtr<ID3D11Device1> d1;
    D3D_FEATURE_LEVEL level = d11->GetFeatureLevel();
    const IID emulated = __uuidof(ID3D11Device);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE; // the canvas holds premultiplied colour (ImGui over transparent)
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd{}; // depth and stencil off
    const bool ok = compile("vs", "vs_4_0", vsb) && compile("ps", "ps_4_0", psb) &&
                    SUCCEEDED(d11->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_canvas.vs)) &&
                    SUCCEEDED(d11->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_canvas.ps)) &&
                    SUCCEEDED(d11->CreateBlendState(&bd, &g_canvas.blend)) &&
                    SUCCEEDED(d11->CreateRasterizerState(&rd, &g_canvas.raster)) &&
                    SUCCEEDED(d11->CreateDepthStencilState(&dd, &g_canvas.depth)) &&
                    SUCCEEDED(d11->QueryInterface(IID_PPV_ARGS(&d1))) &&
                    SUCCEEDED(d1->CreateDeviceContextState((d11->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0u, &level, 1, D3D11_SDK_VERSION, emulated, nullptr, &g_canvas.state));
    if (!ok) { g_canvas.broken = true; g_composite_failures.fetch_add(1); }
    return ok;
}

// Canvas sized and formatted like the back buffer. Caller holds g_canvas_m.
bool ensure_canvas(ID3D11Device* d11, const D3D11_TEXTURE2D_DESC& back, DXGI_FORMAT format)
{
    if (g_canvas.texture && g_canvas.width == back.Width && g_canvas.height == back.Height && g_canvas.format == format) return true;
    g_canvas.texture.Reset();
    g_canvas.srv.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = back.Width;
    td.Height = back.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d11->CreateTexture2D(&td, nullptr, &g_canvas.texture)) ||
        FAILED(d11->CreateShaderResourceView(g_canvas.texture.Get(), nullptr, &g_canvas.srv))) {
        g_canvas.texture.Reset();
        g_canvas.srv.Reset();
        return false;
    }
    g_canvas.width = back.Width;
    g_canvas.height = back.Height;
    g_canvas.format = format;
    return true;
}

// Blend the canvas onto back buffer 0 with the game's immediate context, inside our own
// context state so the game's bindings are untouched.
void composite(ID3D11Device* d11, ID3D11Resource* back_buffer)
{
    ComPtr<ID3D11DeviceContext> ctx;
    d11->GetImmediateContext(&ctx);
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11RenderTargetView> rtv;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = g_canvas.format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (!ctx || FAILED(ctx.As(&ctx1)) || FAILED(d11->CreateRenderTargetView(back_buffer, &rd, &rtv))) {
        g_composite_failures.fetch_add(1);
        return;
    }
    ComPtr<ID3DDeviceContextState> game_state;
    ctx1->SwapDeviceContextState(g_canvas.state.Get(), &game_state);
    D3D11_VIEWPORT vp{0, 0, float(g_canvas.width), float(g_canvas.height), 0, 1};
    const float factor[4] = {};
    ID3D11RenderTargetView* targets[] = {rtv.Get()};
    ID3D11ShaderResourceView* views[] = {g_canvas.srv.Get()};
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->IASetInputLayout(nullptr);
    ctx1->VSSetShader(g_canvas.vs.Get(), nullptr, 0);
    ctx1->PSSetShader(g_canvas.ps.Get(), nullptr, 0);
    ctx1->PSSetShaderResources(0, 1, views);
    ctx1->RSSetState(g_canvas.raster.Get());
    ctx1->RSSetViewports(1, &vp);
    ctx1->OMSetBlendState(g_canvas.blend.Get(), factor, 0xffffffffu);
    ctx1->OMSetDepthStencilState(g_canvas.depth.Get(), 0);
    ctx1->OMSetRenderTargets(1, targets, nullptr);
    ctx1->Draw(3, 0);
    ID3D11ShaderResourceView* none[] = {nullptr};
    ctx1->PSSetShaderResources(0, 1, none); // the canvas is a render target again next frame
    ctx1->OMSetRenderTargets(0, nullptr, nullptr);
    ctx1->SwapDeviceContextState(game_state.Get(), nullptr);
    g_composites.fetch_add(1);
}
} // namespace

bool UnwrapBackBuffer(IDXGISwapChain* sc, unsigned index, ID3D12CommandQueue* queue, unsigned format, ID3D12Resource** out, Borrowed& borrowed)
{
    const int mode = g_menu_buffer.load();
    if (!g_hooked.load() || mode == 0 || !sc || !queue || !out) return false;
    ComPtr<IUnknown> any;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&any)))) return false;
    ComPtr<ID3D11On12Device2> device;
    ComPtr<ID3D11Device> d11;
    if (FAILED(any.As(&device)) || FAILED(any.As(&d11))) return false; // not an 11on12 chain
    // D3D11 swap chains expose only buffer 0: it is always the current back buffer.
    (void)index;
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
    ComPtr<ID3D11Resource> target;
    bool canvas = false;
    if (mode == 1) {
        D3D11_TEXTURE2D_DESC back{};
        buffer->GetDesc(&back);
        std::lock_guard<std::mutex> lk(g_canvas_m);
        if (ensure_pipeline(d11.Get()) && ensure_canvas(d11.Get(), back, static_cast<DXGI_FORMAT>(format))) {
            target = g_canvas.texture;
            canvas = true;
        }
    }
    if (!target) target = buffer; // mode 2, or the composite could not be set up
    const uint64_t t0 = now_us();
    HRESULT hr = device->UnwrapUnderlyingResource(target.Get(), queue, IID_PPV_ARGS(out));
    add_time(g_unwrap_us, g_unwrap_max_us, now_us() - t0);
    if (FAILED(hr) || !*out) { g_unwrap_failures.fetch_add(1); g_last_hr.store(static_cast<long>(hr)); return false; }
    borrowed.token = new Token{device, target, canvas ? ComPtr<ID3D11Resource>(buffer) : ComPtr<ID3D11Resource>{}, d11};
    borrowed.clear = canvas;
    g_unwraps.fetch_add(1);
    return true;
}

void Borrowed::Submitted(ID3D12Fence* f, unsigned long long v) { fence = f; value = v; }

Borrowed::~Borrowed()
{
    auto* t = static_cast<Token*>(token);
    if (!t) return;
    UINT64 v = value;
    ID3D12Fence* fences[] = {fence};
    // Back to 11on12 before Present; with our fence, later D3D11 work on it waits for the menu draw.
    const uint64_t t0 = now_us();
    t->device->ReturnUnderlyingResource(t->resource.Get(), fence ? 1u : 0u, fence ? &v : nullptr, fence ? fences : nullptr);
    add_time(g_return_us, g_return_max_us, now_us() - t0);
    g_returns.fetch_add(1);
    if (t->back_buffer && fence) { // canvas drawn: blend it onto the back buffer through D3D11
        std::lock_guard<std::mutex> lk(g_canvas_m);
        if (g_canvas.owner == t->d11.Get() && g_canvas.state) composite(t->d11.Get(), t->back_buffer.Get());
    }
    delete t;
}

int FormatStatus(char* out, size_t capacity)
{
    std::lock_guard<std::mutex> lk(g_m);
    return std::snprintf(out, capacity,
                         "dx11on12 enabled=%u hooked=%u requests=%u wrapped=%u with_swapchain=%u native=%u probes=%u swapchain_check=%d menu_buffer{mode=%d unwraps=%llu failures=%llu returns=%llu composites=%llu composite_failures=%llu unwrap_us=%llu/max%llu return_us=%llu/max%llu} last_hr=0x%08lX reason=\"%s\"",
                         g_enabled.load() ? 1u : 0u, g_hooked.load() ? 1u : 0u, g_requests.load(), g_wrapped.load(),
                         g_with_swapchain.load(), g_native.load(), g_probes.load(), g_swapchain_check.load(), g_menu_buffer.load(),
                         static_cast<unsigned long long>(g_unwraps.load()), static_cast<unsigned long long>(g_unwrap_failures.load()),
                         static_cast<unsigned long long>(g_returns.load()),
                         static_cast<unsigned long long>(g_composites.load()), static_cast<unsigned long long>(g_composite_failures.load()),
                         static_cast<unsigned long long>(g_unwraps.load() ? g_unwrap_us.load() / g_unwraps.load() : 0),
                         static_cast<unsigned long long>(g_unwrap_max_us.load()),
                         static_cast<unsigned long long>(g_returns.load() ? g_return_us.load() / g_returns.load() : 0),
                         static_cast<unsigned long long>(g_return_max_us.load()),
                         static_cast<unsigned long>(g_last_hr.load()), g_reason);
}

} // namespace na::on12
