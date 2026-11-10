#include "host_dx12/on12/d3d11_on12.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/host_boot/host_boot.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/native11/samplers.hpp"
#include "host_dx12/native_d3d11/live_dlaa.hpp"
#include "host_dx12/native_d3d11/shared_resources.hpp"
#include "host_dx12/observe/producers.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
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
// [dx11on12] route=native (WWII plan B1/B2): the game keeps its own D3D11 device; the native
// input provider hands each frame's scene to the DX12 effects through shared images.
std::atomic<bool> g_route_native{false};
std::atomic<uint32_t> g_native_installs{0}, g_native_install_failures{0};
// [dx11on12] menu_buffer: 1 (default) the menu draws into a canvas texture of ours, which the
// game's own D3D11 context then blends onto the back buffer; 2 the menu draws straight into the
// borrowed back buffer (D3D12 writes to an 11on12 swap chain's presented buffer make the D3D11
// present wait ~2 s on its latency semaphore: probe runs 2026-11-09, modes 1-6); 0 no menu.
std::atomic<int> g_menu_buffer{1};
std::atomic<bool> g_copy_with_d3d11{false};
std::atomic<bool> g_keep_game_swapchain{false};
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

// One startup-log line per creation step (thread id + ms since the request), so a
// stall inside the game's device creation names its step.
void step(const char* what, DWORD thread, ULONGLONG t0)
{
    if (!g_log) return;
    char line[160];
    std::snprintf(line, sizeof line, "dx11on12: step %s (request thread %lu, +%llu ms)", what, thread,
                  static_cast<unsigned long long>(GetTickCount64() - t0));
    g_log(line);
}

// Builds the 11on12 device on our own D3D12 device + direct queue. Returns
// false (nothing handed out, nothing kept) on any failure.
struct DeviceResult {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D12Device> backend;
    ComPtr<ID3D12CommandQueue> queue;
    D3D_FEATURE_LEVEL level{};
};

