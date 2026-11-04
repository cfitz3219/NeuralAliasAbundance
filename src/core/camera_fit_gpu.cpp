#include "core/camera_fit_gpu.hpp"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace na::motion::gpu {
namespace {

constexpr uint32_t kStep = 8;        // sample spacing (the AMD engine's 8x8 block grid)
constexpr uint32_t kIterations = 6;  // Gauss-Newton steps from theta = 0 (3 stopped short on real AMD flow, PC 2026-11-04)
constexpr uint32_t kValues = 30;     // per-sample accumulators: H (21), g (6), energy, count, inliers

const char kSource[] = R"(
Texture2D<float>  t_depth  : register(t0);
Texture2D<float2> t_flow   : register(t1);   // current -> previous, pixels, jitter-stabilised
Texture2D<float>  t_fvalid : register(t2);
ByteAddressBuffer t_basis  : register(t3);   // C (16 floats), then G_0..G_5 (16 each)
RWByteAddressBuffer u_part : register(u0);   // groups x 30 partial sums
RWByteAddressBuffer u_fix  : register(u1);   // see camera_fit_gpu.hpp

cbuffer K : register(b0) {
    uint width, height, step, nbx;
    float jx, jy, depth_clear, robust;
    uint iteration, iterations, groups, samples;
};

#define NV 30
bool finite1(float a) { return (asuint(a) & 0x7fffffffu) < 0x7f800000u; }

float4 mulrow(float4 p, float4x4 m) {
    return float4(
        p.x * m._11 + p.y * m._21 + p.z * m._31 + p.w * m._41,
        p.x * m._12 + p.y * m._22 + p.z * m._32 + p.w * m._42,
        p.x * m._13 + p.y * m._23 + p.z * m._33 + p.w * m._43,
        p.x * m._14 + p.y * m._24 + p.z * m._34 + p.w * m._44);
}
float4x4 loadm(uint off) {
    return float4x4(asfloat(t_basis.Load4(off)), asfloat(t_basis.Load4(off + 16)),
                    asfloat(t_basis.Load4(off + 32)), asfloat(t_basis.Load4(off + 48)));
}
float theta(uint k) { return iteration > 0 ? asfloat(u_fix.Load((17 + k) * 4)) : 0.0; }

groupshared float sh[64 * NV];

[numthreads(64, 1, 1)]
void accum(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    float v[NV];
    [unroll] for (uint i = 0; i < NV; ++i) v[i] = 0;
    const uint s = gid.x * 64 + gi;
    const uint x = (s % nbx) * step + step / 2, y = (s / nbx) * step + step / 2;
    if (s < samples && x < width && y < height) {
        const float d = t_depth[uint2(x, y)];
        const float2 f = t_flow[uint2(x, y)];
        const float fv = t_fvalid[uint2(x, y)];
        const bool ok = finite1(d) && d > 0.0 && d < 1.0 && d != depth_clear && finite1(fv) && fv > 0.0 && fv <= 1.0 &&
                        finite1(f.x) && finite1(f.y) && abs(f.x) < 96.0 && abs(f.y) < 96.0;
        if (ok) {
            // Same pixel convention as the arbiter's reproject(): sample at centre - jitter.
            const float px = (float)x + 0.5 - jx, py = (float)y + 0.5 - jy;
            const float4 ndc = float4(px / width * 2.0 - 1.0, 1.0 - py / height * 2.0, d, 1.0);
            float4 clip = mulrow(ndc, loadm(0));
            float4 dc[6];
            [unroll] for (uint k = 0; k < 6; ++k) {
                dc[k] = mulrow(ndc, loadm(64 + k * 64));
                clip += theta(k) * dc[k];
            }
            if (finite1(clip.w) && clip.w > 1e-12) {
                const float iw = 1.0 / clip.w;
                const float2 pred = float2((clip.x * iw + 1.0) * 0.5 * width, (1.0 - clip.y * iw) * 0.5 * height);
                const float2 r = f - (pred - float2(px, py)); // flow minus the camera vector
                float J0[6], J1[6];
                [unroll] for (uint k = 0; k < 6; ++k) {
                    J0[k] = (dc[k].x * clip.w - clip.x * dc[k].w) * iw * iw * 0.5 * width;
                    J1[k] = -(dc[k].y * clip.w - clip.y * dc[k].w) * iw * iw * 0.5 * height;
                }
                const float rr = dot(r, r), c2 = robust * robust;
                const float w = 1.0 / (1.0 + rr / c2); // Cauchy IRLS weight
                if (finite1(rr)) {
                    uint idx = 0;
                    [unroll] for (uint a = 0; a < 6; ++a)
                        [unroll] for (uint b = a; b < 6; ++b) v[idx++] = w * (J0[a] * J0[b] + J1[a] * J1[b]);
                    [unroll] for (uint a2 = 0; a2 < 6; ++a2) v[21 + a2] = w * (J0[a2] * r.x + J1[a2] * r.y);
                    v[27] = 0.5 * c2 * log(1.0 + rr / c2);
                    v[28] = 1.0;
                    v[29] = rr < 4.0 ? 1.0 : 0.0;
                }
            }
        }
    }
    [unroll] for (uint i2 = 0; i2 < NV; ++i2) sh[gi * NV + i2] = v[i2];
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 32; st > 0; st >>= 1) {
        if (gi < st) for (uint i3 = 0; i3 < NV; ++i3) sh[gi * NV + i3] += sh[(gi + st) * NV + i3];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) for (uint i4 = 0; i4 < NV; ++i4) u_part.Store((gid.x * NV + i4) * 4, asuint(sh[i4]));
}

