// Real D3D12 queues/resources, fake NR model. Proves queue ordering and copied
// input contents, not NVIDIA model quality. CPU waits/readbacks are test-only.
#include "test_support.hpp"
#include "fake_ngx_params.hpp"
#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/observe/producers.hpp"
#include <d3d12sdklayers.h>
#include <cmath>
#include <memory>
#include <vector>

namespace {
constexpr UINT W = 16, H = 12;
test::Device d;
bool fail_evaluate = false;
uint32_t releases_a=0,releases_b=0,destroys_a=0,destroys_b=0,evals_b=0;
struct Witness { test::ComPtr<ID3D12Resource> buffer; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; };
std::vector<Witness> witnesses;
std::vector<std::unique_ptr<test::List>> lists;
std::vector<test::ComPtr<ID3D12Resource>> uploads;
test::FakeParams caps;
int32_t __cdecl Alloc(void** out) { *out = new test::FakeParams; return NVSDK_NGX_Result_Success; }
int32_t __cdecl Destroy(void* p) { ++destroys_a; delete static_cast<test::FakeParams*>(p); return NVSDK_NGX_Result_Success; }
int32_t __cdecl DestroyB(void* p) { ++destroys_b; delete static_cast<test::FakeParams*>(p); return NVSDK_NGX_Result_Success; }
int32_t __cdecl Caps(void** out) { caps.Set("DLSSNR.Available", 1); *out = &caps; return NVSDK_NGX_Result_Success; }
int32_t __cdecl Create(void*, uint32_t feature, void*, void** out) {
    CHECK(feature == 18); *out = reinterpret_cast<void*>(1); return NVSDK_NGX_Result_Success;
}
int32_t __cdecl Release(void*) { ++releases_a; return NVSDK_NGX_Result_Success; }
int32_t __cdecl ReleaseB(void*) { ++releases_b; return NVSDK_NGX_Result_Success; }
int32_t __cdecl Evaluate(void* list, const void*, const void* params, void*) {
    if (fail_evaluate) return static_cast<int32_t>(NVSDK_NGX_Result_Fail);
    auto* p = static_cast<const test::FakeParams*>(params);
    auto* l = static_cast<ID3D12GraphicsCommandList*>(list);
    ID3D12Resource *depth = nullptr, *color = nullptr, *out = nullptr;
    p->Get("DLSSNR.Depth", &depth); p->Get("DLSSNR.Color", &color); p->Get("DLSSNR.Output", &out);
    CHECK(depth && color && out);
    Witness witness;
    UINT64 total = 0;
    const auto desc = depth->GetDesc();
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &witness.fp, nullptr, nullptr, &total);
    witness.buffer = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER b[] = {
        test::Transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        test::Transition(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        test::Transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
    l->ResourceBarrier(3, b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = depth; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = witness.buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = witness.fp;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    l->CopyResource(out, color); // a deterministic stand-in for the NR model
    for (auto& barrier : b) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    l->ResourceBarrier(3, b);
    witnesses.push_back(std::move(witness));
    return NVSDK_NGX_Result_Success;
}
void STDMETHODCALLTYPE Forward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) { q->ExecuteCommandLists(n, l); }
int32_t __cdecl EvaluateB(void* list,const void* feature,const void* params,void* callback) {
    ++evals_b; return Evaluate(list,feature,params,callback);
}
void Drain(ID3D12CommandQueue* q) { test::SignalAndWait(q, d.fence.Get(), d.fence_value, d.event, d.device.Get(), "NR queues"); }

void Upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, float value) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0;
    const auto desc = texture->GetDesc();
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto upload = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* data = nullptr; CHECK(SUCCEEDED(upload->Map(0, nullptr, &data)));
    std::memset(data, 0, static_cast<size_t>(total));
    if (desc.Format == DXGI_FORMAT_R32_FLOAT)
        for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x)
            std::memcpy(static_cast<char*>(data) + y * fp.Footprint.RowPitch + x * sizeof(float), &value, sizeof(float));
    if (desc.Format == DXGI_FORMAT_R16_UNORM) {
        const auto encoded = static_cast<uint16_t>(std::lround(value * 65535.f));
        for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x)
            std::memcpy(static_cast<char*>(data) + y * fp.Footprint.RowPitch + x * sizeof(encoded), &encoded, sizeof(encoded));
    }
    upload->Unmap(0, nullptr);
    auto b = test::Transition(texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    dst.pResource = texture; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter); list->ResourceBarrier(1, &b);
    uploads.push_back(upload);
}
int Input(ID3D12CommandQueue* queue, ID3D12Resource* depth, ID3D12Resource* motion, float value) {
    auto list = std::make_unique<test::List>(); REQUIRE(list->Create(d.device.Get()));
    Upload(list->list.Get(), depth, value); Upload(list->list.Get(), motion, 0.f);
    na::nr::EvalInputs in{}; in.depth = depth; in.motion = motion;
    in.render_w = static_cast<uint32_t>(depth->GetDesc().Width); in.render_h = depth->GetDesc().Height;
    in.out_w = W; in.out_h = H; in.mv_scale_x = in.mv_scale_y = 1.f;
    na::nr::OnEvaluate(list->list.Get(), in);
    REQUIRE(SUCCEEDED(list->list->Close()));
    ID3D12CommandList* submitted[] = {list->list.Get()};
    na::nr::Execute(queue, 1, submitted, Forward);
    lists.push_back(std::move(list));
    return 0;
}
void CheckWitness(size_t index, float value) {
    CHECK(index < witnesses.size()); if (index >= witnesses.size()) return;
    auto& w = witnesses[index]; void* data = nullptr;
    CHECK(SUCCEEDED(w.buffer->Map(0, nullptr, &data)));
    bool good = true;
    for (UINT y = 0; y < w.fp.Footprint.Height; ++y) for (UINT x = 0; x < w.fp.Footprint.Width; ++x) {
        float actual; std::memcpy(&actual, static_cast<char*>(data) + y * w.fp.Footprint.RowPitch + x * sizeof(float), sizeof(float));
        good &= std::fabs(actual - value) < 0.0001f;
    }
    w.buffer->Unmap(0, nullptr);
    CHECK(good); std::printf("depth witness %zu: expected %.2f, %s\n", index, value, good ? "all pixels match" : "MISMATCH");
}
}

