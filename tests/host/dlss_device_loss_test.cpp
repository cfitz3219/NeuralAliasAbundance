// Gate 2 hardware session (label gpu;nvidia — Cort's RTX sessions only):
// ARGON-G2-004 device-loss recovery with the REAL NGX runtime, using
// ID3D12Device5::RemoveDevice (a real, driver-visible device removal without
// a TDR). D3D12 devices are singletons per adapter: while anything still holds
// the removed device, D3D12CreateDevice hands the SAME removed device back, so
// a game could never recover. This test checks that:
//   1. DLSS runs on device 1 (EnsureOn + one evaluation);
//   2. after RemoveDevice and the game releasing its references, a NEW device
//      can be created on that adapter (the backend does not pin the removed one);
//   3. EnsureOn(new device) restarts DLSS there and one evaluation runs.
// SKIP (77) only without an NVIDIA GPU.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

#include <string>

namespace {
struct Gpu {
    test::ComPtr<ID3D12Device> device;
    test::ComPtr<ID3D12CommandQueue> queue;
    test::ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    HANDLE event = nullptr;
    ~Gpu() { if (event) CloseHandle(event); }
};

bool Open(IDXGIAdapter1* a, Gpu& g)
{
    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)))) return false;
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.device->CreateCommandQueue(&q, IID_PPV_ARGS(&g.queue)))) return false;
    if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.event != nullptr;
}

// One real DLSS evaluation on `g`, submitted and waited for.
bool OneFrame(Gpu& g, bool wait = true)
{
    ID3D12Device* dev = g.device.Get();
    const auto rs = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto color = test::CreateTexture(dev, 1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT, rs, D3D12_RESOURCE_FLAG_NONE);
    auto depth = test::CreateTexture(dev, 1280, 720, DXGI_FORMAT_R32_FLOAT, rs, D3D12_RESOURCE_FLAG_NONE);
    auto motion = test::CreateTexture(dev, 1280, 720, DXGI_FORMAT_R16G16_FLOAT, rs, D3D12_RESOURCE_FLAG_NONE);
    auto output = test::CreateTexture(dev, 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!color || !depth || !motion || !output) return false;
    test::List l;
    if (!l.Create(dev)) return false;
    na::dlss::EvalDesc e;
    e.color = color.Get(); e.depth = depth.Get(); e.motion = motion.Get(); e.output = output.Get();
    e.render_w = 1280; e.render_h = 720; e.out_w = 1920; e.out_h = 1080;
    e.reset = true;
    e.quality = na::dlss::Quality::MaxQuality;
    const bool ok = na::dlss::Evaluate(l.list.Get(), e);
    if (!ok) std::printf("evaluate refused: %s\n", na::dlss::GetStats().reason);
    l.list->Close();
    ID3D12CommandList* ls[] = {l.list.Get()};
    na::dlss::BeforeExecute(g.queue.Get(), 1, ls);
    g.queue->ExecuteCommandLists(1, ls);
    na::dlss::OnExecuted(g.queue.Get(), 1, ls);
    if (wait) test::SignalAndWait(g.queue.Get(), g.fence.Get(), g.value, g.event, dev, "one frame");
    return ok;
}
} // namespace

int main(int argc, char** argv)
{
    test::ComPtr<IDXGIFactory6> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    test::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;; ++i) {
        test::ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        a->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            std::printf("adapter: %ls\n", desc.Description);
            adapter = a;
            break;
        }
    }
    if (!adapter) {
        std::printf("dlss_device_loss_test: SKIP (no NVIDIA adapter)\n");
        return test::kSkip;
    }
    std::wstring data_dir = L".";
    if (argc > 1) { const std::string a = argv[1]; data_dir.assign(a.begin(), a.end()); CreateDirectoryW(data_dir.c_str(), nullptr); }

    // 1. DLSS on device 1.
    ID3D12Device* old_ptr = nullptr;
    {
        Gpu g1;
        REQUIRE(Open(adapter.Get(), g1));
        old_ptr = g1.device.Get();
        REQUIRE(na::dlss::EnsureOn(g1.device.Get(), data_dir.c_str()));
        CHECK(OneFrame(g1));
        std::printf("step 1: DLSS ran on device 1\n");

        // 2. Real device removal.
        test::ComPtr<ID3D12Device5> d5;
        REQUIRE(SUCCEEDED(g1.device.As(&d5)));
        d5->RemoveDevice();
        const HRESULT why = g1.device->GetDeviceRemovedReason();
        std::printf("step 2: RemoveDevice -> removed reason 0x%08lX; backend sees removal: %d\n",
                    static_cast<unsigned long>(why), na::dlss::BoundDeviceRemoved() ? 1 : 0);
        CHECK(why != S_OK);
        CHECK(na::dlss::BoundDeviceRemoved());
        // A frame on the removed device must be refused or harmless, never hang.
        const bool on_removed = OneFrame(g1, /*wait=*/false);
        std::printf("step 2: evaluation on the removed device %s\n", on_removed ? "accepted" : "refused");
    } // the "game" releases everything it held on device 1

    // 3. The game recreates its device on the same adapter.
    // (Our execute hook ran on the removed device in step 2; that is where the
    // backend notices the removal and lets go of it.)
    std::printf("step 3: backend attached after removal: %d\n", na::dlss::GetCapabilities().attached ? 1 : 0);
    Gpu g2;
    const bool opened = Open(adapter.Get(), g2);
    const HRESULT fresh = g2.device ? g2.device->GetDeviceRemovedReason() : E_FAIL;
    const bool same = !opened || fresh != S_OK;
    std::printf("step 3: new device %p (old %p), removed reason 0x%08lX -> %s\n", static_cast<void*>(g2.device.Get()),
                static_cast<void*>(old_ptr), static_cast<unsigned long>(fresh),
                fresh == S_OK ? "LIVE new device" : "STILL THE REMOVED DEVICE (something pins it)");
    CHECK(opened && fresh == S_OK);
    if (same) {
        // Diagnose: does releasing the backend's reference free it?
        na::dlss::Shutdown();
        g2.fence.Reset();
        g2.queue.Reset();
        g2.device.Reset();
        Gpu g3;
        const bool live = Open(adapter.Get(), g3) && g3.device->GetDeviceRemovedReason() == S_OK;
        std::printf("diagnosis: after dlss::Shutdown the adapter gives a %s device -> the backend %s the removed device\n",
                    live ? "LIVE" : "removed", live ? "WAS pinning" : "is not what pins");
        return test::finish("dlss_device_loss_test");
    }

    // 4. DLSS restarts on the new device.
    const bool started = na::dlss::EnsureOn(g2.device.Get(), data_dir.c_str());
    const auto caps = na::dlss::GetCapabilities();
    std::printf("step 4: EnsureOn(new device) -> %d, available=%d bound-to-new=%d reason='%s'\n", started ? 1 : 0,
                caps.available ? 1 : 0, na::dlss::IsBoundTo(g2.device.Get()) ? 1 : 0, caps.reason);
    CHECK(started);
    CHECK(na::dlss::IsBoundTo(g2.device.Get()));
    if (started) CHECK(OneFrame(g2));
    std::printf("step 4: DLSS %s on the recreated device\n", started ? "ran" : "did NOT start");
    na::dlss::Shutdown();
    return test::finish("dlss_device_loss_test");
}