groupshared float red[256];
groupshared float tot[NV];

[numthreads(256, 1, 1)]
void solve(uint gi : SV_GroupIndex) {
    for (uint i = 0; i < NV; ++i) {
        float s = 0;
        for (uint g = gi; g < groups; g += 256) s += asfloat(u_part.Load((g * NV + i) * 4));
        red[gi] = s;
        GroupMemoryBarrierWithGroupSync();
        for (uint st = 128; st > 0; st >>= 1) {
            if (gi < st) red[gi] += red[gi + st];
            GroupMemoryBarrierWithGroupSync();
        }
        if (gi == 0) tot[i] = red[0];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi != 0) return;
    const float n = tot[28];
    const float e = n > 0 ? tot[27] / n : 0;
    float th[6];
    [unroll] for (uint k = 0; k < 6; ++k) th[k] = theta(k);
    if (iteration == 0) { u_fix.Store(23 * 4, asuint(e)); u_fix.Store(24 * 4, asuint(n)); }
    if (iteration < iterations) {
        float A[6][6], b[6], sc[6];
        uint idx = 0;
        [unroll] for (uint a = 0; a < 6; ++a)
            [unroll] for (uint c = a; c < 6; ++c) { A[a][c] = tot[idx]; A[c][a] = tot[idx]; ++idx; }
        [unroll] for (uint a1 = 0; a1 < 6; ++a1) { b[a1] = tot[21 + a1]; sc[a1] = A[a1][a1] > 1e-20 ? rsqrt(A[a1][a1]) : 0.0; }
        // Jacobi scaling + Levenberg damping, then Cholesky.
        [unroll] for (uint a2 = 0; a2 < 6; ++a2) {
            [unroll] for (uint c2 = 0; c2 < 6; ++c2) A[a2][c2] *= sc[a2] * sc[c2];
            A[a2][a2] = (sc[a2] > 0 ? A[a2][a2] : 0.0) + 0.01 + (sc[a2] > 0 ? 0.0 : 1.0);
            b[a2] *= sc[a2];
        }
        bool ok = n >= 64;
        float L[6][6];
        [unroll] for (uint r = 0; r < 6; ++r) [unroll] for (uint c3 = 0; c3 < 6; ++c3) L[r][c3] = 0;
        [unroll] for (uint j = 0; j < 6; ++j) {
            float dsum = A[j][j];
            [unroll] for (uint m = 0; m < j; ++m) dsum -= L[j][m] * L[j][m];
            if (!(dsum > 1e-12)) ok = false;
            L[j][j] = sqrt(max(dsum, 1e-12));
            [unroll] for (uint i5 = j + 1; i5 < 6; ++i5) {
                float t = A[i5][j];
                [unroll] for (uint m2 = 0; m2 < j; ++m2) t -= L[i5][m2] * L[j][m2];
                L[i5][j] = t / L[j][j];
            }
        }
        float yv[6], xv[6];
        [unroll] for (uint i6 = 0; i6 < 6; ++i6) {
            float t = b[i6];
            [unroll] for (uint m3 = 0; m3 < i6; ++m3) t -= L[i6][m3] * yv[m3];
            yv[i6] = t / L[i6][i6];
        }
        [unroll] for (int i7 = 5; i7 >= 0; --i7) {
            float t = yv[i7];
            [unroll] for (int m4 = i7 + 1; m4 < 6; ++m4) t -= L[m4][i7] * xv[m4];
            xv[i7] = t / L[i7][i7];
        }
        [unroll] for (uint k2 = 0; k2 < 6; ++k2) {
            const float delta = xv[k2] * sc[k2];
            if (!finite1(delta)) ok = false;
            if (ok) th[k2] += delta;
        }
        if (!ok) {
            [unroll] for (uint k3 = 0; k3 < 6; ++k3) th[k3] = theta(k3);
        }
        [unroll] for (uint k4 = 0; k4 < 6; ++k4) u_fix.Store((17 + k4) * 4, asuint(th[k4]));
        return;
    }
    // Final evaluation (energy at the fitted theta): keep it only if it clearly explains the
    // image better than today's camera motion, on enough samples, and there was motion to fix.
    const float e0 = asfloat(u_fix.Load(23 * 4)), n0 = asfloat(u_fix.Load(24 * 4));
    bool accept = n >= 256.0 && n0 >= 256.0 && e0 > 0.05 && e < 0.7 * e0;
    [unroll] for (uint k5 = 0; k5 < 6; ++k5) accept = accept && finite1(th[k5]);
    u_fix.Store(25 * 4, asuint(e));
    u_fix.Store(26 * 4, asuint(n));
    u_fix.Store(27 * 4, accept ? 1u : 0u);
    [unroll] for (uint r2 = 0; r2 < 4; ++r2) {
        [unroll] for (uint c4 = 0; c4 < 4; ++c4) {
            float gsum = 0;
            [unroll] for (uint k6 = 0; k6 < 6; ++k6) gsum += th[k6] * asfloat(t_basis.Load(64 + k6 * 64 + (r2 * 4 + c4) * 4));
            u_fix.Store((r2 * 4 + c4) * 4, asuint(accept ? gsum : 0.0));
        }
    }
    u_fix.Store(16 * 4, accept ? 1u : 0u);
}
)";

