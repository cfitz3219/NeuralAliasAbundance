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

struct Pair { uint32_t cur_first = 0, prev_first = 0, verts = 0; D3D12_RECT scissor{}; uint32_t check = 0, token = 0; };
// Draws need a unique signature and buffer match in both frames. Otherwise
// two objects sharing a mesh can swap identities when draw order changes.
std::vector<Pair> Match(const std::vector<na::observe::ReplayRecord>& cur, const std::vector<na::observe::ReplayRecord>& prev,
                        uint32_t width, uint32_t height);

struct Output { ID3D12Resource* motion = nullptr; ID3D12Resource* valid = nullptr; };
struct Stats {
    uint64_t frames = 0, fed = 0, draws = 0, prev_draws = 0, matched_exact = 0, matched_loose = 0, unmatched = 0, verts = 0;
    uint64_t checked = 0; // draws from a list not yet submitted at record time, kept only if its GPU stamp matches
    uint32_t last_draws = 0, last_pairs = 0;
};

class Pass {
public:
    bool Init(ID3D12Device* device);
    bool Ready() const { return pso_[0] != nullptr; }
    // In J2's evaluation list. depth: R32 scene depth copy (NPSR). reset: no continuity with the
    // frame before (cut, resize, gap): nothing is fed. Viewport jitter is the
    // current raster offset in render pixels, absent from both sets of clip positions.
    Output Record(ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, ID3D12Resource* depth, uint32_t rw, uint32_t rh, bool reversed, bool reset,
                  const float viewport_jitter[2]);
    Stats GetStats() const { return stats_; }
private:
    bool ensure(uint32_t rw, uint32_t rh);
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_[2]; // [reversed]
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_, rtv_, dsv_;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_, valid_, zdepth_;
    static constexpr uint32_t kSets = 8;
    na::nr::native::RecordingLease descriptor_use_[kSets];
    UINT srv_stride_ = 0, rtv_stride_ = 0;
    uint32_t tw_ = 0, th_ = 0;
    Stats stats_;
};

} // namespace na::j2::replay_motion
