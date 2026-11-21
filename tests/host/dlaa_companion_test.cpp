// The whole handoff with real programs and a real graphics device, and no game: this test plays
// the in-game half, starts NeuralAliasDlaa64.exe and checks that a picture written on this side
// comes back through the companion, in the right order, by GPU fences alone.
//   (no argument)  hardware device, companion in copy mode (no DLSS needed)
//   --warp         the same on the software device
//   --dlss         hardware device, real DLSS in the companion; prints the handoff cost
// CPU waits and readback here belong only to the test's pixel checks.
#include "dlaa_remote/client.hpp"
#include "core/referee_companion.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace na;
#define REQUIRE(x) do { if (!(x)) { std::printf("FAIL line %d: %s  [%s]\n", __LINE__, #x, dlaa_remote::GetStats().reason); return 1; } } while (0)

namespace {
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* image, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = image;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}
bool Shared(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = format; d.SampleDesc.Count = 1;
    d.Flags = flags;
    return SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out)));
}
std::wstring Beside(const wchar_t* file)
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/") + 1) + file;
}
}

int wmain(int argc, wchar_t** argv)
{
    const bool warp = argc > 1 && !wcscmp(argv[1], L"--warp");
    const bool real = argc > 1 && !wcscmp(argv[1], L"--dlss");
    // Every second reply names the frame before it: the in-game half must refuse exactly those,
    // and the Referee's binding rule must call them stale (gap audit 2026-11-21 item 12).
    const bool stale = argc > 2 && !wcscmp(argv[2], L"--stale");
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter1> adapter;
    if (warp) REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        std::printf("SKIP no DirectX 12 device\n");
        return 77;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    REQUIRE(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));
    REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))));
    REQUIRE(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fence_value = 0;
    const auto finish = [&]() {
        queue->Signal(fence.Get(), ++fence_value);
        fence->SetEventOnCompletion(fence_value, event);
        return WaitForSingleObject(event, 30000) == WAIT_OBJECT_0;
    };

    const UINT w = real ? 1280 : 64, h = real ? 720 : 36;
    const auto rt = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto ua = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> color, depth, motion, output, readback;
    REQUIRE(Shared(dev.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, rt, &color));
    REQUIRE(Shared(dev.Get(), w, h, DXGI_FORMAT_R32_FLOAT, rt, &depth));
    REQUIRE(Shared(dev.Get(), w, h, DXGI_FORMAT_R32G32_FLOAT, rt | ua, &motion));
    REQUIRE(Shared(dev.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, rt | ua, &output));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 3;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs))));
    auto rtv_color = rtvs->GetCPUDescriptorHandleForHeapStart();
    auto rtv_depth = rtv_color;
    rtv_depth.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dev->CreateRenderTargetView(color.Get(), nullptr, rtv_color);
    auto rtv_motion = rtv_depth;
    rtv_motion.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dev->CreateRenderTargetView(depth.Get(), nullptr, rtv_depth);
    dev->CreateRenderTargetView(motion.Get(), nullptr, rtv_motion);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};
    UINT64 bytes = 0;
    const auto od = output->GetDesc();
    dev->GetCopyableFootprints(&od, 0, 1, 0, &foot, nullptr, nullptr, &bytes);
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = bytes; b.Height = 1; b.DepthOrArraySize = 1; b.MipLevels = 1;
        b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))));
    }
    REQUIRE(SUCCEEDED(list->Close()));

    wchar_t data[MAX_PATH]{};
    GetTempPathW(MAX_PATH, data);
    wcscat_s(data, L"NeuralAlias-ngx");
    CreateDirectoryW(data, nullptr);
    const std::wstring exe = Beside(L"NeuralAliasDlaa64.exe");
    if (!dlaa_remote::Start(dev.Get(), queue.Get(), data, exe.c_str(), !real, stale ? 2u : 0u)) {
        std::printf("%s companion did not start: %s\n", real ? "SKIP" : "FAIL", dlaa_remote::GetStats().reason);
        return real ? 77 : 1;
    }
    REQUIRE(dlaa_remote::Bind(1, w, h, color.Get(), depth.Get(), motion.Get(), output.Get()));

    const int frames = real ? 120 : stale ? 6 : 3;
    for (int i = 0; i < frames; ++i) {
        // This side's "scene": a flat colour that changes every frame, so a stale or early read shows.
        // With real DLSS the colour stays put: its history would otherwise lag a changing one.
        const int want = 40 + (real ? 0 : i * 60 % 200);
        const float c[4] = {want / 255.f, 0.5f, 0.25f, 1.f};
        const float far_depth[4] = {1.f, 0, 0, 0};
        const float still[4] = {0, 0, 0, 0};
        REQUIRE(SUCCEEDED(alloc->Reset()) && SUCCEEDED(list->Reset(alloc.Get(), nullptr)));
        Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(list.Get(), motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ClearRenderTargetView(rtv_color, c, 0, nullptr);
        list->ClearRenderTargetView(rtv_depth, far_depth, 0, nullptr);
        list->ClearRenderTargetView(rtv_motion, still, 0, nullptr);
        Transition(list.Get(), motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        Transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        REQUIRE(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        dlaa_remote::Frame f{};
        f.slot = 1;
        f.mv_scale_x = f.mv_scale_y = 1.f;
        f.pre_exposure = f.exposure_scale = 1.f;
        f.quality = 5; // DLAA
        f.view_id = 77;
        f.flags = i == 0 ? dlaa_remote::kReset : 0u;
        dlaa_remote::Handoff sent;
        const bool ran = dlaa_remote::Evaluate(f, &sent);
        const bool expect_stale = stale && (i + 1) % 2 == 0;
        const na::referee::CompanionHandoff handoff{static_cast<uint64_t>(i + 1), sent.asked, sent.answered,
                                                    sent.asked_slot, sent.answered_slot, sent.replied, sent.ran};
        const auto binding = na::referee::Judge(handoff);
        std::printf("frame %d: asked=%llu answered=%llu slot=%u/%u ran=%d binding=%s\n", i,
                    static_cast<unsigned long long>(sent.asked), static_cast<unsigned long long>(sent.answered),
                    sent.asked_slot, sent.answered_slot, int(ran), na::referee::CompanionBindingName(binding));
        REQUIRE(sent.replied && sent.asked == static_cast<uint64_t>(i + 1) && sent.asked_slot == 1 && sent.answered_slot == 1);
        REQUIRE(ran == !expect_stale && sent.ran == ran);
        REQUIRE(sent.answered == (expect_stale ? sent.asked - 1 : sent.asked));
        REQUIRE(binding == (expect_stale ? na::referee::CompanionBinding::Stale : na::referee::CompanionBinding::Bound));
        // Recorded after Evaluate, so the queue runs it only once the companion's output exists.
        REQUIRE(SUCCEEDED(list->Reset(alloc.Get(), nullptr)));
        Transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = foot;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        REQUIRE(SUCCEEDED(list->Close()));
        queue->ExecuteCommandLists(1, lists);
        REQUIRE(finish());
        uint8_t* px = nullptr;
        REQUIRE(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&px))));
        const uint8_t* mid = px + foot.Footprint.RowPitch * (h / 2) + 4 * (w / 2);
        const int tolerance = real ? 6 : 1; // DLSS of a flat colour stays that colour, to rounding
        const bool right = abs(mid[0] - want) <= tolerance && abs(mid[1] - 128) <= tolerance && abs(mid[2] - 64) <= tolerance;
        if (!right) std::printf("frame %d: expected %d,128,64 got %d,%d,%d\n", i, want, mid[0], mid[1], mid[2]);
        readback->Unmap(0, nullptr);
        // DLSS needs a few frames of history before a changed flat colour settles.
        if (!real || i == frames - 1) REQUIRE(right || (real && i < 8));
    }
    char line[512];
    dlaa_remote::FormatStatus(line, sizeof line);
    std::printf("%s\n", line);
    const auto stats = dlaa_remote::GetStats();
    REQUIRE(stats.frames == static_cast<uint64_t>(frames) && stats.refused == 0 && !stats.failed);
    REQUIRE(stats.stale == (stale ? static_cast<uint64_t>(frames / 2) : 0u));
    dlaa_remote::Stop();
    std::printf("dlaa_companion_test (%s): all passed\n", real ? "DLSS" : warp ? "software device, copy" : "copy");
    return 0;
}
