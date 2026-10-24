// C6: the generic DLSS backend against a fake NGX runtime on a real D3D12
// device (hardware or WARP). Needs the NGX SDK headers only.
//   1. Validate / FromFrame contract mapping and refusals;
//   2. Attach reads capabilities; QueryOptimal (incl. min/max fallback, DLAA);
//   3. first Evaluate creates the feature with the right size/flags/quality
//      and forces Reset; every evaluation key is written every frame;
//   4. dynamic resolution within the created size reuses the feature;
//   5. an output change retires the old feature, which is released only after
//      the GPU finished its last evaluation (queue held on a fence first);
//   6. work completing on an independent queue never releases a feature whose
//      last use is still held on another queue (R-202 lesson);
//   7. an evaluation never seen submitted makes its feature "leaked" (kept
//      until Shutdown), never released;
//   8. create / evaluate failures latch off with a reason;
//   9. Shutdown releases every handle and destroys every parameter object.
#include "test_support.hpp"
#include "fake_ngx_runtime.hpp"

#include "core/frame_contract.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

#include <nvsdk_ngx.h>

#include <cstring>
#include <vector>


namespace {
// Never dereferenced by the backend (only NGX touches resources).
ID3D12Resource* Fake(uintptr_t v) { return reinterpret_cast<ID3D12Resource*>(v); }

na::dlss::EvalDesc Desc(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh)
{
    na::dlss::EvalDesc d;
    d.color = Fake(0x10);
    d.depth = Fake(0x20);
    d.motion = Fake(0x30);
    d.output = Fake(0x40);
    d.render_w = rw;
    d.render_h = rh;
    d.out_w = ow;
    d.out_h = oh;
    d.jitter_x = 0.25f;
    d.jitter_y = -0.125f;
    d.auto_exposure = true;
    return d;
}

abundance::Frame GoodFrame()
{
    abundance::Frame f;
    f.id = 7;
    f.generation = 1;
    f.route = abundance::Route::native_upscaler;
    const abundance::Extent in{1280, 720}, out{1920, 1080};
    f.color = {1, 7, 1, in};
    f.depth = {2, 7, 1, in};
    f.motion = {3, 7, 1, in};
    f.output = out;
    f.color_space = abundance::ColorSpace::linear_hdr;
    f.motion_space = abundance::MotionSpace::normalized_current_to_previous;
    f.jitter_x = 0.3f;
    f.jitter_y = -0.2f;
    f.jitter_known = f.exposure_policy_known = f.clean_scene = f.motion_excludes_jitter = true;
    return f;
}

// Records one evaluation on a fresh list and closes it (not executed).
bool Record(test::Device& d, test::List& l, const na::dlss::EvalDesc& e)
{
    if (!l.Create(d.device.Get())) return false;
    const bool ok = na::dlss::Evaluate(l.list.Get(), e);
    l.list->Close();
    return ok;
}
void Submit(ID3D12CommandQueue* q, test::List& l)
{
    ID3D12CommandList* lists[] = {l.list.Get()};
    q->ExecuteCommandLists(1, lists);
    na::dlss::OnExecuted(q, 1, lists); // what the host's execute detour does
}
} // namespace

