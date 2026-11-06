#pragma once
#include <d3d12.h>
#include <cstdint>
#include <memory>
namespace na::scale {
// Opt-in no-slider path. Setup before producer installation; Poll runs on the
// existing session worker, never on a draw/Present callback.
void ConfigureAutomaticScene(bool enabled);
void PollAutomaticScene();
bool AutomaticSceneWorkPending();
void AutomaticScenePresent();
struct AutomaticSceneStats {
    bool enabled=false,active=false,reconstructed=false;
    uint32_t render_w=0,render_h=0,out_w=0,out_h=0,members=0,plans=0;
    uint64_t generation=0,operations=0,redirected=0,checkpoints=0,misses=0,frames=0;
    uint64_t poll_builds=0,failed_plans=0,group_builds=0;
    uint64_t fast_hits=0,snapshots=0,presents=0,redirected_frames=0;
    uint64_t reconstructed_frames=0;
    uint64_t snapshot_failures=0;
    uint64_t unknown_operations=0,binding_failures=0,native_boundaries=0,transfer_failures=0;
    uint64_t control_wait_us=0,control_hold_us=0,control_max_us=0,worker_us=0;
    uint64_t constant_copies=0,constant_reuses=0;
    uint64_t shader_heaps=0,shader_descriptors=0,shared_tables=0,rtv_heaps=0,dsv_heaps=0,heap_failures=0;uint32_t heap_hr=0;
    char failure_kinds[256]{};
    char reason[192]{};
};
AutomaticSceneStats GetAutomaticSceneStats();
// Select exact executable recordings around the existing J2 Execute wrapper.
// A previous frame or a recording from a different scene generation is not used.
class AutomaticSceneSubmission {
public:
    AutomaticSceneSubmission(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    ~AutomaticSceneSubmission();
private:
    std::shared_ptr<void> prior_;
};
}
