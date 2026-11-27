// Live x2, still-area steadying (fg/steady_pass.cpp): the production pass writes exactly what Astra's
// reference shader (research/fg-wire-shimmer/fg_static_midpoint.hlsl, 93d279ca, embedded verbatim
// below) writes, on a fixture with still and changing areas, moving and still motion, a thin moving
// strip and picture edges that are not multiples of the 16x16 groups. Twice: with mv_scale 1 (her
// units), and with mv_scale 0.5 against her shader given the motion already halved. Its changed-pixel
// count matches the pixels that changed. Also the motion preparation (fg/motion_prep.cpp): exact
// zeros are filled from the nearest known vectors; Frame Generation's copy has a 1-pixel dead zone.
// SDR (2026-11-26): on an 8-bit sRGB-encoded picture the pass decodes to linear, runs her test and
// midpoint, and encodes back; checked against her shader given the same pictures decoded to linear
// (32-bit float) and encoded back, within one 8-bit code.
#include "test_support.hpp"
#include "host_dx12/fg/motion_prep.hpp"
#include "host_dx12/fg/steady_pass.hpp"

#include <DirectXPackedVector.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace pv = DirectX::PackedVector;

namespace {
constexpr uint32_t W = 83, H = 45;   // display size, not multiples of 16
constexpr uint32_t MW = 42, MH = 23; // motion (render) size

constexpr char kAstra[] = R"(// Offline 2x FG trial for linear FP16 scene colour. Dispatch before HUD composition.
Texture2D<float4> older : register(t0);
Texture2D<float4> newer : register(t1);
Texture2D<float2> motion : register(t2);
RWTexture2D<float4> generated : register(u0);
cbuffer Frame : register(b0) { uint width, height; };
groupshared float diff[28*28];
groupshared float h5[28*24];
groupshared float v5[24*24];
groupshared float h9[24*16];
float proxyLuma(float3 color) {
    return dot(sign(color)*pow(abs(color),1.0/2.2), float3(.2126,.7152,.0722));
}
[numthreads(16,16,1)]
void main(uint3 tile:SV_GroupID, uint3 tid:SV_GroupThreadID, uint lane:SV_GroupIndex) {
    uint2 p=tile.xy*16+tid.xy;
    uint mw,mh;
    motion.GetDimensions(mw,mh);
    int2 edge=int2(width,height)-1;
    for(uint i=lane;i<28*28;i+=256) {
        int2 samplePos=clamp(int2(tile.xy*16)+int2(i%28,i/28)-6,int2(0,0),edge);
        diff[i]=proxyLuma(older[samplePos].rgb)-proxyLuma(newer[samplePos].rgb);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<28*24;i+=256) {
        uint x=i%24,y=i/24;
        float sum=0;
        [unroll] for(uint k=0;k<5;k++) sum+=diff[y*28+x+k];
        h5[i]=sum*.2;
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<24*24;i+=256) {
        float sum=0;
        [unroll] for(uint k=0;k<5;k++) sum+=h5[i+k*24];
        v5[i]=abs(sum*.2);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<24*16;i+=256) {
        uint x=i%16,y=i/16;
        float sum=0;
        [unroll] for(uint k=0;k<9;k++) sum+=v5[y*24+x+k];
        h9[i]=sum/9;
    }
    GroupMemoryBarrierWithGroupSync();
    if(p.x>=width || p.y>=height) return;
    float disagreement=0;
    [unroll] for(uint k=0;k<9;k++) disagreement+=h9[(tid.y+k)*16+tid.x];
    disagreement*=255.0/9;
    uint2 mSize=uint2(mw,mh), size=uint2(width,height);
    uint2 at=min(p*mSize/size,mSize-1);
    float2 scale=float2(size)/float2(mSize);
    // Include adjacent motion so a thin moving object can veto the still-background rule.
    float speed=0;
    [unroll] for(int yy=-1;yy<=1;yy++) {
        [unroll] for(int xx=-1;xx<=1;xx++) {
            float2 v=motion[clamp(int2(at)+int2(xx,yy),int2(0,0),int2(mSize)-1)]*scale;
            speed=max(speed,all(isfinite(v))?length(v):1e6);
        }
    }
    float weight=saturate((.85-speed)/.55)*saturate((2.3-disagreement)/1.4);
    if(weight>0) {
        float4 g=generated[p];
        float3 mid=.5*(older[p].rgb+newer[p].rgb);
        generated[p]=float4(lerp(g.rgb,mid,weight),g.a);
    }
}
)";

