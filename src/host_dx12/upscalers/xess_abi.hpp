// C7b: our own declarations of the XeSS D3D12 API surface that a game calls in
// libxess.dll, written from Intel's public API documentation. Intel's SDK
// headers are NOT vendored (their licence covers binary redistribution); the
// CI job that fetches the public headers at build time checks that every size
// and offset below matches them exactly (tests/host/xess_abi_test.cpp).
// Calling convention: the platform default (x64 has one).
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace na::xess::abi {

using Result = int32_t; // xess_result_t
constexpr Result kSuccess = 0;
constexpr Result kErrorInvalidArgument = -4;
constexpr Result kErrorInvalidContext = -8;

using Context = struct OpaqueContext*; // xess_context_handle_t

#pragma pack(push, 8)
struct Vec2u { // xess_2d_t / xess_coord_t
    uint32_t x;
    uint32_t y;
};

enum Quality : int32_t { // xess_quality_settings_t
    kUltraPerformance = 100,
    kPerformance = 101,
    kBalanced = 102,
    kQuality = 103,
    kUltraQuality = 104,
    kUltraQualityPlus = 105,
    kNativeAA = 106,
};

enum InitFlags : uint32_t { // xess_init_flags_t
    kHighResMv = 1u << 0,
    kInvertedDepth = 1u << 1,
    kExposureScaleTexture = 1u << 2,
    kResponsivePixelMask = 1u << 3,
    kUseNdcVelocity = 1u << 4,
    kExternalDescriptorHeap = 1u << 5,
    kLdrInputColor = 1u << 6,
    kJitteredMv = 1u << 7,
    kEnableAutoExposure = 1u << 8,
};

struct D3D12InitParams { // xess_d3d12_init_params_t
    Vec2u outputResolution;
    Quality qualitySetting;
    uint32_t initFlags;
    uint32_t creationNodeMask;
    uint32_t visibleNodeMask;
    ID3D12Heap* pTempBufferHeap;
    uint64_t bufferHeapOffset;
    ID3D12Heap* pTempTextureHeap;
    uint64_t textureHeapOffset;
    ID3D12PipelineLibrary* pPipelineLibrary;
};

struct D3D12ExecuteParams { // xess_d3d12_execute_params_t
    ID3D12Resource* pColorTexture;
    ID3D12Resource* pVelocityTexture;
    ID3D12Resource* pDepthTexture;
    ID3D12Resource* pExposureScaleTexture;
    ID3D12Resource* pResponsivePixelMaskTexture;
    ID3D12Resource* pOutputTexture;
    float jitterOffsetX;
    float jitterOffsetY;
    float exposureScale;
    uint32_t resetHistory;
    uint32_t inputWidth;
    uint32_t inputHeight;
    Vec2u inputColorBase;
    Vec2u inputMotionVectorBase;
    Vec2u inputDepthBase;
    Vec2u inputResponsiveMaskBase;
    Vec2u reserved0;
    Vec2u outputColorBase;
    ID3D12DescriptorHeap* pDescriptorHeap;
    uint32_t descriptorHeapOffset;
};
#pragma pack(pop)

// libxess.dll exports intercepted by C7b.
using PfnD3D12CreateContext = Result (*)(ID3D12Device*, Context*);
using PfnD3D12Init = Result (*)(Context, const D3D12InitParams*);
using PfnD3D12Execute = Result (*)(Context, ID3D12GraphicsCommandList*, const D3D12ExecuteParams*);
using PfnDestroyContext = Result (*)(Context);
using PfnSetScale = Result (*)(Context, float, float);   // xessSetJitterScale / xessSetVelocityScale
using PfnSetExposureMultiplier = Result (*)(Context, float);

} // namespace na::xess::abi
