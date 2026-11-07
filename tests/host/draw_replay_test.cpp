#include "test_support.hpp"
#include "host_dx12/j2/replay_motion.hpp"

#include <chrono>
#include <cmath>
#include <d3dcompiler.h>
#include <thread>

namespace {
using na::observe::ReplayUse;
struct Use final : ReplayUse {
    na::nr::native::RecordingLease lease;
    bool Ready(ID3D12CommandQueue* queue) const override { return lease.Ready(queue); }
    bool Retired() const override { return lease.Retired(); }
    bool Completed() const override { return lease.Completed(); }
};
std::shared_ptr<ReplayUse> retain(ID3D12GraphicsCommandList* list)
{
    static auto marker = std::make_shared<int>();
    auto use = std::make_shared<Use>();
    use->lease = na::nr::native::RetainRecording(list, marker);
    return use->lease ? use : nullptr;
}
void __cdecl sink(const na_event_batch*, void*) {}
}

int main()
{
    test::ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    test::Device d;
    if (!test::CreateDevice(d)) return test::kSkip;
    auto* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), false));
    na::observe::SetRasterStateTracking(true);
    na_observe_config config{};
    config.size_bytes = sizeof config;
    REQUIRE(na_observe_start(&config, &sink, nullptr) == NA_OK);

    constexpr UINT w = 32, h = 24;
    constexpr auto depth_write = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    constexpr auto npsr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto copy_source = D3D12_RESOURCE_STATE_COPY_SOURCE;
    const auto depth = test::CreateTexture(dev, w, h, DXGI_FORMAT_R32_TYPELESS, depth_write,
                                           D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    REQUIRE(depth);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    test::ComPtr<ID3D12DescriptorHeap> heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))));
    const auto dsv = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_D32_FLOAT;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(depth.Get(), &view, dsv);
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()), 0);

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 1;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &param;
    test::ComPtr<ID3DBlob> bytes, vs;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, nullptr)));
    test::ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&root))));
    const char shader[] = R"(
