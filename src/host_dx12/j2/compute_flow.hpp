#pragma once
#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <cstddef>
namespace na::motion::gpu {struct Inputs;}
namespace na::j2::compute_flow {
struct Frame {
    // slot is the caller's ring hint; private resources follow recording leases.
    uint64_t frame=0,previous=0;uint32_t generation=0,slot=0;bool reset=false,hdr=false;
    float jitter[2]{};
    // false: only the M3 camera fit runs (the camera needs reconstructing but the user turned
    // the object-motion residual off); no residual, no tier 3.
    bool residual=true;
    bool camera_reconstruct=false; // the caller found the camera matrix carries no view (status only)
};
struct Ticket {std::shared_ptr<void> context;uint32_t image=0,previous=0,slot=0;Frame frame;bool pair=false;};
// fix: the M3 camera correction buffer of this recording (camera_fit_gpu.hpp), or nullptr. The
// residual was derived against the corrected camera, so the arbiter must use the same fix.
struct Tier3 {ID3D12Resource *residual=nullptr,*valid=nullptr,*mask=nullptr,*fix=nullptr;};
// Called inside J2's restorable inline scope, AFTER its colour/depth resolve.
// All inputs NPSR. No CPU waits; allocations/retirement reuse RecordingLease.
Ticket Record(ID3D12GraphicsCommandList*,ID3D12CommandQueue*,ID3D12Resource* color,ID3D12Resource* depth,
              const na::motion::gpu::Inputs&,const Frame&,Tier3&);
void Committed(const Ticket&); // only after J2's actual submission, never on record
// Acceptance looseness 0..100 (Cort 2026-11-03): 0 = the original Anno-run gates; higher values
// widen the image, temporal and depth-neighbour tests and let objects against clear depth (sky)
// through. Read at each Record.
void SetLoosen(uint32_t percent);
struct Gates { float image_ratio, image_abs, texture_min, agree_px, agree_cos, consistency_px; uint32_t agree_min; bool sky; };
Gates GatesFor(uint32_t percent); // pure, for tests and the panel
void Reset(); // old contexts stay retained by their executable/GPU recordings
struct Stats {
    uint64_t recorded=0,submitted=0,pairs=0,busy=0,resets=0,accepted=0,total=0;
    uint64_t attempts=0,submitted_pairs=0,waiting_history=0,unavailable=0,samples=0;
    uint64_t last_submitted_frame=0,sample_frame=0,submit_age_ms=0,sample_age_ms=0;
    uint64_t rejected_invalid=0,rejected_small=0,rejected_cost=0,rejected_offscreen=0,rejected_image=0,rejected_flicker=0; // flicker: passed every check, no agreeing previous pair yet
    uint32_t recording_slots=0,image_slots=0;
    uint32_t loosen=0; // flow_loosen used by the latest recording
    // Every completed pair's pixel counts summed (not one sample per 5 s): accepted, invalid,
    // small, cost, offscreen, image, flicker; and the pairs counted.
    uint64_t sum[7]{}; uint64_t sum_pairs=0;
    // M3 camera fit (every completed pair): fits recorded, collected, accepted; the last collected
    // result (theta: translation x/y/z world units, rotation x/y/z rad; mean robust error before/after).
    uint64_t fit_recorded=0,fit_collected=0,fit_accepted=0;
    float fit_theta[6]{};float fit_e0=0,fit_e=0;uint32_t fit_n0=0,fit_n=0;bool fit_last_accepted=false;
    double fit_e0_sum=0,fit_e_sum=0; // summed over accepted fits (mean = sum / fit_accepted)
    // Rejections of fits that passed the flow test: by the jitter-delta hypothesis (counted in
    // fit_flow_rejected_jitter when the plain test alone would have accepted) and by the image check.
    bool camera_reconstruct=false;uint64_t reconstruct_frames=0;
    uint64_t fit_flow_passed=0,fit_rejected_jitter=0,fit_rejected_image=0,fit_only=0;
    float fit_e_alt=0;uint32_t fit_photo_tested=0,fit_photo_better=0,fit_photo_worse=0,fit_photo_regions=0,fit_photo_good=0;float fit_photo_e_fit=0,fit_photo_e_cam=0;
    double ms=0;char reason[160]{};
};
Stats GetStats();
// The result is explicitly a last completed PIXEL sample, with its age; it
// never substitutes an old percentage for evidence that frames still run.
void DescribeStats(const Stats&,char* text,size_t capacity);
}
