// Game motion feed on a real D3D12 device (WARP in CI): the GPU score pass and the convert pass
// against the CPU rules of game_motion_decide.hpp.
//   - a candidate holding the true motion of a moving textured block (UV, current - previous)
//     is used after its frames are measured, as "uv, both signs flipped", and converts to the
//     arbiter convention (v = previous - current, pixels) on the block, 0 + not valid elsewhere;
//   - an all-zero candidate is never used and the feed hands the arbiter nothing;
//   - the candidate is returned to the state it was found in (the game's state).
#include "test_support.hpp"
#include "host_dx12/j2/game_motion.hpp"

#include <algorithm>
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

template<uint32_t Width=W,uint32_t Height=H>
struct Rig {
    static constexpr uint32_t W=Width,H=Height;
    test::Device& d;
    ComPtr<ID3D12Resource> color, cand, up_color, up_cand, rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fc{}, fv{};
    bool half_colour=false;
    explicit Rig(test::Device& dev) : d(dev) {}
    bool init(bool use_half_colour=false)
    {
        ID3D12Device* dev = d.device.Get();
        half_colour=use_half_colour;
        const auto colour_format=half_colour?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
        color = test::CreateTexture(dev, W, H, colour_format, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
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
    void fill(float t, bool motion, bool rejected=false)
    {
        uint8_t* p = nullptr;
        up_color->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const float v = rejected ? ((x/96+y/64)%6==0?pattern(float(x),float(y)):.012f) : picture(x, y, t);
                auto* row=p+fc.Offset+y*fc.Footprint.RowPitch;
                if(half_colour){
                    const auto h=half(v);
                    const uint16_t px[4]={h,h,h,half(1.0f)};
                    std::memcpy(row+x*sizeof px,px,sizeof px);
                }else{
                    const float px[4]={v,v,v,1.0f};
                    std::memcpy(row+x*sizeof px,px,sizeof px);
                }
            }
        up_color->Unmap(0, nullptr);
        up_cand->Map(0, nullptr, reinterpret_cast<void**>(&p));
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const bool m = motion && on_block(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, t);
                const uint16_t v[2] = {half(rejected?.25f:m ? DX / W : 0.f), half(rejected?.5f:m ? DY / H : 0.f)};
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
    Output run(Feed& f, int frames, bool motion, int first = 0, bool candidate_present = true, bool rejected = false, bool scripted_cuts = true)
    {
        Output o{};
        for (int i = first; i < first + frames; ++i) {
            fill(static_cast<float>(i % 7), motion, rejected);
            test::List l;
            if (!l.Create(d.device.Get())) return Output{};
            upload(l.list.Get());
            Candidate c{cand.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 77};
            // frame i % 7 == 0 jumps back to t = 0: not a continuation, so not scored
            o = f.Record(l.list.Get(), static_cast<uint32_t>(i) % kSlotsInFlight, &c, candidate_present ? 1u : 0u, color.Get(), W, H, scripted_cuts && i % 7 == 0);
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

int rejected_benchmark(test::Device& d)
{
    constexpr UINT width=3440,height=1440;
    Rig<width,height> rig(d);
    REQUIRE(rig.init(true));
    Feed feed;
    REQUIRE(feed.Init(d.device.Get(),kSlotsInFlight));
    test::List list;
    REQUIRE(list.Create(d.device.Get()));
    rig.fill(0,false,true);
    rig.upload(list.list.Get());
    ComPtr<ID3D12QueryHeap> query;
    const D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2,0};
    REQUIRE(SUCCEEDED(d.device->CreateQueryHeap(&desc,IID_PPV_ARGS(&query))));
    UINT64 frequency=0;
    REQUIRE(SUCCEEDED(d.queue->GetTimestampFrequency(&frequency))&&frequency);
    auto result=test::CreateBuffer(d.device.Get(),16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(result);
    const Candidate candidate{rig.cand.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,77};
    std::vector<double> samples;
    double full_rate_sum=0;
    for(unsigned i=0;i<176;++i){
        auto* commands=list.list.Get();
        commands->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        const auto output=feed.Record(commands,i%kSlotsInFlight,&candidate,1,rig.color.Get(),width,height,false);
        CHECK(!output.motion);
        commands->EndQuery(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        commands->ResolveQueryData(query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,result.Get(),0);
        REQUIRE(SUCCEEDED(commands->Close()));
        ID3D12CommandList* batch[]={commands};
        d.queue->ExecuteCommandLists(1,batch);
        d.WaitIdle();
        list.Reset();
        void* mapped=nullptr;
        const D3D12_RANGE read{0,16},none{};
        REQUIRE(SUCCEEDED(result->Map(0,&read,&mapped)));
        const auto* ticks=static_cast<const UINT64*>(mapped);
        const double ms=double(ticks[1]-ticks[0])*1000.0/double(frequency);
        if(i>=16&&i<48)full_rate_sum+=ms;
        if(i>=144)samples.push_back(ms);
        result->Unmap(0,&none);
    }
    std::sort(samples.begin(),samples.end());
    const auto stats=feed.GetStats();
    CHECK(stats.scored<100&&stats.deferred>90&&stats.fed==0&&stats.good==0);
    double sum=0;
    for(double sample:samples)sum+=sample;
    std::printf("Rejected candidate, %ux%u: scored %llu, deferred %llu, fed %llu, pixels scored %llu, median %.4f ms mean %.4f ms p95 %.4f ms, full-rate mean %.4f ms\n",
                width,height,static_cast<unsigned long long>(stats.scored),static_cast<unsigned long long>(stats.deferred),
                static_cast<unsigned long long>(stats.fed),static_cast<unsigned long long>(stats.last_scored),
                (samples[15]+samples[16])*.5,sum/samples.size(),samples[30],full_rate_sum/32);
    return test::finish("game_motion_rejected_benchmark");
}
} // namespace

int main(int argc,char** argv)
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("game_motion_test");
    if(argc>1&&std::strcmp(argv[1],"--rejected-benchmark")==0)return rejected_benchmark(d);
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
        // A texture may disappear during a menu/scene change and reappear with
        // the same id. Empty work must neither record/score nor reuse old scores.
        const auto before_gap = f.GetStats();
        const auto gap = rig.run(f, 3, true, 80, false);
        const auto after_gap = f.GetStats();
        CHECK(!gap.motion && after_gap.used_candidate == -1);
        CHECK(after_gap.recorded == before_gap.recorded && after_gap.scored == before_gap.scored);
        const auto warming = rig.run(f, 1, true, 83); // not a scripted cut
        const auto after_warm = f.GetStats();
        CHECK(!warming.motion && after_warm.recorded == after_gap.recorded + 1);
        CHECK(after_warm.scored == after_gap.scored);
        CHECK(after_warm.collected == after_gap.collected); // no old-slot measurements re-enter
        const auto resumed = rig.run(f, 72, true, 84);
        CHECK(resumed.motion && f.GetStats().used_candidate == 0);
        REQUIRE(d.device->GetDeviceRemovedReason() == S_OK);
    }
    {
        Rig rig(d);
        REQUIRE(rig.init());
        Feed f;
        REQUIRE(f.Init(d.device.Get(), kSlotsInFlight));
        CHECK(!rig.run(f,85,false,0,true,true,false).motion);
        const auto rejected=f.GetStats();
        CHECK(rejected.deferred>0 && rejected.fed==0);
        // The same texture starts holding valid motion. There is no cut or
        // identity change to bail out the retry path.
        const auto recovered=rig.run(f,96,true,85,true,false,false);
        CHECK(recovered.motion&&f.GetStats().used_candidate==0&&f.GetStats().used_encoding==7);
        const auto stable=f.GetStats();
        CHECK(rig.run(f,14,true,181,true,false,false).motion);
        CHECK(f.GetStats().deferred==stable.deferred);
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