void upload(test::Device& d, test::List& l, ID3D12Resource* tex, const void* rows, uint32_t row_bytes, uint32_t h, D3D12_RESOURCE_STATES after,
            std::vector<ComPtr<ID3D12Resource>>& keep)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    const auto desc = tex->GetDesc();
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto up = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* m = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&m));
    for (uint32_t y = 0; y < h; ++y) std::memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, static_cast<const uint8_t*>(rows) + size_t(y) * row_bytes, row_bytes);
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    l.list->ResourceBarrier(1, &b);
    keep.push_back(up);
}
ComPtr<ID3D12Resource> readback(test::Device& d, test::List& l, ID3D12Resource* tex, D3D12_RESOURCE_STATES state, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp)
{
    const auto b0 = test::Transition(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b0);
    UINT64 total = 0;
    const auto desc = tex->GetDesc();
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto b1 = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    l.list->ResourceBarrier(1, &b1);
    return rb;
}
template <class T> std::vector<T> rows_of(ID3D12Resource* rb, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, uint32_t row_elems, uint32_t h)
{
    std::vector<T> out(size_t(row_elems) * h);
    uint8_t* m = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
    for (uint32_t y = 0; y < h; ++y) std::memcpy(out.data() + size_t(y) * row_elems, m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, row_elems * sizeof(T));
    rb->Unmap(0, nullptr);
    return out;
}

