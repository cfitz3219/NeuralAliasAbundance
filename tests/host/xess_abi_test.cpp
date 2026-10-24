// C7b: our XeSS ABI declarations (xess_abi.hpp) must match Intel's official
// public headers byte for byte. Built only where the headers are available
// (fetched at build time in CI, or <NA_SDK_ROOT>/xess locally); never vendored.
// Everything is checked at compile time; the program only reports success.
#include "host_dx12/upscalers/xess_abi.hpp"

#include <xess/xess_d3d12.h>

#include <cstddef>
#include <cstdio>

namespace a = na::xess::abi;

#define SAME_SIZE(ours, theirs) static_assert(sizeof(ours) == sizeof(theirs), #ours " size differs from " #theirs)
#define SAME_OFFSET(ours, theirs, field) \
    static_assert(offsetof(ours, field) == offsetof(theirs, field), #field " offset differs")

SAME_SIZE(a::Vec2u, xess_2d_t);
SAME_SIZE(a::Result, xess_result_t);
SAME_SIZE(a::Quality, xess_quality_settings_t);

SAME_SIZE(a::D3D12InitParams, xess_d3d12_init_params_t);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, outputResolution);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, qualitySetting);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, initFlags);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, creationNodeMask);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, visibleNodeMask);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, pTempBufferHeap);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, bufferHeapOffset);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, pTempTextureHeap);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, textureHeapOffset);
SAME_OFFSET(a::D3D12InitParams, xess_d3d12_init_params_t, pPipelineLibrary);

SAME_SIZE(a::D3D12ExecuteParams, xess_d3d12_execute_params_t);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, pColorTexture);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, pVelocityTexture);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, pDepthTexture);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, pExposureScaleTexture);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, pResponsivePixelMaskTexture);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, pOutputTexture);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, jitterOffsetX);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, jitterOffsetY);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, exposureScale);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, resetHistory);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, inputWidth);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, inputHeight);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, inputColorBase);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, inputMotionVectorBase);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, inputDepthBase);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, inputResponsiveMaskBase);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, reserved0);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, outputColorBase);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, pDescriptorHeap);
SAME_OFFSET(a::D3D12ExecuteParams, xess_d3d12_execute_params_t, descriptorHeapOffset);

static_assert(a::kSuccess == XESS_RESULT_SUCCESS, "result");
static_assert(a::kErrorInvalidArgument == XESS_RESULT_ERROR_INVALID_ARGUMENT, "result");
static_assert(a::kErrorInvalidContext == XESS_RESULT_ERROR_INVALID_CONTEXT, "result");
static_assert(a::kUltraPerformance == XESS_QUALITY_SETTING_ULTRA_PERFORMANCE && a::kPerformance == XESS_QUALITY_SETTING_PERFORMANCE &&
                  a::kBalanced == XESS_QUALITY_SETTING_BALANCED && a::kQuality == XESS_QUALITY_SETTING_QUALITY &&
                  a::kUltraQuality == XESS_QUALITY_SETTING_ULTRA_QUALITY && a::kUltraQualityPlus == XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS &&
                  a::kNativeAA == XESS_QUALITY_SETTING_AA,
              "quality values");
static_assert(a::kHighResMv == XESS_INIT_FLAG_HIGH_RES_MV && a::kInvertedDepth == XESS_INIT_FLAG_INVERTED_DEPTH &&
                  a::kExposureScaleTexture == XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE &&
                  a::kResponsivePixelMask == XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK && a::kUseNdcVelocity == XESS_INIT_FLAG_USE_NDC_VELOCITY &&
                  a::kExternalDescriptorHeap == XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP && a::kLdrInputColor == XESS_INIT_FLAG_LDR_INPUT_COLOR &&
                  a::kJitteredMv == XESS_INIT_FLAG_JITTERED_MV && a::kEnableAutoExposure == XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE,
              "init flags");

int main()
{
    std::printf("xess_abi_test: ok (sizes, offsets, enums and flags match Intel's headers)\n");
    return 0;
}
