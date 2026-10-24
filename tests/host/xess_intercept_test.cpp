// C7b: XeSS interception against a stand-in libxess.dll (path in argv[1]).
//   1. ToDlss: flags, sizes, jitter/velocity/exposure scales, NDC velocity,
//      refusals (no depth, sub-rectangles, unknown context);
//   2. InstallForModule hooks the exports once;
//   3. Observe: Execute recorded with its context's Init and scale settings,
//      XeSS still runs;
//   4. Replace (needs NGX headers; fake NGX runtime): DLSS writes XeSS's
//      output and XeSS is skipped; a frame without depth falls back;
//   5. destroyed contexts are forgotten.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/upscalers/xess_abi.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"

#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_runtime.hpp"
#include "lock_order_probe.hpp"
#endif

#include <cstring>

using namespace na::xess::abi;

namespace {
ID3D12Resource* Fake(uintptr_t v) { return reinterpret_cast<ID3D12Resource*>(v); }

na::xess::Observation Good()
{
    na::xess::Observation o;
    o.context_known = true;
    o.init_flags = kInvertedDepth;
    o.quality = kQuality;
    o.out_w = 2560; o.out_h = 1440;
    o.color = Fake(0x10); o.velocity = Fake(0x30); o.depth = Fake(0x20); o.output = Fake(0x40);
    o.in_w = 1707; o.in_h = 960;
    o.jitter_x = 0.25f; o.jitter_y = -0.5f;
    return o;
}
D3D12ExecuteParams Exec()
{
    D3D12ExecuteParams p{};
    p.pColorTexture = Fake(0x10);
    p.pVelocityTexture = Fake(0x30);
    p.pDepthTexture = Fake(0x20);
    p.pOutputTexture = Fake(0x40);
    p.jitterOffsetX = 0.375f;
    p.jitterOffsetY = 0.125f;
    p.exposureScale = 1.f;
    p.resetHistory = 1;
    p.inputWidth = 1280;
    p.inputHeight = 720;
    return p;
}
} // namespace

