// MotionArbiter GPU port (M-001, A34). A D3D12 compute pass that runs the
// SAME arbitration as the CPU reference in motion_arbiter.cpp, per pixel,
// with the same conventions, rejection rules, provenance bits and stats.
//
// Contract with the CPU reference (validated by tests/motion_arbiter_gpu_test):
//   |Î”v| â‰¤ 1e-3 px per component, provenance identical, stats identical.
//   Bit-exactness of the float math is NOT claimed (different fma/ordering).
//
// Resource conventions (all render-resolution, extent == output extent):
//   tier1 motion       RG32_FLOAT   SRV   (optional; flag TIER1)
//   tier1 valid mask   R32_FLOAT    SRV   (optional; flag TIER1_MASK)
//   depth              R32_FLOAT    SRV   (optional; flag TIER2) raw z/w
//   tier3 residual     RG32_FLOAT   SRV   (optional; flag TIER3)
//   tier3 mask         R32_FLOAT    SRV   (optional; flag TIER3_MASK)
//   tier3 valid mask   R32_FLOAT    SRV   (optional; flag TIER3_VALID)
//   output xy          RG32_FLOAT   UAV
//   output provenance  R8_UINT      UAV
//   stats              32-byte raw UAV buffer of 8 uint32 counters (optional)
// All SRVs must be in NON_PIXEL_SHADER_RESOURCE, UAVs in UNORDERED_ACCESS,
// when Dispatch records. The caller owns every resource and all barriers
// before/after; Dispatch records only the dispatch (no waits, no readback).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "core/motion_arbiter.hpp"

namespace na::motion::gpu {

struct Inputs {
    ID3D12Resource* tier1_motion = nullptr;
    ID3D12Resource* tier1_valid = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* tier3_residual = nullptr;
    ID3D12Resource* tier3_mask = nullptr;
    ID3D12Resource* tier3_valid = nullptr;
    // Same meaning as the CPU structs; `valid` flags decide which tiers run.
    bool tier1_valid_flag = false;
    bool tier2_valid_flag = false;
    bool tier3_valid_flag = false;
    float depth_clear = 1.0f;
    Mat4 inv_view_proj_current{};
    Mat4 view_proj_previous{};
    float jitter_current_px[2] = {0, 0};
    Config config{};
};

struct Outputs {
    ID3D12Resource* xy = nullptr;          // RG32_FLOAT, UAV
    ID3D12Resource* provenance = nullptr;  // R8_UINT, UAV
    ID3D12Resource* stats = nullptr;       // optional raw buffer, >= 32 bytes, UAV, zeroed by caller
    uint32_t width = 0, height = 0;
};

// Order of the 8 counters in the stats buffer (uint32 each).
enum StatSlot : uint32_t {
    STAT_ENGINE = 0, STAT_CAMERA = 1, STAT_RESIDUAL_ADDED = 2, STAT_RESIDUAL_SNAPPED = 3,
    STAT_RESIDUAL_CLAMPED = 4, STAT_OFFSCREEN = 5, STAT_INVALID = 6, STAT_REJECTED = 7
};

class Arbiter {
public:
    // Compiles the shader (d3dcompiler_47) and creates the pipeline. Returns
    // false on any failure; the object is then unusable.
    bool Init(ID3D12Device* device, uint32_t max_frames_in_flight = 4);
    // Records one dispatch on `list`. `frame_slot` selects a descriptor
    // block so up to max_frames_in_flight dispatches can be in flight.
    // Returns false for a null/invalid input (nothing recorded).
    bool Dispatch(ID3D12GraphicsCommandList* list, const Inputs& in, const Outputs& out, uint32_t frame_slot);
    // DeriveResidual on the GPU (mirrors na::motion::DeriveResidual): for
    // suppliers with RAW image flow measured on jittered frames,
    //   residual = raw_flow - V_cam - (jitter_prev - jitter_cur),
    // valid = 1 where the camera vector exists, the flow is finite AND (when
    // `raw_valid` is given) the supplier's/expansion's validity at the pixel
    // is in the mask domain's valid range (finite (0,1]). This is how
    // expansion validity and supplier validity are folded into ONE tier-3
    // validity: a merely finite raw vector is never trusted on its own.
    // `raw_flow` RG32_FLOAT SRV, `depth` R32_FLOAT SRV, `raw_valid` R32_FLOAT
    // SRV (optional), `residual_out` RG32_FLOAT UAV, `valid_out` R32_FLOAT
    // UAV, all of extent widthÃ—height.
    bool DispatchDeriveResidual(ID3D12GraphicsCommandList* list, ID3D12Resource* raw_flow, ID3D12Resource* depth,
                                const Inputs& camera, const float jitter_previous_px[2], ID3D12Resource* residual_out,
                                ID3D12Resource* valid_out, uint32_t width, uint32_t height, uint32_t frame_slot,
                                ID3D12Resource* raw_valid = nullptr);
    // Converts a read-back stats buffer (from the most recent Dispatch) into
    // the shared Stats struct, adding whole-tier extent rejections that were
    // decided on the CPU side.
    Stats DecodeStats(const uint32_t counters[8]) const;
    ID3D12Device* Device() const { return device_.Get(); }

private:
    uint64_t whole_rejects_last_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_, pso_derive_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dummy_;   // 1x1 R32_FLOAT for absent inputs
    Microsoft::WRL::ComPtr<ID3D12Resource> dummy2_;  // 1x1 RG32_FLOAT
    Microsoft::WRL::ComPtr<ID3D12Resource> dummy_stats_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dummy_uav_;   // 1x1 R32_FLOAT UAV for unused slots
    UINT stride_ = 0;
    uint32_t slots_ = 0;
};

} // namespace na::motion::gpu
