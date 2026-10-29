// Reconstructed 2026-10 (C3). Mirrors WORKLOG-ROUTE-A A16/A18/A19: the
// optimal-settings callback replacement that makes the ENGINE render at the
// size NeuralAlias chooses. Built only with the NGX SDK headers. Proves:
//   - master switch off: the runtime's answer is preserved;
//   - DLAA: native render size; Quality 3440x1440 -> 2296x960 and
//     Performance -> 1720x720 (8-aligned, the sizes TLOU rendered), Balanced
//     -> 2000x840; dynamic min/max follow the answer;
//   - a failing original callback's result passes through, nothing rewritten.
#include "test_support.hpp"
#include "fake_ngx_params.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/settings.hpp"

namespace {
int32_t g_orig_calls = 0;
int32_t g_orig_result = NVSDK_NGX_Result_Success;
// Stands in for the runtime's own callback: answers "Quality" sizes.
int32_t __cdecl FakeOriginal(void* p)
{
    ++g_orig_calls;
    auto* fp = static_cast<test::FakeParams*>(static_cast<NVSDK_NGX_Parameter*>(p));
    fp->Set(NVSDK_NGX_Parameter_OutWidth, 2293u);
    fp->Set(NVSDK_NGX_Parameter_OutHeight, 960u);
    return g_orig_result;
}
void Ask(test::FakeParams& p) { p.Reset(); p.Set(NVSDK_NGX_Parameter_Width, 3440u); p.Set(NVSDK_NGX_Parameter_Height, 1440u); }
} // namespace

int main()
{
    using M = na::settings::DlssMode;
    auto& st = na::settings::Get();
    na::ngx::SetOriginalOptimalCallbackForTest(reinterpret_cast<void*>(&FakeOriginal));
    test::FakeParams p;

    st.enabled.store(0);
    Ask(p);
    CHECK(na::ngx::OptimalSettingsCallback(&p) == NVSDK_NGX_Result_Success);
    CHECK(g_orig_calls == 1);
    CHECK(p.U(NVSDK_NGX_Parameter_OutWidth) == 2293u && p.U(NVSDK_NGX_Parameter_OutHeight) == 960u);
    CHECK(na::ngx::GetOptimalStats().last_overridden == 0);

    st.enabled.store(1);
    struct Case { M mode; unsigned w, h; } cases[] = {{M::DLAA, 3440, 1440}, {M::Quality, 2296, 960},
                                                    {M::Balanced, 2000, 840}, {M::Performance, 1720, 720}};
    for (const Case& c : cases) {
        st.dlss_mode.store(static_cast<uint32_t>(c.mode));
        Ask(p);
        CHECK(na::ngx::OptimalSettingsCallback(&p) == NVSDK_NGX_Result_Success);
        CHECK(p.U(NVSDK_NGX_Parameter_OutWidth) == c.w && p.U(NVSDK_NGX_Parameter_OutHeight) == c.h);
        CHECK(p.U(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width) == c.w);
        CHECK(p.U(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height) == c.h);
        const auto s = na::ngx::GetOptimalStats();
        CHECK(s.last_user_w == 3440 && s.last_user_h == 1440);
        CHECK(s.last_out_w == c.w && s.last_out_h == c.h);
        CHECK(s.last_overridden == 1);
        std::printf("%s -> %ux%u\n", na::settings::NameForMode(c.mode), p.U(NVSDK_NGX_Parameter_OutWidth), p.U(NVSDK_NGX_Parameter_OutHeight));
    }

    // Failure passthrough: nothing rewritten.
    g_orig_result = static_cast<int32_t>(NVSDK_NGX_Result_Fail);
    st.dlss_mode.store(static_cast<uint32_t>(M::DLAA));
    Ask(p);
    CHECK(na::ngx::OptimalSettingsCallback(&p) == static_cast<int32_t>(NVSDK_NGX_Result_Fail));
    CHECK(p.U(NVSDK_NGX_Parameter_OutWidth) == 2293u);
    CHECK(!p.Has(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width));

    // Ray Reconstruction asks its own callback (DLSSDOptimalSettingsCallback):
    // same parameters, same mode-to-size rule, its own original forwarded.
    g_orig_result = NVSDK_NGX_Result_Success;
    na::ngx::SetOriginalOptimalCallbackRRForTest(reinterpret_cast<void*>(&FakeOriginal));
    st.dlss_mode.store(static_cast<uint32_t>(M::Performance));
    const int32_t calls_before = g_orig_calls;
    Ask(p);
    CHECK(na::ngx::OptimalSettingsCallbackRR(&p) == NVSDK_NGX_Result_Success);
    CHECK(g_orig_calls == calls_before + 1);
    CHECK(p.U(NVSDK_NGX_Parameter_OutWidth) == 1720u && p.U(NVSDK_NGX_Parameter_OutHeight) == 720u);
    CHECK(p.U(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width) == 1720u);
    st.dlss_mode.store(static_cast<uint32_t>(M::UltraPerformance));
    Ask(p);
    CHECK(na::ngx::OptimalSettingsCallbackRR(&p) == NVSDK_NGX_Result_Success);
    CHECK(p.U(NVSDK_NGX_Parameter_OutWidth) == 1152u && p.U(NVSDK_NGX_Parameter_OutHeight) == 480u); // 3440/3 -> 1147 -> 8-aligned
    std::printf("RR Ultra Performance -> %ux%u\n", p.U(NVSDK_NGX_Parameter_OutWidth), p.U(NVSDK_NGX_Parameter_OutHeight));
    na::ngx::SetOriginalOptimalCallbackRRForTest(nullptr);

    na::ngx::SetOriginalOptimalCallbackForTest(nullptr);
    st.dlss_mode.store(0);
    return test::finish("ngx_optimal_test");
}
