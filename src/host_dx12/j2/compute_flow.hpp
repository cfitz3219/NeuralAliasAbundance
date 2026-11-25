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
    // The picture was shaded once per block of this many pixels (core/fold_shading.hpp). The encode
    // then averages over one block, so the block grid, which stays put on the screen while the sample
    // moves, is not read as movement.
    uint8_t block_x=1,block_y=1;
    // total only. Sampled measuring (a folded DLSS level on screen, nothing else needing the whole
    // field): the search still runs on the whole picture, but `measure` decides only the pixel (pick)
    // of each sample_x x sample_y block, at its own full-size position in Tier3::total / total_valid;
    // the other pixels of those textures are left as they were (stale). 1 x 1: every pixel.
    uint8_t sample_x=1,sample_y=1,sample_pick_x=0,sample_pick_y=0;
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
    bool use_screen_fit=true; // local to this recording; direct measurements still run
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
    // + residual, never a residual). `total_valid` per pixel: 1 measured (moving by itself, or with
    // the screen, the pictures confirming it), 0.5 the screen's own motion where the pictures could
    // not measure (flat area, newly uncovered; core/screen_motion.hpp), 0 nothing known: there
    // `total` is 0 and that zero is not "still". The block search's raw vector is never handed on.
    // On a scene cut every pixel is 0 / 0.
    // RG32_FLOAT / R32_FLOAT, NON_PIXEL_SHADER_RESOURCE.
    ID3D12Resource *total=nullptr,*total_valid=nullptr;
    // Frame::total with the screen fit: the fitted screen motion `measure` followed for this pair
    // (flow_shaders.hpp fit_solve's model: RG32_FLOAT 8x1, texels 0..5 the eleven terms and the
    // nearness limit, 6.x valid; NON_PIXEL_SHADER_RESOURCE). Null when the fit did not run. The draw
    // replay compares its draws with it (core/replay_movers.hpp).
    ID3D12Resource *screen_model=nullptr;};
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
// VARIANT: flow/frame-candidates
// On by default. The block search's grid is reconsidered against the frame's own most common
// vectors before anything reads it (core/flow_candidates.hpp); off, the grid is used as the search
// left it. For the GPU fixtures' with/without comparison; no setting reaches it. Read at each Record.
void SetFrameCandidates(bool on);
// VARIANT: motion/screen-fit
// On by default. Frame::total only: the screen's own motion is fitted to the block centres the
// pictures can measure, and `measure` follows it where they cannot (core/screen_motion.hpp). Off,
// such pixels are not known (validity 0). For the GPU fixtures' with/without comparison; no setting
// reaches it. Read at each Record.
void SetScreenFit(bool on);
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
    // pair's pixel counts by the measure program's code (0 measured, moving by itself; 1 nothing
    // known; 2 not measurable, follows the screen's motion; 7 measured, moving with the screen, which
    // is exactly still when there is no model; 4 and 5, the search's vector handed on, are no longer
    // written; 3 and 6 are not used). Kept apart from `sum`: those count residuals against a camera.
    uint64_t total_frames=0,total_pairs=0,total_sum[8]{},total_sum_pairs=0;
    uint64_t sampled_pairs=0; // total pairs whose `measure` decided only the pixels a folded level keeps (Frame::sample_x)
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
    // VARIANT flow/frame-candidates (core/flow_candidates.hpp), over completed pairs: pairs the pass ran
    // on, pairs where a block took a candidate, blocks that had a candidate to compare, blocks that
    // took one. The newest completed pair: its frame, its candidates strongest first, packed as
    // flow_candidates.hpp Pack (support << 16 | key; 0 = none), and its two block counts.
    bool cand_on=false; // the latest recording ran the pass
    // VARIANT motion/screen-fit (core/screen_motion.hpp), over completed Frame::total pairs: pairs
    // the fit ran on, pairs with a model, and the pairs without one by reason (fewer confirmed samples
    // than the fit needs; a step could not be solved; fewer than half the samples agree with it).
    // The newest completed pair: its frame, the eleven terms, the nearest depth distance it answers
    // for, whether it is a model, its confirmed samples, those agreeing, their mean robust error
    // (pixels squared), and the steps solved.
    bool screen_on=false; // the latest Frame::total recording ran the fit
    uint64_t screen_pairs=0,screen_valid=0,screen_few=0,screen_unsolved=0,screen_disagree=0,screen_frame=0;
    float screen_theta[11]{},screen_nearness_limit=0,screen_samples=0,screen_agree=0,screen_energy=0;
    bool screen_last_valid=false,screen_last_unsolved=false;uint32_t screen_steps=0;
    // VARIANT motion/screen-fit/depth-layers: pairs whose samples' depths fell into two groups (each
    // then has its own fit; the numbers above are the far group's), and those whose near group has a
    // model. The newest pair: the depth distance that parts the groups (0: one group, because the
    // depths run on without an empty stretch or a side holds too few samples) and the near group's fit.
    uint64_t screen_split=0,screen_near_valid=0;
    float screen_split_nearness=0,screen_near_theta[11]{},screen_near_samples=0,screen_near_agree=0,screen_near_energy=0;
    bool screen_near_last_valid=false;uint32_t screen_near_steps=0;
    uint64_t cand_pairs=0,cand_pairs_changed=0,cand_offered=0,cand_changed=0,cand_frame=0;
    uint32_t cand_top[8]{},cand_last_offered=0,cand_last_changed=0;
    double ms=0;char reason[160]{};
    uint32_t stabilized_format=0; // DXGI_FORMAT of the actual motion-history pictures, not the game's input
};
Stats GetStats();
// The result is explicitly a last completed PIXEL sample, with its age; it
// never substitutes an old percentage for evidence that frames still run.
void DescribeStats(const Stats&,char* text,size_t capacity);
}
