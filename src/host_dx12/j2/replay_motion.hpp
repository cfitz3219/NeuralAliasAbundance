// Draw-replay motion (Cort 2026-11-06, SE5 sweatshirt smear). The observe layer replays every
// qualifying scene draw with stream output (producers.cpp, "Draw replay"), so each frame holds the
// clip position of every vertex the game drew. Here each draw is matched to its twin of the frame
// before (same pipeline, vertex shader, counts; same vertex buffers when they can tell identical
// meshes apart) and both positions are rasterised together: v = p_previous - p_current in render
// pixels (+y down) on the pixels where that draw is the visible surface (its depth matches the scene
// depth). That is exact motion for animated characters, which camera reprojection and optical flow
// cannot give. Output: the arbiter's tier 1 (RG32 motion, R32 mask), NPSR, top-left active extent.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstddef>
#include <vector>
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"

namespace na::j2::replay_motion {

struct Pair { uint32_t cur_first = 0, prev_first = 0, verts = 0; D3D12_RECT scissor{}; uint32_t check = 0, token = 0, prev_check = 0, prev_token = 0;
              uint64_t sig = 0, vb = 0; }; // the draw's identity, for what the pass learns about it
// Draws need a unique signature and buffer match in both frames. Otherwise
// two objects sharing a mesh can swap identities when draw order changes.
std::vector<Pair> Match(const std::vector<na::observe::ReplayRecord>& cur, const std::vector<na::observe::ReplayRecord>& prev,
                        uint32_t width, uint32_t height);

struct Output { ID3D12Resource* motion = nullptr; ID3D12Resource* valid = nullptr; };
// The screen's own motion, to learn which draws move by themselves (core/replay_movers.hpp): the
// pass counts per draw, on a 1-in-16 grid, the pixels where the draw is the visible surface and
// those of them whose replayed motion is more than apart_px + apart_rel * |screen motion| from the
// screen's motion there, and hands the counts to na::observe::ReplayLearn once the GPU has run the
// list. Mode 0: nothing to compare with this frame, nothing is counted and nothing is learned.
struct Reference {
    uint32_t mode = 0;               // 1 the camera, 2 the fitted screen motion (core/screen_motion.hpp)
    float matrix[16] = {};           // 1: current clip -> previous clip, row-major, row-vector (the arbiter's composed matrix)
    ID3D12Resource* fix = nullptr;   // 1: the arbiter's optional M3 correction buffer (NON_PIXEL_SHADER_RESOURCE)
    ID3D12Resource* model = nullptr; // 2: compute_flow's Tier3::screen_model (RG32F 8x1, NON_PIXEL_SHADER_RESOURCE)
    float depth_far = 1.0f;          // the depth value at the far end (the arbiter's depth_clear)
    float apart_px = 1.0f, apart_rel = 0.1f;
};
struct Stats {
    uint64_t frames = 0, fed = 0, draws = 0, prev_draws = 0, matched_exact = 0, matched_loose = 0, unmatched = 0, verts = 0;
    uint64_t checked = 0; // draws from a list not yet submitted at record time, kept only if its GPU stamp matches
    // Read back from the GPU (Sol: CPU counts can rise with an empty mask): pixels on a
    // 1-in-64 grid (x%8==0, y%8==0) that got replay motion, per recorded pass.
    uint64_t gpu_read = 0, gpu_nonzero = 0, gpu_samples = 0; uint32_t gpu_last = 0;
    uint32_t last_draws = 0, last_pairs = 0;
    // Learning (Reference): recorded passes by what they compared with, passes whose counts were
    // read back, and those counts summed (sampled pixels seen, and of them apart from the screen).
    uint64_t learn_camera = 0, learn_screen = 0, learn_none = 0, learn_read = 0, learn_visible = 0, learn_apart = 0;
};

class Pass {
public:
    bool Init(ID3D12Device* device);
    bool Ready() const { return pso_[0] != nullptr; }
    // In J2's evaluation list. depth: R32 scene depth copy (NPSR). reset: no continuity with the
    // frame before (cut, resize, gap): nothing is fed. Viewport jitter is the
    // current raster offset in render pixels, absent from both sets of clip positions.
    Output Record(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, ID3D12Resource* depth, uint32_t rw, uint32_t rh, bool reversed, bool reset,
                  const float viewport_jitter[2], const Reference* reference = nullptr);
    Stats GetStats() const { return stats_; }
private:
    bool ensure(uint32_t rw, uint32_t rh);
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_[2]; // [reversed]
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_, rtv_, dsv_;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_, valid_, zdepth_;
    static constexpr uint32_t kSets = 8;
    static constexpr uint32_t kLearn = 2040;                   // draws of one pass that get counters
    static constexpr uint32_t kCountBytes = 8 + kLearn * 8;    // word 0 the 1-in-64 sample count; from byte 8 two words per draw
    struct Learned { uint64_t sig = 0, vb = 0; };
    std::vector<Learned> learned_[kSets];                      // the draws whose counters a set holds, in counter order
    na::nr::native::RecordingLease descriptor_use_[kSets];
    Microsoft::WRL::ComPtr<ID3D12Resource> count_[kSets], readback_[kSets], zeros_;
    bool pending_read_[kSets] = {};
    void harvest();
    UINT srv_stride_ = 0, rtv_stride_ = 0;
    uint32_t tw_ = 0, th_ = 0;
    Stats stats_;
};

} // namespace na::j2::replay_motion
