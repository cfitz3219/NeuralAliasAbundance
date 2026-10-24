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

#include <algorithm>
#include <atomic>
#include <thread>
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
    na::dlss::BeforeExecute(q, 1, lists); // what the host's execute detour does, before
    q->ExecuteCommandLists(1, lists);
    na::dlss::OnExecuted(q, 1, lists);    // ... and after the original call
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
        // ARGON-G2-009: normalised motion at OUTPUT resolution scales by the output extent.
        f = GoodFrame(); f.motion.extent = f.output; // normalized_current_to_previous
        CHECK(na::dlss::FromFrame(f, r, Quality::MaxQuality, &e, &why));
        CHECK(e.mv_display_res && e.mv_scale_x == 1920.f && e.mv_scale_y == 1080.f);
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
    CHECK(fake::last_create_injector && fake::last_eval_injector);
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

    // ---- Gate 2 regressions (Astra R-301, R-303, R-305) ----
    fake::available = true;
    REQUIRE(na::dlss::Attach(d.device.Get(), fake::MakeApi()));
    D3D12_COMMAND_QUEUE_DESC q2d{}; q2d.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    test::ComPtr<ID3D12CommandQueue> q2;
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q2d, IID_PPV_ARGS(&q2))));
    test::ComPtr<ID3D12Fence> q2_fence;
    REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&q2_fence))));
    uint64_t q2_value = 0;
    const auto kick = [&](ID3D12CommandQueue* q) {
        test::List k; if (!k.Create(d.device.Get())) return; k.list->Close();
        ID3D12CommandList* ls[] = {k.list.Get()};
        q->ExecuteCommandLists(1, ls);
        na::dlss::OnExecuted(q, 1, ls);
    };
    const auto view_desc = [](uint64_t view, uint32_t ow, uint32_t oh) {
        auto e = Desc(1280, 720, ow, oh); e.view_id = view; return e;
    };
    std::vector<test::List> keep; // lists recorded but deliberately never executed stay alive
    keep.reserve(64);

    // R-301: two uses of ONE feature, the earlier held on queue A, the later
    // submitted on queue B (which, by R-303, waits for A). Resizing while both
    // are outstanding must not release the feature.
    {
        test::ComPtr<ID3D12Fence> hold2;
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold2))));
        test::List la, lb;
        REQUIRE(Record(d, la, view_desc(1, 1920, 1080)));
        const uintptr_t hf = fake::last_eval_handle;
        REQUIRE(SUCCEEDED(d.queue->Wait(hold2.Get(), 1)));
        Submit(d.queue.Get(), la);
        REQUIRE(Record(d, lb, view_desc(1, 1920, 1080)));
        CHECK(fake::last_eval_handle == hf); // same feature
        Submit(q2.Get(), lb);
        kick(q2.Get());
        keep.emplace_back();
        REQUIRE(Record(d, keep.back(), view_desc(1, 2560, 1440))); // resize -> hf retired
        CHECK(!fake::Released(hf));                                 // both uses still pending
        CHECK(na::dlss::GetStats().features_pending == 1);
        REQUIRE(SUCCEEDED(hold2->Signal(1)));
        test::SignalAndWait(q2.Get(), q2_fence.Get(), q2_value, d.event, d.device.Get(), "queue B");
        d.WaitIdle();
        kick(d.queue.Get());
        CHECK(fake::Released(hf));
    }
    // R-301: an earlier ABANDONED use stays sticky after a later tracked use completes.
    {
        keep.emplace_back();
        REQUIRE(Record(d, keep.back(), view_desc(2, 1920, 1080))); // never executed
        const uintptr_t hf = fake::last_eval_handle;
        for (int i = 0; i < 600; ++i) kick(d.queue.Get()); // the unexecuted record is abandoned
        test::List lt; // Lost completion proof requires a new independent feature
        REQUIRE(Record(d, lt, view_desc(2, 1920, 1080)));
        CHECK(fake::last_eval_handle != hf);
        Submit(d.queue.Get(), lt);
        d.WaitIdle();
        kick(d.queue.Get());
        const uint64_t leaked = na::dlss::GetStats().features_leaked;
        test::List lr;
        REQUIRE(Record(d, lr, view_desc(2, 2560, 1440)));
        Submit(d.queue.Get(), lr);
        d.WaitIdle();
        kick(d.queue.Get());
        CHECK(!fake::Released(hf));
        CHECK(na::dlss::GetStats().features_leaked == leaked);
    }
    // Completion capacity exhaustion refuses BEFORE NGX records an untracked use.
    {
        keep.emplace_back();
        REQUIRE(keep.back().Create(d.device.Get()));
        bool refused = false;
        for (int i = 0; i < 40; ++i) {
            const int evals = fake::evals;
            if (!na::dlss::Evaluate(keep.back().list.Get(), view_desc(3, 1920, 1080))) {
                CHECK(fake::evals == evals);
                refused = true;
                break;
            }
        }
        CHECK(refused && na::dlss::GetStats().untracked == 0);
        keep.back().list->Close();
        for (int i = 0; i < 600; ++i) kick(d.queue.Get());
    }
    // Each scenario owns a fresh backend; abandoned views must not consume the
    // independent-view or DRS scenario's finite capacity.
    d.WaitIdle();
    na::dlss::Shutdown();
    REQUIRE(na::dlss::Attach(d.device.Get(), fake::MakeApi()));

    // R-303: independent views keep independent features and histories.
    {
        test::List v1a, v2a, v1b;
        REQUIRE(Record(d, v1a, view_desc(11, 1920, 1080)));
        const uintptr_t h_v1 = fake::last_eval_handle;
        REQUIRE(Record(d, v2a, view_desc(12, 1920, 1080)));
        const uintptr_t h_v2 = fake::last_eval_handle;
        CHECK(h_v1 != h_v2);
        CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1); // new view starts fresh
        Submit(d.queue.Get(), v1a);
        d.WaitIdle();
        REQUIRE(Record(d, v1b, view_desc(11, 1920, 1080)));
        CHECK(fake::last_eval_handle == h_v1);
        CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0); // its own history continues
        ID3D12CommandList* ls[] = {v2a.list.Get(), v1b.list.Get()};
        na::dlss::BeforeExecute(d.queue.Get(), 2, ls);
        d.queue->ExecuteCommandLists(2, ls);
        na::dlss::OnExecuted(d.queue.Get(), 2, ls);
        d.WaitIdle();
        na::dlss::ReleaseView(11); // its source was destroyed
        kick(d.queue.Get());
        CHECK(fake::Released(h_v1));
        test::List v3;
        REQUIRE(Record(d, v3, view_desc(13, 1920, 1080))); // a recreated source gets a new id
        CHECK(fake::last_eval_handle != h_v1 && fake::last_eval_handle != h_v2);
        CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1);
        Submit(d.queue.Get(), v3);
        d.WaitIdle();
        // R-303: the same view submitted on another queue while its previous
        // frame is still held: the second queue WAITS on the GPU for it.
        {
            test::ComPtr<ID3D12Fence> hold4;
            REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold4))));
            test::List w1, w2;
            REQUIRE(Record(d, w1, view_desc(14, 1920, 1080)));
            REQUIRE(SUCCEEDED(d.queue->Wait(hold4.Get(), 1)));
            Submit(d.queue.Get(), w1);                      // A: held on queue 1
            REQUIRE(Record(d, w2, view_desc(14, 1920, 1080)));
            CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0); // history continues, in order
            const uint64_t waits = na::dlss::GetStats().queue_waits;
            Submit(q2.Get(), w2);                           // B: queue 2
            CHECK(na::dlss::GetStats().queue_waits == waits + 1);
            ++q2_value;
            REQUIRE(SUCCEEDED(q2->Signal(q2_fence.Get(), q2_value)));
            Sleep(50);
            CHECK(q2_fence->GetCompletedValue() < q2_value); // B cannot finish before A
            REQUIRE(SUCCEEDED(hold4->Signal(1)));
            CHECK(test::WaitFence(q2_fence.Get(), q2_value, d.event, 10000) == test::WaitResult::Ok);
            d.WaitIdle();
        }
        // R-303: independently recorded lists may submit in reverse order.
        // They must NEVER share a mutable NGX feature while order is unknown.
        {
            test::List a, b;
            REQUIRE(Record(d, a, view_desc(16, 1920, 1080)));
            const uintptr_t ha = fake::last_eval_handle;
            REQUIRE(Record(d, b, view_desc(16, 1920, 1080)));
            const uintptr_t hb = fake::last_eval_handle;
            CHECK(hb != ha);
            CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1);
            test::ComPtr<ID3D12Fence> held;
            REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held))));
            REQUIRE(SUCCEEDED(d.queue->Wait(held.Get(), 1)));
            Submit(d.queue.Get(), b); // later recorded frame submitted first, held
            Submit(q2.Get(), a);
            test::SignalAndWait(q2.Get(), q2_fence.Get(), q2_value, d.event, d.device.Get(), "record-ahead independent feature");
            CHECK(!fake::Released(hb));
            REQUIRE(SUCCEEDED(held->Signal(1)));
            d.WaitIdle();
            test::List next;
            REQUIRE(Record(d, next, view_desc(16, 1920, 1080)));
            CHECK(fake::last_eval_handle == hb);
            CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
            Submit(d.queue.Get(), next);
            d.WaitIdle();
        }
        // Work from another device is refused before NGX sees it.
        test::ComPtr<IDXGIAdapter> warp;
        test::ComPtr<ID3D12Device> other_dev;
        if (SUCCEEDED(d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
            SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other_dev))) &&
            other_dev.Get() != d.device.Get()) {
            test::List lo;
            REQUIRE(lo.Create(other_dev.Get()));
            const int evals = fake::evals;
            CHECK(!na::dlss::Evaluate(lo.list.Get(), view_desc(15, 1920, 1080)));
            CHECK(fake::evals == evals);
            CHECK(std::strstr(na::dlss::GetStats().reason, "different graphics device") != nullptr);
            lo.list->Close();
        } else {
            std::printf("device-mismatch check skipped: no second distinct D3D12 device here\n");
        }
    }

    // R-305: when every feature slot is owned, a new feature is refused; no
    // handle is ever dropped, and every created handle is released exactly once.
    {
        test::ComPtr<ID3D12Fence> hold3;
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold3))));
        REQUIRE(SUCCEEDED(d.queue->Wait(hold3.Get(), 1)));
        std::vector<test::List> held(24);
        int refused = 0;
        for (uint32_t i = 0; i < 24; ++i) {
            if (!Record(d, held[i], view_desc(20, 1920 + 32 * i, 1080 + 18 * i))) { ++refused; continue; }
            Submit(d.queue.Get(), held[i]);
        }
        CHECK(refused > 0);
        CHECK(na::dlss::GetStats().refused_busy > 0);
        CHECK(!na::dlss::GetCapabilities().latched_off); // capacity is transient, not a failure
        REQUIRE(SUCCEEDED(hold3->Signal(1)));
        d.WaitIdle();
        kick(d.queue.Get());
    }
    d.WaitIdle();
    na::dlss::Shutdown();
    REQUIRE(na::dlss::Attach(d.device.Get(), fake::MakeApi()));
    // ARGON-G2-001: dynamic resolution up AND down within the stated maximum
    // keeps one feature and one history (reset only on the first frame).
    {
        const uint64_t created = na::dlss::GetStats().features_created;
        const uint32_t widths[10] = {960, 1000, 1114, 1200, 1280, 1180, 1050, 1280, 990, 1114};
        for (int i = 0; i < 10; ++i) {
            auto e = Desc(widths[i], widths[i] * 9 / 16, 1920, 1080);
            e.view_id = 30;
            e.max_render_w = 1280; e.max_render_h = 720;
            e.quality = na::dlss::QualityForScale(1280, 720, 1920, 1080); // from the maximum, fixed per source
            test::List l;
            REQUIRE(Record(d, l, e));
            CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == (i == 0 ? 1 : 0));
            CHECK(fake::U(fake::last_eval, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) == widths[i]);
            if (i == 0) CHECK(fake::U(fake::last_create, NVSDK_NGX_Parameter_Width) == 1280); // created at the maximum
            Submit(d.queue.Get(), l);
            d.WaitIdle();
        }
        CHECK(na::dlss::GetStats().features_created == created + 1);
        auto too_big = Desc(1300, 731, 1920, 1080);
        too_big.max_render_w = 1280; too_big.max_render_h = 720;
        CHECK(na::dlss::Validate(too_big) != nullptr);
    }
    for (int i = 0; i < 600; ++i) kick(d.queue.Get());
    d.WaitIdle();
    na::dlss::Shutdown();
    // ARGON-G2-004: one start-up transaction for every producer.
    {
        static std::atomic<int> inits{0};
        static ID3D12Device* attach_to = nullptr;
        attach_to = d.device.Get();
        na::dlss::SetInitOwnOverrideForTests([](ID3D12Device*, const wchar_t*) {
            inits.fetch_add(1);
            Sleep(20); // widen the window a racing producer would need
            return na::dlss::Attach(attach_to, fake::MakeApi());
        });
        // Two producers starting at once: exactly one start-up.
        std::thread p1([&] { na::dlss::EnsureOn(d.device.Get(), L"."); });
        std::thread p2([&] { na::dlss::EnsureOn(d.device.Get(), L"."); });
        p1.join();
        p2.join();
        CHECK(inits.load() == 1);
        CHECK(na::dlss::EnsureOn(d.device.Get(), L".")); // already running here
        // Another LIVE device never takes DLSS over.
        ID3D12Device* foreign = reinterpret_cast<ID3D12Device*>(static_cast<uintptr_t>(0x1234));
        na::dlss::SetDeviceRemovedProbeForTests([](ID3D12Device*) { return false; });
        CHECK(!na::dlss::EnsureOn(foreign, L"."));
        CHECK(na::dlss::GetCapabilities().attached && inits.load() == 1);
        // Deterministic recovery interleaving: simulate removal until the first
        // producer's initializer establishes the replacement generation.
        static std::atomic<bool> removed{true};
        na::dlss::SetDeviceRemovedProbeForTests([](ID3D12Device*) { return removed.load(); });
        na::dlss::SetInitOwnOverrideForTests([](ID3D12Device* device, const wchar_t*) {
            inits.fetch_add(1);
            Sleep(20);
            const bool ok = na::dlss::Attach(device, fake::MakeApi());
            removed.store(false);
            return ok;
        });
        bool recovered1 = false, recovered2 = false;
        std::thread r1([&] { recovered1 = na::dlss::EnsureOn(d.device.Get(), L"."); });
        std::thread r2([&] { recovered2 = na::dlss::EnsureOn(d.device.Get(), L"."); });
        r1.join(); r2.join();
        CHECK(recovered1 && recovered2 && inits.load() == 2);
        CHECK(na::dlss::IsBoundTo(d.device.Get()));
        na::dlss::SetDeviceRemovedProbeForTests(nullptr);
        na::dlss::SetInitOwnOverrideForTests(nullptr);
    }
    na::dlss::Shutdown();
    {
        // Every handle the fake ever handed out was released exactly once.
        std::vector<uintptr_t> rel = fake::released;
        std::sort(rel.begin(), rel.end());
        CHECK(std::adjacent_find(rel.begin(), rel.end()) == rel.end());
        CHECK(rel.size() == static_cast<size_t>(fake::next_handle - 0x1000));
    }
    CHECK(fake::allocs == fake::destroys);
    return test::finish("dlss_backend_test");
}
