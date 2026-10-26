#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>

namespace na::j2 {

// Runs on J2's own direct list. The caller owns transitions and waits for all
// previous uses before replacing this object; descriptor slots follow J2 fences.
// Native ResolveSubresource handles color. Depth uses the nearest covered
// sample (max for reverse-Z, min otherwise), preserving depth rather than
// averaging foreground and background into a fictitious surface.
class MsaaResolve {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12RootSignature> root_;
    Ptr<ID3D12PipelineState> depth_pso_, write_pso_;
    Ptr<ID3D12DescriptorHeap> heap_, rtv_;
    Ptr<ID3D12Resource> depth_copy_;
    UINT stride_ = 0, rtv_stride_ = 0;
    DXGI_FORMAT color_format_ = DXGI_FORMAT_UNKNOWN, depth_view_ = DXGI_FORMAT_UNKNOWN;

    static bool compile(const char* source, const char* entry, const char* target, Ptr<ID3DBlob>& code) {
        Ptr<ID3DBlob> errors;
        return SUCCEEDED(D3DCompile(source, std::strlen(source), "J2 MSAA", nullptr, nullptr,
                                    entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT index) const {
        auto h = heap_->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(index) * stride_; return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT index) const {
        auto h = heap_->GetGPUDescriptorHandleForHeapStart(); h.ptr += UINT64(index) * stride_; return h;
    }
public:
    ID3D12Resource* DepthCopy() const { return depth_copy_.Get(); }

    bool Init(ID3D12Device* dev, UINT width, UINT height, DXGI_FORMAT color_format,
              const D3D12_RESOURCE_DESC& depth_desc, DXGI_SAMPLE_DESC color_samples, UINT slots) {
        device_ = dev; color_format_ = color_format;
        const bool stencil = depth_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                             depth_desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                             depth_desc.Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        depth_view_ = stencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
        D3D12_RESOURCE_DESC d = depth_desc;
        d.Width = width; d.Height = height; d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = stencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL; // no DENY_SHADER_RESOURCE, even when the game has it
        d.Alignment = 0;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&depth_copy_)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots * 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, slots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_)))) return false;
        stride_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtv_stride_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_DESCRIPTOR_RANGE ranges[2] = {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
        D3D12_ROOT_PARAMETER params[3]{};
        for (UINT i = 0; i < 2; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable = {1, &ranges[i]};
        }
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants = {0, 0, 3};
        D3D12_ROOT_SIGNATURE_DESC rd{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        Ptr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)))) return false;
        static const char* depth_shader = R"(
Texture2DMS<float> source : register(t0);
RWTexture2D<float> target : register(u0);
cbuffer Params : register(b0) { uint width; uint height; uint reverse_z; };
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
    if (p.x >= width || p.y >= height) return;
    uint source_width, source_height, samples;
    source.GetDimensions(source_width, source_height, samples);
    float nearest = source.Load(p.xy, 0);
    for (uint sample = 1; sample < samples; ++sample) {
        float depth = source.Load(p.xy, sample);
        nearest = reverse_z ? max(nearest, depth) : min(nearest, depth);
    }
    target[p.xy] = nearest;
})";
        Ptr<ID3DBlob> cs, vs, ps;
        if (!compile(depth_shader, "main", "cs_5_0", cs)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
        cp.pRootSignature = root_.Get(); cp.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (FAILED(dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&depth_pso_)))) return false;
        static const char* write_shader = R"(
Texture2D<float4> source : register(t0);
float4 vertex(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2,-2) + float2(-1,1), 0, 1);
}
float4 pixel(float4 pos : SV_Position) : SV_Target { return source.Load(int3(int2(pos.xy),0)); }
)";
        if (!compile(write_shader, "vertex", "vs_5_0", vs) || !compile(write_shader, "pixel", "ps_5_0", ps)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gp{};
        gp.pRootSignature = root_.Get();
        gp.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        gp.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        gp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        auto& blend = gp.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        gp.SampleMask = UINT_MAX;
        gp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gp.RasterizerState.DepthClipEnable = TRUE;
        gp.RasterizerState.MultisampleEnable = TRUE;
        gp.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        gp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gp.NumRenderTargets = 1; gp.RTVFormats[0] = color_format;
        gp.SampleDesc = color_samples;
        return SUCCEEDED(dev->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&write_pso_)));
    }

    // DepthCopy plane 0 is NPSR, destination is UAV. No game list or heap touched.
    void ResolveDepth(ID3D12GraphicsCommandList* list, ID3D12Resource* destination,
                      UINT width, UINT height, bool reverse, UINT slot) {
        const UINT base = slot * 3;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = depth_view_; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device_->CreateShaderResourceView(depth_copy_.Get(), &srv, cpu(base));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(destination, nullptr, &uav, cpu(base + 1));
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()}; list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get()); list->SetPipelineState(depth_pso_.Get());
        list->SetComputeRootDescriptorTable(0, gpu(base));
        list->SetComputeRootDescriptorTable(1, gpu(base + 1));
        const UINT params[3] = {width, height, reverse ? 1u : 0u};
        list->SetComputeRoot32BitConstants(2, 3, params, 0);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }

    // CopyTextureRegion cannot expand a plain texture to MSAA. Draw an unblended
    // fullscreen triangle into the selected RTV slice, writing all its samples.
    void WriteColor(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                    ID3D12Resource* target, UINT slice, UINT width, UINT height, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = color_format_; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device_->CreateShaderResourceView(source, &srv, cpu(slot * 3 + 2));
        D3D12_RENDER_TARGET_VIEW_DESC view{}; view.Format = color_format_;
        if (target->GetDesc().DepthOrArraySize > 1) {
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray.FirstArraySlice = slice; view.Texture2DMSArray.ArraySize = 1;
        } else view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        auto handle = rtv_->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(slot) * rtv_stride_;
        device_->CreateRenderTargetView(target, &view, handle);
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()}; list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root_.Get()); list->SetPipelineState(write_pso_.Get());
        list->SetGraphicsRootDescriptorTable(0, gpu(slot * 3 + 2));
        list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
        D3D12_RECT rect{0, 0, LONG(width), LONG(height)};
        list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &rect);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }
};
} // namespace na::j2
