// Reconstructed 2026-10 (C3). Mirrors WORKLOG-ROUTE-A A11/A14/A20/A42.
// Built only with the NVIDIA NGX SDK headers (NA_HAVE_NGX_SDK). Against a fake
// NVSDK_NGX_Parameter it proves:
//   - ReadEvalParams round-trips every key it claims, and keys the runtime did
//     not provide keep their poison markers (never invented);
//   - ApplyCreateOverrides is a no-op with the master switch off, writes the
//     quality value matching OUR mode (DLAA path for native), and writes the
//     chosen preset into all six mode keys only when a preset is chosen;
//   - StripPresetHints resets all six keys; RestoreQualityValue writes back.
#include "test_support.hpp"
#include "fake_ngx_params.hpp"
#include <nvsdk_ngx_defs_dlssd.h> // Ray Reconstruction preset keys
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/settings.hpp"

int main()
{
    using na::ngx::EvalParams;
    auto& st = na::settings::Get();
    const char* kPresetKeys[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};

    // ---- ReadEvalParams: full set, then poison preservation.
    {
        test::FakeParams p;
        auto* color = reinterpret_cast<ID3D12Resource*>(0x1100);
        auto* depth = reinterpret_cast<ID3D12Resource*>(0x2200);
        auto* mv = reinterpret_cast<ID3D12Resource*>(0x3300);
        auto* out = reinterpret_cast<ID3D12Resource*>(0x4400);
        p.Set(NVSDK_NGX_Parameter_Width, 2296u); p.Set(NVSDK_NGX_Parameter_Height, 960u);
        p.Set(NVSDK_NGX_Parameter_OutWidth, 3440u); p.Set(NVSDK_NGX_Parameter_OutHeight, 1440u);
        p.Set(NVSDK_NGX_Parameter_Reset, 1);
        p.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.25f); p.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, -0.375f);
        p.Set(NVSDK_NGX_Parameter_MV_Scale_X, 2296.0f); p.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 960.0f);
        p.Set(NVSDK_NGX_Parameter_Color, color); p.Set(NVSDK_NGX_Parameter_Depth, depth);
        p.Set(NVSDK_NGX_Parameter_MotionVectors, mv); p.Set(NVSDK_NGX_Parameter_Output, out);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 2296u);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 960u);
        p.Set(NVSDK_NGX_Parameter_PerfQualityValue, 2);
        p.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0x43);
        EvalParams ep{};
        REQUIRE(na::ngx::ReadEvalParams(&p, &ep));
        CHECK(ep.valid == 1);
        CHECK(ep.in_width == 2296 && ep.in_height == 960 && ep.out_width == 3440 && ep.out_height == 1440);
        CHECK(ep.reset == 1);
        CHECK(ep.jitter_x == 0.25f && ep.jitter_y == -0.375f);
        CHECK(ep.mv_scale_x == 2296.0f && ep.mv_scale_y == 960.0f);
        CHECK(ep.color == 0x1100 && ep.depth == 0x2200 && ep.motion == 0x3300 && ep.output == 0x4400);
        CHECK(ep.exposure == 0); // not provided -> null, never invented
        CHECK(ep.subrect_width == 2296 && ep.subrect_height == 960);
        CHECK(ep.perf_quality == 2);
        CHECK(ep.create_flags == 0x43);
    }
    {
        test::FakeParams p; // nothing provided at all
        EvalParams ep{};
        CHECK(na::ngx::ReadEvalParams(&p, &ep));
        CHECK(ep.reset == 0xFFFFFFFFu && ep.in_width == 0xFFFFFFFFu && ep.out_height == 0xFFFFFFFFu);
        CHECK(ep.jitter_x == 1e38f && ep.mv_scale_y == 1e38f);
        CHECK(ep.subrect_width == 0xFFFFFFFFu && ep.perf_quality == 0xFFFFFFFFu && ep.create_flags == 0xFFFFFFFFu);
        CHECK(ep.color == 0 && ep.depth == 0 && ep.motion == 0 && ep.output == 0);
    }

    // ---- ApplyCreateOverrides.
    {
        test::FakeParams p;
        st.enabled.store(0); st.preset.store(11);
        CHECK(!na::ngx::ApplyCreateOverrides(&p));
        CHECK(p.values.empty()); // master switch off: the game's object is untouched
    }
    {
        test::FakeParams p;
        st.enabled.store(1); st.dlss_mode.store(static_cast<uint32_t>(na::settings::DlssMode::DLAA)); st.preset.store(-1);
        CHECK(na::ngx::ApplyCreateOverrides(&p));
        CHECK(p.I(NVSDK_NGX_Parameter_PerfQualityValue) == NVSDK_NGX_PerfQuality_Value_DLAA);
        for (const char* k : kPresetKeys) CHECK(!p.Has(k)); // Automatic: no preset hints written
    }
    {
        test::FakeParams p;
        st.dlss_mode.store(static_cast<uint32_t>(na::settings::DlssMode::Performance)); st.preset.store(11); // K
        CHECK(na::ngx::ApplyCreateOverrides(&p));
        CHECK(p.I(NVSDK_NGX_Parameter_PerfQualityValue) == NVSDK_NGX_PerfQuality_Value_MaxPerf);
        for (const char* k : kPresetKeys) CHECK(p.U(k) == 11u);
        // A42 fallback helpers.
        CHECK(na::ngx::StripPresetHints(&p));
        for (const char* k : kPresetKeys) CHECK(p.U(k) == 0u); // Default
        CHECK(na::ngx::RestoreQualityValue(&p, NVSDK_NGX_PerfQuality_Value_MaxQuality));
        CHECK(p.I(NVSDK_NGX_Parameter_PerfQualityValue) == NVSDK_NGX_PerfQuality_Value_MaxQuality);
    }
    CHECK(!na::ngx::ApplyCreateOverrides(nullptr));

    // ---- Ray Reconstruction (feature 13) is the game's upscaler in RT games
    // (Phantom Blade Zero): same quality value, its OWN preset keys (D/E/F only),
    // never the Super Resolution keys. Frame Generation (11) is left untouched.
    const char* kRrKeys[] = {NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
                             NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality};
    {
        test::FakeParams p;
        st.enabled.store(1); st.dlss_mode.store(static_cast<uint32_t>(na::settings::DlssMode::UltraPerformance));
        st.preset.store(13); st.rr_preset.store(5); // SR M chosen, RR E chosen
        CHECK(na::ngx::ApplyCreateOverridesForFeature(13, &p));
        CHECK(p.I(NVSDK_NGX_Parameter_PerfQualityValue) == NVSDK_NGX_PerfQuality_Value_UltraPerformance);
        for (const char* k : kRrKeys) CHECK(p.U(k) == 5u);
        for (const char* k : kPresetKeys) CHECK(!p.Has(k)); // RR never gets SR's preset (M is "do not use" for RR)
        CHECK(na::ngx::StripRrPresetHints(&p));
        for (const char* k : kRrKeys) CHECK(p.U(k) == 0u);
    }
    {
        test::FakeParams p;
        st.rr_preset.store(0); // Automatic: quality value only
        CHECK(na::ngx::ApplyCreateOverridesForFeature(13, &p));
        CHECK(p.Has(NVSDK_NGX_Parameter_PerfQualityValue));
        for (const char* k : kRrKeys) CHECK(!p.Has(k));
    }
    {
        test::FakeParams p;
        CHECK(!na::ngx::ApplyCreateOverridesForFeature(11, &p)); // Frame Generation: not ours to change
        CHECK(p.values.empty());
        CHECK(na::ngx::ApplyCreateOverridesForFeature(1, &p));   // Super Resolution path unchanged
        for (const char* k : kPresetKeys) CHECK(p.U(k) == 13u);
        for (const char* k : kRrKeys) CHECK(!p.Has(k));
    }
    st.preset.store(-1); st.rr_preset.store(0); st.dlss_mode.store(0);
    return test::finish("ngx_params_test");
}