bool build_on12(IDXGIAdapter* adapter, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT level_count,
                DeviceResult& result, DWORD thread, ULONGLONG t0)
{
    if (!g_on12_create || !g_d3d12_create) { note("D3D11On12 or D3D12 is not available; native DX11"); return false; }
    step("D3D12CreateDevice", thread, t0);
    ComPtr<ID3D12Device> d12;
    HRESULT hr = g_d3d12_create(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    if (FAILED(hr) || !d12) { note("no D3D12 device on the game's adapter; native DX11", hr); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    step("CreateCommandQueue", thread, t0);
    if (FAILED(hr = d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { note("D3D12 queue creation failed; native DX11", hr); return false; }
    IUnknown* queues[] = {queue.Get()};
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL chosen{};
    step("D3D11On12CreateDevice", thread, t0);
    hr = g_on12_create(d12.Get(), flags, levels, level_count, queues, 1, 0, &d11, &ctx, &chosen);
    if (FAILED(hr) || !d11) { note("D3D11On12CreateDevice refused the game's request; native DX11", hr); return false; }
    step("swap chain check", thread, t0);
    if (!swapchain_supported(d11.Get())) return false;
    step("done", thread, t0);
    result = {std::move(d11), std::move(ctx), std::move(d12), std::move(queue), chosen};
    return true;
}

// The game's request runs on a worker thread of ours, and the game waits at most
// kCreateTimeoutMs. The private build runs independently of host attachment.
// A driver stall still falls back to native DX11 instead of blocking forever.
constexpr DWORD kCreateTimeoutMs = 25000;
struct CreateJob {
    IDXGIAdapter* adapter = nullptr;
    UINT flags = 0;
    D3D_FEATURE_LEVEL levels[16]{};
    UINT level_count = 0;
    bool default_levels = false;
    DWORD thread = 0;
    ULONGLONG t0 = 0;
    std::mutex m;
    bool finished = false, abandoned = false, ok = false;
    DeviceResult result;
    HANDLE done = nullptr;
    ~CreateJob()
    {
        if (adapter) adapter->Release();
        if (done) CloseHandle(done);
    }
};

DWORD WINAPI create_worker(LPVOID param)
{
    std::shared_ptr<CreateJob> job(*static_cast<std::shared_ptr<CreateJob>*>(param));
    delete static_cast<std::shared_ptr<CreateJob>*>(param);
    ++t_inside; // 11on12 may call D3D11CreateDevice itself: inner calls go to the original
    // The intercepted game thread is waiting for this worker. The worker's
    // own DXGI factory calls cannot also wait on the startup installation.
    na::host::InternalFactoryScope private_factories;
    DeviceResult result;
    const bool ok = build_on12(job->adapter, job->flags, job->default_levels ? nullptr : job->levels,
                              job->level_count, result, job->thread, job->t0);
    --t_inside;
    std::lock_guard<std::mutex> lk(job->m);
    job->finished = true;
    if (job->abandoned) { // the game already went native: drop what we built
        note("a timed-out 11on12 creation finished late and was released; the game runs native DX11");
    } else {
        job->ok = ok;
        job->result = std::move(result);
    }
    SetEvent(job->done);
    return 0;
}

bool create_on12(IDXGIAdapter* adapter, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT level_count,
                 ID3D11Device** out_device, D3D_FEATURE_LEVEL* out_level, ID3D11DeviceContext** out_context)
{
    auto job = std::make_shared<CreateJob>();
    job->adapter = adapter;
    if (adapter) adapter->AddRef();
    job->flags = flags;
    job->default_levels = !levels || !level_count;
    job->level_count = job->default_levels ? 0 : (level_count < 16 ? level_count : 16);
    for (UINT i = 0; i < job->level_count; ++i) job->levels[i] = levels[i];
    job->thread = GetCurrentThreadId();
    job->t0 = GetTickCount64();
    job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->done) { note("could not start the 11on12 creation; native DX11", HRESULT_FROM_WIN32(GetLastError())); return false; }
    step("request", job->thread, job->t0);
    auto* param = new std::shared_ptr<CreateJob>(job);
    HANDLE worker = CreateThread(nullptr, 0, &create_worker, param, 0, nullptr);
    if (!worker) {
        delete param;
        note("could not start the 11on12 creation; native DX11", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    CloseHandle(worker);
    const DWORD wait = WaitForSingleObject(job->done, kCreateTimeoutMs);
    std::lock_guard<std::mutex> lk(job->m);
    if (!job->finished) {
        job->abandoned = true;
        (void)wait;
        note("11on12 device creation did not finish in 25 s (see the last 'step' line); native DX11");
        return false;
    }
    if (!job->ok) return false;
    auto& result = job->result;
    remember(result.device.Get(), result.backend.Get(), result.queue.Get());
    if (out_level) *out_level = result.level;
    if (out_context) *out_context = result.context.Detach();
    *out_device = result.device.Detach();
    return true;
}

void report_wrapped()
{
    g_wrapped.fetch_add(1);
    note("the game received a D3D11On12 device backed by our D3D12 queue");
}

// Requests this layer answers: a real device on hardware, no debug layer.
bool wanted(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, UINT flags, void* out_device)
{
    if (!g_enabled.load() || t_inside > 0) return false;
    if (na::host::IsInternalGraphicsCall()) return false;
    if (!out_device) { g_probes.fetch_add(1); return false; } // feature-level probe
    if (flags & D3D11_CREATE_DEVICE_DEBUG) { note("debug-layer device requested; native DX11"); return false; }
    if (adapter ? type != D3D_DRIVER_TYPE_UNKNOWN : type != D3D_DRIVER_TYPE_HARDWARE) {
        note("software/WARP/reference device requested; native DX11"); return false;
    }
    return true;
}

bool setup_native_menu(ID3D11Device* d11); // below, with the menu canvas

// Native route: the game's own device, created by the original call, gets the input provider with
// the DX12 effects handoff as its consumer. The provider's probe device goes straight to the
// original (t_inside is held by the caller).
void install_native(ID3D11Device* device)
{
    if (!device) return;
    if (!setup_native_menu(device)) g_native_install_failures.fetch_add(1); // DLAA can still run; the menu cannot
    // DLAA texture detail (mip bias) and AF on the game's own samplers, live from the menu's state.
    if (!na::native11::InstallSamplers(device, [] { return na::observe::SamplerMipBias(); },
                                       [] { return na::observe::SamplerAnisotropyWanted(); }))
        note("native DX11 route: the sampler hooks (texture detail / AF) could not be installed");
    // Jitter through the game's viewports: no camera layout has to be proven per game.
    na::native11::SetJitterRoute(na::native11::JitterRoute::Viewport);
    if (na::native11::Install(device, &na::native_d3d11::EvaluateFrame, nullptr, true)) {
        g_native_installs.fetch_add(1);
        note("native DX11 route: the input provider is on the game's own D3D11 device");
    } else {
        g_native_install_failures.fetch_add(1);
        note("native DX11 route: the input provider could not hook the game's device");
    }
}

HRESULT WINAPI DetourCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                  const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk,
                                  ID3D11Device** out_device, D3D_FEATURE_LEVEL* out_level, ID3D11DeviceContext** out_context)
{
    if (g_route_native.load() && wanted(adapter, type, flags, out_device)) {
        g_requests.fetch_add(1);
        ++t_inside;
        const HRESULT hr = g_orig_create(adapter, type, sw, flags, levels, level_count, sdk, out_device, out_level, out_context);
        if (SUCCEEDED(hr) && *out_device) install_native(*out_device);
        --t_inside;
        return hr;
    }
    if (wanted(adapter, type, flags, out_device)) {
        g_requests.fetch_add(1);
        ++t_inside;
        const bool ok = create_on12(adapter, flags, levels, level_count, out_device, out_level, out_context);
        --t_inside;
        if (ok) { report_wrapped(); return S_OK; }
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
    if (g_route_native.load() && wanted(adapter, type, flags, out_device)) {
        g_requests.fetch_add(1);
        ++t_inside;
        const HRESULT hr = g_orig_create_sc(adapter, type, sw, flags, levels, level_count, sdk, desc, out_sc, out_device, out_level, out_context);
        if (SUCCEEDED(hr) && *out_device) install_native(*out_device);
        --t_inside;
        return hr;
    }
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
            report_wrapped();
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
    const bool native = GetPrivateProfileIntW(L"native11", L"enabled", 0, ini) == 1;
    const bool bridge = GetPrivateProfileIntW(L"dx11on12", L"enabled", 0, ini) == 1;
    if (!native && !bridge) {
        std::snprintf(g_reason, sizeof g_reason, "Native DX11 and D3D11On12 disabled.");
        return;
    }
    HMODULE d3d11 = system_module(L"d3d11.dll");
    if (!d3d11) { note("system d3d11.dll not found; DX11 bridge off"); return; }
    if (!native) {
        HMODULE d3d12 = system_module(L"d3d12.dll");
        g_on12_create = reinterpret_cast<On12CreateDeviceFn>(GetProcAddress(d3d11, "D3D11On12CreateDevice"));
        g_d3d12_create = d3d12 ? reinterpret_cast<D3D12CreateDeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    }
    void* create = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    void* create_sc = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    bool ok = create && create_sc &&
              na::hooking::HookFunction(create, reinterpret_cast<void*>(&DetourCreateDevice), reinterpret_cast<void**>(&g_orig_create)) &&
              na::hooking::HookFunction(create_sc, reinterpret_cast<void*>(&DetourCreateDeviceAndSwapChain), reinterpret_cast<void**>(&g_orig_create_sc));
    if (!ok) { note("could not hook D3D11CreateDevice; DX11 bridge off"); return; }
    g_menu_buffer.store(static_cast<int>(GetPrivateProfileIntW(L"dx11on12", L"menu_buffer", 1, ini)));
    g_copy_with_d3d11.store(GetPrivateProfileIntW(L"dx11on12", L"copy_with_d3d11", 0, ini) != 0);
    g_keep_game_swapchain.store(GetPrivateProfileIntW(L"dx11on12", L"keep_game_swapchain", 0, ini) != 0);
    g_route_native.store(native);
    g_hooked.store(true);
    g_enabled.store(true);
    note(g_route_native.load() ? "D3D11 device creation hooked; native DX11 route (no translation), DLAA through shared images"
                               : "D3D11 device creation hooked; the game's DX11 device will run on D3D12");
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

namespace {
ComPtr<ID3D11Device> swapchain_device(IDXGISwapChain* chain)
{
    ComPtr<ID3D11Device> device;
    if (!g_hooked.load() || !chain) return device;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))) {
        ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) backbuffer->GetDevice(&device);
    }
    return device;
}
} // namespace

ID3D12CommandQueue* QueueForSwapchain(IDXGISwapChain* chain)
{
    return QueueFor(swapchain_device(chain).Get());
}

ID3D12Device* DeviceForSwapchain(IDXGISwapChain* chain)
{
    const auto device = swapchain_device(chain);
    IUnknown* id = identity_of(device.Get());
    if (!id) return nullptr;
    std::lock_guard<std::mutex> lk(g_m);
    for (const auto& e : g_entries)
        if (e.identity == id && e.device) { e.device->AddRef(); return e.device; }
    return nullptr;
}

void FlushPresentedWork(IDXGISwapChain* chain)
{
    const auto device = swapchain_device(chain);
    ComPtr<ID3D11DeviceContext> context;
    if (device) device->GetImmediateContext(&context);
    if (context) context->Flush();
}

namespace {
struct Token {
    bool native = false;                // native DX11 route: the canvas is a shared image, nothing to return
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
bool composite(ID3D11Device* d11, ID3D11Resource* back_buffer)
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
        return false;
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
    return true;
}
} // namespace

namespace {
// ---- Native DX11 route: the Home menu --------------------------------------
// The game keeps its own D3D11 device and swap chain. The overlay draws the menu with D3D12 as it
// does for the bridge's kept 11on12 chains: our D3D12 device and queue on the game's adapter are
// registered for the game's device (QueueFor / DeviceForSwapchain), and UnwrapBackBuffer hands the
// overlay a canvas. Here the canvas is one image shared by both APIs; after the overlay's submit,
// our queue signals a shared fence, the game's immediate context waits on it (GPU-side) and the
// canvas is blended onto the back buffer with the same composite as the bridge.
struct NativeMenu {
    IUnknown* identity = nullptr; // the game's device (identity only)
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    std::unique_ptr<na::native_d3d11::SharedFences> fences;
    uint64_t native_done = 0, effects_done = 0;
    ComPtr<ID3D12Resource> canvas12;
};
std::mutex g_nm_m;
NativeMenu g_nm;
std::atomic<uint64_t> g_native_menu_draws{0}, g_native_menu_failures{0};

bool is_native_device(IUnknown* device)
{
    IUnknown* id = identity_of(device);
    std::lock_guard<std::mutex> lk(g_nm_m);
    return id && id == g_nm.identity;
}

// Our D3D12 device (the adapter's one device: the effects handoff gets the same) and a direct
// queue, plus the shared fence the game's context waits on. Registered like a bridge device.
bool setup_native_menu(ID3D11Device* d11)
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    NativeMenu m;
    m.d11 = d11;
    m.identity = identity_of(d11);
    HRESULT hr = d11->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) {
        na::host::InternalFactoryScope internal;
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m.d12));
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(hr)) hr = m.d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m.queue));
    if (SUCCEEDED(hr)) {
        m.fences = std::make_unique<na::native_d3d11::SharedFences>();
        hr = m.fences->Create(d11, m.d12.Get(), m.queue.Get());
    }
    if (FAILED(hr) || !m.identity) {
        note("native DX11 route: the menu's D3D12 queue or shared fence could not be created", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    m.queue->SetName(L"NeuralAlias native DX11 menu queue");
    remember(d11, m.d12.Get(), m.queue.Get());
    std::lock_guard<std::mutex> lk(g_nm_m);
    g_nm = std::move(m);
    return true;
}

// The shared canvas, sized and formatted like the back buffer. Caller holds g_canvas_m and g_nm_m.
bool ensure_native_canvas(const D3D11_TEXTURE2D_DESC& back, DXGI_FORMAT format)
{
    if (g_canvas.texture && g_nm.canvas12 && g_canvas.width == back.Width && g_canvas.height == back.Height && g_canvas.format == format) return true;
    if (g_nm.canvas12 && !g_nm.fences->Retired(g_nm.native_done, g_nm.effects_done)) return false;
    g_canvas.texture.Reset();
    g_canvas.srv.Reset();
    g_nm.canvas12.Reset();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = back.Width;
    rd.Height = back.Height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    ComPtr<ID3D12Resource> canvas;
    ComPtr<ID3D11Device1> d1;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    HANDLE h = nullptr;
    HRESULT hr = g_nm.d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&canvas));
    if (SUCCEEDED(hr)) hr = g_nm.d12->CreateSharedHandle(canvas.Get(), nullptr, GENERIC_ALL, nullptr, &h);
    if (SUCCEEDED(hr)) hr = g_nm.d11.As(&d1);
    if (SUCCEEDED(hr)) hr = d1->OpenSharedResource1(h, IID_PPV_ARGS(&texture));
    if (h) CloseHandle(h);
    if (SUCCEEDED(hr)) hr = g_nm.d11->CreateShaderResourceView(texture.Get(), nullptr, &srv);
    if (FAILED(hr)) { g_last_hr.store(static_cast<long>(hr)); return false; }
    canvas->SetName(L"NeuralAlias native DX11 menu canvas");
    g_nm.canvas12 = canvas;
    g_nm.native_done = g_nm.effects_done = 0;
    g_canvas.texture = texture;
    g_canvas.srv = srv;
    g_canvas.width = back.Width;
    g_canvas.height = back.Height;
    g_canvas.format = format;
    return true;
}

