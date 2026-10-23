#include "core/motion_arbiter_gpu.hpp"

#include <d3dcompiler.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace na::motion::gpu {
namespace {

// The shader mirrors motion_arbiter.cpp. Comments cite the CPU lines.
const char kSource[] = R"(
Texture2D<float2> t_engine   : register(t0);
Texture2D<float>  t_evalid   : register(t1);
Texture2D<float>  t_depth    : register(t2);
Texture2D<float2> t_residual : register(t3);
Texture2D<float>  t_mask     : register(t4);
Texture2D<float>  t_rvalid   : register(t5);
RWTexture2D<float2> u_xy     : register(u0);
RWTexture2D<uint>   u_prov   : register(u1);
RWByteAddressBuffer u_stats  : register(u2);
RWTexture2D<float>  u_valid  : register(u3);   // DeriveResidual: per-pixel validity

cbuffer C : register(b0) {
    row_major float4x4 inv_vp_cur;   // row-major storage (same bytes as the CPU Mat4), row-vector algebra
    row_major float4x4 vp_prev;
    float2 jitter_cur;
    float  depth_clear;
    float  snap_px;
    float  max_px;
    uint   width;
    uint   height;
    uint   flags;          // bit0 tier1, bit1 tier1 mask, bit2 tier2, bit3 tier3, bit4 tier3 mask, bit5 tier3 valid, bit6 stats
    float2 jitter_prev;    // DeriveResidual only
};

#define F_T1      1u
#define F_T1MASK  2u
#define F_T2      4u
#define F_T3      8u
#define F_T3MASK  16u
#define F_T3VALID 32u
#define F_STATS   64u

#define PROV_INVALID 0u
#define PROV_ENGINE 1u
#define PROV_CAMERA 2u
#define PROV_CAMERA_RESIDUAL 3u
#define PROV_F_FILTERED 0x10u
#define PROV_F_OFFSCREEN 0x20u

// Bit test: exponent all ones = Inf or NaN. isnan()/isinf() can be folded away by compilers/drivers.
bool finite1(float a) { return (asuint(a) & 0x7fffffffu) < 0x7f800000u; }
bool finite2(float2 a) { return finite1(a.x) && finite1(a.y); }
void stat(uint slot) { if (flags & F_STATS) { uint old; u_stats.InterlockedAdd(slot * 4, 1u, old); } }

// mask_state: 0 valid, 1 not written, 2 malformed (motion_arbiter.cpp:28)
uint mask_state(float m) {
    if (!finite1(m) || m < 0.0 || m > 1.0) return 2u;
    return m > 0.0 ? 0u : 1u;
}

float4 mulrow(float4 p, float4x4 m) {
    // row-vector product, same summation order as the CPU (p0*m0c + p1*m1c + p2*m2c + p3*m3c)
    return float4(
        p.x * m._11 + p.y * m._21 + p.z * m._31 + p.w * m._41,
        p.x * m._12 + p.y * m._22 + p.z * m._32 + p.w * m._42,
        p.x * m._13 + p.y * m._23 + p.z * m._33 + p.w * m._43,
        p.x * m._14 + p.y * m._24 + p.z * m._34 + p.w * m._44);
}

// ReprojectPixel (motion_arbiter.cpp:69-97)
bool reproject(uint x, uint y, float depth, out float2 v, out bool offscreen) {
    v = 0; offscreen = false;
    if (!finite1(depth) || depth < 0.0 || depth > 1.0 || depth == depth_clear) return false;
    if (!finite2(jitter_cur)) return false;
    float px = (float)x + 0.5 - jitter_cur.x;
    float py = (float)y + 0.5 - jitter_cur.y;
    float4 ndc = float4(px / width * 2.0 - 1.0, 1.0 - py / height * 2.0, depth, 1.0);
    float4 world = mulrow(ndc, inv_vp_cur);
    if (!finite1(world.w) || abs(world.w) < 1e-20) return false;
    world /= world.w;
    world.w = 1.0;
    float4 clip = mulrow(world, vp_prev);
    if (!finite1(clip.w) || clip.w <= 0.0) return false;
    float nx = clip.x / clip.w, ny = clip.y / clip.w;
    if (!finite2(float2(nx, ny))) return false;
    float ppx = (nx + 1.0) * 0.5 * width, ppy = (1.0 - ny) * 0.5 * height;
    v = float2(ppx - px, ppy - py);
    offscreen = ppx < 0.0 || ppy < 0.0 || ppx >= width || ppy >= height;
    return finite2(v);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint2 p = id.xy;
    float2 o = 0;
    uint prov = PROV_INVALID;

    // ---- Tier 1 (motion_arbiter.cpp:150-164)
    if (flags & F_T1) {
        uint ms = (flags & F_T1MASK) ? mask_state(t_evalid[p]) : 0u;
        if (ms == 2u) stat(7);
        if (ms == 0u) {
            float2 e = t_engine[p];
            if (finite2(e)) {
                u_xy[p] = e;
                u_prov[p] = PROV_ENGINE;
                stat(0);
                return;
            }
            stat(7);
        }
    }

    // ---- Tier 2 (166-175)
    bool have_cam = false, offscreen = false;
    float2 c = 0;
    if (flags & F_T2) {
        have_cam = reproject(p.x, p.y, t_depth[p], c, offscreen);
        if (!have_cam) stat(7);
    }
    if (!have_cam) {
        u_xy[p] = 0;
        u_prov[p] = PROV_INVALID;
        stat(6);
        return;
    }

    // ---- Tier 3 (177-198) with shape_residual (42-59)
    float2 r = 0;
    bool have_res = false, filtered = false;
    if (flags & F_T3) {
        uint vs = (flags & F_T3VALID) ? mask_state(t_rvalid[p]) : 0u;
        if (vs == 2u) stat(7);
        bool rvalid = vs == 0u;
        float m = (flags & F_T3MASK) ? t_mask[p] : 1.0;
        if (rvalid && finite1(m) && m >= 0.0 && m <= 1.0) {
            if (m > 0.0) {
                float2 rr = t_residual[p];
                if (finite2(rr)) {
                    r = rr * m;
                    float mag = sqrt(r.x * r.x + r.y * r.y);
                    if (mag < snap_px) {
                        if (mag != 0.0) filtered = true;
                        r = 0;
                        stat(3);
                        have_res = false;
                    } else {
                        if (mag > max_px) {
                            float k = max_px / mag;
                            r *= k;
                            stat(4);
                            filtered = true;
                        }
                        have_res = true;
                    }
                } else {
                    stat(7);
                }
            }
        } else if (rvalid) {
            stat(7);
        }
    }

    o = c + r;
    prov = have_res ? PROV_CAMERA_RESIDUAL : PROV_CAMERA;
    if (filtered) prov |= PROV_F_FILTERED;
    if (offscreen) { prov |= PROV_F_OFFSCREEN; stat(5); }
    stat(1);
    if (have_res) stat(2);
    u_xy[p] = o;
    u_prov[p] = prov;
}

// DeriveResidual (motion_arbiter.cpp:99-122): residual = raw_flow - V_cam - (jitter_prev - jitter_cur);
// valid = 1 where the camera vector exists, the raw flow is finite and (flag F_T3VALID)
// the supplied raw validity t_rvalid is in the mask domain's valid range; else 0 with residual 0.
// Inputs: t_residual = RAW flow (RG32), t_depth, optional t_rvalid; outputs u_xy = residual, u_valid.
[numthreads(8, 8, 1)]
void derive_residual(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    uint2 p = id.xy;
    float2 jd = jitter_prev - jitter_cur;
    float2 f = t_residual[p];
    float2 c = 0; bool off = false;
    bool rv_ok = (flags & F_T3VALID) ? (mask_state(t_rvalid[p]) == 0u) : true;
    bool ok = rv_ok && finite2(jd) && reproject(p.x, p.y, t_depth[p], c, off) && finite2(f);
    u_xy[p] = ok ? f - c - jd : float2(0, 0);
    u_valid[p] = ok ? 1.0 : 0.0;
}
)";

