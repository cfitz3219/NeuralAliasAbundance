// M3: rigid camera-motion correction fitted on the GPU in the same frame (2026-11-04).
//
// Cort's SE5 05dc62b run: the camera term of our motion vectors was ~0 while the player
// walked (the image translated by up to 11 px, depth-scaled); the selected matrix lacked the
// camera translation. Rather than hunting a per-game origin, the camera motion between the
// exact image pair the consumer uses is estimated from depth + reliable optical flow on
// static background (established pose estimation: back-projected depth + 2D
// correspondences + robust least squares, cf. OpenCV solvePnP/RANSAC refinement):
//   * six parameters (translation x/y/z, small rotation about the eye) as a correction of
//     the existing composed reprojection C (FitBasis, motion_arbiter_gpu.hpp), never added
//     twice: theta = 0 is exactly today's camera motion;
//   * IRLS Gauss-Newton with a Cauchy weight (1.5 px), Jacobi-scaled and damped, 6 steps
//     from theta = 0; flow samples at 8x8 block centres (the AMD engine's grid);
//   * HUD, sky (clear depth), moving objects and bad matches are down-weighted by the robust
//     weight or excluded by validity; the expansion's depth-aware validity is required;
//   * still guard: the correction is used only if the mean robust error at theta = 0 is
//     above 0.05 (a still scene has nothing to fix) AND the fit lowers it by >= 30 % over at
//     least 256 samples; otherwise the buffer says invalid and tier 2 stays unchanged;
//   * jitter-convention guard (Cort's 0ce11a8 run, 2026-11-04): the flow pair is stabilised with
//     the declared jitter; when the colour carries none, the flow shows the jitter delta dj as a
//     whole-screen ~1 px shift that is not motion. Besides the plain test, the fit must pass the
//     same test with a half-pixel dead zone (AMD flow is integral) against the better of the two
//     camera hypotheses (camera, camera + dj), so a fit that only explains dj is rejected;
//   * image check (FitImages given): the fit uses the even cells of the 8x8 grid; on the odd
//     cells the pair's images must prefer the fitted vector over the best camera hypothesis
//     (5x5 luma error), in most samples and in at least 2/3 of the 3x3 screen regions with
//     evidence, so a large moving foreground cannot become the camera (Astra's correction);
//   * everything stays on the GPU in the recording list: no CPU read of this frame's result,
//     no last-frame transform. The result is read back only for diagnostics (Collect).
// Output buffer (256 bytes, raw): words 0..15 G, 16 valid, 17..22 theta, 23 E0, 24 n0, 25 E,
// 26 n, 27 accepted (final), 28 flow test passed, 29 quantised reference min(55, 56),
// 30 image cells tested, 31 better, 32 worse, 33/34 summed image error fit/camera (x4096),
// 35..43 better per region, 44..52 worse per region, 53 regions with evidence, 54 good regions,
// 55 quantised E0 (camera), 56 quantised E0 (camera + dj), 57 quantised E at the fit.
#pragma once

#include "core/motion_arbiter_gpu.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace na::motion::gpu {

struct FitResult {
    bool accepted = false;
    float theta[6] = {};
    float energy0 = 0, energy = 0; // mean robust error at theta = 0 / at the fit
    uint32_t samples0 = 0, samples = 0;
    bool flow_accepted = false;  // passed the flow test (before the image check)
    float energy_alt = 0;        // quantised mean robust error at theta = 0 with the jitter-delta hypothesis
    float energy_q0 = 0, energy_qfit = 0; // quantised (half-pixel dead zone) at theta = 0 / at the fit
    uint32_t photo_tested = 0, photo_better = 0, photo_worse = 0, photo_regions = 0, photo_regions_good = 0;
    float photo_e_fit = 0, photo_e_cam = 0; // mean image error on the tested cells
};

// The pair's encoded images (R8G8B8A8_UNORM, w x h, NON_PIXEL_SHADER_RESOURCE) for the image
// check, and the stabilisation jitter delta (current - previous, pixels) used by the encode.
struct FitImages {
    ID3D12Resource* current = nullptr;
    ID3D12Resource* previous = nullptr;
    float jitter_delta_px[2] = {};
};

class CameraFit {
public:
    bool Init(ID3D12Device* device);
    // Records the fit for recording slot `slot` (the caller reuses a slot only after its GPU
    // work completed). depth R32_FLOAT, flow RG32_FLOAT (current -> previous, pixels,
    // jitter-stabilised), flow_valid R32_FLOAT: all NON_PIXEL_SHADER_RESOURCE, w x h.
    // Returns the slot's fix buffer, in NON_PIXEL_SHADER_RESOURCE, or nullptr (nothing recorded).
    ID3D12Resource* Record(ID3D12GraphicsCommandList* list, uint32_t slot, ID3D12Resource* depth, ID3D12Resource* flow,
                           ID3D12Resource* flow_valid, const FitBasis& basis, const float jitter_px[2], float depth_clear,
                           uint32_t width, uint32_t height, const FitImages* images = nullptr);
    // Diagnostics of the slot's last recording; only after that recording's GPU work completed.
    bool Collect(uint32_t slot, FitResult* out);
    bool Ready() const { return pso_accum_ != nullptr; }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> upload, fix, part, readback;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        uint32_t groups = 0;
        bool recorded = false;
    };
    bool prepare(Slot& s, uint32_t groups);
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_accum_, pso_solve_, pso_validate_, pso_decide_;
    std::vector<Slot> slots_;
    UINT stride_ = 0;
};

} // namespace na::motion::gpu
