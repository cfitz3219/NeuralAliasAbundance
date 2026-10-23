#include "host_dx12/capture/frame_capture.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace na::capture {
namespace {

struct Plane {
    const char* name;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    D3D12_RESOURCE_DESC desc{};
    UINT64 bytes = 0;
};

struct Frame {
    uint32_t index = 0;
    EvalInputs in{};
    std::vector<Plane> planes; // color, depth, motion, dlss_output, backbuffer
    UINT64 fence_value = 0;
};

struct State {
    std::mutex m;
    std::atomic<bool> pending{false};
    std::atomic<ULONGLONG> start_at{0}; // A44: tick count at which the capture starts
    uint32_t requested = 0, evaluated = 0, written = 0, errors = 0;
    wchar_t dir[MAX_PATH] = {};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    std::unique_ptr<Frame> current; // evaluate done, awaiting Present
    std::vector<std::unique_ptr<Frame>> queued; // signalled, awaiting write
    std::thread writer;
    std::atomic<bool> stop{false};
    HANDLE wake = nullptr;
};
State g;

bool ensure_device(ID3D12Device* dev)
{
    if (g.device) return true;
    g.device = dev;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc))))
        return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&g.list))))
        return false;
    g.list->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    return true;
}

// Records: barrier `from`â†’COPY_SOURCE, copy subresource 0 into a fresh
// readback buffer, barrier back. Returns false if the plane can't be copied.
bool record_plane(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                  D3D12_RESOURCE_STATES from, const char* name, Plane& out)
{
    if (!res) return false;
    out.name = name;
    out.desc = res->GetDesc();
    if (out.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&out.desc, 0, 1, 0, &out.footprint, nullptr, nullptr, &total);
    out.bytes = total;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&out.readback))))
        return false;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (from != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dst.pResource = out.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = out.footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (from != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = from;
        list->ResourceBarrier(1, &b);
    }
    return true;
}

void write_frame(const Frame& f)
{
    wchar_t path[MAX_PATH];
    for (const Plane& p : f.planes) {
        if (!p.readback) continue;
        swprintf_s(path, L"%s\\frame%u-%S.raw", g.dir, f.index, p.name);
        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(p.bytes)};
        if (FAILED(p.readback->Map(0, &range, &mapped))) continue;
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path, L"wb") == 0 && fp) {
            fwrite(mapped, 1, static_cast<size_t>(p.bytes), fp);
            fclose(fp);
        }
        D3D12_RANGE none{0, 0};
        p.readback->Unmap(0, &none);
    }
    swprintf_s(path, L"%s\\frame%u-meta.json", g.dir, f.index);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"wb") == 0 && fp) {
        fprintf(fp, "{\n  \"frame\": %u,\n  \"render\": [%u, %u],\n  \"output\": [%u, %u],\n"
                    "  \"jitter\": [%.6f, %.6f],\n  \"mv_scale\": [%.4f, %.4f],\n  \"reset\": %u,\n"
                    "  \"planes\": [\n",
                f.index, f.in.render_w, f.in.render_h, f.in.out_w, f.in.out_h, f.in.jitter_x,
                f.in.jitter_y, f.in.mv_scale_x, f.in.mv_scale_y, f.in.reset);
        bool first = true;
        for (const Plane& p : f.planes) {
            if (!p.readback) continue;
            fprintf(fp, "%s    {\"name\": \"%s\", \"width\": %llu, \"height\": %u, \"dxgi_format\": %u, "
                        "\"row_pitch\": %u, \"bytes\": %llu}",
                    first ? "" : ",\n", p.name, static_cast<unsigned long long>(p.desc.Width),
                    p.desc.Height, static_cast<unsigned>(p.desc.Format), p.footprint.Footprint.RowPitch,
                    static_cast<unsigned long long>(p.bytes));
            first = false;
        }
        fprintf(fp, "\n  ]\n}\n");
        fclose(fp);
    }
}

