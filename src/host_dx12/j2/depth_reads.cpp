#include "depth_reads.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/nr/native_fg.hpp"
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
using na::nr::native::RecordingLease;
struct Entry {
    uint64_t id = 0;                 // registry id of the game's depth
    ComPtr<ID3D12Resource> depth;    // the game's depth (kept: its address cannot be reused under the twin)
    std::shared_ptr<const na::scale::DepthUnjitter> pass;
    std::mutex m;
    RecordingLease filled[8];        // the latest recordings that brought the twin up to date
    uint32_t next = 0;
    bool redirected = false;         // the game's views name the twin
    uint32_t sweeps = 0;             // sweeps made since the hand-over (a few, to catch views copied meanwhile)
};
struct Parked { std::shared_ptr<Entry> entry; uint64_t at = 0; uint32_t back_sweeps = 0; };
struct State {
    std::mutex m;
    std::shared_ptr<Entry> live;      // read by the listeners through `current`
    std::vector<Parked> parked;       // twins no view should name any more
    uint64_t ticks = 0, refused_id = 0, latched_id = 0;
    std::string refused;              // why the selected depth has no twin
    bool latched = false;             // the twin could not be kept current: off until the depth changes
};
State& S() { static auto* s = new State(); return *s; }
std::mutex g_current_m;
std::shared_ptr<Entry> g_current;
std::shared_ptr<Entry> current() { std::lock_guard<std::mutex> lock(g_current_m); return g_current; }
void set_current(std::shared_ptr<Entry> e) { std::lock_guard<std::mutex> lock(g_current_m); g_current = std::move(e); }

// corrected: the twin was drawn with the jitter taken out; copied: it got the game's depth as it is
// (the scene was not nudged, the nudge is not known here, or a draw could not be handed back here);
// upgraded: a copy was followed by the corrected draw before the recording's next draw.
std::atomic<uint64_t> g_corrected{0}, g_copied{0}, g_upgraded{0}, g_no_sample{0}, g_not_restorable{0};
std::atomic<bool> g_stop{false};
std::atomic<const char*> g_stop_why{""};
void stop(const char* why) { g_stop_why.store(why, std::memory_order_release); g_stop.store(true, std::memory_order_release); }

constexpr uint32_t kReadable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
// This thread's recording that took a plain copy at its barrier and wants the corrected draw
// before its next draw or dispatch (when the game has bound everything a host draw must hand back).
struct Pending { ID3D12GraphicsCommandList* list = nullptr; uint64_t recording = 0; uint32_t state = 0, tries = 0; bool all_planes = false; };
thread_local Pending t_pending;
void drop_pending() { t_pending = {}; na::observe::SetDepthReadPending(nullptr); }

void keep(Entry& e, RecordingLease lease) { std::lock_guard<std::mutex> lock(e.m); e.filled[e.next++ % 8] = std::move(lease); }
// The corrected draw, then the game's state back. False: the recording now holds host state (stopped for good).
bool draw(const std::shared_ptr<Entry>& e, ID3D12GraphicsCommandList* list, const na::observe::SceneRaster& raster, uint32_t state, bool all_planes)
{
    float jx = 0, jy = 0;
    na::observe::DecodeSceneViewportJitter(raster.sample, nullptr, &jx, &jy);
    e->pass->RecordRefresh(list, jx, jy, raster.expanded, static_cast<D3D12_RESOURCE_STATES>(state), all_planes);
    const bool restored = na::observe::RestoreListState(list);
    // The game's own viewports are bound now: its next scene draw takes the sample again.
    na::observe::ViewportJitterDropped(list);
    if (!restored) { stop("a recording could not be handed back"); return false; }
    return true;
}
// After the game's barrier on the twinned depth (already issued), on the recording that holds it.
void on_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, uint32_t before, uint32_t after, bool all_planes)
{
    if (t_pending.list == list) drop_pending(); // the depth's state changed: what was waiting no longer applies
    const auto e = current();
    if (!e || e->depth.Get() != depth || g_stop.load(std::memory_order_acquire)) return;
    if (!(after & kReadable) || (before & kReadable)) return; // not readable now, or it already was (nothing was written in between)
    // From here the twin MUST be brought up to date on this recording, at this point: the game's reads follow.
    auto lease = na::nr::native::RetainRecording(list, e);
    if (!lease) { stop("the recording's lifetime is not known"); return; }
    na::observe::SceneRaster raster;
    const bool nudged = na::observe::SceneRasterFor(list, e->id, raster);
    if (!nudged) g_no_sample.fetch_add(1, std::memory_order_relaxed);
    if (nudged && na::observe::ListHostDrawAllowed(list)) {
        if (!draw(e, list, raster, after, all_planes)) return;
        g_corrected.fetch_add(1, std::memory_order_relaxed);
    } else if (na::observe::ListHostCopyAllowed(list)) {
        e->pass->RecordCopy(list, static_cast<D3D12_RESOURCE_STATES>(after), all_planes);
        g_copied.fetch_add(1, std::memory_order_relaxed);
        if (nudged) {
            t_pending = {list, na::observe::ListRecording(list), after, 0, all_planes};
            if (t_pending.recording) na::observe::SetDepthReadPending(list); else t_pending = {};
        }
    } else { stop("the twin could not be brought up to date at a barrier"); return; }
    keep(*e, std::move(lease));
}
// Before a draw or dispatch of a recording that is waiting (see Pending).
void on_before_draw(ID3D12GraphicsCommandList* list)
{
    const Pending p = t_pending;
    if (p.list != list) { na::observe::SetDepthReadPending(p.list); return; }
    const auto e = current();
    if (!e || g_stop.load(std::memory_order_acquire) || na::observe::ListRecording(list) != p.recording) { drop_pending(); return; }
    na::observe::SceneRaster raster;
    if (!na::observe::SceneRasterFor(list, e->id, raster)) { drop_pending(); return; }
    if (!na::observe::ListHostDrawAllowed(list)) {
        // Not yet (a query is counting, or something is still unbound): the copy stands; try at the next few draws.
        if (++t_pending.tries >= 16) { g_not_restorable.fetch_add(1, std::memory_order_relaxed); drop_pending(); }
        return;
    }
    drop_pending();
    auto lease = na::nr::native::RetainRecording(list, e);
    if (!lease) return; // the copy stands
    if (!draw(e, list, raster, p.state, p.all_planes)) return;
    g_upgraded.fetch_add(1, std::memory_order_relaxed);
    keep(*e, std::move(lease));
}
// Hands the game's views back to its own depth and parks the twin.
void retire(State& s)
{
    auto e = std::move(s.live);
    s.live.reset();
    if (!e) return;
    na::observe::SetDepthReadTwin(nullptr, nullptr);
    set_current({});
    uint32_t back = 0;
    if (e->redirected) { na::observe::RedirectExistingDepthReads(e->depth.Get(), e->depth.Get()); back = 2; }
    s.parked.push_back({std::move(e), s.ticks, back});
}
} // namespace

