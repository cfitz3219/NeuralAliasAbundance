#include "host_dx12/nvof/flow_expand.hpp"

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace na::nvof {
namespace {
const char kSource[] = R"(
Texture2D<int2>  t_grid  : register(t0);
Texture2D<float> t_depth : register(t1);
RWTexture2D<float2> u_out   : register(u0);
RWTexture2D<float>  u_valid : register(u1);
cbuffer C : register(b0) { uint grid; uint out_w; uint out_h; uint pad; float depth_clear; float depth_tol; float consistency_px; float vector_scale; };

float2 cell(int2 c, int2 dim) {
    c = clamp(c, int2(0, 0), dim - 1);
    // S10.5 -> engine pixels (NVOFA-specific), then x vector_scale when the
    // engine ran on a downscaled image (A58): output pixels = engine px * scale.
    return float2(t_grid[c]) / 32.0 * vector_scale;
}
// Raw depth at a block centre (clamped to the frame).
float cell_depth(int2 c, int2 dim) {
    c = clamp(c, int2(0, 0), dim - 1);
    int2 p = int2(c.x * grid + grid / 2, c.y * grid + grid / 2);
    p = min(p, int2(out_w - 1, out_h - 1));
    return t_depth[p];
}
// A39-DEPTH-EXPAND-REVIEW R1: the same finite / [0,1] / not-clear rule for
// the destination depth AND every neighbouring block-centre depth.
bool depth_ok(float d) {
    return (asuint(d) & 0x7fffffffu) < 0x7f800000u && d != depth_clear && d >= 0.0 && d <= 1.0;
}
void neighbours(uint2 id, int2 dim, out int2 c0, out float2 f) {
    float2 g = (float2(id) + 0.5) / float(grid) - 0.5;
    c0 = int2(floor(g));
    f = g - float2(c0);
}

[numthreads(8, 8, 1)]
void expand_plain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= out_w || id.y >= out_h) return;
    uint gw, gh; t_grid.GetDimensions(gw, gh);
    int2 dim = int2(gw, gh), c0; float2 f;
    neighbours(id.xy, dim, c0, f);
    float2 v00 = cell(c0, dim), v10 = cell(c0 + int2(1, 0), dim);
    float2 v01 = cell(c0 + int2(0, 1), dim), v11 = cell(c0 + int2(1, 1), dim);
    u_out[id.xy] = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}

[numthreads(8, 8, 1)]
void expand_depth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= out_w || id.y >= out_h) return;
    uint gw, gh; t_grid.GetDimensions(gw, gh);
    int2 dim = int2(gw, gh), c0; float2 f;
    neighbours(id.xy, dim, c0, f);
    float d = t_depth[id.xy];
    bool d_ok = depth_ok(d);
    int2 cs[4] = { c0, c0 + int2(1, 0), c0 + int2(0, 1), c0 + int2(1, 1) };
    float bw[4] = { (1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y };
    float2 acc = 0; float wsum = 0;
    float2 vmin = 1e9, vmax = -1e9;
    float tol = depth_tol * (abs(d) + 0.01);
    [unroll] for (int i = 0; i < 4; ++i) {
        float cd = cell_depth(cs[i], dim);
        float2 v = cell(cs[i], dim);
        bool same = d_ok && depth_ok(cd) && abs(cd - d) <= tol;
        float w = same ? bw[i] : 0.0;
        acc += v * w; wsum += w;
        if (same) { vmin = min(vmin, v); vmax = max(vmax, v); }
    }
    if (wsum > 0.0) {
        float2 spread = vmax - vmin;
        bool consistent = max(spread.x, spread.y) <= consistency_px;
        u_out[id.xy] = acc / wsum;
        u_valid[id.xy] = consistent ? 1.0 : 0.0;
    } else {
        // No block shares this pixel's surface (or invalid depth): not usable.
        u_out[id.xy] = 0;
        u_valid[id.xy] = 0.0;
    }
}
)";
struct Constants { uint32_t grid, out_w, out_h, pad; float depth_clear, depth_tol, consistency_px, vector_scale; };