// Astra's shader as she wrote it: SRV t0-t2, UAV u0, constants {width, height}.
struct Reference {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    bool Init(ID3D12Device* dev)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
        D3D12_ROOT_PARAMETER p[2]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[0].DescriptorTable = {2, ranges};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[1].Constants = {0, 0, 2};
        D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> sig, err, code;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root))))
            return false;
        if (FAILED(D3DCompile(kAstra, sizeof(kAstra) - 1, "astra_reference", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            if (err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        return SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso))) && SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
    }
    void Record(ID3D12Device* dev, ID3D12GraphicsCommandList* list, ID3D12Resource* gen, ID3D12Resource* older, ID3D12Resource* newer, ID3D12Resource* motion)
    {
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        ID3D12Resource* srvs[3] = {older, newer, motion};
        for (ID3D12Resource* r : srvs) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = r->GetDesc().Format;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(r, &v, cpu);
            cpu.ptr += inc;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = gen->GetDesc().Format; // FP16 in her runs; 32-bit float for the SDR comparison
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(gen, nullptr, &u, cpu);
        ID3D12DescriptorHeap* heaps[] = {heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root.Get());
        list->SetPipelineState(pso.Get());
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        const uint32_t k[2] = {W, H};
        list->SetComputeRoot32BitConstants(1, 2, k, 0);
        list->Dispatch((W + 15) / 16, (H + 15) / 16, 1);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = gen;
        list->ResourceBarrier(1, &b);
    }
};
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("fg_steady_pass_test");
    const DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    {   // A format other than linear FP16 is refused with a plain reason.
        na::fg::SteadyPass refused;
        char why[112] = {};
        CHECK(!refused.Init(d.device.Get(), DXGI_FORMAT_R11G11B10_FLOAT, why, sizeof why));
        CHECK(std::strstr(why, "format") != nullptr);
    }
    // Pictures: a textured scene. Left part identical in both real frames (still), a block that
    // changes brightness between them (disagreement), HDR values above 1. The generated picture
    // differs everywhere from the real ones.
    const size_t n = size_t(W) * H;
    std::vector<pv::HALF> older(n * 4), newer(n * 4), gen(n * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = (size_t(y) * W + x) * 4;
            const float base = 0.05f + 0.4f * float((x * 7 + y * 13) % 17) / 17.f + (x == 60 ? 3.f : 0.f);
            const bool changing = x >= 30 && x < 50 && y >= 10 && y < 30;
            for (int c = 0; c < 3; ++c) {
                older[i + c] = pv::XMConvertFloatToHalf(base * (1.f + 0.1f * c));
                newer[i + c] = pv::XMConvertFloatToHalf(changing ? base * 1.8f + 0.2f : base * (1.f + 0.1f * c) + (x % 3 == 0 ? 0.001f : 0.f));
                gen[i + c] = pv::XMConvertFloatToHalf(base * 0.7f + 0.05f * c);
            }
            older[i + 3] = newer[i + 3] = gen[i + 3] = pv::XMConvertFloatToHalf(1.f);
        }
    // Motion (render px, R32G32): still left of x 20, slow (0.2) to x 26, moving right of it, a
    // thin moving strip at row 5, one non-finite vector.
    std::vector<float> motion(size_t(MW) * MH * 2, 0.f);
    for (uint32_t y = 0; y < MH; ++y)
        for (uint32_t x = 0; x < MW; ++x) {
            float* v = &motion[(size_t(y) * MW + x) * 2];
            if (x >= 26) { v[0] = 2.f; v[1] = -1.f; }
            else if (x >= 20) { v[0] = 0.2f; v[1] = 0.1f; }
            if (y == 5) v[0] = 3.f;
        }
    motion[(size_t(15) * MW + 10) * 2] = INFINITY;
    std::vector<float> halved(motion.size());
    for (size_t i = 0; i < motion.size(); ++i) halved[i] = motion[i] * 0.5f; // exact (power of two)

    na::fg::SteadyPass pass;
    char why[112] = {};
    REQUIRE(pass.Init(d.device.Get(), fmt, why, sizeof why));
    Reference ref;
    REQUIRE(ref.Init(d.device.Get()));
    auto run = [&](const std::vector<float>& prod_motion, float scale, const std::vector<float>& ref_motion, const char* label) -> int {
        test::List l;
        REQUIRE(l.Create(d.device.Get()));
        std::vector<ComPtr<ID3D12Resource>> keep;
        auto mk = [&](DXGI_FORMAT f, uint32_t w, uint32_t h, bool uav) {
            return test::CreateTexture(d.device.Get(), w, h, f, D3D12_RESOURCE_STATE_COPY_DEST, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        };
        auto t_old = mk(fmt, W, H, false), t_new = mk(fmt, W, H, false), g_prod = mk(fmt, W, H, true), g_ref = mk(fmt, W, H, true);
        auto m_prod = mk(DXGI_FORMAT_R32G32_FLOAT, MW, MH, false), m_ref = mk(DXGI_FORMAT_R32G32_FLOAT, MW, MH, false);
        REQUIRE(t_old && t_new && g_prod && g_ref && m_prod && m_ref);
        const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        upload(d, l, t_old.Get(), older.data(), W * 8, H, srv, keep);
        upload(d, l, t_new.Get(), newer.data(), W * 8, H, srv, keep);
        upload(d, l, g_prod.Get(), gen.data(), W * 8, H, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, keep);
        upload(d, l, g_ref.Get(), gen.data(), W * 8, H, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, keep);
        upload(d, l, m_prod.Get(), prod_motion.data(), MW * 8, MH, srv, keep);
        upload(d, l, m_ref.Get(), ref_motion.data(), MW * 8, MH, srv, keep);
        REQUIRE(pass.Record(l.list.Get(), g_prod.Get(), t_old.Get(), t_new.Get(), m_prod.Get(), W, H, scale, scale, 5));
        ref.Record(d.device.Get(), l.list.Get(), g_ref.Get(), t_old.Get(), t_new.Get(), m_ref.Get());
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fa{}, fb{};
        auto ra = readback(d, l, g_prod.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, fa);
        auto rb = readback(d, l, g_ref.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, fb);
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        const auto a = rows_of<uint16_t>(ra.Get(), fa, W * 4, H), b = rows_of<uint16_t>(rb.Get(), fb, W * 4, H);
        uint32_t mismatches = 0, changed = 0;
        for (size_t i = 0; i < n; ++i) {
            bool diff = false;
            for (int c = 0; c < 4; ++c) {
                if (a[i * 4 + c] != b[i * 4 + c]) ++mismatches;
                if (a[i * 4 + c] != gen[i * 4 + c]) diff = true;
            }
            if (diff) ++changed;
        }
        uint32_t counted = 0;
        const bool have_count = pass.ReadChanged(5, &counted);
        std::printf("%s: pixels %zu, changed %u, counted %u, mismatches against Astra's shader %u\n", label, n, changed, counted, mismatches);
        CHECK(mismatches == 0);
        CHECK(changed > 200 && changed < n - 200); // both kinds of area are present
        if (have_count) CHECK(counted >= changed && counted <= changed + n / 10); // weight > 0 can round back to the same half value
        return 0;
    };
    if (run(motion, 1.f, motion, "mv_scale 1") != 0) return 1;
    if (run(motion, 0.5f, halved, "mv_scale 0.5") != 0) return 1;

    {   // SDR: 8-bit sRGB-encoded pictures (the same scene, clipped to SDR white).
        auto dec = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
        auto enc = [](float c) {
            c = c < 0.f ? 0.f : (c > 1.f ? 1.f : c);
            return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
        };
        auto to8 = [&](const std::vector<pv::HALF>& lin) {
            std::vector<uint8_t> out(n * 4);
            for (size_t i = 0; i < n * 4; ++i) {
                const float v = (i % 4 == 3) ? 1.f : enc(pv::XMConvertHalfToFloat(lin[i]));
                out[i] = static_cast<uint8_t>(std::lround(v * 255.f));
            }
            return out;
        };
        auto to32 = [&](const std::vector<uint8_t>& b) { // her input: the stored codes decoded to linear
            std::vector<float> out(n * 4);
            for (size_t i = 0; i < n * 4; ++i) out[i] = (i % 4 == 3) ? 1.f : dec(float(b[i]) / 255.f);
            return out;
        };
        const auto o8 = to8(older), n8 = to8(newer), g8 = to8(gen);
        const auto oL = to32(o8), nL = to32(n8), gL = to32(g8);
        const DXGI_FORMAT f8 = DXGI_FORMAT_R8G8B8A8_UNORM, fL = DXGI_FORMAT_R32G32B32A32_FLOAT;
        na::fg::SteadyPass sdr;
        char why8[112] = {};
        const bool ready = sdr.Init(d.device.Get(), f8, why8, sizeof why8);
        if (!ready) std::printf("SDR steadying refused: %s\n", why8);
        REQUIRE(ready);
        test::List l;
        REQUIRE(l.Create(d.device.Get()));
        std::vector<ComPtr<ID3D12Resource>> keep;
        auto mk = [&](DXGI_FORMAT f, uint32_t w, uint32_t h, bool uav) {
            return test::CreateTexture(d.device.Get(), w, h, f, D3D12_RESOURCE_STATE_COPY_DEST, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        };
        auto t_o8 = mk(f8, W, H, false), t_n8 = mk(f8, W, H, false), g_prod = mk(f8, W, H, true);
        auto t_oL = mk(fL, W, H, false), t_nL = mk(fL, W, H, false), g_ref = mk(fL, W, H, true);
        auto m_prod = mk(DXGI_FORMAT_R32G32_FLOAT, MW, MH, false), m_ref = mk(DXGI_FORMAT_R32G32_FLOAT, MW, MH, false);
        REQUIRE(t_o8 && t_n8 && g_prod && t_oL && t_nL && g_ref && m_prod && m_ref);
        const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        upload(d, l, t_o8.Get(), o8.data(), W * 4, H, srv, keep);
        upload(d, l, t_n8.Get(), n8.data(), W * 4, H, srv, keep);
        upload(d, l, g_prod.Get(), g8.data(), W * 4, H, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, keep);
        upload(d, l, t_oL.Get(), oL.data(), W * 16, H, srv, keep);
        upload(d, l, t_nL.Get(), nL.data(), W * 16, H, srv, keep);
        upload(d, l, g_ref.Get(), gL.data(), W * 16, H, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, keep);
        upload(d, l, m_prod.Get(), motion.data(), MW * 8, MH, srv, keep);
        upload(d, l, m_ref.Get(), motion.data(), MW * 8, MH, srv, keep);
        REQUIRE(sdr.Record(l.list.Get(), g_prod.Get(), t_o8.Get(), t_n8.Get(), m_prod.Get(), W, H, 1.f, 1.f, 6, 0));
        CHECK(!sdr.Record(l.list.Get(), g_prod.Get(), t_o8.Get(), t_n8.Get(), m_prod.Get(), W, H, 1.f, 1.f, 7, 1)); // no PQ on 8-bit
        ref.Record(d.device.Get(), l.list.Get(), g_ref.Get(), t_oL.Get(), t_nL.Get(), m_ref.Get());
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fa{}, fb{};
        auto ra = readback(d, l, g_prod.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, fa);
        auto rb = readback(d, l, g_ref.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, fb);
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        const auto a = rows_of<uint8_t>(ra.Get(), fa, W * 4, H);
        const auto b = rows_of<float>(rb.Get(), fb, W * 4, H);
        uint32_t off_by_more = 0, off_by_one = 0, changed = 0;
        for (size_t i = 0; i < n; ++i) {
            bool diff = false;
            for (int c = 0; c < 4; ++c) {
                const long want = (c == 3) ? 255 : std::lround(enc(b[i * 4 + c]) * 255.f);
                const long d8 = std::labs(long(a[i * 4 + c]) - want);
                if (d8 > 1) ++off_by_more; else if (d8 == 1) ++off_by_one;
                if (a[i * 4 + c] != g8[i * 4 + c]) diff = true;
            }
            if (diff) ++changed;
        }
        std::printf("SDR 8-bit: pixels %zu, changed %u, off by one code %u, off by more %u (against her shader on the decoded pictures)\n",
                    n, changed, off_by_one, off_by_more);
        CHECK(off_by_more == 0);
        CHECK(changed > 200 && changed < n - 200);
    }

    {   // Motion preparation: fill and dead zone.
        constexpr uint32_t PW = 37, PH = 21;
        std::vector<float> m(size_t(PW) * PH * 2, 0.f);
        auto at = [&](std::vector<float>& v, uint32_t x, uint32_t y) { return &v[(size_t(y) * PW + x) * 2]; };
        at(m, 10, 10)[0] = 4.f;   // one known vector: its neighbours within 16 px take it
        at(m, 30, 3)[1] = 0.3f;   // a short known vector (0.3 * 2 = 0.6 px): Frame Generation's copy drops it
        test::List l;
        REQUIRE(l.Create(d.device.Get()));
        std::vector<ComPtr<ID3D12Resource>> keep;
        auto t_in = test::CreateTexture(d.device.Get(), PW, PH, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        REQUIRE(t_in);
        upload(d, l, t_in.Get(), m.data(), PW * 8, PH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, keep);
        na::fg::MotionPrep prep;
        REQUIRE(prep.Record(l.list.Get(), d.device.Get(), t_in.Get(), 2.f, 2.f, 1.f, 0));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT ff{}, fg{};
        auto rf = readback(d, l, prep.Filled(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ff);
        auto rg = readback(d, l, prep.ForFg(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, fg);
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        auto filled = rows_of<float>(rf.Get(), ff, PW * 2, PH), for_fg = rows_of<float>(rg.Get(), fg, PW * 2, PH);
        CHECK(at(filled, 10, 10)[0] == 4.f);                 // known: unchanged
        CHECK(at(filled, 14, 10)[0] == 4.f);                 // 4 px away on the grid: filled
        CHECK(at(filled, 26, 10)[0] == 4.f);                 // 16 px away: filled (the farthest ring)
        CHECK(at(filled, 0, 20)[0] == 0.f && at(filled, 0, 20)[1] == 0.f); // nothing known within reach: stays zero
        CHECK(at(filled, 30, 3)[1] == 0.3f && at(for_fg, 30, 3)[1] == 0.f); // dead zone only in Frame Generation's copy
        CHECK(at(for_fg, 14, 10)[0] == 4.f);                 // 8 px: kept
        std::printf("motion prep: filled (14,10)=%.2f (26,10)=%.2f, short vector %.2f -> %.2f for Frame Generation\n",
                    at(filled, 14, 10)[0], at(filled, 26, 10)[0], at(filled, 30, 3)[1], at(for_fg, 30, 3)[1]);
    }
    return test::finish("fg_steady_pass_test");
}