int main(int argc, char** argv) {
    test::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    if (!test::CreateDevice(d)) return test::SkipNoDevice("nr_queue_test");
    test::ComPtr<ID3D12InfoQueue> info; d.device.As(&info);
    test::ComPtr<ID3D12CommandQueue> output, alternate;
    D3D12_COMMAND_QUEUE_DESC q{};
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q, IID_PPV_ARGS(&output))));
    REQUIRE(SUCCEEDED(d.device->CreateCommandQueue(&q, IID_PPV_ARGS(&alternate))));
    auto depth = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
    auto motion = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
    auto back = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(depth && motion && back);
    na::nr::Runtime rt{reinterpret_cast<void*>(Create), reinterpret_cast<void*>(Evaluate), reinterpret_cast<void*>(Release),
        reinterpret_cast<void*>(Alloc), reinterpret_cast<void*>(Destroy), reinterpret_cast<void*>(Caps)};
    na::settings::Get().nr_enabled = 1; na::nr::SetRuntime(rt);
    auto present = [&](ID3D12CommandQueue* queue) { na::nr::OnPresent(queue, back.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709); };
    if (argc > 1 && std::strcmp(argv[1], "--runtime-owner") == 0) {
        REQUIRE(Input(d.queue.Get(),depth.Get(),motion.Get(),.1f)==0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get()); // runtime A created feature/params
        auto rt_b=rt;
        rt_b.evaluate_feature=reinterpret_cast<void*>(EvaluateB);
        rt_b.release_feature=reinterpret_cast<void*>(ReleaseB);
        rt_b.destroy_parameters=reinterpret_cast<void*>(DestroyB);
        na::nr::SetRuntime(rt_b); // attaching another core cannot transfer old objects
        REQUIRE(Input(d.queue.Get(),depth.Get(),motion.Get(),.25f)==0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        CHECK(evals_b==0); CheckWitness(0,.25f);
        na::settings::Get().nr_enabled=0; present(output.Get());
        CHECK(releases_a==1 && releases_b==0 && destroys_a==1 && destroys_b==0);
        na::settings::Get().nr_enabled=1; na::nr::ResetLatch();
        REQUIRE(Input(d.queue.Get(),depth.Get(),motion.Get(),.5f)==0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        REQUIRE(Input(d.queue.Get(),depth.Get(),motion.Get(),.75f)==0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        CHECK(evals_b==1); CheckWitness(1,.75f);
        na::nr::Shutdown();
        CHECK(releases_a==1 && releases_b==1 && destroys_a==1 && destroys_b==1);
        REQUIRE(d.device->GetDeviceRemovedReason()==S_OK);
        return test::finish("nr_runtime_owner_test");
    }
    if (argc > 1 && std::strcmp(argv[1], "--retry-init") == 0) {
        na::nr::FailNextDeviceInitializationForTests();
        REQUIRE(Input(d.queue.Get(),depth.Get(),motion.Get(),.1f)==0); Drain(d.queue.Get());
        CHECK(na::nr::GetStats().latched_off==1);
        CHECK(na::nr::GetStats().inputs_seen==0);
        na::nr::ResetLatch();
        REQUIRE(Input(d.queue.Get(),depth.Get(),motion.Get(),.25f)==0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        REQUIRE(Input(d.queue.Get(),depth.Get(),motion.Get(),.75f)==0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        CHECK(na::nr::GetStats().active==1 && na::nr::GetStats().latched_off==0);
        CheckWitness(0,.75f);
        na::nr::Shutdown();
        REQUIRE(d.device->GetDeviceRemovedReason()==S_OK);
        return test::finish("nr_retry_init_test");
    }
    if (argc > 1 && std::strcmp(argv[1], "--repeated-recording") == 0) {
        REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .1f) == 0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get()); // create the fake feature
        auto small_depth16 = test::CreateTexture(d.device.Get(), W / 2, H / 2, DXGI_FORMAT_R16_UNORM,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
        auto large = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R16_UNORM,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
        auto other = test::CreateTexture(d.device.Get(), W, H, DXGI_FORMAT_R16_UNORM,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
        REQUIRE(small_depth16 && large && other);
        test::List recording; REQUIRE(recording.Create(d.device.Get()));
        na::nr::EvalInputs in{}; in.motion = motion.Get(); in.out_w = W; in.out_h = H;
        in.mv_scale_x = in.mv_scale_y = 1.f;
        Upload(recording.list.Get(), motion.Get(), 0.f);
        Upload(recording.list.Get(), small_depth16.Get(), .25f);
        in.depth = small_depth16.Get(); in.render_w = W / 2; in.render_h = H / 2;
        na::nr::OnEvaluate(recording.list.Get(), in);
        Upload(recording.list.Get(), large.Get(), .75f);
        in.depth = large.Get(); in.render_w = W; in.render_h = H;
        na::nr::OnEvaluate(recording.list.Get(), in);
        REQUIRE(SUCCEEDED(recording.list->Close()));
        // Leave both copies unsubmitted while other lists recycle more than
        // the old 64-table conversion ring. Their descriptors must stay intact.
        for (int i = 0; i < 80; ++i) {
            REQUIRE(Input(d.queue.Get(), other.Get(), motion.Get(), .4f) == 0);
            Drain(d.queue.Get());
        }
        ID3D12CommandList* submitted[] = {recording.list.Get()};
        na::nr::Execute(d.queue.Get(), 1, submitted, Forward);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        CHECK(na::nr::GetStats().inputs_seen == 83);
        CHECK(na::nr::GetStats().input_submitted == 83);
        CHECK(na::nr::GetStats().skipped == 0);
        CheckWitness(0, .75f); // newest evaluate in actual list order
        na::nr::Shutdown();
        REQUIRE(d.device->GetDeviceRemovedReason() == S_OK);
        if (info) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); info->GetMessage(i, message, &size);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::printf("D3D12: %s\n", message->pDescription); CHECK(false); }
        }
        return test::finish("nr_repeated_recording_test");
    }
    if (argc > 1 && std::strcmp(argv[1], "--discard-recording") == 0) {
        REQUIRE(na::observe::InstallListResetTracking(d.device.Get(), na::nr::OnCommandListReset));
        // More than the entire slot pool: neither discarded lists nor reset
        // lists may exhaust it or publish copies that were never executed.
        for (int i = 0; i < 40; ++i) {
            test::List recording; REQUIRE(recording.Create(d.device.Get()));
            na::nr::EvalInputs in{}; in.depth = depth.Get(); in.motion = motion.Get();
            in.render_w = in.out_w = W; in.render_h = in.out_h = H;
            in.mv_scale_x = in.mv_scale_y = 1.f;
            na::nr::OnEvaluate(recording.list.Get(), in);
            REQUIRE(SUCCEEDED(recording.list->Close()));
            if ((i % 2) == 0) {
                REQUIRE(SUCCEEDED(recording.list->Reset(recording.alloc.Get(), nullptr)));
                REQUIRE(SUCCEEDED(recording.list->Close()));
                ID3D12CommandList* submitted[] = {recording.list.Get()};
                na::nr::Execute(d.queue.Get(), 1, submitted, Forward);
                Drain(d.queue.Get());
                CHECK(na::nr::GetStats().input_submitted == 0);
            } // odd recordings die without execution
        }
        CHECK(na::nr::GetStats().inputs_seen == 40);
        CHECK(na::nr::GetStats().skipped == 0);
        REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .1f) == 0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .2f) == 0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        CHECK(na::nr::GetStats().active == 1);
        CheckWitness(0, .2f);
        na::nr::Shutdown();
        return test::finish("nr_discard_recording_test");
    }
    if (argc > 1) {
        REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .1f) == 0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .2f) == 0);
        Drain(d.queue.Get()); present(output.Get()); Drain(output.Get());
        CHECK(na::nr::GetStats().active == 1);
        na::settings::Get().nr_enabled = 0; present(output.Get());
        na::settings::Get().nr_enabled = 1; na::nr::ResetLatch();
        // Older completed input must remain usable while the newest one is
        // gated by the original Present. This is the actual off/on lifecycle.
        REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .3f) == 0);
        Drain(d.queue.Get());
        // Native FG can release the next render submission only through work
        // queued by the original Present, AFTER our Present callback returns.
        test::ComPtr<ID3D12Fence> released, finished;
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&released))));
        REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finished))));
        REQUIRE(SUCCEEDED(d.queue->Wait(released.Get(), 1)));
        REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .25f) == 0);
        present(output.Get());
        REQUIRE(SUCCEEDED(output->Signal(released.Get(), 1))); // original Present
        REQUIRE(SUCCEEDED(output->Signal(finished.Get(), 1)));
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        auto result = test::WaitFence(finished.Get(), 1, ev, 100);
        CHECK(result == test::WaitResult::Ok);
        std::printf("Present dependency: %s; NR submitted=%llu\n", test::WaitResultName(result), na::nr::GetStats().gpu_submitted);
        released->Signal(1); // bounded cleanup even on the old implementation
        Drain(d.queue.Get()); Drain(output.Get()); CloseHandle(ev);
        present(output.Get()); Drain(output.Get());
        CHECK(na::nr::GetStats().active == 1 && na::nr::GetStats().evaluates == 2);
        CheckWitness(0, .2f); CheckWitness(1, .25f);
        na::nr::Shutdown();
        return test::finish("nr_present_dependency_test");
    }
    REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .1f) == 0);
    Drain(d.queue.Get()); present(output.Get()); // feature creation
    Drain(d.queue.Get()); Drain(output.Get());
    REQUIRE(!na::nr::GetStats().latched_off);

    test::ComPtr<ID3D12Fence> gate, probe;
    HANDLE probe_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(probe_event);
    REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
    REQUIRE(SUCCEEDED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&probe))));
    // A late producer: NR defers without queuing a wait on the present queue.
    REQUIRE(SUCCEEDED(d.queue->Wait(gate.Get(), 1)));
    REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .25f) == 0); present(output.Get());
    REQUIRE(SUCCEEDED(output->Signal(probe.Get(), 1)));
    auto wait = test::WaitFence(probe.Get(), 1, probe_event, 60);
    CHECK(wait == test::WaitResult::Ok); // pending input must not stall Present
    REQUIRE(SUCCEEDED(gate->Signal(1))); Drain(d.queue.Get()); present(output.Get()); Drain(output.Get()); CheckWitness(0, .25f);

    // A deliberately late consumer: the next producer must finish without
    // overwriting the snapshot NR will read. This also avoids an FG dependency
    // cycle when the presentation queue itself waits for that next producer.
    REQUIRE(SUCCEEDED(output->Wait(probe.Get(), 2))); // game/FG dependency on a future producer submission
    REQUIRE(SUCCEEDED(output->Wait(gate.Get(), 2)));
    REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .5f) == 0); Drain(d.queue.Get()); present(output.Get());
    REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .75f) == 0);
    REQUIRE(SUCCEEDED(d.queue->Signal(probe.Get(), 2)));
    ResetEvent(probe_event);
    wait = test::WaitFence(probe.Get(), 2, probe_event, 1000); CHECK(wait == test::WaitResult::Ok);
    REQUIRE(SUCCEEDED(gate->Signal(2))); Drain(output.Get()); Drain(d.queue.Get()); CheckWitness(1, .5f);
    present(output.Get()); Drain(output.Get()); CheckWitness(2, .75f);

    // Both queues may change. Their previously submitted work remains ordered.
    REQUIRE(Input(alternate.Get(), depth.Get(), motion.Get(), .9f) == 0); Drain(alternate.Get()); present(d.queue.Get());
    Drain(alternate.Get()); Drain(d.queue.Get()); CheckWitness(3, .9f);
    REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .6f) == 0); present(d.queue.Get()); Drain(d.queue.Get()); CheckWitness(4, .6f);
    const auto waits_before = na::nr::GetStats().cross_queue_waits;
    REQUIRE(Input(d.queue.Get(), depth.Get(), motion.Get(), .4f) == 0); present(d.queue.Get()); Drain(d.queue.Get()); CheckWitness(5, .4f);
    CHECK(na::nr::GetStats().cross_queue_waits == waits_before); // steady same-queue fast path
    const auto before_idle = na::nr::GetStats(); present(d.queue.Get());
    CHECK(na::nr::GetStats().evaluates == before_idle.evaluates);
    CHECK(na::nr::GetStats().active == before_idle.active); // generated presents keep a stable status

    // Scaled modes (including Ultra Performance) use the same synchronized inputs.
    auto small_depth = test::CreateTexture(d.device.Get(), W / 2, H / 2, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
    auto small_motion = test::CreateTexture(d.device.Get(), W / 2, H / 2, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(Input(alternate.Get(), small_depth.Get(), small_motion.Get(), .3f) == 0); Drain(alternate.Get()); present(output.Get());
    Drain(alternate.Get()); Drain(output.Get()); CheckWitness(6, .3f);

    // Even failure recovery must defer the pass until producer completion.
    fail_evaluate = true;
    REQUIRE(SUCCEEDED(alternate->Wait(gate.Get(), 3)));
    REQUIRE(Input(alternate.Get(), small_depth.Get(), small_motion.Get(), .8f) == 0); present(output.Get());
    REQUIRE(SUCCEEDED(output->Signal(probe.Get(), 3))); ResetEvent(probe_event);
    wait = test::WaitFence(probe.Get(), 3, probe_event, 60); CHECK(wait == test::WaitResult::Ok);
    REQUIRE(SUCCEEDED(gate->Signal(3))); Drain(alternate.Get()); present(output.Get()); Drain(output.Get());
    auto s = na::nr::GetStats();
    CHECK(s.latched_off && s.evaluates == 7 && s.gpu_submitted == 9 && s.gpu_completed == 9);
    CHECK(s.input_submitted == s.input_completed && s.cross_queue_waits == 0);
    std::printf("NR evals=%llu submitted/completed=%llu/%llu input=%llu/%llu cross_queue_waits=%llu\n",
        s.evaluates, s.gpu_submitted, s.gpu_completed, s.input_submitted, s.input_completed, s.cross_queue_waits);
    na::nr::Shutdown(); lists.clear(); uploads.clear(); witnesses.clear();
    CloseHandle(probe_event);
    REQUIRE(d.device->GetDeviceRemovedReason() == S_OK);
    if (info) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); info->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { std::printf("D3D12: %s\n", message->pDescription); CHECK(false); }
    }
    return test::finish("nr_queue_test");
}
