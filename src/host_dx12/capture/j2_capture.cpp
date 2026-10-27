#include "host_dx12/capture/j2_capture.hpp"

#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace na::capture::j2 {
namespace {

enum PlaneIndex { kColor = 0, kDepth, kMotion, kOutput, kPresent, kPlaneCount };
const char* const kPlaneNames[kPlaneCount] = {"color", "depth", "motion", "dlssout", "present"};

struct Plane {
    ComPtr<ID3D12Resource> readback;   // owned by the slot; reused only when the slot is free
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    D3D12_RESOURCE_DESC desc{};
    UINT64 bytes = 0;
    bool valid = false;                // recorded for the slot's current use
};

enum class SlotState : uint8_t { Free, Recorded, Submitted, Ready, Writing, Retired };

struct Slot {
    SlotState state = SlotState::Free;
    Plane planes[kPlaneCount];
    ComPtr<ID3D12CommandAllocator> alloc; // the presented-image copy: this slot's own
    ComPtr<ID3D12GraphicsCommandList> list;
    Identity id{};
    uint32_t index = 0;          // frame number within the capture session
    uint32_t session = 0;
    UINT64 eval_fence = 0, present_fence = 0;
    uint64_t submit_ordinal = 0; // queue submission counter at submit
    uint64_t late_writes = 0;
    bool closed = false;         // J2 closed its frame at Present
    wchar_t dir[MAX_PATH] = {};
};

struct State {
    std::mutex m;
    std::atomic<bool> armed{false};
    std::atomic<ULONGLONG> start_at{0};
    uint32_t session = 0;
    uint32_t requested = 0;
    Stats stats{};
    wchar_t dir[MAX_PATH] = {};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    Slot slots[kSlots];
    // Detached: a joinable std::thread in a static object would terminate the process at
    // exit, and joining from DLL detach can deadlock on the loader lock.
    std::atomic<bool> writer_running{false};
    std::atomic<bool> stop{false};
    HANDLE wake = nullptr;
};
State g;

bool ensure_device(ID3D12Device* dev)
{
    if (g.device) return g.device.Get() == dev;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    g.device = dev;
    return true;
}

bool ensure_readback(Plane& p, ID3D12Resource* res)
{
    p.valid = false;
    if (!res) return false;
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    if (!total) return false;
    if (!p.readback || p.bytes != total) {
        p.readback.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&p.readback))))
            return false;
        p.bytes = total;
    }
    p.desc = d;
    p.footprint = fp;
    return true;
}

// state -> COPY_SOURCE, copy subresource 0 into the slot's readback, back to state.
void record_copy(ID3D12GraphicsCommandList* l, ID3D12Resource* res, D3D12_RESOURCE_STATES state, Plane& p)
{
    D3D12_RESOURCE_BARRIER b[2] = {};
    UINT nb = 0;
    if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; // finish the writer (DLSS) before copying
        b[nb++].UAV.pResource = res;
    }
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nb].Transition.pResource = res;
        b[nb].Transition.Subresource = 0;
        b[nb].Transition.StateBefore = state;
        b[nb++].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (nb) l->ResourceBarrier(nb, b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    dst.pResource = p.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = p.footprint;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER back{};
        back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        back.Transition.pResource = res;
        back.Transition.Subresource = 0;
        back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        back.Transition.StateAfter = state;
        l->ResourceBarrier(1, &back);
    }
    p.valid = true;
}

Slot* slot_of(uint32_t token)
{
    if (!token || token > kSlots) return nullptr;
    return &g.slots[token - 1];
}

// Closed slots that will not get a presented image any more become ready without it.
void release_unpresented(uint64_t newer_than)
{
    for (auto& s : g.slots)
        if (s.state == SlotState::Submitted && s.closed && s.id.present_ordinal < newer_than) {
            s.state = SlotState::Ready;
            ++g.stats.without_present;
        }
}

