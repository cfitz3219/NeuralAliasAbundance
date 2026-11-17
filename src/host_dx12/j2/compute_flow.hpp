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
    float jitter[2]{}; // nominal on input; Ticket.frame carries the calibrated encode shift
    float raster_jitter[2]{}; bool raster_sample=false;
    bool raster_confirmed=false; // exact viewport writes reached this source recording; not a learned convention
    uint64_t source_generation=0;
    // false: only the M3 camera fit runs (the camera needs reconstructing but the user turned
    // the object-motion residual off); no residual, no tier 3.
    bool residual=true;
    bool camera_reconstruct=false; // the caller found the camera matrix carries no view (status only)
    // VARIANT: motion/measured-total
    // No camera for this recording. Nothing camera-derived is read: no matrices, no fit, no residual,
    // none of the residual gates and not the looseness slider (of the motion::gpu::Inputs passed to
    // Record only depth_clear is used, and that comes from how the game drew its depth). The pair's
    // motion is measured on the two pictures (flow_shaders.hpp `measure`, core/total_motion.hpp)
    // and handed on as TOTAL motion in Tier3::total / total_valid. `residual` is ignored.
    bool total=false;
    // total only: the newest frame already submitted with a DLSS reset (0 = none yet). A scene cut
    // measured on a later pair than that makes Record ask for a reset through Ticket::cut_reset.
    uint64_t last_reset_frame=0;
};
struct Ticket {std::shared_ptr<void> context;uint32_t image=0,previous=0,slot=0;Frame frame;bool pair=false;uint64_t colour_epoch=0;uint32_t colour_convention=0;
    // Frame::total: a scene cut was measured on a pair after Frame::last_reset_frame, read back
    // without waiting on the GPU: THIS frame must carry the DLSS reset (the caller sets it).
    bool cut_reset=false;
    // Why there is no pair (0 = there is one): 1 the caller asked for a new history, 2 no picture
    // yet, 3 the last picture is not the frame before, 4 another source generation, 5 the colour's
    // jitter convention was relearned, 6 the raster facts changed kind, 7 the last picture's
    // recording has not been submitted.
    uint8_t no_pair=0;};
// fix: the M3 camera correction buffer of this recording (camera_fit_gpu.hpp), or nullptr. The
// residual was derived against the corrected camera, so the arbiter must use the same fix.
struct Tier3 {ID3D12Resource *residual=nullptr,*valid=nullptr,*mask=nullptr,*fix=nullptr;
    // Frame::total only; null unless this recording measured a pair. TOTAL motion of the pair,
    // measured from the two pictures with no camera: v = p_previous - p_current in render pixels,
    // +x right, +y down, in the unjittered frame at raster pixel centres (the arbiter's convention
    // for a tier-1 supplier, to be passed with Tier1Source::Estimated; the same quantity as camera
    // + residual, never a residual). `total_valid` per pixel: 1 measured (moving, or still with the
    // pictures confirming it), 0.5 the block search's vector handed on because the pictures could
    // not confirm it (flat area, newly uncovered, from outside the frame), 0 nothing known: there
    // `total` is 0 and that zero is not "still". On a scene cut every pixel is 0 / 0.
    // RG32_FLOAT / R32_FLOAT, NON_PIXEL_SHADER_RESOURCE.
    ID3D12Resource *total=nullptr,*total_valid=nullptr;};
// Called inside J2's restorable inline scope, AFTER its colour/depth resolve.
// All inputs NPSR. No CPU waits; allocations/retirement reuse RecordingLease.
Ticket Record(ID3D12GraphicsCommandList*,ID3D12CommandQueue*,ID3D12Resource* color,ID3D12Resource* depth,
              const na::motion::gpu::Inputs&,const Frame&,Tier3&);
void Committed(const Ticket&); // only after J2's actual submission, never on record
// Frame::total, after DLSS, when the ticket has a pair. While a measured scene cut waits for its
// DLSS reset to be submitted, the temporal output still holds the old scene: on those frames
// `output` (UNORDERED_ACCESS) is overwritten with `colour` (NON_PIXEL_SHADER_RESOURCE, the picture
// DLSS was given). The decision is made on the GPU in execution order, with no CPU wait. May be
// recorded on another list than Record's, submitted after it on the same queue.
void RecordCutShow(ID3D12GraphicsCommandList*,const Ticket&,ID3D12Resource* colour,ID3D12Resource* output);
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
    uint32_t colour_jitter_convention=0;uint64_t colour_jitter_epoch=0,colour_jitter_changes=0;
    // Every completed pair's pixel counts summed (not one sample per 5 s): accepted, invalid,
    // small, cost, offscreen, image, flicker; and the pairs counted.
    uint64_t sum[7]{}; uint64_t sum_pairs=0;
    // Frame::total recordings (no camera): recordings asked for, pairs measured, and every completed
    // pair's pixel counts by the measure program's code (0 measured moving, 1 nothing known, 4 the
    // search's vector from outside the frame, 5 the search's vector unconfirmed, 7 measured still;
    // 2, 3 and 6 are not used). Kept apart from `sum`: those count residuals against a camera.
    uint64_t total_frames=0,total_pairs=0,total_sum[8]{},total_sum_pairs=0;
    bool total_mode=false; // the latest recording was a Frame::total one (the last sample's counts are then the measure codes)
    // Scene cuts measured (AMD's whole-frame scene change on a total pair), the newest one's frame,
    // and resets asked for through Ticket::cut_reset.
    uint64_t total_cuts=0,total_cut_frame=0,total_cut_resets=0;
    // Recordings without a pair, by Ticket::no_pair's code (index 0 unused).
    uint64_t no_pair[8]{};
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
    uint32_t stabilized_format=0; // DXGI_FORMAT of the actual motion-history pictures, not the game's input
};
Stats GetStats();
// The result is explicitly a last completed PIXEL sample, with its age; it
// never substitutes an old percentage for evidence that frames still run.
void DescribeStats(const Stats&,char* text,size_t capacity);
}
