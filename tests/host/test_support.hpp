// Shared helpers for the reconstructed host/GPU tests (C3, 2026-10). A test
// that needs a D3D12 device tries hardware first, then WARP; when neither
// exists it exits with kSkip (CTest SKIP_RETURN_CODE), never with a fake pass.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <dbghelp.h>
#include <tlhelp32.h>

namespace test {
using Microsoft::WRL::ComPtr;
constexpr int kSkip = 77;
inline int g_failures = 0;
inline DWORD g_main_thread = GetCurrentThreadId(); // initialised on the thread that runs main()
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++test::g_failures; } } while (0)
#define REQUIRE(cond) do { if (!(cond)) { std::printf("FATAL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)
// Exit watchdog (diagnostic, 2026-10-28): a test that has reported its result but has
// not exited 20 s later prints every thread's stack (module+offset, symbol when a PDB
// is present) and terminates with 3, instead of hanging until CTest's timeout. Covers
// teardown after finish(): locals' destructors, atexit and static destructors.
// "exit: atexit reached" marks where in teardown a hang sits.
inline void DumpThreadStacks() {
    HMODULE dh = LoadLibraryW(L"dbghelp.dll");
    if (!dh) { std::printf("exit watchdog: dbghelp unavailable\n"); return; }
    auto sym_init = reinterpret_cast<decltype(&SymInitialize)>(GetProcAddress(dh, "SymInitialize"));
    auto walk = reinterpret_cast<decltype(&StackWalk64)>(GetProcAddress(dh, "StackWalk64"));
    auto fta = reinterpret_cast<decltype(&SymFunctionTableAccess64)>(GetProcAddress(dh, "SymFunctionTableAccess64"));
    auto modbase = reinterpret_cast<decltype(&SymGetModuleBase64)>(GetProcAddress(dh, "SymGetModuleBase64"));
    auto from_addr = reinterpret_cast<decltype(&SymFromAddr)>(GetProcAddress(dh, "SymFromAddr"));
    if (!sym_init || !walk || !fta || !modbase || !from_addr) return;
    HANDLE proc = GetCurrentProcess();
    sym_init(proc, nullptr, TRUE);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{}; te.dwSize = sizeof te;
    // Two passes: the main (exiting) thread first, then the rest; idle thread-pool
    // workers (NtWaitForWorkViaWorkerFactory) are skipped so the dump stays short.
    for (int pass = 0; pass < 2; ++pass)
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if ((te.th32ThreadID == g_main_thread) != (pass == 0)) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx)) {
            STACKFRAME64 f{};
            std::string lines;
            bool idle_pool = false;
            f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
            f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
            f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < 48 && walk(IMAGE_FILE_MACHINE_AMD64, proc, th, &f, &ctx, nullptr, fta, modbase, nullptr) && f.AddrPC.Offset; ++i) {
                const DWORD64 pc = f.AddrPC.Offset, base = modbase(proc, pc);
                char mod[MAX_PATH] = "?";
                if (base) { GetModuleFileNameA(reinterpret_cast<HMODULE>(base), mod, MAX_PATH); }
                const char* m = std::strrchr(mod, '\\'); m = m ? m + 1 : mod;
                alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
                auto* si = reinterpret_cast<SYMBOL_INFO*>(buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
                DWORD64 disp = 0;
                const bool named = from_addr(proc, pc, &disp, si) != FALSE;
                char line[512];
                std::snprintf(line, sizeof line, "  %s+0x%llx %s\n", m, static_cast<unsigned long long>(pc - base), named ? si->Name : "");
                lines += line;
                if (i == 0 && named && std::strcmp(si->Name, "NtWaitForWorkViaWorkerFactory") == 0) idle_pool = true;
            }
            if (!idle_pool) std::printf("exit watchdog: thread %lu%s\n%s", te.th32ThreadID, te.th32ThreadID == g_main_thread ? " (main)" : "", lines.c_str());
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    std::fflush(stdout);
}
inline DWORD WINAPI ExitWatchdog(void*) {
    Sleep(20000);
    std::printf("exit watchdog: still running 20 s after the result: teardown hang\n");
    DumpThreadStacks();
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}
inline int finish(const char* name) {
    std::printf("%s: %s (%d failure%s)\n", name, test::g_failures ? "FAILED" : "ok", test::g_failures, test::g_failures == 1 ? "" : "s");
    std::fflush(stdout);
    std::atexit([] { std::printf("exit: atexit reached\n"); std::fflush(stdout); });
    if (HANDLE t = CreateThread(nullptr, 0, ExitWatchdog, nullptr, 0, nullptr)) CloseHandle(t);
    return test::g_failures ? 1 : 0;
}