struct Constants {
    float inv_vp_cur[16];
    float vp_prev[16];
    float jitter[2];
    float depth_clear, snap_px, max_px;
    uint32_t width, height, flags;
    float jitter_prev[2];
};
static_assert(sizeof(Constants) == 42 * 4, "root constant size");

bool create_dummy(ID3D12Device* dev, DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 1;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                  IID_PPV_ARGS(&out)));
}

} // namespace

bool Arbiter::Init(ID3D12Device* device, uint32_t max_frames_in_flight)
{
    if (!device || max_frames_in_flight == 0) return false;
    device_ = device;
    slots_ = max_frames_in_flight;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 6;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 6;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    params[1].Constants.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_))))
        return false;

    ComPtr<ID3DBlob> code;
    // Strict IEEE so NaN/Inf checks behave like the CPU (no fast-math folding).
    UINT cf = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "motion_arbiter.hlsl", nullptr, nullptr, "main", "cs_5_0", cf, 0,
                          &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_.Get();
    pd.CS.pShaderBytecode = code->GetBufferPointer();
    pd.CS.BytecodeLength = code->GetBufferSize();
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_)))) return false;
    ComPtr<ID3DBlob> code2;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "motion_arbiter.hlsl", nullptr, nullptr, "derive_residual", "cs_5_0", cf, 0,
                          &code2, &err)))
        return false;
    pd.CS.pShaderBytecode = code2->GetBufferPointer();
    pd.CS.BytecodeLength = code2->GetBufferSize();
    if (FAILED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_derive_)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 10 * slots_;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
    stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!create_dummy(device, DXGI_FORMAT_R32_FLOAT, dummy_) || !create_dummy(device, DXGI_FORMAT_R32G32_FLOAT, dummy2_))
        return false;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = 1; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   nullptr, IID_PPV_ARGS(&dummy_uav_))))
            return false;
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 32;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     nullptr, IID_PPV_ARGS(&dummy_stats_)));
}

