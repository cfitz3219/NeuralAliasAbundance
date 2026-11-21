// The draw-replay rasteriser (j2/replay_motion.hpp) fed by another producer's positions: the native
// DX11 route hands it a ReplayFrame of buffers it filled itself (native11/draw_replay.hpp), instead of
// the DX12 observe layer's. D3D12 hardware, else WARP (the hosted runner runs it). Checks:
//   - a triangle one pixel to the right of where it was gives -1 px of motion where it is the visible
//     surface, with the viewport jitter removed, and no motion elsewhere;
//   - on the widened grid of the DX11 viewport jitter (half a pixel each side) the motion is in that
//     grid's pixels, (W+1)/W of a target pixel, and the same pixels are covered;
//   - a reset or a frame with no previous positions gives nothing.
#include "test_support.hpp"
#include "host_dx12/j2/replay_motion.hpp"

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("replay_motion_frame_test");
    auto* dev = d.device.Get();
    constexpr UINT w = 32, h = 24;
    constexpr auto npsr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto copy_source = D3D12_RESOURCE_STATE_COPY_SOURCE;

    na::j2::replay_motion::Pass pass;
    REQUIRE(pass.Init(dev));
    test::List list;
    REQUIRE(list.Create(dev) && SUCCEEDED(list.list->Close()));
    na::nr::native::Reset(list.list.Get()); // observed from a Reset onwards, like the route's own list
    list.Reset();

    // Positions: the same triangle in two frames, one pixel apart in x (current to the right).
    const float tri[3][2] = {{-.5f, -.65f}, {-.5f, .65f}, {.6f, .65f}};
    float cur[3][4], prev[3][4];
    for (int i = 0; i < 3; ++i) {
        cur[i][0] = tri[i][0]; cur[i][1] = tri[i][1]; cur[i][2] = .5f; cur[i][3] = 1.f;
        std::memcpy(prev[i], cur[i], sizeof cur[i]);
        prev[i][0] -= 2.f / float(w);
    }
    auto upload = test::CreateBuffer(dev, 2 * sizeof cur, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto cur_buf = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    auto prev_buf = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    REQUIRE(upload && cur_buf && prev_buf);
    {
        void* m = nullptr;
        REQUIRE(SUCCEEDED(upload->Map(0, nullptr, &m)));
        std::memcpy(m, cur, sizeof cur);
        std::memcpy(static_cast<uint8_t*>(m) + sizeof cur, prev, sizeof prev);
        upload->Unmap(0, nullptr);
    }
    // The scene depth: the triangle's depth everywhere (R32, read as the route's shared depth).
    auto depth = test::CreateTexture(dev, w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(depth);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    test::ComPtr<ID3D12DescriptorHeap> rtv_heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap))));
    dev->CreateRenderTargetView(depth.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    {
        list.list->CopyBufferRegion(cur_buf.Get(), 0, upload.Get(), 0, sizeof cur);
        list.list->CopyBufferRegion(prev_buf.Get(), 0, upload.Get(), sizeof cur, sizeof prev);
        const float half[4] = {.5f, 0, 0, 0};
        list.list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(), half, 0, nullptr);
        auto b = test::Transition(depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, npsr);
        list.list->ResourceBarrier(1, &b);
        REQUIRE(SUCCEEDED(list.list->Close()));
        ID3D12CommandList* lists[] = {list.list.Get()};
        { na::nr::native::Submission s(d.queue.Get(), 1, lists); d.queue->ExecuteCommandLists(1, lists); }
        d.WaitIdle();
    }
    constexpr UINT pitch = 256;
    auto rb = test::CreateBuffer(dev, 2 * h * pitch, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(rb);

    const na::observe::ReplayRecord draw{1, 2, 0, 3, w, h, {0, 0, int32_t(w), int32_t(h)}};
    struct Seen { bool fed = false; float mx = 0, my = 0, valid_in = 0, valid_out = 0; };
    const auto run = [&](bool expanded, bool reset, bool with_prev) {
        Seen seen;
        na::nr::native::Reset(list.list.Get());
        list.Reset();
        na::observe::ReplayFrame f;
        f.cur = cur_buf.Get();
        f.prev = with_prev ? prev_buf.Get() : nullptr;
        f.cur_draws = {draw};
        if (with_prev) f.prev_draws = {draw};
        const float jitter[2] = {.25f, -.375f};
        const auto out = pass.Record(list.list.Get(), d.queue.Get(), depth.Get(), w, h, false, reset, jitter, nullptr, f, expanded);
        seen.fed = out.motion != nullptr;
        if (out.motion) {
            ID3D12Resource* textures[] = {out.motion, out.valid};
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
        if (FAILED(list.list->Close())) { std::printf("FAIL: list did not close\n"); ++test::g_failures; return seen; }
        ID3D12CommandList* lists[] = {list.list.Get()};
        { na::nr::native::Submission s(d.queue.Get(), 1, lists); d.queue->ExecuteCommandLists(1, lists); }
        d.WaitIdle();
        if (out.motion) {
            std::byte* mapped = nullptr;
            if (SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
                const auto* motion = reinterpret_cast<const float*>(mapped + 7 * pitch);
                const auto* valid_row7 = reinterpret_cast<const float*>(mapped + (h + 7) * pitch);
                const auto* valid_row20 = reinterpret_cast<const float*>(mapped + (h + 20) * pitch);
                seen.mx = motion[24]; seen.my = motion[25]; // pixel (12, 7): inside the triangle
                seen.valid_in = valid_row7[12];
                seen.valid_out = valid_row20[30];            // pixel (30, 20): outside
                rb->Unmap(0, nullptr);
            }
        }
        return seen;
    };

    const Seen plain = run(false, false, true);
    std::printf("target grid: fed %d motion (12,7) %.6f %.6f valid %.0f outside %.0f\n", plain.fed, plain.mx, plain.my, plain.valid_in, plain.valid_out);
    CHECK(plain.fed && plain.valid_in == 1.f && plain.valid_out == 0.f);
    CHECK(std::fabs(plain.mx + 1.f) < 1e-3f && std::fabs(plain.my) < 1e-3f);
    const Seen wide = run(true, false, true);
    const float wide_px = -float(w + 1) / float(w);
    std::printf("widened grid: fed %d motion (12,7) %.6f %.6f (expected %.6f) valid %.0f outside %.0f\n", wide.fed, wide.mx, wide.my,
                wide_px, wide.valid_in, wide.valid_out);
    CHECK(wide.fed && wide.valid_in == 1.f && wide.valid_out == 0.f);
    CHECK(std::fabs(wide.mx - wide_px) < 1e-3f && std::fabs(wide.my) < 1e-3f);
    CHECK(!run(false, true, true).fed);   // reset: nothing
    CHECK(!run(false, false, false).fed); // no previous positions: nothing
    const auto st = pass.GetStats();
    std::printf("pass: frames %llu fed %llu pairs %llu raster verts %llu\n", (unsigned long long)st.frames, (unsigned long long)st.fed,
                (unsigned long long)st.matched_exact, (unsigned long long)st.verts);
    CHECK(st.fed == 2);
    return test::finish("replay_motion_frame_test");
}
