#pragma once

// The NA1 enhancer on the card (docs/NA1-ENHANCER.md). It runs after DLSS and before sharpening on a
// Direct3D 12 command list; the DX11 route already evaluates on a D3D12 device over shared textures,
// so this one implementation gives both APIs the same picture.
//
// The picture maths lives in na1.hlsl and is kept in step by hand with the processor reference in
// core/na1_reference.hpp; tests/host/na1_test.cpp compares the two pixel by pixel.
//
// How a caller uses it, once per DLAA frame:
//
//     auto frame = na::na1::ScenePass().Prepare(device, queue, inputs, na::na1::CurrentOptions());
//     if (frame && frame->Record(list)) {
//         if (frame->Color()) { /* copy frame->Color() over the DLSS output, in this same list */ }
//         /* keep `frame` alive until the GPU has finished this list (the caller's fence ring) */
//     }
//     ... after the list has been handed to ExecuteCommandLists on `queue`:
//     if (frame) frame->Submitted(queue);
//
// When Prepare returns nothing, the plain DLAA picture goes on and Stats::reason says why.

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/na1_reference.hpp"

namespace na::na1 {

// A lighting change from one of the other parts (section 6 of the contract): a per-pixel layer in
// stops, 0 meaning no change. R16_FLOAT or R32_FLOAT, the picture's size, in
// NON_PIXEL_SHADER_RESOURCE during Record, consumed inside the same command list. A part that cannot
// trust its inputs passes no layer. A layer of the wrong size or format is left out and the reason
// line says so.
struct Delta {
    ID3D12Resource* layer = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float gpu_ms = 0;
    float mean_darkening = 0, worst_tile = 0;  // the layer's own darkening at dial 1, in stops, positive = darker
    const char* name = nullptr;
};

struct Inputs {
    // The DLSS output: NON_PIXEL_SHADER_RESOURCE during Record, never written and never transitioned.
    // One of R16G16B16A16_FLOAT, R32G32B32A32_FLOAT, R11G11B10_FLOAT, R8G8B8A8_UNORM. An 8-bit
    // picture is always treated as sRGB (stored bytes); R8G8B8A8_UNORM_SRGB is accepted when the
    // texture itself is typeless, and is then read as its stored bytes.
    ID3D12Resource* color = nullptr;
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    // Raw device depth, R32_FLOAT, the same size as the picture, NON_PIXEL_SHADER_RESOURCE.
    ID3D12Resource* depth = nullptr;
    DXGI_FORMAT depth_format = DXGI_FORMAT_R32_FLOAT;
    uint32_t width = 0, height = 0;  // the whole texture, not a part of it
    bool srgb = false;               // informational: the format decides (see above)
    bool reversed = false;           // depth 1 is near
    bool reset = false;              // camera cut or history reset: dials and scene values snap
    Delta delta[2];
    uint64_t now_ms = 0;             // a steady clock in milliseconds; 0 = not given, the system tick count is used
    float frame_ms = 0;              // the game's frame time, for the Referee's time allowance
};

struct Options { bool enabled = false; };

struct Stats {
    uint64_t recorded = 0, allocations = 0, busy = 0, samples = 0;
    bool compiling = false;
    float gpu_ms = 0;                              // NA1's own passes, from the last measurement
    float texture = 0, light = 0, colour = 0, key = 0;  // the dials the last frame was drawn with
    char reason[160] = {};
};

class Frame {
public:
    ~Frame();
    // Records NA1's compute passes. Sets its own descriptor heap, compute root signature and
    // pipeline state and does not put the caller's back. Needs a DIRECT or COMPUTE list. Never
    // compiles, allocates or waits. False means nothing was recorded.
    bool Record(ID3D12GraphicsCommandList*);
    // The enhanced picture: the same size as Inputs::color, in the same format (R8G8B8A8_UNORM for
    // an sRGB-typed input: the two copy into each other), left in NON_PIXEL_SHADER_RESOURCE.
    // Valid only inside the command list Record was called on: with a known queue the texture is
    // shared in GPU order and the next Record on that queue writes it again.
    // NULL MEANS "PICTURE UNCHANGED, COPY NOTHING": the Referee has every dial at zero and this
    // frame only measures the picture for it.
    ID3D12Resource* Color() const;
    // Tell NA1 that the command list holding this Frame's Record has been handed to
    // ExecuteCommandLists on `queue`. Call it after that ExecuteCommandLists call (straight after
    // is right; for a list the game submits itself, at the point the submission is known). Never
    // call it for a list that was thrown away. This is how the Referee's measurement comes back
    // without anything waiting: NA1 signals a fence of its own on the queue and reads its numbers
    // on a later frame, once the fence has passed. It costs one ID3D12CommandQueue::Signal, and
    // nothing at all on the frames that carry no measurement. Without this call NA1 never gets a
    // measurement, the dials stay at zero, and the reason line says so.
    void Submitted(ID3D12CommandQueue* queue);
    // True when this frame carries a measurement (so Submitted matters for it).
    bool Measures() const;
    // Tests only: the working textures of the passes, 0 yq, 1 b1 after x, 2 b1, 3 yq2, 4 b2 after x,
    // 5 b2, 6 yq4, 7 b3 after x, 8 b3 (names as in core/na1_reference.hpp Pyramid).
    ID3D12Resource* StageForTests(unsigned index) const;
private:
    friend class Pass;
    struct Work;
    std::unique_ptr<Work> work_;
    Frame();
};

class Pass {
public:
    Pass();
    ~Pass();
    // Switched off returns at once and allocates nothing. The first switched-on call prepares the
    // programs on a background worker and returns nothing until they are ready. Prepare records no
    // GPU commands. Every time it returns nothing, Stats::reason says why in plain words.
    // A known queue lets the working textures be shared in GPU order; with a null queue every
    // Frame gets its own.
    // The caller keeps the returned Frame alive until the GPU has finished the list it was recorded
    // on (its existing fence ring or recording lease).
    std::shared_ptr<Frame> Prepare(ID3D12Device*, ID3D12CommandQueue*, const Inputs&, const Options&);
    // Safe and cheap from any thread, with or without a device.
    Stats GetStats() const;
    bool WaitPrepared(uint32_t milliseconds);  // offline tests only
    // The caller could not run NA1 this frame (DLSS did not run, a debug view is up, ...).
    void Unavailable(const char* reason);
    // The existing Referee's paired thumbnails, cell for cell: the DLSS output and the presented
    // screen, both as log2(luma + 1e-4) of the stored value. Any thread; it copies the values and
    // fits the display curve before it returns (a few thousand cells; call it where the thumbnails
    // are read, not inside a game call). An HDR screen is noted and the assumed curve is used.
    void SetScreenPair(const float* output_log2, const float* screen_log2, uint32_t count, bool screen_is_sdr);
    // True once per governor change: the text for the "na1_governor" log event.
    bool TakeGovernorLine(char* out, size_t n);
    // True once per change of display curve: the text for the "na1_curve" log event.
    bool TakeCurveLine(char* out, size_t n);

    // Tests only. Pinned: these dials and scene values (display curve included) are used as they
    // are, with no glide, and the governor is not consulted. Measurements still run.
    void PinForTests(const ref::Dials&, const ref::Scene&);
    void UnpinForTests();
    // Tests only: waits until no measurement is on its way (it has been read, or none was taken).
    // The caller must have let the GPU finish first.
    bool WaitMeasuredForTests(uint32_t milliseconds);
    // Tests only: the cells of the last measurement that came back (core/na1_reference.hpp Cell).
    bool LastCellsForTests(std::vector<float>& cells) const;
private:
    struct State;
    std::shared_ptr<State> state_;  // shared with the background job that reads a measurement
};

// The one scene processor, shared by the DX12 and DX11 routes.
Pass& ScenePass();
// Reads the [na1] enabled switch every call: no restart, no change callback.
Options CurrentOptions();

}  // namespace na::na1