struct Constants {
    uint32_t width, height, step, nbx;
    float jx, jy, depth_clear, robust;
    uint32_t iteration, iterations, groups, samples;
};
static_assert(sizeof(Constants) == 12 * 4, "root constants");

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return x;
}
D3D12_RESOURCE_BARRIER uav(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    x.UAV.pResource = r;
    return x;
}

} // namespace

bool CameraFit::Init(ID3D12Device* device)
{
    if (!device) return false;
    device_ = device;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 4;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ComPtr<ID3DBlob> sig, err, a, s;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_))))
        return false;
    const UINT cf = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "camera_fit.hlsl", nullptr, nullptr, "accum", "cs_5_0", cf, 0, &a, &err)) ||
        FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "camera_fit.hlsl", nullptr, nullptr, "solve", "cs_5_0", cf, 0, &s, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.CS = {a->GetBufferPointer(), a->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_accum_)))) return false;
    pd.CS = {s->GetBufferPointer(), s->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_solve_)))) { pso_accum_.Reset(); return false; }
    stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool CameraFit::prepare(Slot& s, uint32_t groups)
{
    auto buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav_ok, ComPtr<ID3D12Resource>& r) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = uav_ok ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        return SUCCEEDED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    };
    if (!s.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.heap)))) return false;
        if (!buffer(sizeof(FitBasis), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false, s.upload) ||
            !buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true, s.fix) ||
            !buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false, s.readback)) {
            s.heap.Reset();
            return false;
        }
    }
    if (s.groups < groups) {
        s.part.Reset();
        if (!buffer(UINT64(groups) * kValues * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, s.part)) return false;
        s.groups = groups;
    }
    return true;
}

