// In-game half of the DLAA companion (see protocol.hpp). Used by a 32-bit game, where DLSS cannot
// be loaded into the game itself; a 64-bit build can also be told to use it, which is how the
// handoff is measured against DLSS running in the game.
#pragma once
#include "protocol.hpp"
#include <d3d12.h>
#include <cstddef>

namespace na::dlaa_remote {

// True when DLAA must go through the companion: a 32-bit build always; a 64-bit build only when
// the environment variable NA_DLAA_COMPANION=1 is set (tests and measurements).
bool Wanted();

struct Stats {
    bool started = false, available = false, failed = false;
    uint64_t frames = 0, refused = 0;
    // The handoff cost Cort asked to see as a number: time the game's thread spends per frame
    // between asking the companion and getting its answer, and the companion's own share of it.
    uint32_t last_wait_us = 0, max_wait_us = 0, last_companion_us = 0;
    uint64_t total_wait_us = 0;
    char reason[160] = {};
};
Stats GetStats();
int FormatStatus(char* out, size_t capacity);

// Starts the companion program (NeuralAliasDlaa64.exe beside this module, or `exe` when given),
// creates the two fences on `device` and waits for the companion to report whether DLSS can run.
// False with a reason in GetStats() when it cannot. Safe to call again after it succeeded.
bool Start(ID3D12Device* device, ID3D12CommandQueue* queue, const wchar_t* data_path,
           const wchar_t* exe = nullptr, bool copy_for_tests = false);

// Gives the companion the four textures of one frame slot. All four live on shared heaps of
// `device`. Call again whenever a slot's textures are recreated.
bool Bind(uint32_t slot, uint32_t width, uint32_t height, ID3D12Resource* color, ID3D12Resource* depth,
          ID3D12Resource* motion, ID3D12Resource* output);

// Call after the command list that finishes the slot's inputs (and leaves all four textures in
// the COMMON state) has been executed on the queue given to Start. Signals the companion's
// starting fence, asks it to run DLSS for this slot and makes the queue wait for its result, so
// work executed afterwards sees the finished output. `frame.slot` and the scalar fields must be
// filled in; `frame.value` is assigned here. True when DLSS ran. Whatever the outcome, the queue
// is never left waiting for something that cannot arrive.
bool Evaluate(Frame frame);

// Ends the companion. The caller guarantees no frame is in flight.
void Stop();

} // namespace na::dlaa_remote
