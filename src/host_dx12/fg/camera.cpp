#include "host_dx12/fg/camera.hpp"
#include <mutex>
#include <array>
namespace na::fg {
namespace {
struct Entry { uint64_t frame=0; uint32_t viewport=0; Camera camera{}; };
std::array<Entry,64> entries;
std::mutex mutex;
CameraStats stats;
thread_local uint64_t frame_id=0;
thread_local uint32_t view_id=0;
}
void RecordCamera(uint64_t frame, uint32_t view, const Camera& c) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!frame || !ValidCamera(c)) { ++stats.rejected; return; }
    auto& e=entries[frame%entries.size()]; e={frame,view,c}; e.camera.extension=nullptr;
    ++stats.recorded;
}
bool FindCamera(uint64_t frame,uint32_t view,Camera& c) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto& e=entries[frame%entries.size()];
    if (!frame || e.frame!=frame || e.viewport!=view) { ++stats.missed; return false; }
    c=e.camera; ++stats.matched; return true;
}
CameraStats GetCameraStats() { std::lock_guard<std::mutex> lock(mutex); return stats; }
EvaluationScope::EvaluationScope(uint32_t frame,uint32_t view):previous_frame(frame_id),previous_view(view_id) { frame_id=uint64_t(frame)+1; view_id=view; }
EvaluationScope::~EvaluationScope() { frame_id=previous_frame; view_id=previous_view; }
uint64_t EvaluationFrame() { return frame_id; }
uint32_t EvaluationViewport() { return view_id; }
}