cbuffer C : register(b0) { float shift; }
float4 vs(uint id : SV_VertexID) : SV_Position {
    const float2 verts[3] = { float2(-.5,-.65), float2(-.5,.65), float2(.6,.65) };
    return float4(verts[id] + float2(shift,0), .5, 1);
})";
    REQUIRE(SUCCEEDED(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0",
                                 0, 0, &vs, nullptr)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    test::ComPtr<ID3D12PipelineState> pso;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso))));

    na::j2::replay_motion::Pass pass;
    {
        const na::observe::HostWorkScope host;
        REQUIRE(pass.Init(dev));
    }
    test::List list, other;
    REQUIRE(list.Create(dev) && other.Create(dev));
    constexpr UINT pitch = 256;
    auto rb = test::CreateBuffer(dev, 2 * h * pitch, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(rb);

    using na::j2::replay_motion::Match;
    const na::observe::ReplayRecord draw{1, 2, 0, 3, w, h, {0, 0, 14, h}};
    CHECK(Match({draw}, {draw}, w, h).size() == 1);
    CHECK(Match({draw, draw}, {draw}, w, h).empty());
    CHECK(Match({draw}, {draw, draw}, w, h).empty());
    CHECK(Match({draw}, {draw}, w - 1, h).empty());

    bool verified = false;
    uint32_t discarded_frame = 0;
    for (uint32_t frame = 2; frame < 20 && !verified; ++frame) {
        if (frame != 2) {
            na::nr::native::Reset(list.list.Get());
            list.Reset();
        }
        const float jitter[2] = { frame % 2 ? .375f : -.25f, frame % 2 ? -.375f : .25f };
        na::observe::SetSceneViewportJitter(frame, jitter[0], jitter[1]);
        na::observe::SetDrawReplay(frame == 2 ? 0 : frame, retain);
        const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        const D3D12_RECT sc{0, 0, 14, h};
        // Nothing replayed yet this frame: no list can take it. (Once a draw is
        // replayed, any list may take it: an unsubmitted writer is checked on
        // the GPU by its stamp, see the cross-list case below.)
        na::observe::ReplayFrame wrong;
        CHECK(!na::observe::ReplayTakeFrame(other.list.Get(), d.queue.Get(), &wrong));
        list.list->SetGraphicsRootSignature(root.Get());
        const float shift = float(frame % 2) / (w / 2.f);
        list.list->SetGraphicsRoot32BitConstants(0, 1, &shift, 0);
        list.list->SetPipelineState(pso.Get());
        list.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        list.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        list.list->RSSetViewports(1, &vp);
        list.list->RSSetScissorRects(1, &sc);
        list.list->DrawInstanced(3, 1, 0, 0);
        const D3D12_RECT occluded{13, 7, 14, 8};
        list.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, .25f, 0, 1, &occluded);
        auto to_read = test::Transition(depth.Get(), depth_write, npsr);
        list.list->ResourceBarrier(1, &to_read);
        const na::j2::replay_motion::Output output = [&] {
            const na::observe::HostWorkScope host;
            return pass.Record(list.list.Get(), d.queue.Get(), depth.Get(), w, h, false, false, jitter);
        }();
        if (frame == 2) {
            na::observe::DrawReplayStats stats{};
            na::observe::GetDrawReplayStats(&stats);
            CHECK(!output.motion && stats.replayed == 0);
        }
        if (output.motion) {
            ID3D12Resource* textures[] = {output.motion, output.valid};
            for (UINT i = 0; i < 2; ++i) {
                auto barrier = test::Transition(textures[i], npsr, copy_source);
                list.list->ResourceBarrier(1, &barrier);
                D3D12_TEXTURE_COPY_LOCATION src{textures[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                dst.PlacedFootprint.Offset = uint64_t(i) * h * pitch;
                dst.PlacedFootprint.Footprint = {i ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32_FLOAT, w, h, 1, pitch};
                list.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                list.list->ResourceBarrier(1, &barrier);
            }
        }
        std::swap(to_read.Transition.StateBefore, to_read.Transition.StateAfter);
        list.list->ResourceBarrier(1, &to_read);
        REQUIRE(SUCCEEDED(list.list->Close()));
        if (!discarded_frame && pass.GetStats().frames) {
            // CPU-side capture records existed, but this list never reached the queue.
            discarded_frame = frame;
            CHECK(!output.motion);
            continue;
        }
        if (discarded_frame && frame == discarded_frame + 1) CHECK(!output.motion);
        ID3D12CommandList* commands[] = {list.list.Get()};
        {
            na::nr::native::Submission submitted(d.queue.Get(), 1, commands);
            d.queue->ExecuteCommandLists(1, commands);
        }
        d.WaitIdle();
        if (output.motion) {
            std::byte* mapped = nullptr;
            REQUIRE(SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
            const auto* motion = reinterpret_cast<const float*>(mapped + 7 * pitch);
            const auto* valid = reinterpret_cast<const float*>(mapped + (h + 7) * pitch);
            CHECK(valid[12] == 1.f && valid[0] == 0.f && valid[13] == 0.f && valid[15] == 0.f);
            CHECK(std::abs(motion[24] - (frame % 2 ? -1.f : 1.f)) < 1e-3f);
            CHECK(std::abs(motion[25]) < 1e-3f);
            std::printf("motion at (12,7): %.6f %.6f valid %.0f, outside %.0f\n",
                        motion[24], motion[25], valid[12], valid[0]);
            rb->Unmap(0, nullptr);
            verified = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(discarded_frame != 0 && verified);

    // SE5 keeps each command list for several frames before resetting it.
    // A slot must be reusable once the GPU is done, not once the game resets
    // the list that wrote it (that blocked almost every frame: not_ready=39.6M).
    {
        constexpr uint32_t kRing = 4;
        test::List ring[kRing];
        for (auto& r : ring) { // observed from a Reset onwards, like a game list
            REQUIRE(r.Create(dev) && SUCCEEDED(r.list->Close()));
            na::nr::native::Reset(r.list.Get());
            r.Reset();
        }
        na::observe::DrawReplayStats before{};
        na::observe::GetDrawReplayStats(&before);
        const uint64_t fed_before = pass.GetStats().fed;
        uint32_t fed_frames = 0;
        for (uint32_t frame = 100; frame < 116; ++frame) {
            auto& r = ring[frame % kRing];
            if (frame >= 100 + kRing) { // reused only after kRing frames
                na::nr::native::Reset(r.list.Get());
                r.Reset();
            }
            const float jitter[2] = { frame % 2 ? .375f : -.25f, frame % 2 ? -.375f : .25f };
            na::observe::SetSceneViewportJitter(frame, jitter[0], jitter[1]);
            na::observe::SetDrawReplay(frame, retain);
            const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
            const D3D12_RECT sc{0, 0, 14, h};
            r.list->SetGraphicsRootSignature(root.Get());
            const float shift = float(frame % 2) / (w / 2.f);
            r.list->SetGraphicsRoot32BitConstants(0, 1, &shift, 0);
            r.list->SetPipelineState(pso.Get());
            r.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            r.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            r.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            r.list->RSSetViewports(1, &vp);
            r.list->RSSetScissorRects(1, &sc);
            r.list->DrawInstanced(3, 1, 0, 0);
            auto to_read = test::Transition(depth.Get(), depth_write, npsr);
            r.list->ResourceBarrier(1, &to_read);
            const na::j2::replay_motion::Output output = [&] {
                const na::observe::HostWorkScope host;
                return pass.Record(r.list.Get(), d.queue.Get(), depth.Get(), w, h, false, false, jitter);
            }();
            if (output.motion) ++fed_frames;
            std::swap(to_read.Transition.StateBefore, to_read.Transition.StateAfter);
            r.list->ResourceBarrier(1, &to_read);
            REQUIRE(SUCCEEDED(r.list->Close()));
            ID3D12CommandList* commands[] = {r.list.Get()};
            {
                na::nr::native::Submission submitted(d.queue.Get(), 1, commands);
                d.queue->ExecuteCommandLists(1, commands);
            }
            d.WaitIdle();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        na::observe::DrawReplayStats after{};
        na::observe::GetDrawReplayStats(&after);
        std::printf("ring of %u lists: motion on %u of 16 frames, fed %llu, not_ready %llu, taken %llu, unclaimed %llu\n",
                    kRing, fed_frames, (unsigned long long)(pass.GetStats().fed - fed_before),
                    (unsigned long long)(after.not_ready - before.not_ready), (unsigned long long)(after.taken - before.taken),
                    (unsigned long long)(after.take_unclaimed - before.take_unclaimed));
        // First frame has no previous; every later frame must feed motion.
        CHECK(fed_frames >= 14);
        CHECK(after.not_ready == before.not_ready);
    }
    // f905099 run (take_other_list=12792): SE5 draws its geometry on one list
    // and J2 records on a later one. 98d13f1 run (pending 6723 of 6724, fed 0):
    // J2 records before SE5 submits that list. A list submitted later but
    // before J2's list runs must give correct motion (GPU stamp passes); one
    // that runs after J2's list must leave no motion (stamp fails).
    {
        test::List geo, post;
        REQUIRE(geo.Create(dev) && post.Create(dev));
        REQUIRE(SUCCEEDED(geo.list->Close()) && SUCCEEDED(post.list->Close()));
        uint32_t fed_frames = 0, pending_ok = 0, late_masked = 0;
        for (uint32_t frame = 200; frame < 212; ++frame) {
            na::nr::native::Reset(geo.list.Get());geo.Reset();
            na::nr::native::Reset(post.list.Get());post.Reset();
            const float jitter[2] = { frame % 2 ? .375f : -.25f, frame % 2 ? -.375f : .25f };
            na::observe::SetSceneViewportJitter(frame, jitter[0], jitter[1]);
            na::observe::SetDrawReplay(frame, retain);
            const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
            const D3D12_RECT sc{0, 0, 14, h};
            auto* g = geo.list.Get();
            g->SetGraphicsRootSignature(root.Get());
            const float shift = float(frame % 2) / (w / 2.f);
            g->SetGraphicsRoot32BitConstants(0, 1, &shift, 0);
            g->SetPipelineState(pso.Get());
            g->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            g->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            g->RSSetViewports(1, &vp);
            g->RSSetScissorRects(1, &sc);
            g->DrawInstanced(3, 1, 0, 0);
            auto to_read = test::Transition(depth.Get(), depth_write, npsr);
            // 208 runs J2's list first: the depth hand-over moves into J2's list.
            if (frame != 208) g->ResourceBarrier(1, &to_read);
            else post.list->ResourceBarrier(1, &to_read);
            REQUIRE(SUCCEEDED(g->Close()));
            // 205: geometry submitted after J2 recorded, before J2's list runs.
            // 208: geometry submitted after J2's list ran (its positions are stale).
            const bool submit_first = frame != 205 && frame != 208;
            ID3D12CommandList* gl[] = {g};
            if (submit_first) { na::nr::native::Submission s1(d.queue.Get(), 1, gl); d.queue->ExecuteCommandLists(1, gl); }
            const na::j2::replay_motion::Output output = [&] {
                const na::observe::HostWorkScope host;
                return pass.Record(post.list.Get(), d.queue.Get(), depth.Get(), w, h, false, false, jitter);
            }();
            if (output.motion) {
                ++fed_frames;
                ID3D12Resource* textures[] = {output.motion, output.valid};
                for (UINT i = 0; i < 2; ++i) {
                    auto barrier = test::Transition(textures[i], npsr, copy_source);
                    post.list->ResourceBarrier(1, &barrier);
                    D3D12_TEXTURE_COPY_LOCATION src{textures[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                    D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                    dst.PlacedFootprint.Offset = uint64_t(i) * h * pitch;
                    dst.PlacedFootprint.Footprint = {i ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32_FLOAT, w, h, 1, pitch};
                    post.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                    post.list->ResourceBarrier(1, &barrier);
                }
            }
            std::swap(to_read.Transition.StateBefore, to_read.Transition.StateAfter);
            post.list->ResourceBarrier(1, &to_read);
            REQUIRE(SUCCEEDED(post.list->Close()));
            if (frame == 205) { na::nr::native::Submission s1(d.queue.Get(), 1, gl); d.queue->ExecuteCommandLists(1, gl); }
            ID3D12CommandList* pl[] = {post.list.Get()};
            { na::nr::native::Submission s2(d.queue.Get(), 1, pl); d.queue->ExecuteCommandLists(1, pl); }
            if (frame == 208) { na::nr::native::Submission s1(d.queue.Get(), 1, gl); d.queue->ExecuteCommandLists(1, gl); }
            d.WaitIdle();
            if (output.motion) {
                std::byte* mapped = nullptr;
                REQUIRE(SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped))));
                const auto* motion = reinterpret_cast<const float*>(mapped + 7 * pitch);
                const auto* valid = reinterpret_cast<const float*>(mapped + (h + 7) * pitch);
                const bool right = valid[12] == 1.f && std::abs(motion[24] - (frame % 2 ? -1.f : 1.f)) < 1e-3f && std::abs(motion[25]) < 1e-3f;
                if (frame == 208) {
                    CHECK(valid[12] == 0.f);
                    if (valid[12] == 0.f) ++late_masked;
                } else {
                    CHECK(right);
                    if (frame == 205 && right) ++pending_ok;
                }
                std::printf("frame %u: motion (12,7) %.3f %.3f valid %.0f\n", frame, motion[24], motion[25], valid[12]);
                rb->Unmap(0, nullptr);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        na::observe::DrawReplayStats stats{};
        na::observe::GetDrawReplayStats(&stats);
        std::printf("geometry on another list: motion on %u frames (11 expected), pending ok %u, late masked %u, stamps %llu, pending_stamped %llu\n",
                    fed_frames, pending_ok, late_masked, (unsigned long long)stats.stamps, (unsigned long long)stats.pending_stamped);
        CHECK(fed_frames >= 11); // 200 has no previous
        CHECK(pending_ok == 1);
        CHECK(late_masked == 1);
    }
    test::ComPtr<ID3D12InfoQueue> messages;
    if (debug && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&messages)))) {
        std::puts("D3D12 debug layer enabled.");
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            messages->GetMessage(i, nullptr, &size);
            std::vector<std::byte> bytes_for_message(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes_for_message.data());
            if (SUCCEEDED(messages->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::printf("D3D12 error %d: %s\n", message->ID, message->pDescription);
                CHECK(false);
            }
        }
    }
    na::observe::SetDrawReplay(0, nullptr);
    na_observe_stop();
    na::observe::SetProcessExiting();
    return test::finish("draw_replay_test");
}
