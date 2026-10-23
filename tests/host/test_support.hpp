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

namespace test {
using Microsoft::WRL::ComPtr;
constexpr int kSkip = 77;
inline int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++test::g_failures; } } while (0)
#define REQUIRE(cond) do { if (!(cond)) { std::printf("FATAL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)
inline int finish(const char* name) {
    std::printf("%s: %s (%d failure%s)\n", name, test::g_failures ? "FAILED" : "ok", test::g_failures, test::g_failures == 1 ? "" : "s");
    return test::g_failures ? 1 : 0;
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
    // Blocks until everything submitted so far has executed. Tests only.
    void WaitIdle() {
        ++fence_value;
        queue->Signal(fence.Get(), fence_value);
        if (fence->GetCompletedValue() < fence_value) {
            fence->SetEventOnCompletion(fence_value, event);
            WaitForSingleObject(event, 10000);
        }
    }
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
