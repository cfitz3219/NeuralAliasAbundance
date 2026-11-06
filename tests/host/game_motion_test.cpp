// Game motion feed on a real D3D12 device (WARP in CI): the GPU score pass and the convert pass
// against the CPU rules of game_motion_decide.hpp.
//   - a candidate holding the true motion of a moving textured block (UV, current - previous)
//     is used after its frames are measured, as "uv, both signs flipped", and converts to the
//     arbiter convention (v = previous - current, pixels) on the block, 0 + not valid elsewhere;
//   - an all-zero candidate is never used and the feed hands the arbiter nothing;
//   - the candidate is returned to the state it was found in (the game's state).
#include "test_support.hpp"
#include "host_dx12/j2/game_motion.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace na::j2::game_motion;
namespace {
constexpr uint32_t W = 256, H = 160, kSlotsInFlight = 4;
constexpr float DX = 3.5f, DY = -1.0f;
uint16_t half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
}
float pattern(float x, float y) { return 0.5f + 0.25f * std::sin(x * 0.37f) * std::cos(y * 0.29f) + 0.2f * std::sin((x + 2 * y) * 0.11f); }
bool on_block(float x, float y, float t) { const float ox = x - t * DX, oy = y - t * DY; return ox >= 40 && ox < 180 && oy >= 30 && oy < 120; } // ~600 samples on the GPU's stride-4 grid (the decider needs 256)
float picture(uint32_t x, uint32_t y, float t)
{
    const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
    if (on_block(fx, fy, t)) { const float ox = fx - t * DX, oy = fy - t * DY; return 1.0f - 0.8f * pattern(ox * 1.7f, oy * 1.3f); }
    return 0.6f * pattern(fx, fy);
}