void write_slot(const Slot& s)
{
    wchar_t path[MAX_PATH + 64];
    for (int i = 0; i < kPlaneCount; ++i) {
        const Plane& p = s.planes[i];
        if (!p.valid || !p.readback) continue;
        swprintf_s(path, L"%s\\%05u-%S.raw", s.dir, s.index, kPlaneNames[i]);
        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(p.bytes)};
        if (FAILED(p.readback->Map(0, &range, &mapped)) || !mapped) continue;
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path, L"wb") == 0 && fp) {
            fwrite(mapped, 1, static_cast<size_t>(p.bytes), fp);
            fclose(fp);
        }
        D3D12_RANGE none{0, 0};
        p.readback->Unmap(0, &none);
    }
    const Identity& id = s.id;
    swprintf_s(path, L"%s\\%05u-meta.json", s.dir, s.index);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"wb") == 0 && fp) {
        std::fprintf(fp,
            "{\n  \"format\": \"na-j2-capture-1\",\n  \"index\": %u,\n  \"present_ordinal\": %llu,\n"
            "  \"submission\": %llu,\n  \"queue_submission\": %llu,\n  \"recording\": %llu,\n"
            "  \"view\": {\"color\": %llu, \"depth\": %llu, \"dlss_view_id\": %u, \"color_sub\": %u},\n"
            "  \"route\": %u,\n  \"route_mode\": %u,\n  \"history_generation\": %u,\n  \"insertion_signature\": %llu,\n"
            "  \"size\": [%u, %u],\n  \"color_format\": %u,\n  \"jitter_px\": [%.6f, %.6f],\n  \"mv_scale\": [%.4f, %.4f],\n"
            "  \"reset\": %u,\n  \"camera_cut\": %u,\n  \"preset\": %u,\n  \"sharpness\": %u,\n  \"debug_view\": %u,\n"
            "  \"late_writes\": %llu,\n  \"frame_closed\": %s,\n  \"planes\": [\n",
            s.index, (unsigned long long)id.present_ordinal, (unsigned long long)id.submission,
            (unsigned long long)s.submit_ordinal, (unsigned long long)id.recording,
            (unsigned long long)id.view_color, (unsigned long long)id.view_depth, id.view_id, id.color_sub,
            id.route, id.route_mode, id.history_generation, (unsigned long long)id.insertion_signature,
            id.width, id.height, id.color_format, id.jitter_x, id.jitter_y, id.mv_scale_x, id.mv_scale_y,
            id.reset, id.camera_cut, id.preset, id.sharpness, id.debug_view,
            (unsigned long long)s.late_writes, s.closed ? "true" : "false");
        bool first = true;
        for (int i = 0; i < kPlaneCount; ++i) {
            const Plane& p = s.planes[i];
            if (!p.valid) continue;
            std::fprintf(fp, "%s    {\"name\": \"%s\", \"width\": %llu, \"height\": %u, \"dxgi_format\": %u, \"row_pitch\": %u, \"bytes\": %llu}",
                         first ? "" : ",\n", kPlaneNames[i], (unsigned long long)p.desc.Width, p.desc.Height,
                         static_cast<unsigned>(p.desc.Format), p.footprint.Footprint.RowPitch, (unsigned long long)p.bytes);
            first = false;
        }
        std::fprintf(fp, "\n  ]\n}\n");
        std::fclose(fp);
    }
    swprintf_s(path, L"%s\\index.csv", s.dir);
    fp = nullptr;
    if (_wfopen_s(&fp, path, L"ab") == 0 && fp) {
        if (s.index == 0)
            std::fprintf(fp, "index,present_ordinal,submission,route,route_mode,generation,signature,jitter_x,jitter_y,reset,camera_cut,late_writes,present\n");
        std::fprintf(fp, "%u,%llu,%llu,%u,%u,%u,%llu,%.6f,%.6f,%u,%u,%llu,%u\n", s.index, (unsigned long long)id.present_ordinal,
                     (unsigned long long)id.submission, id.route, id.route_mode, id.history_generation,
                     (unsigned long long)id.insertion_signature, id.jitter_x, id.jitter_y, id.reset, id.camera_cut,
                     (unsigned long long)s.late_writes, s.planes[kPresent].valid ? 1u : 0u);
        std::fclose(fp);
    }
}