bool unwrap_native(IDXGISwapChain* sc, ID3D11Device* d11, DXGI_FORMAT format, ID3D12Resource** out, Borrowed& borrowed)
{
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return false;
    D3D11_TEXTURE2D_DESC back{};
    buffer->GetDesc(&back);
    ComPtr<ID3D12Resource> canvas;
    {
        std::lock_guard<std::mutex> ck(g_canvas_m);
        std::lock_guard<std::mutex> nk(g_nm_m);
        if (!ensure_pipeline(d11) || !ensure_native_canvas(back, format)) {
            g_native_menu_failures.fetch_add(1);
            return false;
        }
        if (g_nm.native_done && FAILED(g_nm.fences->WaitEffects(g_nm.native_done))) {
            g_native_menu_failures.fetch_add(1);
            return false;
        }
        canvas = g_nm.canvas12;
    }
    *out = canvas.Detach();
    auto* t = new Token{};
    t->native = true;
    t->back_buffer = buffer;
    t->d11 = d11;
    borrowed.token = t;
    borrowed.clear = true;
    return true;
}

} // namespace

bool UnwrapBackBuffer(IDXGISwapChain* sc, unsigned index, ID3D12CommandQueue* queue, unsigned format, ID3D12Resource** out, Borrowed& borrowed)
{
    const int mode = g_menu_buffer.load();
    if (g_route_native.load() && g_hooked.load() && sc && queue && out) {
        auto d11 = swapchain_device(sc);
        if (d11 && is_native_device(d11.Get())) {
            return unwrap_native(sc, d11.Get(), static_cast<DXGI_FORMAT>(format), out, borrowed);
        }
    }
    if (!g_hooked.load() || mode == 0 || !sc || !queue || !out) return false;
    ComPtr<ID3D11On12Device2> device;
    auto d11 = swapchain_device(sc);
    if (!d11 || FAILED(d11.As(&device))) return false; // not an 11on12 chain
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
    borrowed.token = new Token{false, device, target, canvas ? ComPtr<ID3D11Resource>(buffer) : ComPtr<ID3D11Resource>{}, d11};
    borrowed.clear = canvas;
    g_unwraps.fetch_add(1);
    return true;
}

