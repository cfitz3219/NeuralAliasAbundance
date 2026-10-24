// C8b (ABI 0.2): a frame with KNOWN passes, recorded through the real D3D12
// runtime (WARP on CI), must come back as exactly those passes:
//   root signature (constants / table / root CBV) decoded per parameter;
//   graphics, compute and stream pipelines with per-stage bytecode hashes,
//   RT/DSV formats and depth state;
//   LIST_RESET generation, then every op in order (op_index 1..N): barrier,
//   clears, render targets resolved to their resources, viewport, root
//   signature, heaps, pipeline, root constants BY VALUE, root table resolved
//   to (heap, slot, version), root CBV resolved to (resource, offset), draw,
//   copy, LIST_CLOSE; nothing flagged unobserved on a fully hooked list.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/observe/producers.hpp"

#include <d3dcompiler.h>

#if defined(_MSC_VER)
#pragma warning(disable : 4324) // pipeline-stream subobjects are padded to pointer alignment by design
#endif

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
std::mutex g_m;
std::vector<std::vector<uint8_t>> g_events;

void __cdecl Sink(const na_event_batch* b, void*)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(b) + b->header_bytes;
    uint32_t used = 0;
    std::lock_guard<std::mutex> lock(g_m);
    while (used < b->payload_bytes) {
        const auto* h = reinterpret_cast<const na_event_header*>(p + used);
        if (h->size_bytes < sizeof(na_event_header)) break;
        g_events.emplace_back(p + used, p + used + h->size_bytes);
        used += h->size_bytes;
    }
}

uint64_t Fnv(const void* d, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= static_cast<const uint8_t*>(d)[i]; h *= 1099511628211ull; }
    return h;
}

test::ComPtr<ID3DBlob> Compile(const char* src, const char* entry, const char* target)
{
    test::ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
        std::printf("compile %s failed: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

const char* kShaders = R"(
cbuffer Root : register(b0) { float4 tint; };
cbuffer Cb : register(b1) { float4 extra; };
Texture2D tex : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0.5, 1); }
float4 PS(float4 pos : SV_Position) : SV_Target { return tint + extra + tex.Load(int3(0, 0, 0)); }
RWTexture2D<float4> outp : register(u0);
[numthreads(8, 8, 1)] void CS(uint3 id : SV_DispatchThreadID) { outp[id.xy] = tint; }
)";
} // namespace