void writer_main()
{
    while (!g.stop.load()) {
        WaitForSingleObject(g.wake, 200);
        std::vector<std::unique_ptr<Frame>> batch;
        {
            std::lock_guard<std::mutex> lock(g.m);
            batch.swap(g.queued);
        }
        for (auto& f : batch) {
            if (g.fence->GetCompletedValue() < f->fence_value) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                g.fence->SetEventOnCompletion(f->fence_value, ev);
                WaitForSingleObject(ev, 5000);
                CloseHandle(ev);
            }
            write_frame(*f);
            std::lock_guard<std::mutex> lock(g.m);
            ++g.written;
        }
    }
}

} // namespace

void Request(uint32_t frames, const wchar_t* out_dir, uint32_t delay_ms)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!out_dir || frames == 0) return;
    g.start_at = GetTickCount64() + delay_ms;
    wcsncpy_s(g.dir, out_dir, _TRUNCATE);
    CreateDirectoryW(g.dir, nullptr);
    g.requested = frames;
    g.evaluated = 0;
    g.written = 0;
    g.errors = 0;
    g.current.reset();
    if (!g.wake) g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.writer.joinable()) {
        g.stop.store(false);
        g.writer = std::thread(&writer_main);
    }
    g.pending.store(true, std::memory_order_release);
}

bool Pending()
{
    if (!g.pending.load(std::memory_order_acquire)) return false;
    return GetTickCount64() >= g.start_at.load(std::memory_order_relaxed);
}

uint32_t DelayRemainingMs()
{
    if (!g.pending.load(std::memory_order_acquire)) return 0;
    ULONGLONG now = GetTickCount64(), at = g.start_at.load(std::memory_order_relaxed);
    return now >= at ? 0u : static_cast<uint32_t>(at - now);
}

bool OnEvaluate(ID3D12GraphicsCommandList* list, const EvalInputs& in)
{
    if (!Pending() || !list) return false;
    std::lock_guard<std::mutex> lock(g.m);
    if (g.evaluated >= g.requested) return false;
    ComPtr<ID3D12Device> dev;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !ensure_device(dev.Get())) {
        ++g.errors;
        return false;
    }
    auto f = std::make_unique<Frame>();
    f->index = g.evaluated;
    f->in = in;
    f->planes.resize(5);
    bool ok = true;
    ok &= record_plane(list, in.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "color", f->planes[0]);
    ok &= record_plane(list, in.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "depth", f->planes[1]);
    ok &= record_plane(list, in.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "motion", f->planes[2]);
    ok &= record_plane(list, in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "dlssout", f->planes[3]);
    if (!ok) ++g.errors;
    g.current = std::move(f);
    ++g.evaluated;
    return ok;
}

void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* backbuffer)
{
    if (!Pending() || !queue) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.current || !g.device) return;
    std::unique_ptr<Frame> f = std::move(g.current);
    g.alloc->Reset();
    g.list->Reset(g.alloc.Get(), nullptr);
    if (backbuffer)
        record_plane(g.list.Get(), backbuffer, D3D12_RESOURCE_STATE_PRESENT, "backbuffer", f->planes[4]);
    g.list->Close();
    ID3D12CommandList* lists[] = {g.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    f->fence_value = g.fence_next++;
    queue->Signal(g.fence.Get(), f->fence_value);
    g.queued.push_back(std::move(f));
    SetEvent(g.wake);
    if (g.evaluated >= g.requested) g.pending.store(false, std::memory_order_release);
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    return Stats{g.requested, g.evaluated, g.written, g.errors};
}

void Shutdown()
{
    g.pending.store(false);
    if (g.writer.joinable()) {
        // Let the writer drain what is queued, then stop.
        for (int i = 0; i < 50; ++i) {
            {
                std::lock_guard<std::mutex> lock(g.m);
                if (g.queued.empty()) break;
            }
            SetEvent(g.wake);
            Sleep(50);
        }
        Sleep(250);
        g.stop.store(true);
        SetEvent(g.wake);
        g.writer.join();
    }
    std::lock_guard<std::mutex> lock(g.m);
    g.current.reset();
    g.queued.clear();
    g.list.Reset();
    g.alloc.Reset();
    g.fence.Reset();
    g.device.Reset();
    if (g.wake) CloseHandle(g.wake);
    g.wake = nullptr;
}

} // namespace na::capture