void Borrowed::Submitted(ID3D12Fence* f, unsigned long long v) { fence = f; value = v; }

Borrowed::~Borrowed()
{
    auto* t = static_cast<Token*>(token);
    if (!t) return;
    if (t->native) { // the overlay submitted the menu draw on our queue: order the game's context after it, then blend
        bool drawn = false;
        if (fence) {
            std::lock_guard<std::mutex> ck(g_canvas_m);
            std::lock_guard<std::mutex> nk(g_nm_m);
            if (g_nm.fences && g_canvas.owner == t->d11.Get() && g_canvas.state &&
                SUCCEEDED(g_nm.fences->SignalEffects(&g_nm.effects_done)) &&
                SUCCEEDED(g_nm.fences->WaitNative(g_nm.effects_done))) {
                na::native11::OwnWorkScope own;
                const bool blended = composite(t->d11.Get(), t->back_buffer.Get());
                drawn = SUCCEEDED(g_nm.fences->SignalNative(&g_nm.native_done)) && blended;
            }
        }
        (drawn ? g_native_menu_draws : g_native_menu_failures).fetch_add(1);
        delete t;
        return;
    }
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

// ---- Translated presentation ------------------------------------------------
namespace {

// D3D12 swap chains are flip-model and accept only these buffer formats. The
// game's sRGB/typeless variants stay on its virtual buffer (same family, so a
// plain copy keeps the bytes).
DXGI_FORMAT flip_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
bool typeless(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
           f == DXGI_FORMAT_R16G16B16A16_TYPELESS;
}
DXGI_SWAP_EFFECT flip_effect(DXGI_SWAP_EFFECT e)
{
    return e == DXGI_SWAP_EFFECT_SEQUENTIAL || e == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                                                                                      : DXGI_SWAP_EFFECT_FLIP_DISCARD;
}
UINT flip_count(UINT n) { return n == 0 ? 0u : (n < 2 ? 2u : (n > 16 ? 16u : n)); }
UINT flip_flags(UINT f) { return f & ~static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE); }
// The D3D12 chain's buffers only need to be presented, copied into and read by the program.
DXGI_USAGE flip_usage(DXGI_USAGE u) { return DXGI_USAGE_RENDER_TARGET_OUTPUT | (u & DXGI_USAGE_SHADER_INPUT); }

struct TChain {
    IDXGISwapChain* sc = nullptr; // identity only; never AddRef'ed (the game owns the chain)
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11On12Device> on12;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    DXGI_FORMAT game_format = DXGI_FORMAT_UNKNOWN;
    UINT game_count = 1;
    DXGI_SWAP_EFFECT game_effect = DXGI_SWAP_EFFECT_DISCARD;
    DXGI_SAMPLE_DESC game_samples{1, 0};
    DXGI_USAGE game_usage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    UINT width = 0, height = 0;
    ComPtr<ID3D12Resource> virt12;   // the game's back buffer, owned here
    ComPtr<ID3D11Resource> virt11;   // the same texture as D3D11 sees it (wrapped once)
    D3D12_RESOURCE_STATES virt_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ComPtr<ID3D12Fence> fence;
    uint64_t fence_value = 0;
    HANDLE event = nullptr;
    // A resize in flight: what the game asked for, adopted only when DXGI succeeds.
    bool resizing = false;
    UINT pending_count = 0;
    DXGI_FORMAT pending_format = DXGI_FORMAT_UNKNOWN;
    struct Slot { ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list; uint64_t value = 0; } slots[4];
    uint32_t next = 0;
};
// Recursive: BeforePresent asks the chain for its D3D12 buffer through the hooked GetBuffer.
std::recursive_mutex g_tm;
TChain g_tchains[4];
// A deferred backbuffer destruction can re-enter the swap chain's Release
// from the translation worker while ResizeBuffers waits for that worker.
std::atomic<IDXGISwapChain*> g_tchain_ids[std::size(g_tchains)]{};
std::atomic<uint32_t> g_tchain_count{0};
std::atomic<uint64_t> g_translated{0}, g_untranslated{0}, g_tcopies{0}, g_tcopy_failures{0}, g_tresizes{0}, g_tresize_waits{0};
std::atomic<uint64_t> g_tcopies_d3d11{0};
// Where a present's time goes (CPU, this thread), since the last status line: the
// D3D11->D3D12 handoff (release + flush), our copy submission, the reacquire, and the
// DXGI Present itself. Cort 2026-11-09: main menu 17-20 fps, GPU 0%, two busy threads.
std::atomic<uint64_t> g_t_release_us{0}, g_t_release_max{0}, g_t_copy_us{0}, g_t_copy_max{0}, g_t_acquire_us{0}, g_t_acquire_max{0},
    g_t_present_us{0}, g_t_present_max{0}, g_t_presents{0}, g_t_handoffs{0};
std::atomic<long> g_tlast_hr{0};
char g_treason[160] = "No translated swap chain yet.";

void tnote(const char* reason, HRESULT hr = S_OK)
{
    {
        std::lock_guard<std::mutex> lk(g_m);
        std::snprintf(g_treason, sizeof g_treason, "%s", reason);
    }
    g_tlast_hr.store(static_cast<long>(hr));
    if (g_log) {
        char line[224];
        std::snprintf(line, sizeof line, "dx11on12 present: %s (hr=0x%08lX)", reason, static_cast<unsigned long>(hr));
        g_log(line);
    }
}

TChain* tfind(IDXGISwapChain* sc) // caller holds g_tm
{
    if (!sc) return nullptr;
    for (auto& c : g_tchains) if (c.sc == sc) return &c;
    return nullptr;
}

bool d3d12_interface(REFIID riid)
{
    return riid == __uuidof(ID3D12Resource) || riid == __uuidof(ID3D12Resource1) ||
           riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Object);
}

