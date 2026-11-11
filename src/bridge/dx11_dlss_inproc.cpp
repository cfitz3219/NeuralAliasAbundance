// NGX D3D11 DLSS Super Resolution / DLAA. CMake compiles this file whenever
// the NGX SDK headers exist (so CI checks it), but links it into products only
// when the NGX static library exists; otherwise dx11_dlss_inproc_stub.cpp is
// used instead.
#include "bridge/dx11_dlss_inproc.hpp"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_helpers.h>

#include <cstdio>

namespace na::bridge {

namespace {
// Same custom-engine project id as our DX12 instance (dlss_own_ngx.cpp).
constexpr const char* kProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
constexpr const char* kEngineVersion = "NeuralAliasAbundance";

int CreateFlagsFor(const FrameRecord& f)
{
    int flags = 0;
    if (f.flags & kHdr) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (!(f.flags & kMvDisplayRes)) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (f.flags & kMvJittered) flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (f.flags & kDepthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (f.flags & kAutoExposure) flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    return flags;
}
} // namespace

struct Dx11Dlss::Impl {
    ID3D11Device* device = nullptr; // not owned (the game's)
    bool initialised = false;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    SessionLimits feature_limits{};
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
        if (feature) NVSDK_NGX_D3D11_ReleaseFeature(feature);
        feature = nullptr;
        feature_mode = feature_preset = ~0u;
        feature_flags = -1;
    }
};

Dx11Dlss::Dx11Dlss() : impl_(std::make_unique<Impl>()) {}
Dx11Dlss::~Dx11Dlss() { Shutdown(); }

bool Dx11Dlss::BuiltWithDlss() { return true; }

const char* Dx11Dlss::Init(ID3D11Device* device, const wchar_t* data_path)
{
    Impl& s = *impl_;
    Shutdown();
    if (!device || !data_path) return "DLSS start-up was called without a device or data folder.";
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
                                                             data_path, device, nullptr);
    if (NVSDK_NGX_FAILED(r)) return s.fail("The NVIDIA DLSS runtime did not start on this DX11 device. Is this an RTX GPU with a current driver?", r);
    s.initialised = true;
    s.device = device;
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&s.params);
    if (NVSDK_NGX_FAILED(r) || !s.params) { const char* why = s.fail("DLSS did not report its capabilities", r); Shutdown(); return why; }
    int available = 0;
    if (NVSDK_NGX_FAILED(s.params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
        int init_result = 0;
        s.params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
        const char* why = s.fail("DLSS Super Resolution is not available on this DX11 device", static_cast<NVSDK_NGX_Result>(init_result));
        Shutdown();
        return why;
    }
    return nullptr;
}

const char* Dx11Dlss::Evaluate(ID3D11DeviceContext* context, const InProcResources& res, const FrameRecord& f, const SessionLimits& limits)
{
    Impl& s = *impl_;
    if (!s.initialised || !s.params) return "DLSS is not running on this device.";
    if (!context || !res.color || !res.depth || !res.motion || !res.output) return "DLSS was given a missing input.";

    const int flags = CreateFlagsFor(f);
    const bool same = s.feature && s.feature_limits.max_render_w == limits.max_render_w &&
                      s.feature_limits.max_render_h == limits.max_render_h && s.feature_limits.output_w == limits.output_w &&
                      s.feature_limits.output_h == limits.output_h && s.feature_mode == f.mode && s.feature_preset == f.preset &&
                      s.feature_flags == flags;
    if (!same) {
        s.release_feature();
        // Render presets apply at creation; set every mode's hint so the
        // requested preset holds whichever model DLSS picks for this mode.
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
        const NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(context, &s.feature, s.params, &c);
        if (NVSDK_NGX_FAILED(r) || !s.feature) { s.feature = nullptr; return s.fail("DLSS could not create its feature for this size and mode", r); }
        s.feature_limits = limits;
        s.feature_mode = f.mode;
        s.feature_preset = f.preset;
        s.feature_flags = flags;
    }

    NVSDK_NGX_D3D11_DLSS_Eval_Params e{};
    e.Feature.pInColor = res.color;
    e.Feature.pInOutput = res.output;
    e.pInDepth = res.depth;
    e.pInMotionVectors = res.motion;
    e.InJitterOffsetX = f.jitter_x;
    e.InJitterOffsetY = f.jitter_y;
    e.InRenderSubrectDimensions.Width = f.render_w;
    e.InRenderSubrectDimensions.Height = f.render_h;
    e.InReset = (f.flags & kReset) ? 1 : 0;
    e.InMVScaleX = f.mv_scale_x;
    e.InMVScaleY = f.mv_scale_y;
    e.InPreExposure = f.pre_exposure;
    e.InExposureScale = f.exposure_scale;
    e.InFrameTimeDeltaInMsec = f.frame_ms;
    const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(context, s.feature, s.params, &e);
    if (NVSDK_NGX_FAILED(r)) return s.fail("DLSS failed on this frame", r);
    return nullptr;
}

void Dx11Dlss::Shutdown()
{
    Impl& s = *impl_;
    s.release_feature();
    if (s.params) NVSDK_NGX_D3D11_DestroyParameters(s.params);
    s.params = nullptr;
    if (s.initialised) NVSDK_NGX_D3D11_Shutdown1(s.device);
    s.initialised = false;
    s.device = nullptr;
}


} // namespace na::bridge
