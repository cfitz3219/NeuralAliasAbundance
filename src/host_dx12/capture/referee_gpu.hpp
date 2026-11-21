#pragma once
// The referee on the graphics card (stage 1 of the unify plan: it watches and writes down, it
// changes nothing). A few times a second it takes one pair of consecutive DLSS evaluations:
//   the first:  the input colour is copied (the "previous" picture of the motion check);
//   the second: input colour, motion and DLSS's output are copied;
//   its Present: the image handed to Present is copied, and on our own command list, on the
//                game's queue, small programs reduce the five images to the referee's samples
//                (core/referee.hpp), read back with no wait.
// The copies in the evaluation's list are plain copies into the referee's own textures, so the
// game's pipeline state is never touched. The evaluation side is the same for DX12 and DX11 (the
// DX11 route evaluates DLSS on a D3D12 queue): both reach it through capture/j2_capture's calls,
// whose token carries the referee's token too.
//
// Log: event `referee` every few seconds (core/referee.hpp Format), and `referee_state` when it
// starts, stops, or cannot run (with the reason).
#include <d3d12.h>
#include <cstdint>
#include <vector>
#include "core/referee_selection.hpp"
#include "core/referee_trial.hpp"

namespace na::referee { struct PlacementScope; }
namespace na::referee::gpu {

struct Evaluation {
    ID3D12Resource* color = nullptr;  D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* motion = nullptr; D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* output = nullptr; D3D12_RESOURCE_STATES output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    uint64_t present_ordinal = 0;     // the frame this evaluation belongs to
    uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
    float jitter_x = 0, jitter_y = 0; // pixels, as DLSS was told
    float mv_scale_x = 1, mv_scale_y = 1;
    bool reset = false;               // DLSS history reset: no pair across it
    bool owns_present = false;        // render-small: the output is the presented image (not judged)
    bool mv_jittered = false;
    bool mv_display_res = false;
    uint8_t scene_jitter_amount = 0; // verified against the submitted raster
    shadow::Context context;
};

using ExecuteFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

struct Presentation {
    uint64_t ordinal = 0;
    const void* swapchain = nullptr;
    bool eligible = false;
    bool submitted = false;
    bool native11 = false;
    shadow::Context alternate;
    bool await_result = false;
};

// Whether this build carries the Referee's GPU checks at all. The Home menu
// shows the switch either way and says so when they are absent.
bool Supported();
// Cheap: whether the next evaluation should be offered (a sample is due or half taken).
bool Wanted();
// A provider can reset DLSS history between scheduled samples.
void NotifyEvaluationReset();
// In the evaluation's list, after DLSS ran. Returns a token (0: nothing recorded).
uint32_t Record(ID3D12GraphicsCommandList* list, const Evaluation& e);
void Submitted(uint32_t token, ID3D12CommandQueue* queue, uint8_t tools = 0, uint8_t applied = 0);
// The list will not be submitted by us. may_still_run: someone else may still run it.
void Discard(uint32_t token, bool may_still_run);
bool NeedsScreen(Presentation frame);
void OnPresent(Presentation frame, ID3D12CommandQueue* queue, ID3D12Resource* presented, ExecuteFn exec);
void PresentResult(const void* swapchain, uint64_t ordinal, bool accepted);
bool TakeTrialSample(uint64_t ticket, TrialSample& sample);
uint8_t SceneJitterAmount(const void* swapchain, shadow::Path path, uint64_t color,
                          uint64_t depth, uint64_t generation, uint64_t frame, uint64_t screen_proof = 0);
void InvalidatePresentation(const void* swapchain);
// Consume a visual rollback decision scoped to a native DX11 screen proof.
uint64_t TakeNativeScreenLoss(const void* swapchain);

struct Stats {
    uint64_t samples = 0, pairs = 0, screens = 0, logged = 0;
    uint64_t pre = 0, main = 0, restarted = 0, discarded = 0, unpaired = 0, failed = 0;
    uint64_t trace_pairs = 0, trace_reversals = 0, rejected_presents = 0;
    uint64_t trial_pairs = 0, trial_comparable = 0;
    uint64_t parity_matched = 0, parity_recognitions = 0; // referee/parity-role-swap
    bool running = false, stopped = false;
    char why[160] = "";
};
Stats GetStats();
// VARIANT: referee/parity-role-swap
// True once the trace recognized the
// parity signature on the current presentation; consulted for admission of
// referee/proved-roles-kept by both adapters.
bool ParityRoleSwapSeen(const void* swapchain, const PlacementScope& scope);

// The last sample as read back (core/referee.hpp layout) and what it holds; for tests and tools.
struct LastSample {
    std::vector<float> data;
    bool screen = false, tiles = false, pair = false;
    float jitter_change = 0;
    uint64_t number = 0; // GetStats().samples when it was read
    uint32_t centered_layout = 0; // nonzero only if multiple regions favored the same padded mapping
    uint8_t flicker_stage = 0;
};
void EnableSampleReceiptForTests();
LastSample GetLastSample();

} // namespace na::referee::gpu