// Wait until our fence reaches `value` (bounded: never hang the game's thread).
bool twait(TChain& c, uint64_t value, DWORD ms)
{
    if (!value || !c.fence || c.fence->GetCompletedValue() >= value) return true;
    if (value == UINT64_MAX) return false; // a submission without a fence receipt: only a drain clears it
    if (!c.event) c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!c.event || FAILED(c.fence->SetEventOnCompletion(value, c.event))) return false;
    // The auto-reset event may still carry the signal of an earlier registration that timed
    // out: only the fence value itself says the work is done. Wait out the budget for it.
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = now >= deadline ? 0u : static_cast<DWORD>(deadline - now);
        const DWORD w = WaitForSingleObject(c.event, left);
        if (c.fence->GetCompletedValue() >= value) return true;
        if (w != WAIT_OBJECT_0 || !left) return false;
    }
}

// Everything submitted to the queue so far, D3D11 work included, has finished.
bool tdrain(TChain& c)
{
    if (!c.queue || !c.fence) return true;
    if (c.ctx) c.ctx->Flush();
    if (FAILED(c.queue->Signal(c.fence.Get(), ++c.fence_value))) return false;
    const bool done = twait(c, c.fence_value, 2000);
    if (!done) g_tresize_waits.fetch_add(1);
    else for (auto& s : c.slots) if (s.value == UINT64_MAX) s.value = 0; // everything before this drain has completed
    return done;
}

HRESULT copy_on_d3d11(TChain& c, ID3D12Resource* target, DXGI_FORMAT format, bool same_size)
{
    D3D11_RESOURCE_FLAGS flags{};
    ComPtr<ID3D11Resource> wrapper;
    HRESULT hr = c.on12->CreateWrappedResource(
        target, &flags, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
        IID_PPV_ARGS(&wrapper));
    if (FAILED(hr)) return hr;
    ID3D11Resource* dest = wrapper.Get(); // a newly wrapped resource starts acquired
    if (c.game_samples.Count > 1) {
        c.ctx->ResolveSubresource(dest, 0, c.virt11.Get(), 0, format);
    } else if (same_size) {
        c.ctx->CopyResource(dest, c.virt11.Get());
    } else {
        const auto desc = target->GetDesc();
        const D3D11_BOX overlap{0, 0, 0, static_cast<UINT>((std::min<UINT64>)(desc.Width, c.width)),
                               (std::min)(desc.Height, c.height), 1};
        c.ctx->CopySubresourceRegion(dest, 0, 0, 0, 0, c.virt11.Get(), 0, &overlap);
    }
    // Only the destination crosses the API boundary. The translator owns both
    // the game's writes and this copy, through the same immediate context.
    c.on12->ReleaseWrappedResources(&dest, 1);
    // Keeping the wrapper across frames also keeps an indirect reference to
    // the swap chain and hides the game's last Release from our teardown hook.
    wrapper.Reset();
    c.ctx->Flush();
    hr = c.queue->Signal(c.fence.Get(), c.fence_value + 1);
    if (SUCCEEDED(hr)) ++c.fence_value;
    return hr;
}

void drop_virtual(TChain& c)
{
    c.virt11.Reset();
    c.virt12.Reset();
}

// The game's back buffer: a D3D12 texture of ours with the format, samples and
// size the game asked for, wrapped once for D3D11 and kept acquired by D3D11
// except inside our Present copy.
bool build_virtual(TChain& c)
{
    drop_virtual(c);
    if (!c.width || !c.height || c.game_format == DXGI_FORMAT_UNKNOWN) return false;
    const bool msaa = c.game_samples.Count > 1;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = c.width;
    rd.Height = c.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = c.game_format;
    rd.SampleDesc = c.game_samples;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!msaa && (c.game_usage & DXGI_USAGE_UNORDERED_ACCESS)) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    c.virt_state = msaa ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
    HRESULT hr = c.d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, c.virt_state, nullptr, IID_PPV_ARGS(&c.virt12));
    if (FAILED(hr)) { tnote("the game's back buffer could not be created", hr); return false; }
    D3D11_RESOURCE_FLAGS f11{};
    f11.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) f11.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    // D3D11 takes it in our copy's state and leaves it there when we take it back.
    hr = c.on12->CreateWrappedResource(c.virt12.Get(), &f11, c.virt_state, c.virt_state, IID_PPV_ARGS(&c.virt11));
    if (FAILED(hr)) { c.virt12.Reset(); tnote("the game's back buffer could not be shared with D3D11", hr); return false; }
    ID3D11Resource* r[] = {c.virt11.Get()};
    c.on12->AcquireWrappedResources(r, 1);
    return true;
}

bool tlist(TChain& c, TChain::Slot*& out)
{
    TChain::Slot& s = c.slots[c.next++ % 4];
    if (!twait(c, s.value, 1000)) return false;
    if (!s.alloc) {
        if (FAILED(c.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc))) ||
            FAILED(c.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc.Get(), nullptr, IID_PPV_ARGS(&s.list)))) {
            s.alloc.Reset(); s.list.Reset(); return false;
        }
    } else if (FAILED(s.alloc->Reset()) || FAILED(s.list->Reset(s.alloc.Get(), nullptr))) {
        return false;
    }
    out = &s;
    return true;
}

void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    l->ResourceBarrier(1, &b);
}

bool init_translated(TChain& c, IDXGISwapChain* sc, IUnknown* device)
{
    ComPtr<IUnknown> unk(device);
    ComPtr<ID3D12CommandQueue> queue;
    queue.Attach(QueueFor(device));
    if (!queue || FAILED(unk.As(&c.d11)) || FAILED(unk.As(&c.on12)) || FAILED(queue->GetDevice(IID_PPV_ARGS(&c.d12)))) return false;
    c.queue = queue;
    c.d11->GetImmediateContext(&c.ctx);
    if (!c.ctx || FAILED(c.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)))) return false;
    c.sc = sc;
    return true;
}

void read_size(TChain& c)
{
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(c.sc->GetDesc(&d))) { c.width = d.BufferDesc.Width; c.height = d.BufferDesc.Height; } // our module: real values
}

