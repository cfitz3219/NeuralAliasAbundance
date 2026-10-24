// C7a: FSR 3.1+ (FidelityFX API) interception against a stand-in runtime DLL
// (ffx_stub.dll, path in argv[1]).
//   1. ToDlss: flag/size/state mapping and refusals (pure);
//   2. InstallForModule hooks all five exports once (idempotent);
//   3. Observe mode: an upscale dispatch is recorded field-for-field with its
//      context's creation flags, and FSR still runs; other dispatches pass;
//   4. Replace mode (needs NGX headers; fake NGX runtime on a real/WARP list):
//      DLSS gets the mapped inputs and FSR is skipped; incompatible output
//      state or an unknown context falls back to FSR with a reason;
//   5. destroyed contexts are forgotten.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"

#include <Kits/FidelityFX/api/include/ffx_api.h>
#include <Kits/FidelityFX/upscalers/include/ffx_upscale.h>

#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_runtime.hpp"
#include "lock_order_probe.hpp"
#endif

#include <cstring>
#include <d3d12sdklayers.h>
#include <vector>

namespace {
ID3D12Resource* Fake(uintptr_t v) { return reinterpret_cast<ID3D12Resource*>(v); }

FfxApiResource Res(uintptr_t v, uint32_t state)
{
    FfxApiResource r{};
    r.resource = reinterpret_cast<void*>(v);
    r.state = state;
    return r;
}

ffxDispatchDescUpscale UpscaleDesc(void* list)
{
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = list;
    d.color = Res(0x10, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = Res(0x20, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = Res(0x30, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = Res(0x40, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {0.3125f, -0.1875f};
    d.motionVectorScale = {1280.f, 720.f};
    d.renderSize = {1280, 720};
    d.upscaleSize = {1920, 1080};
    d.preExposure = 1.5f;
    d.frameTimeDelta = 16.6f;
    d.cameraNear = 0.1f;
    d.cameraFar = 1000.f;
    d.cameraFovAngleVertical = 1.0f;
    d.reset = true;
    return d;
}

na::ffx::Observation GoodObservation()
{
    na::ffx::Observation o;
    o.context_known = true;
    o.context_flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    o.max_upscale_w = 1920;
    o.max_upscale_h = 1080;
    o.color = Fake(0x10);
    o.depth = Fake(0x20);
    o.motion = Fake(0x30);
    o.output = Fake(0x40);
    o.color_state = o.motion_state = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    o.depth_state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    o.output_state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    o.render_w = 1280;
    o.render_h = 720;
    o.jitter_x = 0.25f;
    o.jitter_y = -0.25f;
    o.mv_scale_x = 1280.f;
    o.mv_scale_y = 720.f;
    o.pre_exposure = 2.f;
    return o;
}
} // namespace

int main(int argc, char** argv)
{
    // 1. Pure mapping.
    {
        na::dlss::EvalDesc d;
        const char* why = "unset";
        auto o = GoodObservation();
        CHECK(na::ffx::ToDlss(o, &d, &why));
        CHECK(why == nullptr);
        CHECK(d.out_w == 1920 && d.out_h == 1080); // upscaleSize 0 -> context max
        CHECK(d.hdr && d.depth_inverted && !d.mv_jittered && !d.mv_display_res && !d.auto_exposure);
        CHECK(d.jitter_x == 0.25f && d.jitter_y == -0.25f && d.mv_scale_x == 1280.f && d.pre_exposure == 2.f);
        CHECK(d.quality == na::dlss::Quality::MaxQuality); // 1.5x
        o.upscale_w = 2560; o.upscale_h = 1440;
        CHECK(na::ffx::ToDlss(o, &d, &why) && d.quality == na::dlss::Quality::MaxPerformance); // 2.0x
        o = GoodObservation(); o.context_flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
                                                  FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        CHECK(na::ffx::ToDlss(o, &d, &why) && d.mv_jittered && d.mv_display_res && d.auto_exposure);
        o = GoodObservation(); o.render_w = 1920; o.render_h = 1080;
        CHECK(na::ffx::ToDlss(o, &d, &why) && d.quality == na::dlss::Quality::DLAA);
        o = GoodObservation(); o.pre_exposure = 0.f;
        CHECK(na::ffx::ToDlss(o, &d, &why) && d.pre_exposure == 1.f);
        o = GoodObservation(); o.context_known = false;
        CHECK(!na::ffx::ToDlss(o, &d, &why) && why && std::strstr(why, "before"));
        o = GoodObservation(); o.output_state = FFX_API_RESOURCE_STATE_COMMON; // ARGON-G2-003: transitioned, not refused
        CHECK(na::ffx::ToDlss(o, &d, &why));
        o = GoodObservation(); o.color_state = FFX_API_RESOURCE_STATE_COPY_DEST;
        CHECK(na::ffx::ToDlss(o, &d, &why));
        D3D12_RESOURCE_STATES generic{};
        CHECK(na::ffx::D3D12State(FFX_API_RESOURCE_STATE_GENERIC_READ, &generic));
        CHECK(generic == (D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        o = GoodObservation(); o.output_state = 0x4000; // no D3D12 equivalent
        CHECK(!na::ffx::ToDlss(o, &d, &why) && why && std::strstr(why, "translate"));
        o = GoodObservation(); o.max_render_w = 1280; o.max_render_h = 720; o.render_w = 1000; o.render_h = 562;
        CHECK(na::ffx::ToDlss(o, &d, &why) && d.max_render_w == 1280 && d.render_w == 1000 && d.quality == na::dlss::Quality::MaxQuality);
        o = GoodObservation(); o.exposure = Fake(0x50); o.exposure_state = FFX_API_RESOURCE_STATE_COPY_DEST;
        CHECK(na::ffx::ToDlss(o, &d, &why) && d.exposure == Fake(0x50));
        o.exposure_state = 0x4000;
        CHECK(!na::ffx::ToDlss(o, &d, &why));
        o = GoodObservation(); o.motion = nullptr;
        CHECK(!na::ffx::ToDlss(o, &d, &why) && why);
        o = GoodObservation(); o.jitter_x = 4.f; // out of range -> DLSS validation refuses
        CHECK(!na::ffx::ToDlss(o, &d, &why) && why);
        { const wchar_t* n = L"C:\\Games\\X\\AMD_FidelityFX_DX12.dll"; CHECK(na::ffx::IsFfxModuleName(n, wcslen(n))); }
        { const wchar_t* n = L"amd_fidelityfx_loader_dx12.dll"; CHECK(na::ffx::IsFfxModuleName(n, wcslen(n))); }
        { const wchar_t* n = L"amd_fidelityfx_vk.dll"; CHECK(!na::ffx::IsFfxModuleName(n, wcslen(n))); }
    }

    // 2. Hook the stand-in runtime.
    REQUIRE(argc > 1);
    HMODULE stub = LoadLibraryA(argv[1]);
    REQUIRE(stub != nullptr);
    const uint32_t installed = na::ffx::InstallForModule(stub);
    if (installed != 5) {
        char err[256] = {};
        na::hooking::LastError(err, sizeof(err));
        std::printf("InstallForModule hooked %u (hooking: %s)\n", installed, err);
    }
    CHECK(installed == 5);
    CHECK(na::ffx::InstallForModule(stub) == 0); // idempotent
    CHECK(na::ffx::GetStats().modules_hooked == 1 && na::ffx::GetStats().hooked_functions == 5);
    auto create = reinterpret_cast<PfnFfxCreateContext>(reinterpret_cast<void*>(GetProcAddress(stub, "ffxCreateContext")));
    auto destroy = reinterpret_cast<PfnFfxDestroyContext>(reinterpret_cast<void*>(GetProcAddress(stub, "ffxDestroyContext")));
    auto dispatch = reinterpret_cast<PfnFfxDispatch>(reinterpret_cast<void*>(GetProcAddress(stub, "ffxDispatch")));
    auto counts = reinterpret_cast<int (*)(int)>(reinterpret_cast<void*>(GetProcAddress(stub, "StubCounts")));
    REQUIRE(create && destroy && dispatch && counts);

    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    cd.maxRenderSize = {1280, 720};
    cd.maxUpscaleSize = {1920, 1080};
    ffxContext ctx = nullptr;
    REQUIRE(create(&ctx, &cd.header, nullptr) == FFX_API_RETURN_OK && ctx);
    CHECK(counts(0) == 1);
    CHECK(na::ffx::GetStats().upscale_contexts == 1);

    // 3. Observe (default).
    CHECK(na::ffx::GetMode() == na::ffx::Mode::Observe);
    {
        auto u = UpscaleDesc(reinterpret_cast<void*>(0x99));
        CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
        CHECK(counts(3) == 1); // FSR ran
        na::ffx::Observation o;
        REQUIRE(na::ffx::LastObservation(&o));
        CHECK(o.serial == 1 && o.context == ctx && o.context_known);
        CHECK(o.context_flags == cd.flags && o.max_render_w == 1280 && o.max_upscale_h == 1080);
        CHECK(o.list == reinterpret_cast<ID3D12GraphicsCommandList*>(0x99));
        CHECK(o.color == Fake(0x10) && o.depth == Fake(0x20) && o.motion == Fake(0x30) && o.output == Fake(0x40));
        CHECK(o.exposure == nullptr && o.reactive == nullptr);
        CHECK(o.depth_state == FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ && o.output_state == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        CHECK(o.render_w == 1280 && o.upscale_w == 1920 && o.jitter_x == 0.3125f && o.jitter_y == -0.1875f);
        CHECK(o.mv_scale_y == 720.f && o.pre_exposure == 1.5f && o.reset && o.camera_far == 1000.f);
        ffxApiHeader other{};
        other.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK;
        CHECK(dispatch(&ctx, &other) == FFX_API_RETURN_OK);
        CHECK(counts(2) == 2 && na::ffx::GetStats().other_dispatches == 1);
    }

    // 4. Replace mode.
    na::ffx::SetMode(na::ffx::Mode::ReplaceWithDlss);
#if defined(NA_HAVE_NGX_SDK)
    test::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    test::Device d;
    if (test::CreateDevice(d)) {
        REQUIRE(na::dlss::Attach(d.device.Get(), fake::MakeApi()));
        test::List l;
        REQUIRE(l.Create(d.device.Get()));
        {
            const int fsr_before = counts(3), evals_before = fake::evals;
            auto u = UpscaleDesc(l.list.Get());
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
            CHECK(counts(3) == fsr_before);         // FSR skipped
            CHECK(fake::evals == evals_before + 1); // DLSS ran on the game's list
            CHECK(na::ffx::GetStats().replaced == 1);
            CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_Output) == Fake(0x40)); // into FSR's output
            CHECK(fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X) == 0.3125);
            CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) ==
                  (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        }
        {
            const int fsr_before = counts(3);
            auto u = UpscaleDesc(l.list.Get());
            u.output.state = 0x4000; // untranslatable
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
            CHECK(counts(3) == fsr_before + 1); // fell back to FSR
            CHECK(na::ffx::GetStats().fallbacks == 1 && std::strstr(na::ffx::GetStats().reason, "translate"));
        }
        {
            ffxContext unknown = reinterpret_cast<ffxContext>(0x5555);
            const int fsr_before = counts(3);
            auto u = UpscaleDesc(l.list.Get());
            CHECK(dispatch(&unknown, &u.header) == FFX_API_RETURN_OK);
            CHECK(counts(3) == fsr_before + 1);
            CHECK(na::ffx::GetStats().unknown_context == 1 && std::strstr(na::ffx::GetStats().reason, "before"));
        }
        auto last_reset = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(stub, "StubLastReset")));
        REQUIRE(last_reset != nullptr);
        const auto dlss_reset = [] { return fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset); };
        // R-304: switching between DLSS and FSR forces a reset on whichever side
        // missed frames; uninterrupted runs keep history. The game's own
        // descriptor (reset=false) is never modified.
        {
            auto u = UpscaleDesc(l.list.Get());
            u.reset = false;
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK && dlss_reset() == 1); // FSR ran last
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK && dlss_reset() == 0); // DLSS continues
            auto fb = u;
            fb.motionVectors.resource = nullptr; // forces the FSR fallback
            const int fsr_before = counts(3);
            CHECK(dispatch(&ctx, &fb.header) == FFX_API_RETURN_OK && counts(3) == fsr_before + 1);
            CHECK(last_reset() == 1);  // FSR missed the DLSS frames
            CHECK(fb.reset == false);  // caller's descriptor untouched
            CHECK(dispatch(&ctx, &fb.header) == FFX_API_RETURN_OK && last_reset() == 0); // FSR continues
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK && dlss_reset() == 1);  // back to DLSS
            // R-309: a FAILED native frame does not hand the history to FSR:
            // the next FSR frame after DLSS still resets.
            auto fail_next = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(stub, "StubFailNextDispatch")));
            REQUIRE(fail_next != nullptr);
            fail_next();
            CHECK(dispatch(&ctx, &fb.header) == FFX_API_RETURN_ERROR && last_reset() == 1);
            CHECK(dispatch(&ctx, &fb.header) == FFX_API_RETURN_OK && last_reset() == 1); // still owed
            CHECK(dispatch(&ctx, &fb.header) == FFX_API_RETURN_OK && last_reset() == 0); // now FSR's
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK && dlss_reset() == 1);  // back to DLSS
            na::ffx::SetMode(na::ffx::Mode::Observe);
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK && last_reset() == 1); // mode change
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK && last_reset() == 0);
            na::ffx::SetMode(na::ffx::Mode::ReplaceWithDlss);
        }
        // R-303: two contexts of the same size keep separate DLSS histories, and a
        // destroyed-then-recreated context never inherits the old one.
        {
            ffxContext ctx2 = nullptr;
            REQUIRE(create(&ctx2, &cd.header, nullptr) == FFX_API_RETURN_OK && ctx2);
            auto u = UpscaleDesc(l.list.Get());
            u.reset = false;
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
            const uintptr_t h_ctx = fake::last_eval_handle;
            CHECK(dispatch(&ctx2, &u.header) == FFX_API_RETURN_OK);
            const uintptr_t h_ctx2 = fake::last_eval_handle;
            CHECK(h_ctx2 != h_ctx && dlss_reset() == 1);
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
            CHECK(fake::last_eval_handle == h_ctx && dlss_reset() == 0);
            CHECK(destroy(&ctx2, nullptr) == FFX_API_RETURN_OK);
            ffxContext ctx3 = nullptr;
            REQUIRE(create(&ctx3, &cd.header, nullptr) == FFX_API_RETURN_OK && ctx3);
            CHECK(dispatch(&ctx3, &u.header) == FFX_API_RETURN_OK);
            CHECK(fake::last_eval_handle != h_ctx && fake::last_eval_handle != h_ctx2 && dlss_reset() == 1);
            CHECK(destroy(&ctx3, nullptr) == FFX_API_RETURN_OK);
        }
        // ARGON-G2-005: DLSS evaluation runs without the interceptor's lock, so a
        // stats read (the menu) from inside it cannot wait on it. With the old
        // locking this self-deadlocks on the same thread.
        {
            fake::eval_hook = [] { (void)na::ffx::GetStats(); (void)na::ffx::LastObservation(nullptr); };
            const uint64_t replaced = na::ffx::GetStats().replaced;
            auto u = UpscaleDesc(l.list.Get());
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
            CHECK(na::ffx::GetStats().replaced == replaced + 1);
            fake::eval_hook = nullptr;
        }
        // ARGON-G2-003: a COMMON output is transitioned to UAV and back, not refused.
        {
            const uint64_t replaced = na::ffx::GetStats().replaced, barriers = na::ffx::GetStats().barriers;
            auto u = UpscaleDesc(l.list.Get());
            u.reset = false;
            // A real texture: the barrier is recorded on a real list.
            auto out_tex = test::CreateTexture(d.device.Get(), 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                               D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            REQUIRE(out_tex);
            u.output = Res(reinterpret_cast<uintptr_t>(out_tex.Get()), FFX_API_RESOURCE_STATE_COMMON);
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
            CHECK(na::ffx::GetStats().replaced == replaced + 1 && na::ffx::GetStats().barriers == barriers + 1);
            CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_Output) == out_tex.Get());
        }
        l.list->Close();
        na::dlss::Shutdown();
        REQUIRE(l.Create(d.device.Get()));
        REQUIRE(na::dlss::Attach(d.device.Get(), fake::MakeApi()));
        ffxContext victim = nullptr;
        REQUIRE(create(&victim, &cd.header, nullptr) == FFX_API_RETURN_OK);
        auto concurrent = UpscaleDesc(l.list.Get());
        ffxReturnCode_t eval_result = FFX_API_RETURN_ERROR, destroy_result = FFX_API_RETURN_ERROR;
        lock_probe::Run([&] { eval_result = dispatch(&ctx, &concurrent.header); },
                        [&] { destroy_result = destroy(&victim, nullptr); },
                        [] { (void)na::ffx::GetStats(); });
        CHECK(eval_result == FFX_API_RETURN_OK && destroy_result == FFX_API_RETURN_OK);
        // ARGON-G2-001: dynamic resolution inside the context's maxRenderSize keeps one feature and its history.
        {
            const int creates = fake::creates;
            const uint32_t widths[10] = {960, 1000, 1114, 1200, 1280, 1180, 1050, 1280, 990, 1114};
            for (int i = 0; i < 10; ++i) {
                auto u = UpscaleDesc(l.list.Get());
                u.reset = false;
                u.renderSize = {widths[i], widths[i] * 9 / 16};
                CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
                CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
            }
            CHECK(fake::creates == creates); // created once at 1280x720 earlier, never again
        }
        l.list->Close();
        na::dlss::Shutdown();
        // G2-003: execute real transitions, assert exposure state at NGX entry
        // and after return, for both success and failed evaluation/fallback.
        test::ComPtr<ID3D12InfoQueue> info;
        d.device.As(&info);
        for (int failure = 0; failure < 2; ++failure) {
            REQUIRE(na::dlss::Attach(d.device.Get(), fake::MakeApi()));
            test::List states;
            REQUIRE(states.Create(d.device.Get()));
            auto color = test::CreateTexture(d.device.Get(), 1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
            auto depth = test::CreateTexture(d.device.Get(), 1280, 720, DXGI_FORMAT_R32_FLOAT,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
            auto motion = test::CreateTexture(d.device.Get(), 1280, 720, DXGI_FORMAT_R16G16_FLOAT,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
            auto output = test::CreateTexture(d.device.Get(), 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            auto exposure = test::CreateTexture(d.device.Get(), 1, 1, DXGI_FORMAT_R32_FLOAT,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
            REQUIRE(color && depth && motion && output && exposure);
            test::ComPtr<ID3D12DebugCommandList> debug_list;
            states.list.As(&debug_list);
            std::printf("exposure state test: debug assertions %s, failed-evaluate=%d\n", debug_list ? "enabled" : "unavailable", failure);
            static ID3D12DebugCommandList* probe_list = nullptr;
            static ID3D12Resource* probe_exposure = nullptr;
            probe_list = debug_list.Get(); probe_exposure = exposure.Get();
            if (info) info->ClearStoredMessages();
            fake::eval_hook = [] {
                if (probe_list) CHECK(probe_list->AssertResourceState(probe_exposure, 0,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            };
            auto u = UpscaleDesc(states.list.Get());
            u.color = Res(reinterpret_cast<uintptr_t>(color.Get()), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            u.depth = Res(reinterpret_cast<uintptr_t>(depth.Get()), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            u.motionVectors = Res(reinterpret_cast<uintptr_t>(motion.Get()), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            u.output = Res(reinterpret_cast<uintptr_t>(output.Get()), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            u.exposure = Res(reinterpret_cast<uintptr_t>(exposure.Get()), FFX_API_RESOURCE_STATE_COPY_DEST);
            const int native0 = counts(3), eval0 = fake::evals;
            fake::eval_result = failure ? NVSDK_NGX_Result_FAIL_InvalidParameter : NVSDK_NGX_Result_Success;
            CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
            CHECK(fake::evals == eval0 + 1 && counts(3) == native0 + failure);
            if (debug_list) CHECK(debug_list->AssertResourceState(exposure.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST));
            fake::eval_hook = nullptr;
            fake::eval_result = NVSDK_NGX_Result_Success;
            REQUIRE(SUCCEEDED(states.list->Close()));
            ID3D12CommandList* lists[] = {states.list.Get()};
            na::dlss::BeforeExecute(d.queue.Get(), 1, lists);
            d.queue->ExecuteCommandLists(1, lists);
            na::dlss::OnExecuted(d.queue.Get(), 1, lists);
            d.WaitIdle();
            if (info) {
                for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                    SIZE_T bytes = 0; info->GetMessage(i, nullptr, &bytes);
                    std::vector<unsigned char> storage(bytes);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if (SUCCEEDED(info->GetMessage(i, message, &bytes))) {
                        const bool error = message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
                        if (error) std::printf("D3D12: %s\n", message->pDescription);
                        CHECK(!error);
                    }
                }
            } else std::printf("exposure debug-state assertions unavailable; real barrier submission checked\n");
            na::dlss::Shutdown();
        }
    } else {
        std::printf("replace-mode checks skipped: no D3D12 device\n");
    }
#else
    {
        const int fsr_before = counts(3);
        auto u = UpscaleDesc(reinterpret_cast<void*>(0x99));
        u.output.state = 0x4000; // refused before DLSS is consulted
        CHECK(dispatch(&ctx, &u.header) == FFX_API_RETURN_OK);
        CHECK(counts(3) == fsr_before + 1); // no SDK: always FSR
        CHECK(na::ffx::GetStats().fallbacks == 1);
    }
#endif
    na::ffx::SetMode(na::ffx::Mode::Observe);

    // 5. Destroy forgets the context.
    ffxContext gone = ctx;
    const int destroys_before = counts(1);
    CHECK(destroy(&ctx, nullptr) == FFX_API_RETURN_OK && counts(1) == destroys_before + 1);
    {
        auto u = UpscaleDesc(reinterpret_cast<void*>(0x99));
        CHECK(dispatch(&gone, &u.header) == FFX_API_RETURN_OK);
        na::ffx::Observation o;
        REQUIRE(na::ffx::LastObservation(&o));
        CHECK(!o.context_known);
    }
    CHECK(na::ffx::GetStats().contexts_destroyed == static_cast<uint64_t>(counts(1)));
    return test::finish("ffx_intercept_test");
}
