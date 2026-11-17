#include "host_dx12/j2/projection_window.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/producers.hpp"
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace na::j2 {
namespace {
std::atomic<uint64_t> g_scissored{0}, g_restored{0};

// Per recording thread: the list whose scissor we last replaced. A game records one list
// per thread at a time; another list or recording starts clean.
struct Applied {
    const void* list = nullptr;
    uint64_t recording = 0, game_serial = 0;
    bool clipped = false;
};
thread_local Applied t_applied;

// Runs after each game OMSetRenderTargets / RSSetScissorRects (state changes, not draws).
void on_raster_state(ID3D12GraphicsCommandList* list, const na::observe::RasterScissors& s)
{
    Applied& a = t_applied;
    if (a.list != list || a.recording != s.recording) a = {list, s.recording, 0, false};
    uint64_t depth = 0;
    uint32_t ew = 0, eh = 0;
    const bool on = ProjectionWindowScissor(&depth, &ew, &eh) && depth && ew && eh;
    const auto* game = reinterpret_cast<const D3D12_RECT*>(s.rects);
    if (!on || s.ds != depth || !s.count) {
        // The game's own rectangles are still the list state: put them back if our clip is live.
        if (a.clipped && a.game_serial == s.scissor_serial && s.count) {
            list->RSSetScissorRects(s.count, game);
            g_restored.fetch_add(1, std::memory_order_relaxed);
        }
        a.clipped = false;
        return;
    }
    if (a.clipped && a.game_serial == s.scissor_serial) return; // same rectangles, clip still in place
    D3D12_RECT clip[16];
    bool changed = false;
    for (uint32_t i = 0; i < s.count; ++i) {
        clip[i] = game[i];
        clip[i].right = (std::min)(clip[i].right, static_cast<LONG>(ew));
        clip[i].bottom = (std::min)(clip[i].bottom, static_cast<LONG>(eh));
        if (clip[i].right < clip[i].left) clip[i].right = clip[i].left;
        if (clip[i].bottom < clip[i].top) clip[i].bottom = clip[i].top;
        changed |= std::memcmp(&clip[i], &game[i], sizeof clip[i]) != 0;
    }
    a.game_serial = s.scissor_serial;
    a.clipped = changed;
    if (!changed) return; // already inside the corner
    list->RSSetScissorRects(s.count, clip);
    g_scissored.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

void InstallProjectionWindowScissor() { na::observe::SetRasterStateListener(&on_raster_state); }
uint64_t ProjectionWindowScissored() { return g_scissored.load(); }
uint64_t ProjectionWindowScissorRestored() { return g_restored.load(); }
} // namespace na::j2