bool translate_common(IUnknown* device, DXGI_FORMAT format, ID3D12CommandQueue** queue)
{
    if (!g_hooked.load() || !device || !queue) return false;
    if (is_native_device(device)) return false; // native DX11 route: the game keeps its own swap chain
    *queue = QueueFor(device);
    if (!*queue) return false; // not our 11on12 device: the game's request is left as it was
    if (g_keep_game_swapchain.load(std::memory_order_relaxed)) {
        (*queue)->Release(); *queue = nullptr;
        g_untranslated.fetch_add(1);
        tnote("the game keeps its D3D11On12 swap chain and presentation ownership");
        return false;
    }
    if (flip_format(format) == DXGI_FORMAT_UNKNOWN) {
        (*queue)->Release(); *queue = nullptr;
        g_untranslated.fetch_add(1);
        tnote("the game's back-buffer format has no D3D12 swap chain equivalent; 11on12 swap chain kept");
        return false;
    }
    return true;
}

} // namespace

bool Enabled() { return g_hooked.load() && !g_route_native.load(); }
bool NativeRoute() { return g_enabled.load() && g_route_native.load(); }

bool TranslateSwapChain(IUnknown* device, const DXGI_SWAP_CHAIN_DESC& game, DXGI_SWAP_CHAIN_DESC& real, ID3D12CommandQueue** queue)
{
    if (!translate_common(device, game.BufferDesc.Format, queue)) return false;
    real = game;
    real.BufferDesc.Format = flip_format(game.BufferDesc.Format);
    real.BufferCount = flip_count(game.BufferCount ? game.BufferCount : 1);
    real.SwapEffect = flip_effect(game.SwapEffect);
    real.SampleDesc = {1, 0};
    real.BufferUsage = flip_usage(game.BufferUsage);
    real.Flags = flip_flags(game.Flags);
    return true;
}

bool TranslateSwapChain1(IUnknown* device, const DXGI_SWAP_CHAIN_DESC1& game, DXGI_SWAP_CHAIN_DESC1& real, ID3D12CommandQueue** queue)
{
    if (!translate_common(device, game.Format, queue)) return false;
    real = game;
    real.Format = flip_format(game.Format);
    real.BufferCount = flip_count(game.BufferCount ? game.BufferCount : 1);
    real.SwapEffect = flip_effect(game.SwapEffect);
    real.SampleDesc = {1, 0};
    real.BufferUsage = flip_usage(game.BufferUsage);
    real.Flags = flip_flags(game.Flags);
    if (real.Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH) real.Scaling = DXGI_SCALING_STRETCH; // composition-only mode
    return true;
}

bool AttachSwapChain(IDXGISwapChain* chain, IUnknown* device, DXGI_FORMAT game_format, UINT game_count, DXGI_SWAP_EFFECT game_effect,
                     DXGI_SAMPLE_DESC game_samples, DXGI_USAGE game_usage)
{
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    TChain* c = tfind(chain);
    if (!c) for (auto& k : g_tchains) if (!k.sc) { c = &k; break; }
    if (!c) { tnote("more than four translated swap chains; the game gets its own chain"); return false; }
    auto& published = g_tchain_ids[c - g_tchains];
    const bool reused = c->sc == chain;
    if (c->event) CloseHandle(c->event);
    *c = TChain{};
    auto fail = [&](const char* why) {
        *c = TChain{};
        if (reused) {
            published.store(nullptr);
            g_tchain_count.fetch_sub(1);
        }
        g_untranslated.fetch_add(1);
        tnote(why);
        return false;
    };
    if (!init_translated(*c, chain, device)) return fail("the translated swap chain could not be attached; the game gets its own chain");
    c->game_format = game_format;
    c->game_count = game_count;
    c->game_effect = game_effect;
    c->game_samples = game_samples.Count ? game_samples : DXGI_SAMPLE_DESC{1, 0};
    c->game_usage = game_usage;
    read_size(*c);
    if (!build_virtual(*c)) return fail("the game's back buffer could not be built; the game gets its own chain");
    published.store(chain);
    if (!reused) g_tchain_count.fetch_add(1);
    g_translated.fetch_add(1);
    char line[160];
    std::snprintf(line, sizeof line, "the game presents through a D3D12 swap chain (%ux%u, format %d, %u sample(s))", c->width, c->height,
                  static_cast<int>(game_format), c->game_samples.Count);
    tnote(line);
    return true;
}

bool IsTranslated(IDXGISwapChain* chain)
{
    if (!chain) return false;
    for (const auto& id : g_tchain_ids) if (id.load() == chain) return true;
    return false;
}

bool HandleGetBuffer(IDXGISwapChain* chain, unsigned index, const GUID& riid, void** out, long* hr)
{
    (void)index; // D3D11 semantics: every index the game may ask for is its one back buffer
    if (!g_tchain_count.load(std::memory_order_relaxed) || !out || d3d12_interface(riid)) return false;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    TChain* c = tfind(chain);
    if (!c) return false;
    if (!c->virt11) { *out = nullptr; *hr = DXGI_ERROR_INVALID_CALL; return true; }
    *hr = c->virt11->QueryInterface(riid, out);
    return true;
}

bool HandleGetDevice(IDXGISwapChain* chain, const GUID& riid, void** out)
{
    if (!g_tchain_count.load(std::memory_order_relaxed) || !out) return false;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    TChain* c = tfind(chain);
    return c && c->d11 && SUCCEEDED(c->d11->QueryInterface(riid, out));
}