bool Arbiter::Dispatch(ID3D12GraphicsCommandList* list, const Inputs& in, const Outputs& out, uint32_t frame_slot)
{
    if (!list || !pso_ || !out.xy || !out.provenance || out.width == 0 || out.height == 0 || frame_slot >= slots_)
        return false;
    if (!ValidateConfig(in.config)) return false;
    // Extent checks mirror field_ok(): a tier whose extent mismatches is rejected as a whole.
    auto extent_ok = [&](ID3D12Resource* r) {
        if (!r) return false;
        D3D12_RESOURCE_DESC d = r->GetDesc();
        return d.Width == out.width && d.Height == out.height;
    };
    uint32_t flags = 0;
    bool use1 = in.tier1_valid_flag && extent_ok(in.tier1_motion) && (!in.tier1_valid || extent_ok(in.tier1_valid));
    bool use2 = in.tier2_valid_flag && extent_ok(in.depth);
    bool use3 = in.tier3_valid_flag && extent_ok(in.tier3_residual) && (!in.tier3_mask || extent_ok(in.tier3_mask)) &&
                (!in.tier3_valid || extent_ok(in.tier3_valid));
    if (use1) flags |= 1u;
    if (use1 && in.tier1_valid) flags |= 2u;
    if (use2) flags |= 4u;
    if (use3) flags |= 8u;
    if (use3 && in.tier3_mask) flags |= 16u;
    if (use3 && in.tier3_valid) flags |= 32u;
    if (out.stats) flags |= 64u;
    // Whole-tier rejections are counted by the caller-visible stats exactly
    // like the CPU (motion_arbiter.cpp:138-140): add width*height per rejected tier.
    uint32_t whole_rejects = 0;
    if (in.tier1_valid_flag && !use1) ++whole_rejects;
    if (in.tier2_valid_flag && !use2) ++whole_rejects;
    if (in.tier3_valid_flag && !use3) ++whole_rejects;

    uint32_t base = frame_slot * 10;
    auto cpu = [&](uint32_t i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(base + i) * stride_;
        return h;
    };
    auto srv = [&](uint32_t i, ID3D12Resource* r, DXGI_FORMAT fmt, ID3D12Resource* fallback) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r ? r : fallback, &d, cpu(i));
    };
    srv(0, use1 ? in.tier1_motion : nullptr, DXGI_FORMAT_R32G32_FLOAT, dummy2_.Get());
    srv(1, use1 ? in.tier1_valid : nullptr, DXGI_FORMAT_R32_FLOAT, dummy_.Get());
    srv(2, use2 ? in.depth : nullptr, DXGI_FORMAT_R32_FLOAT, dummy_.Get());
    srv(3, use3 ? in.tier3_residual : nullptr, DXGI_FORMAT_R32G32_FLOAT, dummy2_.Get());
    srv(4, use3 ? in.tier3_mask : nullptr, DXGI_FORMAT_R32_FLOAT, dummy_.Get());
    srv(5, use3 ? in.tier3_valid : nullptr, DXGI_FORMAT_R32_FLOAT, dummy_.Get());
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Format = DXGI_FORMAT_R32G32_FLOAT;
        device_->CreateUnorderedAccessView(out.xy, nullptr, &d, cpu(6));
        d.Format = DXGI_FORMAT_R8_UINT;
        device_->CreateUnorderedAccessView(out.provenance, nullptr, &d, cpu(7));
        D3D12_UNORDERED_ACCESS_VIEW_DESC b{};
        b.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        b.Format = DXGI_FORMAT_R32_TYPELESS;
        b.Buffer.NumElements = 8;
        b.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device_->CreateUnorderedAccessView(out.stats ? out.stats : dummy_stats_.Get(), nullptr, &b, cpu(8));
        d.Format = DXGI_FORMAT_R32_FLOAT;
        device_->CreateUnorderedAccessView(dummy_uav_.Get(), nullptr, &d, cpu(9)); // u3 unused by main
    }

    Constants c{};
    std::memcpy(c.inv_vp_cur, in.inv_view_proj_current.m, sizeof(c.inv_vp_cur));
    std::memcpy(c.vp_prev, in.view_proj_previous.m, sizeof(c.vp_prev));
    c.jitter[0] = in.jitter_current_px[0];
    c.jitter[1] = in.jitter_current_px[1];
    c.depth_clear = in.depth_clear;
    c.snap_px = in.config.residual_snap_px;
    c.max_px = in.config.residual_max_px;
    c.width = out.width;
    c.height = out.height;
    c.flags = flags;

    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pso_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE g = heap_->GetGPUDescriptorHandleForHeapStart();
    g.ptr += static_cast<UINT64>(base) * stride_;
    list->SetComputeRootDescriptorTable(0, g);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &c, 0);
    list->Dispatch((out.width + 7) / 8, (out.height + 7) / 8, 1);
    // Whole-tier rejections are known on the CPU side (extent checks) and are
    // added by DecodeStats, exactly like motion_arbiter.cpp:138-140.
    whole_rejects_last_ = whole_rejects * static_cast<uint64_t>(out.width) * out.height;
    return true;
}

