// NGX Vulkan DLSS Super Resolution / DLAA. CMake compiles this file whenever the
// NGX SDK headers exist (so CI checks it) and links it into the layer only when
// the NGX static library exists; otherwise vk_dlss_stub.cpp is used.
#include "host_vk/vk_dlss.hpp"

// The SDK's helpers use `= {0}` initialisers; keep its warnings out of ours.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#include <cstdio>

namespace na::vk {

namespace {
// Same custom-engine project id as our DX12 and DX11 instances.
constexpr const char* kProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
constexpr const char* kEngineVersion = "NeuralAliasAbundance";

int CreateFlagsFor(const na::bridge::FrameRecord& f)
{
    using namespace na::bridge;
    int flags = 0;
    if (f.flags & kHdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (!(f.flags & kMvDisplayRes)) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (f.flags & kMvJittered) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (f.flags & kDepthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (f.flags & kAutoExposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    return flags;
}

NVSDK_NGX_Resource_VK Res(const DlssImage& i, bool rw)
{
    VkImageSubresourceRange range{i.aspect, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(i.view, i.image, range, i.format, i.width, i.height, rw);
}
} // namespace

struct VkDlss::Impl {
    VkDevice device = VK_NULL_HANDLE;
    bool initialised = false;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    na::bridge::SessionLimits feature_limits{};
    uint32_t feature_mode = ~0u, feature_preset = ~0u;
    int feature_flags = -1;
    char reason[160] = {};

    const char* fail(const char* what, NVSDK_NGX_Result r)
    {
        std::snprintf(reason, sizeof(reason), "%s (DLSS code 0x%08X).", what, static_cast<unsigned>(r));
        return reason;
    }
    void release_feature()
    {
        if (feature) NVSDK_NGX_VULKAN_ReleaseFeature(feature);
        feature = nullptr;
        feature_mode = feature_preset = ~0u;
        feature_flags = -1;
    }
};

VkDlss::VkDlss() : impl_(std::make_unique<Impl>()) {}
VkDlss::~VkDlss() { Shutdown(); }
bool VkDlss::BuiltWithDlss() { return true; }
bool VkDlss::Ready() const { return impl_->initialised && impl_->params; }

void VkDlss::RequiredExtensions(std::vector<std::string>* instance_ext, std::vector<std::string>* device_ext)
{
    unsigned int ni = 0, nd = 0;
    const char** inst = nullptr;
    const char** dev = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &inst, &nd, &dev))) return;
    for (unsigned int i = 0; i < ni && inst; ++i) instance_ext->emplace_back(inst[i]);
    for (unsigned int i = 0; i < nd && dev; ++i) device_ext->emplace_back(dev[i]);
}

const char* VkDlss::Init(VkInstance instance, VkPhysicalDevice phys, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                         PFN_vkGetDeviceProcAddr gdpa, const wchar_t* data_path, const wchar_t* dll_dir)
{
    Impl& s = *impl_;
    Shutdown();
    if (!device || !data_path) return "DLSS start-up was called without a device or data folder.";
    NVSDK_NGX_FeatureCommonInfo info{};
    const wchar_t* paths[1] = {dll_dir};
    if (dll_dir && dll_dir[0]) {
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
    }
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, data_path,
                                                              instance, phys, device, gipa, gdpa, &info);
    if (NVSDK_NGX_FAILED(r)) return s.fail("The NVIDIA DLSS runtime did not start on this Vulkan device. Is this an RTX GPU with a current driver?", r);
    s.initialised = true;
    s.device = device;
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&s.params);
    if (NVSDK_NGX_FAILED(r) || !s.params) { const char* why = s.fail("DLSS did not report its capabilities", r); Shutdown(); return why; }
    int available = 0;
    if (NVSDK_NGX_FAILED(s.params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
        int init_result = 0;
        s.params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
        const char* why = s.fail("DLSS Super Resolution is not available on this Vulkan device", static_cast<NVSDK_NGX_Result>(init_result));
        Shutdown();
        return why;
    }
    return nullptr;
}

const char* VkDlss::Evaluate(VkCommandBuffer cmd, const DlssResources& res, const na::bridge::FrameRecord& f,
                             const na::bridge::SessionLimits& limits)
{
    Impl& s = *impl_;
    if (!s.initialised || !s.params) return "DLSS is not running on this device.";
    if (!cmd || !res.color.view || !res.depth.view || !res.motion.view || !res.output.view) return "DLSS was given a missing input.";

    const int flags = CreateFlagsFor(f);
    const bool same = s.feature && s.feature_limits.max_render_w == limits.max_render_w &&
                      s.feature_limits.max_render_h == limits.max_render_h && s.feature_limits.output_w == limits.output_w &&
                      s.feature_limits.output_h == limits.output_h && s.feature_mode == f.mode && s.feature_preset == f.preset &&
                      s.feature_flags == flags;
    if (!same) {
        s.release_feature();
        const unsigned int pr = f.preset ? f.preset : static_cast<unsigned int>(NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
        s.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, pr);
        s.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, pr);
        s.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, pr);
        s.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, pr);
        s.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, pr);
        s.params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, pr);
        NVSDK_NGX_DLSS_Create_Params c{};
        c.Feature.InWidth = limits.max_render_w;
        c.Feature.InHeight = limits.max_render_h;
        c.Feature.InTargetWidth = limits.output_w;
        c.Feature.InTargetHeight = limits.output_h;
        c.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(f.mode);
        c.InFeatureCreateFlags = flags;
        c.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(s.device, cmd, 1, 1, &s.feature, s.params, &c);
        if (NVSDK_NGX_FAILED(r) || !s.feature) { s.feature = nullptr; return s.fail("DLSS could not create its feature for this size and mode", r); }
        s.feature_limits = limits;
        s.feature_mode = f.mode;
        s.feature_preset = f.preset;
        s.feature_flags = flags;
    }

    NVSDK_NGX_Resource_VK color = Res(res.color, false), depth = Res(res.depth, false), motion = Res(res.motion, false),
                          output = Res(res.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params e{};
    e.Feature.pInColor = &color;
    e.Feature.pInOutput = &output;
    e.pInDepth = &depth;
    e.pInMotionVectors = &motion;
    e.InJitterOffsetX = f.jitter_x;
    e.InJitterOffsetY = f.jitter_y;
    e.InRenderSubrectDimensions.Width = f.render_w;
    e.InRenderSubrectDimensions.Height = f.render_h;
    e.InReset = (f.flags & na::bridge::kReset) ? 1 : 0;
    e.InMVScaleX = f.mv_scale_x;
    e.InMVScaleY = f.mv_scale_y;
    e.InPreExposure = f.pre_exposure;
    e.InExposureScale = f.exposure_scale;
    e.InFrameTimeDeltaInMsec = f.frame_ms;
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, s.feature, s.params, &e);
    if (NVSDK_NGX_FAILED(r)) return s.fail("DLSS failed on this frame", r);
    return nullptr;
}

void VkDlss::Shutdown()
{
    Impl& s = *impl_;
    s.release_feature();
    if (s.params) NVSDK_NGX_VULKAN_DestroyParameters(s.params);
    s.params = nullptr;
    if (s.initialised) NVSDK_NGX_VULKAN_Shutdown1(s.device);
    s.initialised = false;
    s.device = VK_NULL_HANDLE;
}

} // namespace na::vk