void RewriteDesc(IDXGISwapChain* chain, DXGI_SWAP_CHAIN_DESC* d)
{
    if (!d || !g_tchain_count.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    const TChain* c = tfind(chain);
    if (!c) return;
    d->BufferDesc.Format = c->game_format;
    d->BufferCount = c->game_count;
    d->SwapEffect = c->game_effect;
    d->SampleDesc = c->game_samples;
    d->BufferUsage = c->game_usage;
}

void RewriteDesc1(IDXGISwapChain* chain, DXGI_SWAP_CHAIN_DESC1* d)
{
    if (!d || !g_tchain_count.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    const TChain* c = tfind(chain);
    if (!c) return;
    d->Format = c->game_format;
    d->BufferCount = c->game_count;
    d->SwapEffect = c->game_effect;
    d->SampleDesc = c->game_samples;
    d->BufferUsage = c->game_usage;
}

void BeforePresent(IDXGISwapChain* chain, unsigned flags)
{
    if (!g_tchain_count.load(std::memory_order_relaxed) || (flags & DXGI_PRESENT_TEST)) return;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    TChain* c = tfind(chain);
    if (!c || c->resizing || !c->virt11) return;
    // The chain's current buffer, asked for the way a DX12 game asks: through
    // the hooked GetBuffer / GetCurrentBackBufferIndex, so the presenter's
    // buffers (when it owns this chain) are what we fill.
    ComPtr<IDXGISwapChain3> sc3;
    ComPtr<ID3D12Resource> target;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&sc3)))) { g_tcopy_failures.fetch_add(1); return; }
    const UINT index = sc3->GetCurrentBackBufferIndex();
    HRESULT hr = chain->GetBuffer(index, IID_PPV_ARGS(&target));
    if (FAILED(hr) || !target) { g_tcopy_failures.fetch_add(1); g_tlast_hr.store(hr); return; }
    const D3D12_RESOURCE_DESC td = target->GetDesc();
    const bool msaa = c->game_samples.Count > 1;
    const bool same_size = td.Width == c->width && td.Height == c->height;
    if (flip_format(td.Format) != flip_format(c->game_format) || (msaa && !same_size)) {
        g_tcopy_failures.fetch_add(1); // nothing is transferred this frame; the game's buffer stays acquired
        tnote(msaa ? "the chain's buffer size differs from the game's multisampled back buffer; no resolve this frame"
                   : "the chain's buffer format no longer matches the game's back buffer; picture not copied");
        return;
    }
    if (g_copy_with_d3d11.load(std::memory_order_relaxed)) {
        const uint64_t t0 = now_us();
        hr = copy_on_d3d11(*c, target.Get(), typeless(c->game_format) ? td.Format : c->game_format, same_size);
        add_time(g_t_copy_us, g_t_copy_max, now_us() - t0);
        g_t_handoffs.fetch_add(1, std::memory_order_relaxed);
        if (SUCCEEDED(hr)) {
            g_tcopies.fetch_add(1, std::memory_order_relaxed);
            g_tcopies_d3d11.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_tcopy_failures.fetch_add(1);
            tnote("D3D11 could not transfer the translated picture", hr);
        }
        return;
    }
    // D3D11's frame goes to the queue first; D3D12 owns the virtual buffer until we give it back.
    ID3D11Resource* r[] = {c->virt11.Get()};
    const uint64_t t_release = now_us();
    c->on12->ReleaseWrappedResources(r, 1);
    c->ctx->Flush();
    const uint64_t t_copy = now_us();
    add_time(g_t_release_us, g_t_release_max, t_copy - t_release);
    g_t_handoffs.fetch_add(1, std::memory_order_relaxed);
    TChain::Slot* s = nullptr;
    if (tlist(*c, s)) {
        ID3D12GraphicsCommandList* l = s->list.Get();
        const D3D12_RESOURCE_STATES dst = msaa ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST;
        barrier(l, target.Get(), D3D12_RESOURCE_STATE_PRESENT, dst);
        if (msaa) {
            l->ResolveSubresource(target.Get(), 0, c->virt12.Get(), 0, typeless(c->game_format) ? td.Format : c->game_format);
        } else if (same_size) {
            l->CopyResource(target.Get(), c->virt12.Get());
        } else { // a size the game has not caught up with yet: copy what overlaps
            D3D12_TEXTURE_COPY_LOCATION to{}, from{};
            to.pResource = target.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = 0;
            from.pResource = c->virt12.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = 0;
            const D3D12_BOX box{0, 0, 0, static_cast<UINT>((std::min<UINT64>)(td.Width, c->width)), (std::min)(td.Height, c->height), 1};
            l->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        }
        barrier(l, target.Get(), dst, D3D12_RESOURCE_STATE_PRESENT);
        if (SUCCEEDED(hr = l->Close())) {
            // The hooked ExecuteCommandLists: the program observes this copy as the
            // game's final write of the presented picture. A transfer counts only with
            // its fence receipt; without one the slot is never reused before a full drain.
            ID3D12CommandList* lists[] = {l};
            c->queue->ExecuteCommandLists(1, lists);
            hr = c->queue->Signal(c->fence.Get(), c->fence_value + 1);
            if (SUCCEEDED(hr)) {
                s->value = ++c->fence_value;
                g_tcopies.fetch_add(1, std::memory_order_relaxed);
            } else {
                s->value = UINT64_MAX; // unknown completion: tlist waits (bounded) and the slot stays out of rotation until a drain
                g_tcopy_failures.fetch_add(1); g_tlast_hr.store(hr);
            }
        } else {
            g_tcopy_failures.fetch_add(1); g_tlast_hr.store(hr);
        }
    } else {
        g_tcopy_failures.fetch_add(1);
    }
    const uint64_t t_acquire = now_us();
    add_time(g_t_copy_us, g_t_copy_max, t_acquire - t_copy);
    c->on12->AcquireWrappedResources(r, 1); // the game draws its next frame into it
    add_time(g_t_acquire_us, g_t_acquire_max, now_us() - t_acquire);
}

void AfterPresent(IDXGISwapChain* chain, unsigned flags, unsigned long long present_us)
{
    if (!g_tchain_count.load(std::memory_order_relaxed) || (flags & DXGI_PRESENT_TEST) || !IsTranslated(chain)) return;
    add_time(g_t_present_us, g_t_present_max, present_us);
    g_t_presents.fetch_add(1, std::memory_order_relaxed);
}

bool BeforeResize(IDXGISwapChain* chain, unsigned* count, DXGI_FORMAT* format, unsigned* flags)
{
    if (!g_tchain_count.load(std::memory_order_relaxed)) return true;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    TChain* c = tfind(chain);
    if (!c) return true;
    // DXGI resizes D3D12 buffers only when no GPU work still uses them: our copies first. A
    // copy that has not completed within the budget refuses the resize here, before anything
    // of the game's is touched (a later resize retries). The game's buffer is handed back from
    // D3D11 but kept, with everything advertised about it, until DXGI has decided.
    if (!tdrain(*c)) { tnote("a copy to the chain had not completed; this ResizeBuffers is refused unchanged"); return false; }
    if (!g_copy_with_d3d11.load(std::memory_order_relaxed)) {
        if (c->virt11) { ID3D11Resource* r[] = {c->virt11.Get()}; c->on12->ReleaseWrappedResources(r, 1); }
        c->ctx->Flush();
    }
    c->resizing = true;
    c->pending_count = *count ? *count : c->game_count;
    c->pending_format = *format == DXGI_FORMAT_UNKNOWN ? c->game_format : *format;
    if (*format != DXGI_FORMAT_UNKNOWN) {
        const DXGI_FORMAT f = flip_format(*format);
        if (f == DXGI_FORMAT_UNKNOWN) c->pending_format = c->game_format; // unsupported: the chain and the game's buffer keep their format
        *format = f;
    }
    if (*count) *count = flip_count(*count);
    *flags = flip_flags(*flags);
    g_tresizes.fetch_add(1);
    return true;
}

