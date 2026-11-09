#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"

#include <windows.h>
#include <d3d11.h>
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
// [dx11on12] menu_buffer: 0 never borrow the back buffer (menu does not draw), 1 borrow and return
// with the menu's fence, 2 borrow and return with no fence (same queue, so already ordered),
// 3 borrow and return but skip the GPU draw (diagnostic: separates the draw from the borrow).
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
struct Token { ComPtr<ID3D11On12Device2> device; ComPtr<ID3D11Resource> buffer; };
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

bool UnwrapBackBuffer(IDXGISwapChain* sc, unsigned index, ID3D12CommandQueue* queue, ID3D12Resource** out, Borrowed& borrowed)
{
    if (!g_hooked.load() || g_menu_buffer.load() == 0 || !sc || !queue || !out) return false;
    ComPtr<IUnknown> any;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&any)))) return false;
    ComPtr<ID3D11On12Device2> device;
    if (FAILED(any.As(&device))) return false; // not an 11on12 chain
    ComPtr<ID3D11Resource> buffer;
    // D3D11 swap chains expose only buffer 0: it is always the current back buffer (the runtime
    // rotates the rest). Asking for the D3D12-style current index borrows a buffer that is queued
    // for display, which 11on12 must first wait out (probe e13b422: Presents of up to 2 s with the
    // menu open, none without it).
    (void)index;
    HRESULT hr = sc->GetBuffer(0, IID_PPV_ARGS(&buffer));
    const uint64_t t0 = now_us();
    if (SUCCEEDED(hr)) hr = device->UnwrapUnderlyingResource(buffer.Get(), queue, IID_PPV_ARGS(out));
    add_time(g_unwrap_us, g_unwrap_max_us, now_us() - t0);
    if (FAILED(hr) || !*out) { g_unwrap_failures.fetch_add(1); g_last_hr.store(static_cast<long>(hr)); return false; }
    auto* t = new Token{device, buffer};
    borrowed.token = t;
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
    if (g_menu_buffer.load() == 2) fence = nullptr; // our draw went to 11on12's own queue: already ordered
    // Back to 11on12 before Present; with our fence, 11on12 waits for the menu draw.
    const uint64_t t0 = now_us();
    t->device->ReturnUnderlyingResource(t->buffer.Get(), fence ? 1u : 0u, fence ? &v : nullptr, fence ? fences : nullptr);
    add_time(g_return_us, g_return_max_us, now_us() - t0);
    g_returns.fetch_add(1);
    delete t;
}

bool DrawOnBorrowed() { return g_menu_buffer.load() != 3; }

int FormatStatus(char* out, size_t capacity)
{
    std::lock_guard<std::mutex> lk(g_m);
    return std::snprintf(out, capacity,
                         "dx11on12 enabled=%u hooked=%u requests=%u wrapped=%u with_swapchain=%u native=%u probes=%u swapchain_check=%d menu_buffer{mode=%d unwraps=%llu failures=%llu returns=%llu unwrap_us=%llu/max%llu return_us=%llu/max%llu} last_hr=0x%08lX reason=\"%s\"",
                         g_enabled.load() ? 1u : 0u, g_hooked.load() ? 1u : 0u, g_requests.load(), g_wrapped.load(),
                         g_with_swapchain.load(), g_native.load(), g_probes.load(), g_swapchain_check.load(), g_menu_buffer.load(),
                         static_cast<unsigned long long>(g_unwraps.load()), static_cast<unsigned long long>(g_unwrap_failures.load()),
                         static_cast<unsigned long long>(g_returns.load()),
                         static_cast<unsigned long long>(g_unwraps.load() ? g_unwrap_us.load() / g_unwraps.load() : 0),
                         static_cast<unsigned long long>(g_unwrap_max_us.load()),
                         static_cast<unsigned long long>(g_returns.load() ? g_return_us.load() / g_returns.load() : 0),
                         static_cast<unsigned long long>(g_return_max_us.load()),
                         static_cast<unsigned long>(g_last_hr.load()), g_reason);
}

} // namespace na::on12