bool Arbiter::DispatchDeriveResidual(ID3D12GraphicsCommandList* list, ID3D12Resource* raw_flow, ID3D12Resource* depth,
                                     const Inputs& camera, const float jitter_previous_px[2], ID3D12Resource* residual_out,
                                     ID3D12Resource* valid_out, uint32_t width, uint32_t height, uint32_t frame_slot,
                                     ID3D12Resource* raw_valid)
{
    if (!list || !pso_derive_ || !raw_flow || !depth || !residual_out || !valid_out || !jitter_previous_px ||
        width == 0 || height == 0 || frame_slot >= slots_ || !camera.tier2_valid_flag)
        return false;
    auto extent_ok = [&](ID3D12Resource* r) {
        D3D12_RESOURCE_DESC d = r->GetDesc();
        return d.Width == width && d.Height == height;
    };
    if (!extent_ok(raw_flow) || !extent_ok(depth) || !extent_ok(residual_out) || !extent_ok(valid_out)) return false;
    if (raw_valid && !extent_ok(raw_valid)) return false;

    uint32_t base = frame_slot * 10;
    auto cpu = [&](uint32_t i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(base + i) * stride_;
        return h;
    };
    auto srv = [&](uint32_t i, ID3D12Resource* r, DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(r, &d, cpu(i));
    };
    srv(0, dummy2_.Get(), DXGI_FORMAT_R32G32_FLOAT);
    srv(1, dummy_.Get(), DXGI_FORMAT_R32_FLOAT);
    srv(2, depth, DXGI_FORMAT_R32_FLOAT);
    srv(3, raw_flow, DXGI_FORMAT_R32G32_FLOAT);
    srv(4, dummy_.Get(), DXGI_FORMAT_R32_FLOAT);
    srv(5, raw_valid ? raw_valid : dummy_.Get(), DXGI_FORMAT_R32_FLOAT);
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    d.Format = DXGI_FORMAT_R32G32_FLOAT;
    device_->CreateUnorderedAccessView(residual_out, nullptr, &d, cpu(6));
    d.Format = DXGI_FORMAT_R8_UINT;
    // u1 (provenance) is unused by derive_residual; bind a dummy typed UAV.
    d.Format = DXGI_FORMAT_R32_FLOAT;
    device_->CreateUnorderedAccessView(dummy_uav_.Get(), nullptr, &d, cpu(7));
    D3D12_UNORDERED_ACCESS_VIEW_DESC b{};
    b.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    b.Format = DXGI_FORMAT_R32_TYPELESS;
    b.Buffer.NumElements = 8;
    b.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(dummy_stats_.Get(), nullptr, &b, cpu(8));
    d.Format = DXGI_FORMAT_R32_FLOAT;
    device_->CreateUnorderedAccessView(valid_out, nullptr, &d, cpu(9));

    Constants c{};
    std::memcpy(c.inv_vp_cur, camera.inv_view_proj_current.m, sizeof(c.inv_vp_cur));
    std::memcpy(c.vp_prev, camera.view_proj_previous.m, sizeof(c.vp_prev));
    c.jitter[0] = camera.jitter_current_px[0];
    c.jitter[1] = camera.jitter_current_px[1];
    c.jitter_prev[0] = jitter_previous_px[0];
    c.jitter_prev[1] = jitter_previous_px[1];
    c.depth_clear = camera.depth_clear;
    c.snap_px = 0.f;
    c.max_px = 0.f;
    c.width = width;
    c.height = height;
    c.flags = 4u | (raw_valid ? 32u : 0u); // tier 2 (+ raw validity supplied)
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(pso_derive_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE g = heap_->GetGPUDescriptorHandleForHeapStart();
    g.ptr += static_cast<UINT64>(base) * stride_;
    list->SetComputeRootDescriptorTable(0, g);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &c, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

Stats Arbiter::DecodeStats(const uint32_t counters[8]) const
{
    Stats s{};
    s.pixels_engine = counters[STAT_ENGINE];
    s.pixels_camera = counters[STAT_CAMERA];
    s.pixels_residual_added = counters[STAT_RESIDUAL_ADDED];
    s.pixels_residual_snapped = counters[STAT_RESIDUAL_SNAPPED];
    s.pixels_residual_clamped = counters[STAT_RESIDUAL_CLAMPED];
    s.pixels_offscreen = counters[STAT_OFFSCREEN];
    s.pixels_invalid = counters[STAT_INVALID];
    s.pixels_rejected = counters[STAT_REJECTED] + whole_rejects_last_;
    if (s.pixels_engine) s.tier_used = Tier::EngineDxil;
    else if (s.pixels_residual_added) s.tier_used = Tier::ResidualOpticalFlow;
    else if (s.pixels_camera) s.tier_used = Tier::CameraReprojection;
    else s.tier_used = Tier::None;
    return s;
}

} // namespace na::motion::gpu