int main(int argc, char** argv)
{
    // 1. Pure mapping.
    {
        na::dlss::EvalDesc d;
        const char* why = "unset";
        auto o = Good();
        CHECK(na::xess::ToDlss(o, &d, &why) && why == nullptr);
        CHECK(d.render_w == 1707 && d.out_w == 2560 && d.depth_inverted && d.hdr && !d.mv_display_res && !d.mv_jittered);
        CHECK(d.mv_scale_x == 1.f && d.mv_scale_y == 1.f && d.jitter_x == 0.25f && d.jitter_y == -0.5f);
        CHECK(d.quality == na::dlss::Quality::MaxQuality); // 2560/1707 = 1.5
        o = Good(); o.jitter_scale_x = -1.f; o.velocity_scale_x = 2.f; o.exposure_scale = 2.f; o.exposure_multiplier = 0.5f;
        CHECK(na::xess::ToDlss(o, &d, &why) && d.jitter_x == -0.25f && d.mv_scale_x == 2.f && d.exposure_scale == 1.f);
        o = Good(); o.init_flags = kUseNdcVelocity | kLdrInputColor | kJitteredMv | kHighResMv | kEnableAutoExposure;
        CHECK(na::xess::ToDlss(o, &d, &why));
        CHECK(!d.hdr && d.mv_jittered && d.mv_display_res && d.auto_exposure && !d.depth_inverted);
        CHECK(d.mv_scale_x == 1280.f && d.mv_scale_y == -720.f); // NDC -> output pixels, Y flipped
        o = Good(); o.init_flags = kExposureScaleTexture | kEnableAutoExposure; o.exposure = Fake(0x50);
        CHECK(na::xess::ToDlss(o, &d, &why) && d.exposure == Fake(0x50) && !d.auto_exposure);
        o = Good(); o.quality = kNativeAA; o.in_w = 2560; o.in_h = 1440;
        CHECK(na::xess::ToDlss(o, &d, &why) && d.quality == na::dlss::Quality::DLAA);
        o = Good(); o.depth = nullptr;
        CHECK(!na::xess::ToDlss(o, &d, &why) && why && std::strstr(why, "depth"));
        o = Good(); o.nonzero_bases = true;
        CHECK(!na::xess::ToDlss(o, &d, &why) && why && std::strstr(why, "C13"));
        o = Good(); o.context_known = false;
        CHECK(!na::xess::ToDlss(o, &d, &why) && why);
        { const wchar_t* n = L"D:\\Game\\LIBXESS.DLL"; CHECK(na::xess::IsXessModuleName(n, wcslen(n))); }
        { const wchar_t* n = L"libxess_dx11.dll"; CHECK(!na::xess::IsXessModuleName(n, wcslen(n))); }
    }

    // 2. Hook the stand-in.
    REQUIRE(argc > 1);
    HMODULE stub = LoadLibraryA(argv[1]);
    REQUIRE(stub != nullptr);
    const uint32_t installed = na::xess::InstallForModule(stub);
    if (installed != 7) {
        char err[256] = {};
        na::hooking::LastError(err, sizeof(err));
        std::printf("InstallForModule hooked %u (hooking: %s)\n", installed, err);
    }
    CHECK(installed == 7);
    CHECK(na::xess::InstallForModule(stub) == 0);
    auto create = reinterpret_cast<PfnD3D12CreateContext>(reinterpret_cast<void*>(GetProcAddress(stub, "xessD3D12CreateContext")));
    auto init = reinterpret_cast<PfnD3D12Init>(reinterpret_cast<void*>(GetProcAddress(stub, "xessD3D12Init")));
    auto execute = reinterpret_cast<PfnD3D12Execute>(reinterpret_cast<void*>(GetProcAddress(stub, "xessD3D12Execute")));
    auto destroy = reinterpret_cast<PfnDestroyContext>(reinterpret_cast<void*>(GetProcAddress(stub, "xessDestroyContext")));
    auto set_jitter = reinterpret_cast<PfnSetScale>(reinterpret_cast<void*>(GetProcAddress(stub, "xessSetJitterScale")));
    auto set_velocity = reinterpret_cast<PfnSetScale>(reinterpret_cast<void*>(GetProcAddress(stub, "xessSetVelocityScale")));
    auto executes = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(stub, "XessStubExecutes")));
    REQUIRE(create && init && execute && destroy && set_jitter && set_velocity && executes);

    test::Device dev;
    const bool have_device = test::CreateDevice(dev);
    ID3D12Device* device_ptr = have_device ? dev.device.Get() : reinterpret_cast<ID3D12Device*>(0x77);
    Context ctx = nullptr;
    REQUIRE(create(device_ptr, &ctx) == kSuccess && ctx);
    D3D12InitParams ip{};
    ip.outputResolution = {1920, 1080};
    ip.qualitySetting = kQuality;
    ip.initFlags = kInvertedDepth | kLdrInputColor;
    REQUIRE(init(ctx, &ip) == kSuccess);
    CHECK(set_jitter(ctx, -1.f, -1.f) == kSuccess);
    CHECK(set_velocity(ctx, 1280.f, 720.f) == kSuccess);
    CHECK(na::xess::GetStats().contexts_initialized == 1);

    // 3. Observe.
    {
        auto p = Exec();
        CHECK(execute(ctx, reinterpret_cast<ID3D12GraphicsCommandList*>(0x99), &p) == kSuccess);
        CHECK(executes() == 1);
        na::xess::Observation o;
        REQUIRE(na::xess::LastObservation(&o));
        CHECK(o.context_known && o.device == device_ptr && o.init_flags == ip.initFlags && o.out_w == 1920 && o.quality == kQuality);
        CHECK(o.jitter_scale_x == -1.f && o.velocity_scale_y == 720.f);
        CHECK(o.color == Fake(0x10) && o.velocity == Fake(0x30) && o.depth == Fake(0x20) && o.output == Fake(0x40));
        CHECK(o.jitter_x == 0.375f && o.reset && o.in_w == 1280 && !o.nonzero_bases);
        p.inputDepthBase = {4, 0};
        CHECK(execute(ctx, reinterpret_cast<ID3D12GraphicsCommandList*>(0x99), &p) == kSuccess);
        REQUIRE(na::xess::LastObservation(&o));
        CHECK(o.nonzero_bases);
    }

    // 4. Replace.
    na::xess::SetMode(na::xess::Mode::ReplaceWithDlss);
