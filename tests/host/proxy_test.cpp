// Reconstructed 2026-10 (C3). The DXGI forwarding test from milestone 1:
// loads NeuralAliasDXGI.dll EXPLICITLY (never installed as dxgi.dll here),
// resolves the forwarded factory exports, creates a genuine DX12 device +
// swap chain through the proxy, presents, resizes, checks the status export
// (factory call count, system dxgi loaded, host NOT started because no
// NeuralAliasHost.ini sits beside the DLL), retires the COM objects and
// unloads the DLL. Usage: proxy_test <path to NeuralAliasDXGI.dll>
#include "test_support.hpp"
#include "runtime/proxy_api.hpp"

#include <string>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { std::printf("usage: proxy_test <NeuralAliasDXGI.dll>\n"); return 2; }
    // Guard: the proxy must not be activated by a stray ini next to it.
    {
        std::wstring ini = argv[1];
        size_t slash = ini.find_last_of(L"\\/");
        ini = (slash == std::wstring::npos ? L"" : ini.substr(0, slash + 1)) + L"NeuralAliasHost.ini";
        if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) { std::printf("refusing: %ls exists beside the DLL\n", ini.c_str()); return 2; }
    }
    HMODULE proxy = LoadLibraryW(argv[1]);
    REQUIRE(proxy != nullptr);

    using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    using Factory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto create0 = reinterpret_cast<FactoryFn>(reinterpret_cast<void*>(GetProcAddress(proxy, "CreateDXGIFactory")));
    auto create1 = reinterpret_cast<FactoryFn>(reinterpret_cast<void*>(GetProcAddress(proxy, "CreateDXGIFactory1")));
    auto create2 = reinterpret_cast<Factory2Fn>(reinterpret_cast<void*>(GetProcAddress(proxy, "CreateDXGIFactory2")));
    auto query = reinterpret_cast<abundance::QueryProxyStatus>(reinterpret_cast<void*>(GetProcAddress(proxy, "AbundanceQueryProxyStatus")));
    REQUIRE(create0 && create1 && create2 && query);
    CHECK(GetProcAddress(proxy, "DXGIGetDebugInterface1") != nullptr);
    CHECK(GetProcAddress(proxy, "DXGIDeclareAdapterRemovalSupport") != nullptr);

    abundance::ProxyStatus st{}; st.size = sizeof(st);
    REQUIRE(query(&st) == 1);
    CHECK(st.abi_version == 1);
    CHECK(st.factory_calls == 0);
    CHECK(st.system_loaded == 0); // lazy: nothing loaded until the first exported call
    CHECK((st.reserved & 0x3) == 0); // host not enabled/started without an ini
    abundance::ProxyStatus bad{}; bad.size = 4;
    CHECK(query(&bad) == 0); // size check honoured
    CHECK(query(nullptr) == 0);

    test::ComPtr<IDXGIFactory> f0; test::ComPtr<IDXGIFactory1> f1; test::ComPtr<IDXGIFactory2> f2;
    CHECK(SUCCEEDED(create0(IID_PPV_ARGS(&f0))));
    CHECK(SUCCEEDED(create1(IID_PPV_ARGS(&f1))));
    REQUIRE(SUCCEEDED(create2(0, IID_PPV_ARGS(&f2))));
    REQUIRE(query(&st) == 1);
    CHECK(st.factory_calls == 3);
    CHECK(st.system_loaded == 1);
    CHECK(GetModuleHandleW(L"dxgi.dll") != nullptr);

    // A real device through the proxied factory.
    test::ComPtr<ID3D12Device> device;
    for (UINT i = 0; !device; ++i) {
        test::ComPtr<IDXGIAdapter1> a;
        if (f1->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    }
    if (!device) {
        test::ComPtr<IDXGIFactory4> f4; f2.As(&f4);
        test::ComPtr<IDXGIAdapter> warp;
        if (f4 && SUCCEEDED(f4->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    }
    if (!device) { FreeLibrary(proxy); return test::SkipNoDevice("proxy_test"); }

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    test::ComPtr<ID3D12CommandQueue> queue; REQUIRE(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));
    HWND hwnd = test::CreateTestWindow(L"NeuralAlias proxy test");
    REQUIRE(hwnd != nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = 320; sd.Height = 180; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    test::ComPtr<IDXGISwapChain1> chain;
    REQUIRE(SUCCEEDED(f2->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &chain)));
    {
        test::ComPtr<ID3D12Resource> bb; CHECK(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&bb))));
        D3D12_RESOURCE_DESC d = bb->GetDesc(); CHECK(d.Width == 320 && d.Height == 180);
    }
    HRESULT pr = chain->Present(0, 0);
    CHECK(SUCCEEDED(pr) || pr == DXGI_STATUS_OCCLUDED);
    // Resize: no buffer references may be held (the GetBuffer ComPtr above is released).
    CHECK(SUCCEEDED(chain->ResizeBuffers(2, 256, 144, DXGI_FORMAT_UNKNOWN, 0)));
    {
        test::ComPtr<ID3D12Resource> bb; CHECK(SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&bb))));
        D3D12_RESOURCE_DESC d = bb->GetDesc(); CHECK(d.Width == 256 && d.Height == 144);
    }
    pr = chain->Present(0, 0);
    CHECK(SUCCEEDED(pr) || pr == DXGI_STATUS_OCCLUDED);

    // Retire everything (GPU idle first), then unload the proxy.
    {
        test::ComPtr<ID3D12Fence> fence; device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        queue->Signal(fence.Get(), 1);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence->GetCompletedValue() < 1) { fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 10000); }
        CloseHandle(ev);
    }
    chain.Reset(); queue.Reset(); device.Reset(); f0.Reset(); f1.Reset(); f2.Reset();
    DestroyWindow(hwnd);
    CHECK(FreeLibrary(proxy) != 0);
    CHECK(GetModuleHandleW(L"NeuralAliasDXGI.dll") == nullptr);
    return test::finish("proxy_test");
}