// R-204 (Gate 1): every CPU wait on the GPU is checked. A failed Signal /
// SetEventOnCompletion, a timeout, a wait error or device removal ends the test
// immediately (before any allocator reset or resource reuse) with the reason,
// instead of returning as if the GPU were idle.
enum class WaitResult { Ok, Timeout, Failed, DeviceRemoved };
inline const char* WaitResultName(WaitResult r) {
    switch (r) { case WaitResult::Ok: return "ok"; case WaitResult::Timeout: return "timeout";
                 case WaitResult::Failed: return "failed"; default: return "device removed"; }
}
inline WaitResult WaitFence(ID3D12Fence* fence, uint64_t value, HANDLE event, DWORD timeout_ms) {
    uint64_t done = fence->GetCompletedValue();
    if (done == UINT64_MAX) return WaitResult::DeviceRemoved;
    if (done >= value) return WaitResult::Ok;
    if (FAILED(fence->SetEventOnCompletion(value, event))) return WaitResult::Failed;
    const DWORD w = WaitForSingleObject(event, timeout_ms);
    if (w == WAIT_TIMEOUT) return WaitResult::Timeout;
    if (w != WAIT_OBJECT_0) return WaitResult::Failed;
    done = fence->GetCompletedValue();
    if (done == UINT64_MAX) return WaitResult::DeviceRemoved;
    return done >= value ? WaitResult::Ok : WaitResult::Failed;
}
[[noreturn]] inline void FatalWait(const char* what, WaitResult r, ID3D12Device* device) {
    const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
    std::printf("FATAL GPU wait (%s): %s, device removed reason 0x%08lX - stopping before any resource reuse\n",
                what, WaitResultName(r), static_cast<unsigned long>(reason));
    std::fflush(stdout);
    std::exit(1);
}
// Signals `fence` on `queue` with ++value and waits (checked). Tests only.
inline void SignalAndWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t& value, HANDLE event,
                          ID3D12Device* device, const char* what, DWORD timeout_ms = 10000) {
    ++value;
    if (FAILED(queue->Signal(fence, value))) FatalWait(what, WaitResult::Failed, device);
    const WaitResult r = WaitFence(fence, value, event, timeout_ms);
    if (r != WaitResult::Ok) FatalWait(what, r, device);
}

struct Device {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t fence_value = 0;
    bool warp = false;
    ~Device() { if (event) CloseHandle(event); }
    // Blocks until everything submitted so far has executed; ends the test on any failure.
    void WaitIdle() { SignalAndWait(queue.Get(), fence.Get(), fence_value, event, device.Get(), "WaitIdle"); }
};
// Returns false when no D3D12 device (hardware or WARP) can be created.
inline bool CreateDevice(Device& d, bool allow_warp = true) {
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory)))) return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (d.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{}; a->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)))) {
            std::printf("device: %ls\n", desc.Description); break;
        }
    }
    if (!d.device && allow_warp) {
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
            SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)))) {
            d.warp = true; std::printf("device: WARP (software)\n");
        }
    }
    if (!d.device) return false;
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d.device->CreateCommandQueue(&q, IID_PPV_ARGS(&d.queue)))) return false;
    if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)))) return false;
    d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return d.event != nullptr;
}
inline int SkipNoDevice(const char* name) {
    std::printf("%s: SKIP (no D3D12 device available on this machine)\n", name);
    return kSkip;
}
struct List {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    bool Create(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
        if (FAILED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, type, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
        return true;
    }
    void Reset() { alloc->Reset(); list->Reset(alloc.Get(), nullptr); }
};
inline ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* dev, uint64_t bytes, D3D12_HEAP_TYPE heap,
                                           D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}
inline ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                                            D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = fmt; td.SampleDesc.Count = 1; td.Flags = flags;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}
inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t{}; t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r; t.Transition.StateBefore = a; t.Transition.StateAfter = b;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; return t;
}
// Creates a plain top-level window for swap-chain tests (message pump not required for these tests).
inline HWND CreateTestWindow(const wchar_t* title, int w = 320, int h = 180) {
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NeuralAliasHostTest";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, wc.hInstance, nullptr);
}
} // namespace test
