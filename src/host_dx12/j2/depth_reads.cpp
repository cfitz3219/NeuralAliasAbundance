#include "depth_reads.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/scale/depth_unjitter.hpp"
#include <wrl/client.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace na::j2::depth_reads {
namespace {
using Microsoft::WRL::ComPtr;
struct Entry {
    uint64_t id = 0;                 // registry id of the game's depth
    ComPtr<ID3D12Resource> depth;    // the game's depth (kept: the twin's views name its size and format)
    std::shared_ptr<const na::scale::DepthUnjitter> pass;
    uint64_t refreshed_at_install = 0;
    bool redirected = false;         // the game's views name the twin
};
struct Parked { std::shared_ptr<Entry> entry; uint64_t at = 0; };
struct State {
    std::mutex m;
    std::shared_ptr<Entry> live;      // read by the listener through `current`
    std::vector<Parked> parked;       // twins no view should name any more; released much later
    uint64_t ticks = 0, refused_id = 0, latched_id = 0;
    std::string refused;              // why the selected depth has no twin
    bool latched = false;             // a recording could not be restored: off until the depth changes
};
State& S() { static auto* s = new State(); return *s; }
std::mutex g_current_m;
std::shared_ptr<Entry> g_current;
std::shared_ptr<Entry> current() { std::lock_guard<std::mutex> lock(g_current_m); return g_current; }
void set_current(std::shared_ptr<Entry> e) { std::lock_guard<std::mutex> lock(g_current_m); g_current = std::move(e); }

std::atomic<uint64_t> g_refreshed{0}, g_no_restore{0}, g_no_pixel_read{0}, g_restore_failed{0}, g_no_sample{0};
std::atomic<bool> g_stop{false};

// The game moved its depth from "being written" to "readable" on this recording (already issued).
void on_readable(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, uint32_t state_after, bool)
{
    const auto e = current();
    if (!e || e->depth.Get() != depth || g_stop.load(std::memory_order_acquire)) return;
    if (!(state_after & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) { g_no_pixel_read.fetch_add(1, std::memory_order_relaxed); return; }
    // Nothing is recorded unless everything it changes can be put back.
    if (!na::observe::ListStateRestorable(list) || !na::observe::ListRasterStateTracked(list)) { g_no_restore.fetch_add(1, std::memory_order_relaxed); return; }
    // The nudge the depth was drawn with: this recording's own scene sample, else the one the scene's
    // recordings pinned last. None = the scene was not nudged: the twin is then a plain copy.
    uint64_t sample = na::observe::ListSceneViewportJitter(list, e->id);
    if (!sample) sample = na::observe::LastSceneViewportJitter();
    float jx = 0, jy = 0;
    if (!sample || !na::observe::DecodeSceneViewportJitter(sample, nullptr, &jx, &jy)) { jx = jy = 0; g_no_sample.fetch_add(1, std::memory_order_relaxed); }
    e->pass->RecordRefresh(list, jx, jy);
    if (!na::observe::RestoreListState(list)) {
        // Cannot happen after the two checks above on the same recording; if it does, stop for good rather than
        // leave a second recording with the host's bindings.
        g_restore_failed.fetch_add(1, std::memory_order_relaxed);
        g_stop.store(true, std::memory_order_release);
        return;
    }
    g_refreshed.fetch_add(1, std::memory_order_relaxed);
}
// Lets what the game has submitted finish, so no view being rewritten is in use on the card.
void drain(ID3D12CommandQueue* queue)
{
    if (!queue) return;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return;
    if (SUCCEEDED(queue->Signal(fence.Get(), 1)) && SUCCEEDED(fence->SetEventOnCompletion(1, event))) WaitForSingleObject(event, 250);
    CloseHandle(event);
}
// Hands the game's views back to its own depth and parks the twin.
void retire(State& s, ID3D12CommandQueue* queue)
{
    auto e = std::move(s.live);
    s.live.reset();
    if (!e) return;
    na::observe::SetDepthReadTwin(nullptr, nullptr);
    set_current({});
    if (e->redirected) { drain(queue); na::observe::RedirectExistingDepthReads(e->depth.Get(), e->depth.Get()); }
    s.parked.push_back({std::move(e), s.ticks});
}
} // namespace

// VARIANT: depth-reads/twin (chosen by what is observed: viewport jitter supplying samples, and a scene depth this can twin)
void Tick(ID3D12CommandQueue* queue, uint64_t scene_depth, bool want)
{
    State& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    ++s.ticks;
    // A parked twin is released long after its last possible use (recordings in flight, views not yet rewritten).
    for (size_t i = 0; i < s.parked.size();)
        if (s.ticks - s.parked[i].at > 600) s.parked.erase(s.parked.begin() + static_cast<std::ptrdiff_t>(i)); else ++i;
    if (g_stop.load(std::memory_order_acquire) && !s.latched) { // a recording could not be restored: off for this depth
        s.latched = true; s.latched_id = s.live ? s.live->id : scene_depth;
        retire(s, queue);
    }
    if (s.latched && scene_depth != s.latched_id) { s.latched = false; g_stop.store(false, std::memory_order_release); }
    if (!want || !scene_depth) {
        retire(s, queue);
        s.refused.clear(); s.refused_id = 0;
        return;
    }
    if (s.latched) return;
    if (s.live && s.live->id != scene_depth) retire(s, queue);
    if (!s.live) {
        if (s.refused_id == scene_depth) return; // refused once for this depth: the reason is in the log
        ComPtr<ID3D12Resource> depth;
        depth.Attach(na::observe::AcquireResource(scene_depth));
        if (!depth) return;
        ComPtr<ID3D12Device> device;
        if (FAILED(depth->GetDevice(IID_PPV_ARGS(&device)))) return;
        auto e = std::make_shared<Entry>();
        e->id = scene_depth; e->depth = depth;
        std::string error;
        e->pass = na::scale::DepthUnjitter::Create(device.Get(), depth.Get(), error);
        if (!e->pass) { s.refused = error; s.refused_id = scene_depth; return; }
        s.refused.clear(); s.refused_id = 0;
        e->refreshed_at_install = g_refreshed.load(std::memory_order_relaxed);
        s.live = e;
        set_current(e);
        na::observe::SetDepthReadableListener(&on_readable);
        // Step 1: the twin is filled at the game's barriers, but no view names it yet (twin = null).
        na::observe::SetDepthReadTwin(depth.Get(), nullptr);
        return;
    }
    // Step 2, a frame or more later, once the twin has real content: views go to it.
    if (!s.live->redirected && g_refreshed.load(std::memory_order_relaxed) > s.live->refreshed_at_install) {
        drain(queue);
        na::observe::SetDepthReadTwin(s.live->depth.Get(), s.live->pass->Twin());
        na::observe::RedirectExistingDepthReads(s.live->depth.Get(), s.live->pass->Twin());
        s.live->redirected = true;
    }
}
int Format(char* out, size_t n)
{
    if (!out || !n) return 0;
    State& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    const auto o = na::observe::GetDepthReadStats();
    UINT64 w = 0; UINT h = 0; UINT format = 0;
    if (s.live) { const auto d = s.live->depth->GetDesc(); w = d.Width; h = d.Height; format = d.Format; }
    const char* state = s.latched ? "stopped" : !s.live ? (s.refused.empty() ? "off" : "refused") : s.live->redirected ? "on" : "filling";
    return std::snprintf(out, n,
        "depth_reads variant=depth-reads/twin state=%s depth=%llux%u format=%u refreshed=%llu views_created=%llu views_rewritten=%llu "
        "readable_barriers=%llu not_shader_readable=%llu not_pixel_readable=%llu not_restorable=%llu restore_failed=%llu no_sample=%llu why=\"%s\"",
        state, static_cast<unsigned long long>(w), h, format, static_cast<unsigned long long>(g_refreshed.load()),
        static_cast<unsigned long long>(o.created), static_cast<unsigned long long>(o.swept), static_cast<unsigned long long>(o.readable),
        static_cast<unsigned long long>(o.other), static_cast<unsigned long long>(g_no_pixel_read.load()),
        static_cast<unsigned long long>(g_no_restore.load()), static_cast<unsigned long long>(g_restore_failed.load()),
        static_cast<unsigned long long>(g_no_sample.load()),
        s.latched ? "a recording could not be restored" : s.refused.c_str());
}
void Shutdown()
{
    State& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    retire(s, nullptr);
    na::observe::SetDepthReadableListener(nullptr);
    s.parked.clear(); s.refused.clear(); s.refused_id = 0; s.latched = false;
    g_stop.store(false, std::memory_order_release);
}
} // namespace na::j2::depth_reads