ID3D12Resource* CameraFit::Record(ID3D12GraphicsCommandList* l, uint32_t slot, ID3D12Resource* depth, ID3D12Resource* flow,
                                  ID3D12Resource* flow_valid, const FitBasis& basis, const float jitter_px[2], float depth_clear,
                                  uint32_t width, uint32_t height)
{
    if (!Ready() || !l || !depth || !flow || !flow_valid || !jitter_px || !width || !height) return nullptr;
    const uint32_t nbx = (width + kStep - 1) / kStep, nby = (height + kStep - 1) / kStep;
    const uint32_t samples = nbx * nby, groups = (samples + 63) / 64;
    if (slots_.size() <= slot) slots_.resize(slot + 1);
    Slot& s = slots_[slot];
    if (!prepare(s, groups)) return nullptr;
    void* p = nullptr;
    if (FAILED(s.upload->Map(0, nullptr, &p)) || !p) return nullptr;
    std::memcpy(p, &basis, sizeof basis);
    s.upload->Unmap(0, nullptr);

    auto cpu = [&](uint32_t i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = s.heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * stride_;
        return h;
    };
    auto tex = [&](uint32_t i, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = f;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r, &d, cpu(i));
    };
    tex(0, depth, DXGI_FORMAT_R32_FLOAT);
    tex(1, flow, DXGI_FORMAT_R32G32_FLOAT);
    tex(2, flow_valid, DXGI_FORMAT_R32_FLOAT);
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = sizeof(FitBasis) / 4;
        d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device_->CreateShaderResourceView(s.upload.Get(), &d, cpu(3));
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        u.Buffer.NumElements = groups * kValues;
        device_->CreateUnorderedAccessView(s.part.Get(), nullptr, &u, cpu(4));
        u.Buffer.NumElements = 64;
        device_->CreateUnorderedAccessView(s.fix.Get(), nullptr, &u, cpu(5));
    }
    constexpr auto S = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, U = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    {
        auto b = transition(s.fix.Get(), S, U);
        l->ResourceBarrier(1, &b);
    }
    ID3D12DescriptorHeap* heaps[] = {s.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(root_.Get());
    l->SetComputeRootDescriptorTable(0, s.heap->GetGPUDescriptorHandleForHeapStart());
    Constants c{};
    c.width = width; c.height = height; c.step = kStep; c.nbx = nbx;
    c.jx = jitter_px[0]; c.jy = jitter_px[1]; c.depth_clear = depth_clear; c.robust = 1.5f;
    c.iterations = kIterations; c.groups = groups; c.samples = samples;
    for (uint32_t it = 0; it <= kIterations; ++it) {
        c.iteration = it;
        l->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &c, 0);
        l->SetPipelineState(pso_accum_.Get());
        l->Dispatch(groups, 1, 1);
        D3D12_RESOURCE_BARRIER b[2] = {uav(s.part.Get()), uav(s.fix.Get())};
        l->ResourceBarrier(2, b);
        l->SetPipelineState(pso_solve_.Get());
        l->Dispatch(1, 1, 1);
        l->ResourceBarrier(2, b);
    }
    {
        auto b = transition(s.fix.Get(), U, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->ResourceBarrier(1, &b);
        l->CopyBufferRegion(s.readback.Get(), 0, s.fix.Get(), 0, 256);
        b = transition(s.fix.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, S);
        l->ResourceBarrier(1, &b);
    }
    s.recorded = true;
    return s.fix.Get();
}

bool CameraFit::Collect(uint32_t slot, FitResult* out)
{
    if (!out || slot >= slots_.size() || !slots_[slot].recorded || !slots_[slot].readback) return false;
    Slot& s = slots_[slot];
    s.recorded = false;
    void* p = nullptr;
    D3D12_RANGE range{0, 256};
    if (FAILED(s.readback->Map(0, &range, &p)) || !p) return false;
    uint32_t w[64];
    std::memcpy(w, p, sizeof w);
    D3D12_RANGE none{0, 0};
    s.readback->Unmap(0, &none);
    auto f = [&](int i) { float v; std::memcpy(&v, &w[i], 4); return v; };
    *out = FitResult{};
    out->accepted = w[27] != 0;
    for (int k = 0; k < 6; ++k) out->theta[k] = f(17 + k);
    out->energy0 = f(23);
    out->samples0 = static_cast<uint32_t>(f(24));
    out->energy = f(25);
    out->samples = static_cast<uint32_t>(f(26));
    return true;
}

} // namespace na::motion::gpu