void AfterResize(IDXGISwapChain* chain, long result)
{
    if (!g_tchain_count.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    TChain* c = tfind(chain);
    if (!c || !c->resizing) return;
    c->resizing = false;
    if (FAILED(result)) {
        // The game keeps the exact buffer it had: back under D3D11, same description.
        tnote("the game's ResizeBuffers was refused; its back buffer is unchanged", result);
        if (c->virt11 && !g_copy_with_d3d11.load(std::memory_order_relaxed)) {
            ID3D11Resource* r[] = {c->virt11.Get()};
            c->on12->AcquireWrappedResources(r, 1);
        }
        return;
    }
    c->game_count = c->pending_count;
    c->game_format = c->pending_format;
    read_size(*c);
    if (!build_virtual(*c)) tnote("the game's back buffer could not be rebuilt after ResizeBuffers");
}

void BeforeFinalRelease(IDXGISwapChain* chain)
{
    if (!g_tchain_count.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    if (TChain* c = tfind(chain)) tdrain(*c);
}

void ForgetSwapChain(IDXGISwapChain* chain)
{
    if (!g_tchain_count.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::recursive_mutex> lk(g_tm);
    TChain* c = tfind(chain);
    if (!c) return;
    g_tchain_ids[c - g_tchains].store(nullptr);
    tdrain(*c);
    if (c->event) CloseHandle(c->event);
    *c = TChain{};
    g_tchain_count.fetch_sub(1);
}

int FormatStatus(char* out, size_t capacity)
{
    std::lock_guard<std::mutex> lk(g_m);
    // Per-present CPU time since the last status line (avg/max us), then the window restarts.
    const auto take = [](std::atomic<uint64_t>& total, std::atomic<uint64_t>& peak, uint64_t count, unsigned long long& avg, unsigned long long& mx) {
        const uint64_t t = total.exchange(0), m = peak.exchange(0);
        avg = count ? t / count : 0;
        mx = m;
    };
    const uint64_t handoffs = g_t_handoffs.exchange(0), presents = g_t_presents.exchange(0);
    unsigned long long rel_avg, rel_max, copy_avg, copy_max, acq_avg, acq_max, pres_avg, pres_max;
    take(g_t_release_us, g_t_release_max, handoffs, rel_avg, rel_max);
    take(g_t_copy_us, g_t_copy_max, handoffs, copy_avg, copy_max);
    take(g_t_acquire_us, g_t_acquire_max, handoffs, acq_avg, acq_max);
    take(g_t_present_us, g_t_present_max, presents, pres_avg, pres_max);
    const int n = std::snprintf(out, capacity,
                                "dx11on12_present chains=%u translated=%llu untranslated=%llu copies=%llu copies_d3d11=%llu copy_failures=%llu resizes=%llu resize_wait_timeouts=%llu "
                                "window{presents=%llu release_flush_us=%llu/max%llu copy_us=%llu/max%llu acquire_us=%llu/max%llu dxgi_present_us=%llu/max%llu} last_hr=0x%08lX reason=\"%s\" | ",
                                g_tchain_count.load(), static_cast<unsigned long long>(g_translated.load()),
                                static_cast<unsigned long long>(g_untranslated.load()), static_cast<unsigned long long>(g_tcopies.load()),
                                static_cast<unsigned long long>(g_tcopies_d3d11.load()),
                                static_cast<unsigned long long>(g_tcopy_failures.load()), static_cast<unsigned long long>(g_tresizes.load()),
                                static_cast<unsigned long long>(g_tresize_waits.load()),
                                static_cast<unsigned long long>(presents), rel_avg, rel_max, copy_avg, copy_max, acq_avg, acq_max, pres_avg, pres_max,
                                static_cast<unsigned long>(g_tlast_hr.load()), g_treason);
    if (n < 0 || static_cast<size_t>(n) >= capacity) return n;
    out += n;
    capacity -= static_cast<size_t>(n);
    int extra = 0;
    if (g_route_native.load()) {
        // Native route: provider and handoff state, then the bridge's own counters (requests only).
        int m = std::snprintf(out, capacity, "native11_route=active installs=%u install_failures=%u menu{draws=%llu failures=%llu} | ",
                              g_native_installs.load(), g_native_install_failures.load(),
                              static_cast<unsigned long long>(g_native_menu_draws.load()),
                              static_cast<unsigned long long>(g_native_menu_failures.load()));
        if (m > 0 && static_cast<size_t>(m) < capacity) { out += m; capacity -= static_cast<size_t>(m); extra += m; }
        m = na::native11::FormatStatus(out, static_cast<unsigned long>(capacity));
        if (m > 0 && static_cast<size_t>(m) + 3 < capacity) { out += m; capacity -= static_cast<size_t>(m); extra += m; m = std::snprintf(out, capacity, " | "); out += m; capacity -= static_cast<size_t>(m); extra += m; }
        m = na::native_d3d11::FormatStatus(out, capacity);
        if (m > 0 && static_cast<size_t>(m) + 3 < capacity) { out += m; capacity -= static_cast<size_t>(m); extra += m; m = std::snprintf(out, capacity, " | "); out += m; capacity -= static_cast<size_t>(m); extra += m; }
        m = na::native11::FormatSamplerStatus(out, static_cast<unsigned long>(capacity));
        if (m > 0 && static_cast<size_t>(m) + 3 < capacity) { out += m; capacity -= static_cast<size_t>(m); extra += m; m = std::snprintf(out, capacity, " | "); out += m; capacity -= static_cast<size_t>(m); extra += m; }
    }
    return n + extra + std::snprintf(out, capacity,
                         "dx11on12 enabled=%u hooked=%u requests=%u wrapped=%u with_swapchain=%u native=%u probes=%u swapchain_check=%d menu_buffer{mode=%d unwraps=%llu failures=%llu returns=%llu composites=%llu composite_failures=%llu unwrap_us=%llu/max%llu return_us=%llu/max%llu} last_hr=0x%08lX reason=\"%s\"",
                         Enabled() ? 1u : 0u, g_hooked.load() ? 1u : 0u, g_requests.load(), g_wrapped.load(),
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