bool compile(ID3D12Device* dev, ID3D12RootSignature* root, const char* entry, ComPtr<ID3D12PipelineState>& pso)
{
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(kSource, sizeof(kSource) - 1, "flow_expand.hlsl", nullptr, nullptr, entry, "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &err)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.CS.pShaderBytecode = code->GetBufferPointer();
    pd.CS.BytecodeLength = code->GetBufferSize();
    return SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
}
} // namespace

bool FlowExpand::Init(ID3D12Device* device, uint32_t max_frames_in_flight)
{
    if (!device || !max_frames_in_flight) return false;
    device_ = device;
    slots_ = max_frames_in_flight;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) return false;
    if (!compile(device, root_.Get(), "expand_plain", pso_plain_) || !compile(device, root_.Get(), "expand_depth", pso_depth_)) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4 * slots_;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
    stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // 1x1 dummies for unused slots.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 1; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_R32_FLOAT; d.SampleDesc.Count = 1;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&dummy_srv_))))
        return false;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummy_uav_)));
}

bool FlowExpand::Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* grid_tex, uint32_t grid, ID3D12Resource* out,
                          uint32_t out_w, uint32_t out_h, uint32_t frame_slot)
{
    return dispatch(list, grid_tex, grid, nullptr, out, out_w, out_h, frame_slot);
}

bool FlowExpand::DispatchDepthAware(ID3D12GraphicsCommandList* list, ID3D12Resource* grid_tex, uint32_t grid,
                                    const ExpandParams& params, ID3D12Resource* out, uint32_t out_w, uint32_t out_h,
                                    uint32_t frame_slot)
{
    if (!params.depth) return false;
    // A39-DEPTH-EXPAND-REVIEW R2: reject bad numeric configuration before recording.
    auto finite = [](float v) { return v == v && v - v == 0.0f; };
    if (!finite(params.depth_tolerance) || params.depth_tolerance < 0.0f) return false;
    if (!finite(params.consistency_px) || params.consistency_px < 0.0f) return false;
    if (!finite(params.depth_clear) || params.depth_clear < 0.0f || params.depth_clear > 1.0f) return false;
    return dispatch(list, grid_tex, grid, &params, out, out_w, out_h, frame_slot);
}

bool FlowExpand::dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* grid_tex, uint32_t grid, const ExpandParams* p,
                          ID3D12Resource* out, uint32_t out_w, uint32_t out_h, uint32_t frame_slot)
{
    if (!list || !pso_plain_ || !grid_tex || !out || !grid || !out_w || !out_h || frame_slot >= slots_) return false;
    if (p) {
        D3D12_RESOURCE_DESC dd = p->depth->GetDesc();
        if (dd.Width != out_w || dd.Height != out_h) return false;
        if (p->validity_out) {
            D3D12_RESOURCE_DESC vd = p->validity_out->GetDesc();
            if (vd.Width != out_w || vd.Height != out_h) return false;
        }
    }
    uint32_t base = frame_slot * 4;
    auto cpu = [&](uint32_t i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(base + i) * stride_;
        return h;
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R16G16_SINT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(grid_tex, &sd, cpu(0));
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    device_->CreateShaderResourceView(p ? p->depth : dummy_srv_.Get(), &sd, cpu(1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32G32_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(out, nullptr, &ud, cpu(2));
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    device_->CreateUnorderedAccessView(p && p->validity_out ? p->validity_out : dummy_uav_.Get(), nullptr, &ud, cpu(3));
    Constants c{grid, out_w, out_h, 0, p ? p->depth_clear : 1.0f, p ? p->depth_tolerance : 0.0f, p ? p->consistency_px : 0.0f, vector_scale_};
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_.Get());
    list->SetPipelineState(p ? pso_depth_.Get() : pso_plain_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE g = heap_->GetGPUDescriptorHandleForHeapStart();
    g.ptr += static_cast<UINT64>(base) * stride_;
    list->SetComputeRootDescriptorTable(0, g);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &c, 0);
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    return true;
}

} // namespace na::nvof
