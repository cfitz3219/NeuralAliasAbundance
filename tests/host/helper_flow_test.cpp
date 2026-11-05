// The DLSS helper's optical-flow motion (src/bridge/helper_flow, bridge v4) on a
// synthetic scene: a textured block moves 4 px to the right every frame over a
// still textured background, with no camera (identity reprojection), exactly
// what a DX9 game without a found camera sends. The shared colour texture is
// larger than the frame, like the helper's (the colour is copied out first).
//
// Checks, from the third frame on: inside the block the accepted flow and the
// arbiter's final motion are (-4, 0) px (p_previous - p_current), and the still
// background gets no motion. Needs a GPU that runs AMD's optical flow (32/64-lane
// waves); SKIP on WARP (CI), so it runs on a real card only.
#include "test_support.hpp"

#include "bridge/helper_flow.hpp"
#include "core/motion_arbiter_gpu.hpp"

#include <cmath>
#include <vector>

namespace {
constexpr uint32_t kTexW = 640, kTexH = 320; // shared texture (max render size)
constexpr uint32_t kW = 512, kH = 256;       // this frame's render size
constexpr int kBoxX = 160, kBoxY = 80, kBoxW = 128, kBoxH = 96, kStep = 4;

uint16_t Half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) return static_cast<uint16_t>(sign);
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    return static_cast<uint16_t>(sign | (e << 10) | (m >> 13));
}
float Noise(int x, int y, uint32_t seed)
{
    uint32_t v = static_cast<uint32_t>(x) * 1664525u + static_cast<uint32_t>(y) * 1013904223u + seed;
    v ^= v >> 13;
    v *= 2246822519u;
    v ^= v >> 16;
    return float(v & 255) / 255.f;
}
// Smooth enough to match at 8x8 blocks, textured enough to pass the texture gate.
float Pattern(int x, int y, uint32_t seed)
{
    const float a = Noise(x / 3, y / 3, seed), b = Noise(x / 7, y / 7, seed ^ 0x9e3779b9u);
    return .15f + .7f * (.6f * a + .4f * b);
}
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d, /*allow_warp=*/true)) return test::kSkip;
    na::bridge::flow::HelperFlow flow;
    if (!flow.Init(d.device.Get(), 3)) {
        std::printf("SKIP: %s\n", flow.GetStats().reason);
        return test::kSkip;
    }
    na::motion::gpu::Arbiter arbiter;
    REQUIRE(arbiter.Init(d.device.Get(), 3));
    test::List l;
    REQUIRE(l.Create(d.device.Get()));
    const auto S = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto color = test::CreateTexture(d.device.Get(), kTexW, kTexH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto depth = test::CreateTexture(d.device.Get(), kTexW, kTexH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto motion = test::CreateTexture(d.device.Get(), kW, kH, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto prov = test::CreateTexture(d.device.Get(), kW, kH, DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(color && depth && motion && prov);
    auto color_up = test::CreateBuffer(d.device.Get(), uint64_t(kTexW) * kTexH * 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto depth_up = test::CreateBuffer(d.device.Get(), uint64_t(kTexW) * kTexH * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    // Read-back of the final motion (RG32F) and the accepted mask's source data.
    D3D12_RESOURCE_DESC md = motion->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    d.device->GetCopyableFootprints(&md, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    auto rb = test::CreateBuffer(d.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    auto upload = [&](int frame) {
        uint16_t* c = nullptr;
        float* z = nullptr;
        color_up->Map(0, nullptr, reinterpret_cast<void**>(&c));
        depth_up->Map(0, nullptr, reinterpret_cast<void**>(&z));
        const int bx = kBoxX + frame * kStep;
        for (uint32_t y = 0; y < kTexH; ++y)
            for (uint32_t x = 0; x < kTexW; ++x) {
                const bool in = int(x) >= bx && int(x) < bx + kBoxW && int(y) >= kBoxY && int(y) < kBoxY + kBoxH;
                // Inside the block the pattern travels with it (object space).
                const float v = in ? Pattern(int(x) - bx, int(y) - kBoxY, 77u) : Pattern(int(x), int(y), 5u);
                uint16_t* px = c + (size_t(y) * kTexW + x) * 4;
                px[0] = Half(v);
                px[1] = Half(in ? v * .8f : v);
                px[2] = Half(in ? .3f : v * .9f);
                px[3] = Half(1.f);
                z[size_t(y) * kTexW + x] = in ? .5f : .9f;
            }
        color_up->Unmap(0, nullptr);
        depth_up->Unmap(0, nullptr);
    };
    auto copy_in = [&](ID3D12Resource* dst_res, ID3D12Resource* src_buf, DXGI_FORMAT f, uint32_t pitch) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = dst_res;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = src_buf;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {f, kTexW, kTexH, 1, pitch};
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    };

    bool first_state = true;
    int checked = 0;
    for (int frame = 0; frame < 6; ++frame) {
        const uint32_t slot = static_cast<uint32_t>(frame % 3);
        flow.Collect(slot); // WaitIdle below: every earlier slot has finished
        upload(frame);
        if (!first_state) {
            auto a = test::Transition(color.Get(), S, D3D12_RESOURCE_STATE_COPY_DEST);
            auto b = test::Transition(depth.Get(), S, D3D12_RESOURCE_STATE_COPY_DEST);
            l.list->ResourceBarrier(1, &a);
            l.list->ResourceBarrier(1, &b);
        }
        first_state = false;
        copy_in(color.Get(), color_up.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTexW * 8);
        copy_in(depth.Get(), depth_up.Get(), DXGI_FORMAT_R32_FLOAT, kTexW * 4);
        auto a = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, S);
        auto b = test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, S);
        l.list->ResourceBarrier(1, &a);
        l.list->ResourceBarrier(1, &b);

        na::motion::gpu::Inputs in{};
        in.depth = depth.Get();
        in.tier2_valid_flag = true;
        in.depth_clear = 1.f;
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(in.inv_view_proj_current.m, identity, sizeof identity);
        std::memcpy(in.view_proj_previous.m, identity, sizeof identity);
        na::bridge::flow::Frame ff;
        ff.color = color.Get();
        ff.depth = depth.Get();
        ff.w = kW;
        ff.h = kH;
        ff.frame = static_cast<uint64_t>(frame) + 1;
        ff.loosen = 50;
        na::bridge::flow::Tier3 t3;
        const bool have = flow.Record(l.list.Get(), slot, ff, in, t3);
        CHECK(have == (frame >= 1)); // the first frame only captures the previous image
        if (have) {
            in.tier3_valid_flag = true;
            in.tier3_residual = t3.residual;
            in.tier3_valid = t3.valid;
            in.tier3_mask = t3.mask;
        }
        na::motion::gpu::Outputs o{};
        o.xy = motion.Get();
        o.provenance = prov.Get();
        o.width = kW;
        o.height = kH;
        REQUIRE(arbiter.Dispatch(l.list.Get(), in, o, slot));
        auto c1 = test::Transition(motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l.list->ResourceBarrier(1, &c1);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = motion.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto c2 = test::Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        l.list->ResourceBarrier(1, &c2);
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        flow.Submitted(slot);
        d.WaitIdle();
        l.Reset();
        flow.Collect(slot);
        REQUIRE(d.device->GetDeviceRemovedReason() == S_OK);
        if (frame < 2) continue;

        // The block's interior at this frame (a margin of two 8x8 blocks), and the background far from it.
        const int bx = kBoxX + frame * kStep;
        unsigned char* p = nullptr;
        rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
        unsigned in_n = 0, in_good = 0, bg_n = 0, bg_still = 0;
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x) {
                const float* v = reinterpret_cast<const float*>(p + fp.Offset + size_t(y) * fp.Footprint.RowPitch) + 2 * x;
                const bool inside = int(x) >= bx + 16 && int(x) < bx + kBoxW - 16 && int(y) >= kBoxY + 16 && int(y) < kBoxY + kBoxH - 16;
                const bool far = int(x) < bx - 32 || int(x) >= bx + kBoxW + 32 || int(y) < kBoxY - 32 || int(y) >= kBoxY + kBoxH + 32;
                if (inside) {
                    ++in_n;
                    if (std::fabs(v[0] + float(kStep)) <= 1.f && std::fabs(v[1]) <= 1.f) ++in_good;
                } else if (far) {
                    ++bg_n;
                    if (std::fabs(v[0]) <= .25f && std::fabs(v[1]) <= .25f) ++bg_still;
                }
            }
        rb->Unmap(0, nullptr);
        const auto& st = flow.GetStats();
        uint64_t total = 0;
        for (uint64_t c : st.last) total += c;
        std::printf("frame %d: block %u/%u pixels at (-4,0) +-1 px, background %u/%u still; last pair accepted %llu of %llu, "
                    "invalid %llu small %llu image %llu flicker %llu\n",
                    frame, in_good, in_n, bg_still, bg_n, static_cast<unsigned long long>(st.last[0]), static_cast<unsigned long long>(total),
                    static_cast<unsigned long long>(st.last[1]), static_cast<unsigned long long>(st.last[2]),
                    static_cast<unsigned long long>(st.last[5]), static_cast<unsigned long long>(st.last[6]));
        CHECK(in_n && in_good * 2 >= in_n);   // at least half the interior moves exactly with the block
        CHECK(bg_n && bg_still * 100 >= bg_n * 98); // the still background stays still
        ++checked;
    }
    CHECK(checked == 4);
    const auto& st = flow.GetStats();
    CHECK(st.engine == 1 && st.pairs == 5 && st.unavailable == 0 && st.sum_pairs == 5);
    return test::finish("helper_flow_test");
}