void writer_main()
{
    while (!g.stop.load()) {
        WaitForSingleObject(g.wake, 100);
        for (;;) {
            Slot* next = nullptr;
            {
                std::lock_guard<std::mutex> lock(g.m);
                if (!g.fence) break;
                const UINT64 done = g.fence->GetCompletedValue(); // non-blocking; never on the render thread
                if (done == UINT64_MAX) break; // device removed: nothing will complete
                for (auto& s : g.slots) {
                    if (s.state != SlotState::Ready) continue;
                    if (done < s.eval_fence || done < s.present_fence) continue;
                    if (!next || s.index < next->index) next = &s;
                }
                if (next) next->state = SlotState::Writing;
            }
            if (!next) break;
            write_slot(*next); // only the writer touches a Writing slot
            std::lock_guard<std::mutex> lock(g.m);
            next->state = SlotState::Free;
            ++g.stats.written;
        }
    }
    g.writer_running.store(false);
}

} // namespace

void Request(uint32_t frames, const wchar_t* dir, uint32_t delay_ms)
{
    if (!dir || !frames) return;
    std::lock_guard<std::mutex> lock(g.m);
    wcsncpy_s(g.dir, dir, _TRUNCATE);
    CreateDirectoryW(g.dir, nullptr);
    ++g.session;
    g.requested = (std::min)(frames, kMaxFrames);
    const Stats keep = g.stats;
    g.stats = Stats{};
    g.stats.requested = g.requested;
    g.stats.retired = keep.retired; // retired slots stay retired for the process
    g.start_at = GetTickCount64() + delay_ms;
    if (!g.wake) g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.writer_running.load()) {
        g.stop.store(false);
        g.writer_running.store(true);
        std::thread(&writer_main).detach();
    }
    g.armed.store(true, std::memory_order_release);
}

bool RequestTimestamped(uint32_t frames, uint32_t delay_ms)
{
    wchar_t* local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local) return false;
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\NeuralAlias", local);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(dir, L"%s\\NeuralAlias\\captures", local);
    CreateDirectoryW(dir, nullptr);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    swprintf_s(dir, L"%s\\NeuralAlias\\captures\\j2-%04u%02u%02u-%02u%02u%02u", local, t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond);
    CoTaskMemFree(local);
    Request(frames, dir, delay_ms);
    return true;
}

bool Armed()
{
    return g.armed.load(std::memory_order_acquire) && GetTickCount64() >= g.start_at.load(std::memory_order_relaxed);
}

uint32_t RecordEvaluation(ID3D12GraphicsCommandList* list, const Planes& p, const Identity& id)
{
    if (!Armed() || !list) return 0;
    std::lock_guard<std::mutex> lock(g.m);
    if (g.stats.recorded >= g.requested) { g.armed.store(false); return 0; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&dev))) || !ensure_device(dev.Get())) { ++g.stats.errors; return 0; }
    Slot* s = nullptr;
    for (auto& c : g.slots) if (c.state == SlotState::Free) { s = &c; break; }
    if (!s) { ++g.stats.dropped_busy; return 0; } // never wait: the frame is skipped, counted
    ID3D12Resource* res[4] = {p.color, p.depth, p.motion, p.output};
    const D3D12_RESOURCE_STATES st[4] = {p.color_state, p.depth_state, p.motion_state, p.output_state};
    for (int i = 0; i < 4; ++i)
        if (!ensure_readback(s->planes[i], res[i])) { ++g.stats.errors; return 0; } // nothing recorded yet
    for (int i = 0; i < 4; ++i) record_copy(list, res[i], st[i], s->planes[i]);
    s->planes[kPresent].valid = false;
    s->state = SlotState::Recorded;
    s->id = id;
    s->index = g.stats.recorded++;
    s->session = g.session;
    s->eval_fence = s->present_fence = 0;
    s->submit_ordinal = 0;
    s->late_writes = 0;
    s->closed = false;
    wcsncpy_s(s->dir, g.dir, _TRUNCATE);
    if (g.stats.recorded >= g.requested) g.armed.store(false, std::memory_order_release);
    return static_cast<uint32_t>(s - g.slots) + 1;
}