int main()
{
    using na::dlss::Quality;
    // 1. Validation and frame-contract mapping (no device needed).
    {
        auto d = Desc(1280, 720, 1920, 1080);
        CHECK(na::dlss::Validate(d) == nullptr);
        auto bad = d; bad.motion = nullptr; CHECK(na::dlss::Validate(bad) != nullptr);
        bad = d; bad.render_w = 2000; CHECK(na::dlss::Validate(bad) != nullptr);
        bad = d; bad.quality = Quality::DLAA; CHECK(na::dlss::Validate(bad) != nullptr); // DLAA needs native size
        bad = Desc(1920, 1080, 1920, 1080); bad.quality = Quality::DLAA; CHECK(na::dlss::Validate(bad) == nullptr);
        bad = d; bad.jitter_x = 3.f; CHECK(na::dlss::Validate(bad) != nullptr);
        bad = d; bad.mv_scale_y = 0.f; CHECK(na::dlss::Validate(bad) != nullptr);

        na::dlss::FrameResources r{Fake(0x10), Fake(0x20), Fake(0x30), Fake(0x40), nullptr, true};
        na::dlss::EvalDesc e;
        const char* why = "unset";
        CHECK(na::dlss::FromFrame(GoodFrame(), r, Quality::MaxQuality, &e, &why));
        CHECK(why == nullptr);
        CHECK(e.render_w == 1280 && e.render_h == 720 && e.out_w == 1920 && e.out_h == 1080);
        CHECK(e.mv_scale_x == 1280.f && e.mv_scale_y == 720.f); // normalised -> render pixels
        CHECK(e.hdr && e.depth_inverted && e.auto_exposure && !e.mv_jittered && !e.mv_display_res);
        CHECK(e.jitter_x == 0.3f && e.jitter_y == -0.2f);
        auto f = GoodFrame(); f.color_space = abundance::ColorSpace::srgb_sdr;
        CHECK(!na::dlss::FromFrame(f, r, Quality::MaxQuality, &e, &why) && why && std::strstr(why, "linear"));
        f = GoodFrame(); f.jitter_known = false;
        CHECK(!na::dlss::FromFrame(f, r, Quality::MaxQuality, &e, &why) && why);
        f = GoodFrame(); f.motion_space = abundance::MotionSpace::pixels_current_to_previous; f.motion.extent = f.output;
        CHECK(na::dlss::FromFrame(f, r, Quality::MaxQuality, &e, &why));
        CHECK(e.mv_display_res && e.mv_scale_x == 1.f);
    }

    // Evaluate before any runtime: refused with a reason, nothing recorded.
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("dlss_backend_test");
    {
        test::List l;
        CHECK(!Record(d, l, Desc(1280, 720, 1920, 1080)));
        CHECK(na::dlss::GetStats().reason[0] != 0);
        CHECK(!na::dlss::GetCapabilities().attached);
    }

    // 2. Attach + capabilities + optimal settings.
    CHECK(na::dlss::Attach(d.device.Get(), fake::MakeApi()));
    {
        const auto c = na::dlss::GetCapabilities();
        CHECK(c.attached && c.available && !c.own_instance && c.optimal_settings && !c.latched_off);
        na::dlss::OptimalSettings s;
        CHECK(na::dlss::QueryOptimal(3840, 2160, Quality::MaxQuality, &s));
        CHECK(s.valid && s.render_w == 2560 && s.render_h == 1440);
        CHECK(s.min_w == 1280 && s.min_h == 720 && s.max_w == 2560 && s.max_h == 1440); // max falls back
        CHECK(na::dlss::QueryOptimal(3840, 2160, Quality::DLAA, &s) && s.render_w == 3840 && s.max_h == 2160);
    }

    // 3. First evaluation creates the feature; keys and reset.
    const int creates0 = fake::creates;
    test::List l1;
    {
        auto e = Desc(1280, 720, 1920, 1080);
        e.depth_inverted = true;
        e.mv_scale_x = 1280.f; e.mv_scale_y = 720.f;
        REQUIRE(Record(d, l1, e));
        CHECK(fake::creates == creates0 + 1);
        CHECK(fake::U(fake::last_create, NVSDK_NGX_Parameter_Width) == 1280);
        CHECK(fake::U(fake::last_create, NVSDK_NGX_Parameter_OutHeight) == 1080);
        CHECK(fake::I(fake::last_create, NVSDK_NGX_Parameter_PerfQualityValue) == NVSDK_NGX_PerfQuality_Value_MaxQuality);
        const int flags = fake::I(fake::last_create, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
        CHECK(flags == (NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
        CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_Color) == Fake(0x10));
        CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_Output) == Fake(0x40));
        CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_ExposureTexture) == nullptr); // explicitly cleared
        CHECK(fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X) == 0.25);
        CHECK(fake::F(fake::last_eval, NVSDK_NGX_Parameter_MV_Scale_Y) == 720.0);
        CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1); // new feature -> reset
        CHECK(fake::U(fake::last_eval, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) == 1280);
    }
    const uintptr_t h1 = fake::last_eval_handle;
    Submit(d.queue.Get(), l1);
    d.WaitIdle();
    test::List l2;
    {
        auto e = Desc(1280, 720, 1920, 1080);
        e.depth_inverted = true;
        e.mv_scale_x = 1280.f; e.mv_scale_y = 720.f;
        e.exposure = Fake(0x50);
        e.auto_exposure = true; // ignored when a texture is given -> flags change -> recreate
        REQUIRE(Record(d, l2, e));
        CHECK(fake::creates == creates0 + 2);
        CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_ExposureTexture) == Fake(0x50));
        // h1's last use completed (WaitIdle) -> released at once on retirement.
        CHECK(fake::Released(h1));
    }
    Submit(d.queue.Get(), l2);
    d.WaitIdle();
    const uintptr_t h2 = fake::last_eval_handle;
    // 4. Dynamic resolution: smaller render size, same everything else -> no recreate, no reset.
    test::List l3;
    {
        auto e = Desc(1024, 576, 1920, 1080);
        e.depth_inverted = true;
        e.mv_scale_x = 1024.f; e.mv_scale_y = 576.f;
        e.exposure = Fake(0x50);
        REQUIRE(Record(d, l3, e));
        CHECK(fake::creates == creates0 + 2);
        CHECK(fake::last_eval_handle == h2);
        CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
        CHECK(fake::U(fake::last_eval, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) == 1024);
    }

    // 5. Output change while h2's last use (l3) is held on the GPU.
    test::ComPtr<ID3D12Fence> hold;
    REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold))));
    REQUIRE(SUCCEEDED(d.queue->Wait(hold.Get(), 1)));
    Submit(d.queue.Get(), l3);
    test::List l4;
    {
        auto e = Desc(1280, 720, 2560, 1440);
        REQUIRE(Record(d, l4, e));
        CHECK(fake::creates == creates0 + 3);
        CHECK(!fake::Released(h2)); // its last evaluation has not run yet
        CHECK(na::dlss::GetStats().features_pending == 1);
    }
    const uintptr_t h3 = fake::last_eval_handle;

    // 6. Independent queue completes h3's evaluation; h2 (held on queue A) stays.
    D3D12_COMMAND_QUEUE_DESC cqd{}; cqd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    test::ComPtr<ID3D12CommandQueue> other;
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&cqd, IID_PPV_ARGS(&other))));
    test::ComPtr<ID3D12Fence> other_fence;
    REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&other_fence))));
    uint64_t other_value = 0;
    Submit(other.Get(), l4);
    test::SignalAndWait(other.Get(), other_fence.Get(), other_value, d.event, d.device.Get(), "independent queue");
    {
        test::List kick; REQUIRE(kick.Create(d.device.Get())); kick.list->Close();
        Submit(other.Get(), kick); // any submission runs collection
        CHECK(!fake::Released(h2));
        CHECK(!fake::Released(h3)); // still the current feature
    }
    REQUIRE(SUCCEEDED(hold->Signal(1)));
    d.WaitIdle();
    {
        test::List kick; REQUIRE(kick.Create(d.device.Get())); kick.list->Close();
        Submit(d.queue.Get(), kick);
        CHECK(fake::Released(h2)); // now proven complete
        CHECK(na::dlss::GetStats().features_pending == 0);
    }

    // 7. Last use never submitted -> leaked, never released.
    test::List never;
    REQUIRE(Record(d, never, Desc(1280, 720, 2560, 1440))); // h3 used, list abandoned
    test::List l5;
    REQUIRE(Record(d, l5, Desc(1280, 720, 3840, 2160)));    // retires h3
    CHECK(!fake::Released(h3));
    for (int i = 0; i < 600; ++i) {
        test::List kick; REQUIRE(kick.Create(d.device.Get())); kick.list->Close();
        ID3D12CommandList* lists[] = {kick.list.Get()};
        na::dlss::OnExecuted(d.queue.Get(), 1, lists); // collection only; nothing executed
    }
    CHECK(!fake::Released(h3));
    CHECK(na::dlss::GetStats().features_leaked == 1);
    d.WaitIdle();

    // 8. Failures latch with a reason.
    fake::eval_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    {
        test::List l; CHECK(!Record(d, l, Desc(1280, 720, 3840, 2160)));
        CHECK(na::dlss::GetCapabilities().latched_off);
        CHECK(std::strstr(na::dlss::GetStats().reason, "0x") != nullptr);
        const int evals = fake::evals;
        fake::eval_result = NVSDK_NGX_Result_Success;
        test::List again; CHECK(!Record(d, again, Desc(1280, 720, 3840, 2160)));
        CHECK(fake::evals == evals); // latched: NGX is not called again
    }

    // 9. Shutdown releases everything (including the leaked handle) and every parameter object.
    na::dlss::Shutdown();
    CHECK(fake::Released(h3));
    CHECK(fake::allocs == fake::destroys);
    CHECK(!na::dlss::GetCapabilities().attached);

    // Create failure latches too; unavailable runtime refuses to evaluate.
    REQUIRE(na::dlss::Attach(d.device.Get(), fake::MakeApi()));
    fake::create_result = NVSDK_NGX_Result_FAIL_OutOfDate;
    {
        test::List l; CHECK(!Record(d, l, Desc(1280, 720, 1920, 1080)));
        CHECK(na::dlss::GetCapabilities().latched_off);
    }
    na::dlss::Shutdown();
    fake::create_result = NVSDK_NGX_Result_Success;
    fake::available = false;
    CHECK(!na::dlss::Attach(d.device.Get(), fake::MakeApi()));
    CHECK(na::dlss::GetCapabilities().reason[0] != 0);
    {
        test::List l; CHECK(!Record(d, l, Desc(1280, 720, 1920, 1080)));
    }
    na::dlss::Shutdown();
    CHECK(fake::allocs == fake::destroys);
    d.WaitIdle();
    return test::finish("dlss_backend_test");
}