struct Rig {
    test::Device& d;
    ComPtr<ID3D12Resource> color, cand, up_color, up_cand, rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fc{}, fv{};
    explicit Rig(test::Device& dev) : d(dev) {}
    bool init()
    {
        ID3D12Device* dev = d.device.Get();
        color = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
        cand = test::CreateTexture(dev, W, H, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
        if (!color || !cand) return false;
        UINT64 tc = 0, tv = 0;
        D3D12_RESOURCE_DESC a = color->GetDesc(), b = cand->GetDesc();
        dev->GetCopyableFootprints(&a, 0, 1, 0, &fc, nullptr, nullptr, &tc);
        dev->GetCopyableFootprints(&b, 0, 1, 0, &fv, nullptr, nullptr, &tv);
        up_color = test::CreateBuffer(dev, tc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        up_cand = test::CreateBuffer(dev, tv, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        rb = test::CreateBuffer(dev, static_cast<UINT64>(W) * H * 8 + 65536, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        return up_color && up_cand && rb;
    }
    // Frame t: the picture at t, the candidate holding motion t-1 -> t (when `motion`).
    void fill(float t, bool motion)
    {
        uint8_t* p = nullptr;
        up_color->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const float v = picture(x, y, t), px[4] = {v, v, v, 1.0f};
                std::memcpy(p + fc.Offset + y * fc.Footprint.RowPitch + x * 16, px, 16);
            }
        up_color->Unmap(0, nullptr);
        up_cand->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const bool m = motion && on_block(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, t);
                const uint16_t v[2] = {half(m ? DX / W : 0.f), half(m ? DY / H : 0.f)};
                std::memcpy(p + fv.Offset + y * fv.Footprint.RowPitch + x * 4, v, 4);
            }
        up_cand->Unmap(0, nullptr);
    }
    void upload(ID3D12GraphicsCommandList* l)
    {
        D3D12_RESOURCE_BARRIER b[2] = {
            test::Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            test::Transition(cand.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        l->ResourceBarrier(2, b);
        D3D12_TEXTURE_COPY_LOCATION s{}, t{};
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.pResource = up_color.Get(); s.PlacedFootprint = fc; t.pResource = color.Get();
        l->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
        s.pResource = up_cand.Get(); s.PlacedFootprint = fv; t.pResource = cand.Get();
        l->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
        for (auto& x : b) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
        l->ResourceBarrier(2, b);
    }
    // Runs `frames` frames; returns the last Output and leaves the motion readback in rb (when fed).
    Output run(Feed& f, int frames, bool motion, int first = 0)
    {
        Output o{};
        for (int i = first; i < first + frames; ++i) {
            fill(static_cast<float>(i % 7), motion);
            test::List l;
            if (!l.Create(d.device.Get())) return Output{};
            upload(l.list.Get());
            Candidate c{cand.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 77};
            // frame i % 7 == 0 jumps back to t = 0: not a continuation, so not scored
            o = f.Record(l.list.Get(), static_cast<uint32_t>(i) % kSlotsInFlight, &c, 1, color.Get(), W, H, i % 7 == 0);
            if (o.motion) {
                D3D12_RESOURCE_BARRIER b = test::Transition(o.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                l.list->ResourceBarrier(1, &b);
                D3D12_RESOURCE_DESC md = o.motion->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fm{};
                d.device->GetCopyableFootprints(&md, 0, 1, 0, &fm, nullptr, nullptr, nullptr);
                mfp = fm;
                D3D12_TEXTURE_COPY_LOCATION s{}, t{};
                s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.pResource = o.motion;
                t.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; t.pResource = rb.Get(); t.PlacedFootprint = fm;
                l.list->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                l.list->ResourceBarrier(1, &b);
            }
            if (FAILED(l.list->Close())) return Output{};
            ID3D12CommandList* ls[] = {l.list.Get()};
            d.queue->ExecuteCommandLists(1, ls);
            d.WaitIdle();
            last_t = static_cast<float>(i % 7);
        }
        return o;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT mfp{};
    float last_t = 0;
};
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("game_motion_test");
    {
        Rig rig(d);
        REQUIRE(rig.init());
        Feed f;
        REQUIRE(f.Init(d.device.Get(), kSlotsInFlight));
        const Output none = rig.run(f, 8, false);
        CHECK(none.motion == nullptr);
        const Output o = rig.run(f, 72, true, 8);
        REQUIRE(d.device->GetDeviceRemovedReason() == S_OK);
        const Stats s = f.GetStats();
        char text[400];
        f.Describe(text, sizeof text);
        std::printf("moving block: %s\n", text);
        CHECK(s.used_candidate == 0);
        CHECK(s.used_encoding == 7); // uv, x and y flipped: the game's current - previous
        CHECK(s.last_ratio < 0.6);
        CHECK(o.motion != nullptr && o.valid != nullptr);
        if (o.motion) {
            uint8_t* p = nullptr;
            REQUIRE(SUCCEEDED(rig.rb->Map(0, nullptr, reinterpret_cast<void**>(&p))));
            auto at = [&](uint32_t x, uint32_t y, int k) { float v; std::memcpy(&v, p + rig.mfp.Offset + y * rig.mfp.Footprint.RowPitch + x * 8 + k * 4, 4); return v; };
            const float t = rig.last_t;
            const uint32_t bx = static_cast<uint32_t>(110 + t * DX), by = static_cast<uint32_t>(75 + t * DY);
            std::printf("block pixel (%u,%u): %.3f %.3f; wall (5,5): %.3f %.3f\n", bx, by, at(bx, by, 0), at(bx, by, 1), at(5, 5, 0), at(5, 5, 1));
            CHECK(std::fabs(at(bx, by, 0) + DX) < 0.02f && std::fabs(at(bx, by, 1) + DY) < 0.02f); // previous - current
            CHECK(at(5, 5, 0) == 0.f && at(5, 5, 1) == 0.f);
            rig.rb->Unmap(0, nullptr);
        }
    }
    {
        Rig rig(d);
        REQUIRE(rig.init());
        Feed f;
        REQUIRE(f.Init(d.device.Get(), kSlotsInFlight));
        const Output o = rig.run(f, 72, false);
        const Stats s = f.GetStats();
        char text[400];
        f.Describe(text, sizeof text);
        std::printf("empty candidate: %s\n", text);
        CHECK(o.motion == nullptr && s.used_candidate == -1 && s.good == 0);
        REQUIRE(d.device->GetDeviceRemovedReason() == S_OK);
    }
    return test::finish("game_motion_test");
}