#if defined(NA_HAVE_NGX_SDK)
    if (have_device) {
        REQUIRE(na::dlss::Attach(dev.device.Get(), fake::MakeApi()));
        test::List l;
        REQUIRE(l.Create(dev.device.Get()));
        {
            const int before = executes(), evals = fake::evals;
            auto p = Exec();
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess);
            CHECK(executes() == before); // XeSS skipped
            CHECK(fake::evals == evals + 1);
            CHECK(na::xess::GetStats().replaced == 1);
            CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_Output) == Fake(0x40));
            CHECK(fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X) == -0.375); // jitter scale -1
            CHECK(fake::F(fake::last_eval, NVSDK_NGX_Parameter_MV_Scale_X) == 1280.0);
            CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) ==
                  (NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)); // LDR: no IsHDR
        }
        {
            const int before = executes();
            auto p = Exec();
            p.pDepthTexture = nullptr;
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess);
            CHECK(executes() == before + 1); // fell back to XeSS
            CHECK(na::xess::GetStats().fallbacks == 1 && std::strstr(na::xess::GetStats().reason, "depth"));
        }
        auto last_reset = reinterpret_cast<int (*)()>(reinterpret_cast<void*>(GetProcAddress(stub, "XessStubLastReset")));
        REQUIRE(last_reset != nullptr);
        const auto dlss_reset = [] { return fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset); };
        // R-304: backend switches force a reset on the side that missed frames.
        {
            auto p = Exec();
            p.resetHistory = 0;
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && dlss_reset() == 1); // XeSS ran last
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && dlss_reset() == 0);
            auto fb = p;
            fb.pDepthTexture = nullptr; // forces the XeSS fallback
            CHECK(execute(ctx, l.list.Get(), &fb) == kSuccess && last_reset() == 1);
            CHECK(fb.resetHistory == 0); // caller's parameters untouched
            CHECK(execute(ctx, l.list.Get(), &fb) == kSuccess && last_reset() == 0);
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && dlss_reset() == 1);
            // R-309: a FAILED native frame does not hand the history to XeSS.
            auto fail_next = reinterpret_cast<void (*)()>(reinterpret_cast<void*>(GetProcAddress(stub, "XessStubFailNextExecute")));
            REQUIRE(fail_next != nullptr);
            fail_next();
            CHECK(execute(ctx, l.list.Get(), &fb) != kSuccess && last_reset() == 1);
            CHECK(execute(ctx, l.list.Get(), &fb) == kSuccess && last_reset() == 1); // still owed
            CHECK(execute(ctx, l.list.Get(), &fb) == kSuccess && last_reset() == 0);
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && dlss_reset() == 1);
            na::xess::SetMode(na::xess::Mode::Observe);
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && last_reset() == 1);
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && last_reset() == 0);
            na::xess::SetMode(na::xess::Mode::ReplaceWithDlss);
        }
        // R-303: separate contexts, separate histories; recreation starts fresh.
        {
            Context ctx2 = nullptr;
            REQUIRE(create(device_ptr, &ctx2) == kSuccess && ctx2);
            REQUIRE(init(ctx2, &ip) == kSuccess);
            auto p = Exec();
            p.resetHistory = 0;
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess);
            const uintptr_t h1 = fake::last_eval_handle;
            CHECK(execute(ctx2, l.list.Get(), &p) == kSuccess);
            const uintptr_t h2 = fake::last_eval_handle;
            CHECK(h1 != h2 && dlss_reset() == 1);
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && fake::last_eval_handle == h1 && dlss_reset() == 0);
            CHECK(destroy(ctx2) == kSuccess);
            Context ctx3 = nullptr;
            REQUIRE(create(device_ptr, &ctx3) == kSuccess && ctx3);
            REQUIRE(init(ctx3, &ip) == kSuccess);
            CHECK(execute(ctx3, l.list.Get(), &p) == kSuccess);
            CHECK(fake::last_eval_handle != h1 && fake::last_eval_handle != h2 && dlss_reset() == 1);
            CHECK(destroy(ctx3) == kSuccess);
        }
        // ARGON-G2-006: re-initialising an existing context restarts DLSS history.
        {
            auto p = Exec();
            p.resetHistory = 0;
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess);
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
            REQUIRE(init(ctx, &ip) == kSuccess);
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess && fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1);
            fake::eval_hook = [] { (void)na::xess::GetStats(); }; // ARGON-G2-005 for XeSS
            CHECK(execute(ctx, l.list.Get(), &p) == kSuccess);
            fake::eval_hook = nullptr;
        }
        // End the earlier unsubmitted synthetic recording before independent
        // cold-start/lock-order/DRS scenarios consume completion capacity.
        l.list->Close();
        na::dlss::Shutdown();
        REQUIRE(l.Create(dev.device.Get()));
        // G2-002: truly unattached, missed CreateContext, recovered list device.
        static ID3D12Device* recovered = nullptr;
        static int cold_inits = 0;
        na::dlss::SetInitOwnOverrideForTests([](ID3D12Device* device, const wchar_t*) {
            recovered = device; ++cold_inits;
            return na::dlss::Attach(device, fake::MakeApi());
        });
        CHECK(!na::dlss::GetCapabilities().attached);
        Context early = reinterpret_cast<Context>(0xABC0);
        REQUIRE(init(early, &ip) == kSuccess);
        auto cold = Exec();
        const uint64_t replaced0 = na::xess::GetStats().replaced;
        CHECK(execute(early, l.list.Get(), &cold) == kSuccess);
        CHECK(cold_inits == 1 && recovered == dev.device.Get());
        CHECK(na::xess::GetStats().replaced == replaced0 + 1);
        na::dlss::SetInitOwnOverrideForTests(nullptr);
        CHECK(destroy(early) == kSuccess);
        // G2-005: evaluate callback versus destruction of a different context.
        Context victim = nullptr;
        REQUIRE(create(device_ptr, &victim) == kSuccess && init(victim, &ip) == kSuccess);
        Result eval_result = -1, destroy_result = -1;
        auto concurrent = Exec();
        lock_probe::Run([&] { eval_result = execute(ctx, l.list.Get(), &concurrent); },
                        [&] { destroy_result = destroy(victim); },
                        [] { (void)na::xess::GetStats(); });
        CHECK(eval_result == kSuccess && destroy_result == kSuccess);
        // ARGON-G2-001: dynamic input size keeps one feature (created at the output size) and its history.
        {
            const int creates = fake::creates;
            const uint32_t widths[10] = {960, 1000, 1114, 1200, 1280, 1180, 1050, 1280, 990, 1114};
            for (int i = 0; i < 10; ++i) {
                auto p = Exec();
                p.resetHistory = 0;
                p.inputWidth = widths[i];
                p.inputHeight = widths[i] * 9 / 16;
                CHECK(execute(ctx, l.list.Get(), &p) == kSuccess);
                if (i) CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
            }
            CHECK(fake::creates <= creates + 1);
        }
        l.list->Close();
        na::dlss::Shutdown();
    }
#else
    {
        const int before = executes();
        auto p = Exec();
        p.pDepthTexture = nullptr; // refused before DLSS is consulted
        CHECK(execute(ctx, reinterpret_cast<ID3D12GraphicsCommandList*>(0x99), &p) == kSuccess);
        CHECK(executes() == before + 1);
    }
#endif
    na::xess::SetMode(na::xess::Mode::Observe);

    // 5. Destroy.
    Context gone = ctx;
    CHECK(destroy(ctx) == kSuccess);
    {
        auto p = Exec();
        CHECK(execute(gone, reinterpret_cast<ID3D12GraphicsCommandList*>(0x99), &p) == kSuccess);
        na::xess::Observation o;
        REQUIRE(na::xess::LastObservation(&o));
        CHECK(!o.context_known);
    }
    return test::finish("xess_intercept_test");
}