void Submitted(uint32_t token, ID3D12CommandQueue* queue, uint64_t submission)
{
    std::lock_guard<std::mutex> lock(g.m);
    Slot* s = slot_of(token);
    if (!s || s->state != SlotState::Recorded || !queue || !g.fence) return;
    s->eval_fence = g.fence_next++;
    queue->Signal(g.fence.Get(), s->eval_fence);
    s->submit_ordinal = submission;
    s->state = SlotState::Submitted;
    ++g.stats.submitted;
}

void Discard(uint32_t token, bool may_still_run)
{
    std::lock_guard<std::mutex> lock(g.m);
    Slot* s = slot_of(token);
    if (!s || s->state != SlotState::Recorded) return;
    if (may_still_run) {
        // Its copies may still execute at an unknown time: never hand these readbacks out again.
        s->state = SlotState::Retired;
        ++g.stats.retired;
    } else {
        s->state = SlotState::Free; // the list is reset before any GPU use
        ++g.stats.discarded;
    }
}

void OnFrameClosed(uint64_t present_ordinal, uint64_t late_writes)
{
    std::lock_guard<std::mutex> lock(g.m);
    release_unpresented(present_ordinal);
    for (auto& s : g.slots)
        if (s.state == SlotState::Submitted && !s.closed && s.id.present_ordinal == present_ordinal) {
            s.closed = true;
            s.late_writes = late_writes;
        }
    if (g.wake) SetEvent(g.wake);
}

void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* presented, ExecuteFn exec)
{
    std::lock_guard<std::mutex> lock(g.m);
    Slot* s = nullptr;
    for (auto& c : g.slots)
        if (c.state == SlotState::Submitted && c.closed && (!s || c.id.present_ordinal > s->id.present_ordinal)) s = &c;
    if (!s) return;
    if (queue && presented && exec && g.device) {
        // The slot's own allocator: free slots have no GPU work pending (writer freed them
        // only after their fences completed), so resetting it here never stalls.
        const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
        bool ok = true;
        if (!s->alloc) {
            ok = SUCCEEDED(g.device->CreateCommandAllocator(type, IID_PPV_ARGS(&s->alloc))) &&
                 SUCCEEDED(g.device->CreateCommandList(0, type, s->alloc.Get(), nullptr, IID_PPV_ARGS(&s->list)));
            if (ok) s->list->Close();
        }
        ok = ok && SUCCEEDED(s->alloc->Reset()) && SUCCEEDED(s->list->Reset(s->alloc.Get(), nullptr)) &&
             ensure_readback(s->planes[kPresent], presented);
        if (ok) {
            record_copy(s->list.Get(), presented, D3D12_RESOURCE_STATE_PRESENT, s->planes[kPresent]);
            ok = SUCCEEDED(s->list->Close());
        }
        if (ok) {
            ID3D12CommandList* one[] = {s->list.Get()};
            exec(queue, 1, one);
            s->present_fence = g.fence_next++;
            queue->Signal(g.fence.Get(), s->present_fence);
        } else {
            s->planes[kPresent].valid = false;
            ++g.stats.errors;
        }
    }
    if (!s->planes[kPresent].valid) ++g.stats.without_present;
    s->state = SlotState::Ready;
    release_unpresented(s->id.present_ordinal + 1);
    if (g.wake) SetEvent(g.wake);
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.stats;
}

const wchar_t* Directory() { return g.dir; }

void Shutdown()
{
    g.armed.store(false);
    if (g.writer_running.load()) {
        for (int i = 0; i < 40; ++i) { // let the writer drain completed slots
            bool busy = false;
            {
                std::lock_guard<std::mutex> lock(g.m);
                for (auto& s : g.slots) busy |= s.state == SlotState::Ready || s.state == SlotState::Writing;
            }
            if (!busy) break;
            SetEvent(g.wake);
            Sleep(50);
        }
        g.stop.store(true);
        SetEvent(g.wake);
        for (int i = 0; i < 40 && g.writer_running.load(); ++i) Sleep(25);
        if (g.writer_running.load()) return; // still writing: keep its objects alive
    }
    std::lock_guard<std::mutex> lock(g.m);
    for (auto& s : g.slots) s = Slot{};
    g.fence.Reset();
    g.device.Reset();
    g.fence_next = 1;
    if (g.wake) CloseHandle(g.wake);
    g.wake = nullptr;
}

} // namespace na::capture::j2
