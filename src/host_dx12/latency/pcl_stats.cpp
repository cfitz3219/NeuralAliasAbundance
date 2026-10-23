#include "host_dx12/latency/pcl_stats.hpp"
#include <atomic>
#if defined(NA_HAVE_PCL_STATS)
// NVIDIA SDK kept outside source distribution; see dependency pin/document.
#include <pclstats.h>
#pragma warning(push)
#pragma warning(disable: 4100) // unused parameter in NVIDIA's unmodified macro
PCLSTATS_DEFINE();
#pragma warning(pop)
#endif
namespace na::latency::pcl {
namespace {
std::atomic<bool> ready{false}, ping{false}, click{false};
}
void Initialize() {
#if defined(NA_HAVE_PCL_STATS)
    PCLSTATS_INIT(0);
    ready.store(g_PCLStatsQuitEvent && g_PCLStatsPingThread, std::memory_order_release);
#endif
}
bool Available() { return ready.load(std::memory_order_acquire); }
void Mark(uint32_t marker, uint64_t frame) {
    if (!frame || !Available()) return;
#if defined(NA_HAVE_PCL_STATS)
    // Input-sample marker 6 is deprecated in PCL; NVAPI still accepts it.
    if (marker != 6) PCLSTATS_MARKER(marker, frame);
#else
    (void)marker;
#endif
}
void Message(UINT msg, WPARAM key) {
    if (!Available()) return;
#if defined(NA_HAVE_PCL_STATS)
    if (PCLSTATS_IS_PING_MSG_ID(msg) ||
        (msg == WM_KEYDOWN && g_PCLStatsVirtualKey && key == g_PCLStatsVirtualKey)) ping.store(true);
#else
    (void)key;
#endif
    if (msg == WM_LBUTTONDOWN) click.store(true);
}
bool Sample(uint64_t frame) {
    if (ping.exchange(false)) Mark(8, frame); // next frame consuming this input
    const bool pressed = click.exchange(false);
    if (pressed) Mark(7, frame);
    return pressed;
}
}
