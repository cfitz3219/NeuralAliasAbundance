// The Referee as a witness on Route A: a game that runs its own DLSS.
// (Gap audit 2026-11-21 item 12.) The game evaluates DLSS in its own command
// list. Right after a successful evaluation the Referee's existing capture is
// recorded into that same list; it counts only once the game has submitted the
// list, and it is judged against the present that follows. Witness only: the
// existing picture, result, motion and jitter verdicts are logged. No trial is
// started and nothing in the game's frame is changed.
//
// Used only while no other route owns the frame's present receipt (the added
// DLAA routes own it themselves), so two routes never feed one sample.
#pragma once
#include "host_dx12/capture/referee_gpu.hpp"

namespace na::referee::route_a {

struct Evaluated {
    uint64_t feature = 0; // the game's DLSS feature: its history is the temporal sequence
    ID3D12Resource* color = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;
    uint32_t render_w = 0, render_h = 0, output_w = 0, output_h = 0;
    float jitter_x = 0, jitter_y = 0, mv_scale_x = 1, mv_scale_y = 1;
    bool reset = false, mv_jittered = false, mv_low_res = false;
};

// The game's DLSS evaluation returned success in `list`.
void OnEvaluated(ID3D12GraphicsCommandList* list, const Evaluated& evaluated);
// The game's lists were handed to the queue.
void OnExecuted(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists);
// A present of the game's swap chain is closing. Fills the receipt when no
// other route did (ordinal 0) and this route saw the game's DLSS.
void OnPresent(gpu::Presentation& receipt);

struct Stats {
    uint64_t evaluations = 0, recorded = 0, submitted = 0, presents = 0;
    uint64_t other_frame = 0; // recorded in one frame, submitted in another: not judged
    bool active = false; // this route owns the present receipts
};
Stats GetStats();

} // namespace na::referee::route_a