// VARIANT: depth-reads/twin (chosen by what is observed: viewport jitter supplying samples, and a scene depth this can twin)
void Tick(ID3D12CommandQueue* queue, uint64_t scene_depth, bool want)
{
    State& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    ++s.ticks;
    for (size_t i = 0; i < s.parked.size();) {
        auto& p = s.parked[i];
        // A view the game copied while the views were being handed back is found by the next sweeps.
        if (p.back_sweeps && !(s.live && s.live->depth == p.entry->depth)) {
            na::observe::RedirectExistingDepthReads(p.entry->depth.Get(), p.entry->depth.Get());
            --p.back_sweeps;
        }
        // Recordings that use the twin keep it alive themselves (RetainRecording); this only bounds the list.
        if (s.ticks - p.at > 600) s.parked.erase(s.parked.begin() + static_cast<std::ptrdiff_t>(i)); else ++i;
    }
    if (g_stop.load(std::memory_order_acquire) && !s.latched) { // the twin could not be kept current: off for this depth
        s.latched = true; s.latched_id = s.live ? s.live->id : scene_depth;
        retire(s);
    }
    if (s.latched && scene_depth != s.latched_id) { s.latched = false; g_stop.store(false, std::memory_order_release); }
    if (!want || !scene_depth) {
        retire(s);
        s.refused.clear(); s.refused_id = 0;
        return;
    }
    if (s.latched) return;
    if (s.live && s.live->id != scene_depth) retire(s);
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
        s.live = e;
        set_current(e);
        na::observe::SetDepthReadableListener(&on_barrier);
        na::observe::SetDepthPendingListener(&on_before_draw);
        // Step 1: the twin is brought up to date at the game's barriers, but no view names it yet (twin = null).
        na::observe::SetDepthReadTwin(depth.Get(), nullptr);
        return;
    }
    // Step 2: views go to the twin once the card has FINISHED a recording that brought it up to date
    // (a recording that was closed and never submitted proves nothing). From then on every barrier
    // that makes the depth readable brings the twin up to date before the reads that follow it.
    if (!s.live->redirected) {
        bool filled = false;
        {
            // Submitted to the game's queue AND finished there: a discarded recording counts as neither.
            std::lock_guard<std::mutex> el(s.live->m);
            for (const auto& lease : s.live->filled) filled = filled || (lease && lease.SubmissionOrder(queue) && lease.Completed());
        }
        if (!filled) return;
        na::observe::SetDepthReadTwin(s.live->depth.Get(), s.live->pass->Twin());
        na::observe::RedirectExistingDepthReads(s.live->depth.Get(), s.live->pass->Twin());
        s.live->redirected = true;
    } else if (s.live->sweeps < 2) {
        // A view the game copied from one not yet rewritten, while the sweep ran.
        na::observe::RedirectExistingDepthReads(s.live->depth.Get(), s.live->pass->Twin());
        ++s.live->sweeps;
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
        "depth_reads variant=depth-reads/twin api=dx12 state=%s depth=%llux%u format=%u corrected=%llu copied=%llu upgraded=%llu "
        "views_created=%llu views_rewritten=%llu views_left=%llu views_raced=%llu readable_barriers=%llu no_sample=%llu "
        "not_restorable=%llu why=\"%s\"",
        state, static_cast<unsigned long long>(w), h, format, static_cast<unsigned long long>(g_corrected.load()),
        static_cast<unsigned long long>(g_copied.load()), static_cast<unsigned long long>(g_upgraded.load()),
        static_cast<unsigned long long>(o.created), static_cast<unsigned long long>(o.swept), static_cast<unsigned long long>(o.unknown_view),
        static_cast<unsigned long long>(o.raced), static_cast<unsigned long long>(o.readable),
        static_cast<unsigned long long>(g_no_sample.load()), static_cast<unsigned long long>(g_not_restorable.load()),
        s.latched ? g_stop_why.load(std::memory_order_acquire) : s.refused.c_str());
}
void Shutdown()
{
    State& s = S();
    std::lock_guard<std::mutex> lock(s.m);
    retire(s);
    na::observe::SetDepthReadableListener(nullptr);
    na::observe::SetDepthPendingListener(nullptr);
    s.parked.clear(); s.refused.clear(); s.refused_id = 0; s.latched = false;
    g_stop.store(false, std::memory_order_release);
}
} // namespace na::j2::depth_reads