int main()
{
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("observe_passes_test");
    ID3D12Device* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));

    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);

    // Root signature: 0 = 4 constants (b0), 1 = SRV table (t0), 2 = root CBV (b1).
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 4};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor = {1, 0};
    D3D12_ROOT_SIGNATURE_DESC rsd{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    test::ComPtr<ID3DBlob> rs_blob, rs_err;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err)));
    test::ComPtr<ID3D12RootSignature> rs;
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&rs))));
    // Compute root signature: 4 constants + UAV table.
    D3D12_DESCRIPTOR_RANGE urange{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER cparams[2] = {};
    cparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    cparams[0].Constants = {0, 0, 4};
    cparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    cparams[1].DescriptorTable = {1, &urange};
    D3D12_ROOT_SIGNATURE_DESC crsd{2, cparams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    test::ComPtr<ID3DBlob> crs_blob;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&crsd, D3D_ROOT_SIGNATURE_VERSION_1, &crs_blob, &rs_err)));
    test::ComPtr<ID3D12RootSignature> crs;
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, crs_blob->GetBufferPointer(), crs_blob->GetBufferSize(), IID_PPV_ARGS(&crs))));

    auto vs = Compile(kShaders, "VS", "vs_5_0"), ps = Compile(kShaders, "PS", "ps_5_0"), cs = Compile(kShaders, "CS", "cs_5_0");
    REQUIRE(vs && ps && cs);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};
    gd.pRootSignature = rs.Get();
    gd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    gd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = 0xFFFFFFFFu;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.DepthStencilState.DepthEnable = TRUE;
    gd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    gd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    gd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    gd.SampleDesc.Count = 1;
    test::ComPtr<ID3D12PipelineState> gpso, cpso, spso;
    REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&gpso))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = crs.Get();
    cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&cpso))));
    {   // Stream form of a graphics pipeline (ID3D12Device2::CreatePipelineState).
        test::ComPtr<ID3D12Device2> d2;
        if (SUCCEEDED(d.device.As(&d2))) {
            struct alignas(void*) SubRS { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature* v; };
            struct alignas(void*) SubBC { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; };
            struct alignas(void*) SubRT { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_RT_FORMAT_ARRAY v; };
            struct alignas(void*) SubFmt { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; DXGI_FORMAT v; };
            struct alignas(void*) SubTopo { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_PRIMITIVE_TOPOLOGY_TYPE v; };
            struct Stream { SubRS rs; SubBC vs; SubBC ps; SubRT rt; SubFmt dsv; SubTopo topo; } st{};
            st.rs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, rs.Get()};
            st.vs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, gd.VS};
            st.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, gd.PS};
            st.rt.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
            st.rt.v.NumRenderTargets = 1;
            st.rt.v.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            st.dsv = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT_D32_FLOAT};
            st.topo = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE};
            D3D12_PIPELINE_STATE_STREAM_DESC sd{sizeof(st), &st};
            CHECK(SUCCEEDED(d2->CreatePipelineState(&sd, IID_PPV_ARGS(&spso))));
        }
    }

    // Resources and views.
    const uint32_t W = 64, H = 32;
    auto color = test::CreateTexture(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = test::CreateTexture(dev, W, H, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto tex = test::CreateTexture(dev, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_NONE);
    auto copy_dst = test::CreateTexture(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto cbuf = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(color && depth && tex && copy_dst && cbuf);
    test::ComPtr<ID3D12DescriptorHeap> rtvh, dsvh, srvh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh))));
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh))));
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvh))));
    const uint32_t sinc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart(), dsv = dsvh->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color.Get(), nullptr, rtv);
    dev->CreateDepthStencilView(depth.Get(), nullptr, dsv);
    dev->CreateShaderResourceView(tex.Get(), nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{srvh->GetCPUDescriptorHandleForHeapStart().ptr + 2 * sinc});
    const D3D12_GPU_DESCRIPTOR_HANDLE table{srvh->GetGPUDescriptorHandleForHeapStart().ptr + 2 * sinc};

    // The frame.
    test::ComPtr<ID3D12CommandAllocator> alloc;
    test::ComPtr<ID3D12GraphicsCommandList> list;
    REQUIRE(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
    REQUIRE(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))));
    REQUIRE(SUCCEEDED(list->Close()));
    REQUIRE(SUCCEEDED(list->Reset(alloc.Get(), nullptr))); // generation 2, fully hooked table
    D3D12_RESOURCE_BARRIER b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &b);                                                             // 1
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);            // 2
    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);                                      // 3
    list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);                                           // 4
    D3D12_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
    list->RSSetViewports(1, &vp);                                                             // 5
    list->SetGraphicsRootSignature(rs.Get());                                                 // 6
    ID3D12DescriptorHeap* heaps[] = {srvh.Get()};
    list->SetDescriptorHeaps(1, heaps);                                                       // 7
    list->SetPipelineState(gpso.Get());                                                       // 8
    const uint32_t consts[4] = {0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u};
    list->SetGraphicsRoot32BitConstants(0, 4, consts, 0);                                     // 9
    list->SetGraphicsRootDescriptorTable(1, table);                                           // 10
    list->SetGraphicsRootConstantBufferView(2, cbuf->GetGPUVirtualAddress() + 256);           // 11
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);                        // (not observed)
    list->DrawInstanced(3, 1, 0, 0);                                                          // 12
    b = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);                                                             // 13
    list->CopyResource(copy_dst.Get(), color.Get());                                          // 14
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList* ls[] = {list.Get()};
    d.queue->ExecuteCommandLists(1, ls);
    d.WaitIdle();
    REQUIRE(na_observe_stop() == NA_OK);

    const na_object_id list_id = na::observe::IdOf(list.Get()), rs_id = na::observe::IdOf(rs.Get()), gpso_id = na::observe::IdOf(gpso.Get());
    const na_object_id color_id = na::observe::IdOf(color.Get()), depth_id = na::observe::IdOf(depth.Get()), cbuf_id = na::observe::IdOf(cbuf.Get());
    const na_object_id copy_id = na::observe::IdOf(copy_dst.Get()), srvh_id = na::observe::IdOf(srvh.Get());
    CHECK(list_id && rs_id && gpso_id && color_id && depth_id && cbuf_id && copy_id && srvh_id);

    std::lock_guard<std::mutex> lock(g_m);
    // Root signature parameters.
    bool rs_ok = false;
    for (const auto& e : g_events) {
        const auto* r = reinterpret_cast<const na_ev_root_signature_created*>(e.data());
        if (r->hdr.kind != NA_EV_ROOT_SIGNATURE_CREATED || r->root_signature != rs_id) continue;
        CHECK(r->param_count == 3 && r->blob_hash == Fnv(rs_blob->GetBufferPointer(), rs_blob->GetBufferSize()));
        const auto* p = reinterpret_cast<const na_root_param*>(e.data() + r->params_offset);
        rs_ok = r->param_count == 3 && p[0].param_type == NA_ROOT_CONSTANTS && p[0].count == 4 && p[0].shader_register == 0 &&
                p[1].param_type == NA_ROOT_TABLE && p[1].range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV && p[1].range_descriptors == 1 &&
                p[1].visibility == D3D12_SHADER_VISIBILITY_PIXEL && p[2].param_type == NA_ROOT_CBV && p[2].shader_register == 1;
    }
    CHECK(rs_ok);
    // Pipelines.
    bool g_ok = false, c_ok = false, s_ok = !spso;
    for (const auto& e : g_events) {
        const auto* p = reinterpret_cast<const na_ev_pipeline_created*>(e.data());
        if (p->hdr.kind != NA_EV_PIPELINE_CREATED) continue;
        if (p->pipeline == gpso_id)
            g_ok = p->pipeline_kind == NA_PIPELINE_GRAPHICS && p->root_signature == rs_id &&
                   p->stage_hash[NA_STAGE_VS] == Fnv(vs->GetBufferPointer(), vs->GetBufferSize()) &&
                   p->stage_hash[NA_STAGE_PS] == Fnv(ps->GetBufferPointer(), ps->GetBufferSize()) && p->stage_hash[NA_STAGE_CS] == 0 &&
                   p->rt_count == 1 && p->rtv_formats[0] == DXGI_FORMAT_R16G16B16A16_FLOAT && p->dsv_format == DXGI_FORMAT_D32_FLOAT &&
                   p->depth_enable == 1 && p->depth_write == 1 && p->depth_func == D3D12_COMPARISON_FUNC_GREATER && p->blend_enable_mask == 0;
        if (p->pipeline == na::observe::IdOf(cpso.Get()))
            c_ok = p->pipeline_kind == NA_PIPELINE_COMPUTE && p->stage_hash[NA_STAGE_CS] == Fnv(cs->GetBufferPointer(), cs->GetBufferSize()) &&
                   p->root_signature == na::observe::IdOf(crs.Get());
        if (spso && p->pipeline == na::observe::IdOf(spso.Get()))
            s_ok = p->pipeline_kind == NA_PIPELINE_STREAM && p->root_signature == rs_id && !(p->hdr.flags & NA_EVF_METHOD_UNOBSERVED) &&
                   p->stage_hash[NA_STAGE_VS] == Fnv(vs->GetBufferPointer(), vs->GetBufferSize()) && p->rtv_formats[0] == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                   p->dsv_format == DXGI_FORMAT_D32_FLOAT && p->depth_enable == 1 && p->depth_func == D3D12_COMPARISON_FUNC_LESS;
    }
    CHECK(g_ok);
    CHECK(c_ok);
    CHECK(s_ok);

    // Reset -> ops -> close, for our list's generation-2 recording.
    uint64_t reset_gen = 0;
    bool closed = false;
    std::vector<const na_ev_list_op*> ops;
    for (const auto& e : g_events) {
        const auto* h = reinterpret_cast<const na_event_header*>(e.data());
        if (h->kind == NA_EV_LIST_RESET) {
            const auto* r = reinterpret_cast<const na_ev_list_reset_close*>(e.data());
            if (r->list == list_id && r->hresult == 0) reset_gen = r->recording_generation;
        }
        if (h->kind == NA_EV_LIST_CLOSE) {
            const auto* r = reinterpret_cast<const na_ev_list_reset_close*>(e.data());
            if (r->list == list_id && r->recording_generation == 2) closed = true;
        }
        if (h->kind == NA_EV_LIST_OP) {
            const auto* o = reinterpret_cast<const na_ev_list_op*>(e.data());
            if (o->list == list_id && o->recording_generation == 2) ops.push_back(o);
        }
    }
    CHECK(reset_gen == 2);
    CHECK(closed);
    std::printf("recorded ops in generation 2: %zu (expected 14)\n", ops.size());
    const uint32_t expect[] = {NA_OP_RESOURCE_BARRIER, NA_OP_CLEAR_DSV, NA_OP_CLEAR_RTV, NA_OP_OM_SET_RENDER_TARGETS, NA_OP_RS_VIEWPORT,
                               NA_OP_SET_ROOT_SIGNATURE, NA_OP_SET_DESCRIPTOR_HEAPS, NA_OP_SET_PIPELINE, NA_OP_SET_ROOT_CONSTANTS,
                               NA_OP_SET_ROOT_TABLE, NA_OP_SET_ROOT_VIEW, NA_OP_DRAW, NA_OP_RESOURCE_BARRIER, NA_OP_COPY_RESOURCE};
    CHECK(ops.size() == 14);
    for (size_t i = 0; i < ops.size() && i < 14; ++i) {
        const auto* o = ops[i];
        if (o->op != expect[i] || o->op_index != i + 1 || (o->hdr.flags & NA_EVF_METHOD_UNOBSERVED))
            std::printf("FAIL op %zu: code %u (want %u) index %llu flags 0x%x\n", i + 1, o->op, expect[i],
                        static_cast<unsigned long long>(o->op_index), o->hdr.flags);
        CHECK(o->op == expect[i] && o->op_index == i + 1 && !(o->hdr.flags & NA_EVF_METHOD_UNOBSERVED));
    }
    if (ops.size() == 14) {
        const auto payload = [](const na_ev_list_op* o) { return reinterpret_cast<const uint8_t*>(o) + o->payload_offset; };
        const auto* b1 = reinterpret_cast<const na_barrier*>(payload(ops[0]));
        CHECK(ops[0]->a[0] == 1 && b1->resource == color_id && b1->state_before == D3D12_RESOURCE_STATE_COMMON &&
              b1->state_after == D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto* dref = reinterpret_cast<const na_view_ref*>(payload(ops[1]));
        CHECK(dref->resource == depth_id && dref->descriptor_kind == NA_DESC_DSV);
        const auto* cref = reinterpret_cast<const na_view_ref*>(payload(ops[2]));
        float c0 = 0;
        const uint32_t lo = static_cast<uint32_t>(ops[2]->a[0]);
        std::memcpy(&c0, &lo, 4);
        CHECK(cref->resource == color_id && c0 == 0.25f);
        const auto* om = reinterpret_cast<const na_view_ref*>(payload(ops[3]));
        CHECK(ops[3]->a[0] == 1 && ops[3]->a[1] == 1 && ops[3]->payload_bytes == 2 * sizeof(na_view_ref) && om[0].resource == color_id &&
              om[0].format == NA_UNKNOWN_U32 && om[1].resource == depth_id);
        CHECK(ops[5]->a[0] == rs_id && ops[5]->a[1] == 0);
        CHECK(ops[6]->a[0] == srvh_id);
        CHECK(ops[7]->a[0] == gpso_id);
        CHECK(ops[8]->a[0] == 0 && ops[8]->a[2] == 4 && ops[8]->payload_bytes == 16 && std::memcmp(payload(ops[8]), consts, 16) == 0);
        CHECK(ops[9]->a[0] == srvh_id && ops[9]->a[1] == 2 && ops[9]->a[2] == 1 && ops[9]->a[3] == 1);
        CHECK(ops[10]->a[0] == cbuf_id && ops[10]->a[1] == 256 && ops[10]->a[2] == 2 && ops[10]->a[3] == NA_DESC_CBV);
        CHECK(ops[11]->a[0] == 3 && ops[11]->a[1] == 1);
        CHECK(ops[13]->a[0] == copy_id && ops[13]->a[1] == color_id);
    }
    const auto st = na::observe::GetProducerStats();
    std::printf("producers: hooks %u missing %u\n", st.hooks_installed, st.hooks_missing);
    return test::finish("observe_passes_test");
}
