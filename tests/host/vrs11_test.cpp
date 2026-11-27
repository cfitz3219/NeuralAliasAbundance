// D3D11 coarse shading through NVIDIA's driver interface (NVAPI), the deciding fixture for DLSS
// Quality / Performance on the native DX11 route (the fold, core/fold_shading.hpp). D3D11 itself has
// no shading rate; NVAPI offers the same hardware feature for D3D11 on NVIDIA cards. Answers:
//  (i)   immediate context: a full-screen draw at 2x1 / 2x2 runs one half / one quarter of the pixel-shader
//        invocations (pipeline statistics), and a draw after switching it off runs at full rate;
//  (ii)  deferred contexts: does the state record into a command list, or does the immediate context's
//        state apply around ExecuteCommandList, or neither;
//  (iii) is the state cleared by RSSetViewports, ClearState, FinishCommandList / ExecuteCommandList;
//  (iv)  the capability check on this card (and the reason text when absent);
//  (v)   which position a coarse block is shaded at (SV_Position), against D3D12's coarse shading on the
//        same card: the fold keeps the last pixel of each block because D3D12 shades it at the block's centre.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <nvapi.h>

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
int g_failed = 0;
void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

using QueryFn = void*(__cdecl*)(unsigned int);
using InitFn = NvAPI_Status(__cdecl*)();
using CapsFn = NvAPI_Status(__cdecl*)(IUnknown*, NvU32, NV_D3D1x_GRAPHICS_CAPS*);
using CreateSrrvFn = NvAPI_Status(__cdecl*)(ID3D11Device*, ID3D11Resource*, NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC*, ID3D11NvShadingRateResourceView**);
using SetSrrvFn = NvAPI_Status(__cdecl*)(IUnknown*, ID3D11NvShadingRateResourceView*);
using SetRatesFn = NvAPI_Status(__cdecl*)(IUnknown*, NV_D3D11_VIEWPORTS_SHADING_RATE_DESC*);
CapsFn g_caps = nullptr;
CreateSrrvFn g_create_srrv = nullptr;
SetSrrvFn g_set_srrv = nullptr;
SetRatesFn g_set_rates = nullptr;

constexpr UINT W = 256, H = 256;
constexpr char kHlsl[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 p : SV_Position) : SV_Target { return float4(p.xy, 0, 1); }
)";

struct Game {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> imm;
    ComPtr<ID3D11Texture2D> rt, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Texture2D> rate_surface;
    ComPtr<ID3D11NvShadingRateResourceView> srrv;
    ComPtr<ID3D11Query> stats;
};

ComPtr<ID3DBlob> compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> b, err;
    if (FAILED(D3DCompile(kHlsl, sizeof kHlsl - 1, "vrs11", nullptr, nullptr, entry, target, 0, 0, &b, &err))) {
        if (err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
        return nullptr;
    }
    return b;
}

ComPtr<IDXGIAdapter1> nvidia_adapter()
{
    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return nullptr;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; SUCCEEDED(f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a))); ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.VendorId == 0x10DE) { std::printf("device: %ls\n", d.Description); return a; }
        a.Reset();
    }
    return nullptr;
}

// The pipeline state a game frame would have before its scene draw (target, shaders, viewport).
void bind(ID3D11DeviceContext* c, Game& g)
{
    ID3D11RenderTargetView* rtv = g.rtv.Get();
    c->OMSetRenderTargets(1, &rtv, nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(g.vs.Get(), nullptr, 0);
    c->PSSetShader(g.ps.Get(), nullptr, 0);
    const D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
    c->RSSetViewports(1, &vp);
}

// Coarse shading on `c`: the rate image (every tile index 0) and viewport 0's table, or off.
NvAPI_Status coarse(ID3D11DeviceContext* c, Game& g, NV_PIXEL_SHADING_RATE rate, bool bind_view = true)
{
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC vp{};
    vp.enableVariablePixelShadingRate = rate != NV_PIXEL_X1_PER_RASTER_PIXEL;
    for (auto& r : vp.shadingRateTable) r = rate;
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC d{};
    d.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    d.numViewports = rate == NV_PIXEL_X1_PER_RASTER_PIXEL ? 0 : 1;
    d.pViewports = &vp;
    if (bind_view) {
        const NvAPI_Status s = g_set_srrv(c, rate == NV_PIXEL_X1_PER_RASTER_PIXEL ? nullptr : g.srrv.Get());
        if (s != NVAPI_OK) return s;
    }
    return g_set_rates(c, &d);
}

// Pixel-shader invocations of one full-screen draw issued on the immediate context.
UINT64 count_draw(Game& g, bool rebind = true)
{
    if (rebind) bind(g.imm.Get(), g);
    g.imm->Begin(g.stats.Get());
    g.imm->Draw(3, 0);
    g.imm->End(g.stats.Get());
    D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
    while (g.imm->GetData(g.stats.Get(), &s, sizeof s, 0) == S_FALSE) Sleep(0);
    return s.PSInvocations;
}
// The same around ExecuteCommandList: invocations of the executed list.
UINT64 count_list(Game& g, ID3D11CommandList* list, BOOL restore)
{
    g.imm->Begin(g.stats.Get());
    g.imm->ExecuteCommandList(list, restore);
    g.imm->End(g.stats.Get());
    D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
    while (g.imm->GetData(g.stats.Get(), &s, sizeof s, 0) == S_FALSE) Sleep(0);
    return s.PSInvocations;
}

// SV_Position the pixel at (x, y) was shaded with (the target holds it).
void read_position(Game& g, float out[4][2])
{
    g.imm->CopyResource(g.staging.Get(), g.rt.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.imm->Map(g.staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    for (int i = 0; i < 4; ++i) {
        const int x = i & 1, y = i >> 1;
        const float* px = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch) + x * 4;
        out[i][0] = px[0]; out[i][1] = px[1];
    }
    g.imm->Unmap(g.staging.Get(), 0);
}

// D3D12's coarse shading on the same card: the SV_Position of the block at the origin (the DX12 fold's basis).
bool d3d12_positions(IDXGIAdapter1* adapter, float out[2][4][2])
{
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof o6)) || o6.VariableShadingRateTier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED) return false;
    ComPtr<ID3D12CommandQueue> q;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList5> l;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&l)))) return false;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    ComPtr<ID3DBlob> sig, err;
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root)))) return false;
    auto vs = compile("vs", "vs_5_0"), ps = compile("ps", "ps_5_0");
    if (!vs || !ps) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) return false;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rt, rb;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)))) return false;
    D3D12_HEAP_PROPERTIES rp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(W) * 16 * 2; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ComPtr<ID3D12DescriptorHeap> heap;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
    const auto h = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt.Get(), nullptr, h);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    const D3D12_SHADING_RATE rates[2] = {D3D12_SHADING_RATE_2X1, D3D12_SHADING_RATE_2X2};
    for (int k = 0; k < 2; ++k) {
        if (k) { al->Reset(); l->Reset(al.Get(), nullptr); }
        l->SetGraphicsRootSignature(root.Get());
        l->SetPipelineState(pso.Get());
        l->OMSetRenderTargets(1, &h, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        const D3D12_RECT sc{0, 0, LONG(W), LONG(H)};
        l->RSSetViewports(1, &vp);
        l->RSSetScissorRects(1, &sc);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l->RSSetShadingRate(rates[k], nullptr);
        l->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER b{};
        b.Transition.pResource = rt.Get(); b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        l->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, W, 2, 1, W * 16};
        src.pResource = rt.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0, 0, 0, W, 2, 1};
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        l->ResourceBarrier(1, &b);
        l->Close();
        ID3D12CommandList* lists[] = {l.Get()};
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence.Get(), UINT64(k) + 1);
        while (fence->GetCompletedValue() < UINT64(k) + 1) Sleep(1);
        float* p = nullptr;
        if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
        for (int i = 0; i < 4; ++i) { const int x = i & 1, y = i >> 1; out[k][i][0] = p[(y * W + x) * 4]; out[k][i][1] = p[(y * W + x) * 4 + 1]; }
        rb->Unmap(0, nullptr);
    }
    return true;
}
} // namespace

int main()
{
    HMODULE nv = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto query = nv ? reinterpret_cast<QueryFn>(GetProcAddress(nv, "nvapi_QueryInterface")) : nullptr;
    auto init = query ? reinterpret_cast<InitFn>(query(0x0150e828)) : nullptr;
    if (!init || init() != NVAPI_OK) { std::printf("SKIP: NVIDIA's driver interface (nvapi64.dll) is not available on this PC\n"); return 77; }
    g_caps = reinterpret_cast<CapsFn>(query(0x52b1499a));
    g_create_srrv = reinterpret_cast<CreateSrrvFn>(query(0x99ca2dff));
    g_set_srrv = reinterpret_cast<SetSrrvFn>(query(0x1b0c2f83));
    g_set_rates = reinterpret_cast<SetRatesFn>(query(0x34f7938f));
    if (!g_caps || !g_create_srrv || !g_set_srrv || !g_set_rates) { std::printf("SKIP: this driver does not offer D3D11 coarse shading calls\n"); return 77; }
    auto adapter = nvidia_adapter();
    if (!adapter) { std::printf("SKIP: no NVIDIA graphics card\n"); return 77; }

    Game g;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.imm))) return 1;

    // (iv) capability
    NV_D3D1x_GRAPHICS_CAPS caps{};
    const NvAPI_Status cs = g_caps(g.dev.Get(), NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
    std::printf("(iv) capability: status %d, variable pixel shading rate %s, SM %u.%u\n", int(cs),
                caps.bVariablePixelRateShadingSupported ? "SUPPORTED" : "not supported", caps.majorSMVersion, caps.minorSMVersion);
    if (cs != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
        std::printf("reason text: This graphics card or driver has no coarse shading for DirectX 11 (NVIDIA variable rate shading), "
                    "so Quality and Performance cannot save the game's own shading here.\n");
        return 77;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.BindFlags = 0; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    // The rate image: one byte per 16x16 tile, every tile index 0 (the table's entry 0).
    D3D11_TEXTURE2D_DESC rd{};
    rd.Width = W / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH; rd.Height = H / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT; rd.MipLevels = 1; rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R8_UINT; rd.SampleDesc.Count = 1; rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> zeros(size_t(rd.Width) * rd.Height, 0);
    const D3D11_SUBRESOURCE_DATA zd{zeros.data(), rd.Width, 0};
    auto vs = compile("vs", "vs_5_0"), ps = compile("ps", "ps_5_0");
    D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    if (!vs || !ps || FAILED(g.dev->CreateTexture2D(&td, nullptr, &g.rt)) || FAILED(g.dev->CreateTexture2D(&sd, nullptr, &g.staging)) ||
        FAILED(g.dev->CreateRenderTargetView(g.rt.Get(), nullptr, &g.rtv)) ||
        FAILED(g.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs)) ||
        FAILED(g.dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.ps)) ||
        FAILED(g.dev->CreateTexture2D(&rd, &zd, &g.rate_surface)) || FAILED(g.dev->CreateQuery(&qd, &g.stats))) return 1;
    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = DXGI_FORMAT_R8_UINT;
    vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipSlice = 0;
    const NvAPI_Status vs_status = g_create_srrv(g.dev.Get(), g.rate_surface.Get(), &vd, &g.srrv);
    expect(vs_status == NVAPI_OK && g.srrv, "the rate image's view is made");
    if (g_failed) return 1;

    const UINT64 full = UINT64(W) * H;
    // (i) immediate context
    const UINT64 off = count_draw(g);
    expect(coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS) == NVAPI_OK, "2x1 set on the immediate context");
    const UINT64 r21 = count_draw(g);
    float pos21[4][2]{};
    read_position(g, pos21);
    expect(coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS) == NVAPI_OK, "2x2 set on the immediate context");
    const UINT64 r22 = count_draw(g);
    float pos22[4][2]{};
    read_position(g, pos22);
    expect(coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_RASTER_PIXEL) == NVAPI_OK, "coarse shading switched off (interface draw)");
    const UINT64 hud = count_draw(g);
    std::printf("(i) immediate: pixel-shader invocations for %llu pixels: off %llu, 2x1 %llu (%.3f), 2x2 %llu (%.3f), interface after switching off %llu\n",
                full, off, r21, double(r21) / double(full), r22, double(r22) / double(full), hud);
    expect(off == full && r21 == full / 2 && r22 == full / 4 && hud == full, "(i) half at 2x1, a quarter at 2x2, full rate for the interface draw");

    // (iii) state resets on the immediate context
    coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS);
    const UINT64 after_viewports = count_draw(g, true); // bind() calls RSSetViewports, as the game's next pass would
    const D3D11_VIEWPORT vp2{0, 0, float(W), float(H), 0, 1};
    g.imm->RSSetViewports(1, &vp2);
    const UINT64 after_viewports2 = count_draw(g, false);
    g.imm->ClearState();
    const UINT64 after_clear = count_draw(g, true);
    coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS);
    g.imm->OMSetRenderTargets(0, nullptr, nullptr); // a different target, then the same one again
    const UINT64 after_rt = count_draw(g, true);
    std::printf("(iii) immediate 2x1 then: game's RSSetViewports %llu, RSSetViewports again %llu, ClearState %llu, render target changed %llu\n",
                after_viewports, after_viewports2, after_clear, after_rt);

    // (ii) deferred contexts
    ComPtr<ID3D11DeviceContext> dc;
    expect(SUCCEEDED(g.dev->CreateDeferredContext(0, &dc)), "a deferred context");
    // a) set on the deferred context, recorded with the draw
    const NvAPI_Status dset = coarse(dc.Get(), g, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS);
    bind(dc.Get(), g);
    dc->Draw(3, 0);
    ComPtr<ID3D11CommandList> la;
    dc->FinishCommandList(FALSE, &la);
    coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_RASTER_PIXEL);
    const UINT64 d_recorded = la ? count_list(g, la.Get(), FALSE) : 0;
    // b) a list recorded without it, run while the immediate context has it set
    bind(dc.Get(), g);
    dc->Draw(3, 0);
    ComPtr<ID3D11CommandList> lb;
    dc->FinishCommandList(FALSE, &lb);
    coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS);
    const UINT64 d_around = lb ? count_list(g, lb.Get(), TRUE) : 0;
    // c) after a FinishCommandList: does the deferred context keep it for its next recording?
    coarse(dc.Get(), g, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS);
    bind(dc.Get(), g);
    dc->Draw(3, 0);
    ComPtr<ID3D11CommandList> lc1, lc2;
    dc->FinishCommandList(FALSE, &lc1);
    bind(dc.Get(), g);
    dc->Draw(3, 0);
    dc->FinishCommandList(FALSE, &lc2);
    coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_RASTER_PIXEL);
    const UINT64 d_next_recording = lc2 ? count_list(g, lc2.Get(), FALSE) : 0;
    // d) the immediate context's own state over ExecuteCommandList (restore TRUE / FALSE)
    coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS);
    if (lb) g.imm->ExecuteCommandList(lb.Get(), TRUE);
    const UINT64 imm_after_restore = count_draw(g, true);
    coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS);
    if (lb) g.imm->ExecuteCommandList(lb.Get(), FALSE);
    const UINT64 imm_after_norestore = count_draw(g, true);
    coarse(g.imm.Get(), g, NV_PIXEL_X1_PER_RASTER_PIXEL);
    std::printf("(ii) deferred: set on the deferred context (status %d) and recorded: %llu; recorded without it, run while the immediate context has 2x1: %llu; "
                "next recording after FinishCommandList: %llu\n", int(dset), d_recorded, d_around, d_next_recording);
    std::printf("(iii) immediate 2x1 kept over ExecuteCommandList: restore=TRUE %llu, restore=FALSE %llu\n", imm_after_restore, imm_after_norestore);

    // (v) where a coarse block is shaded: D3D11 (NVAPI) against D3D12 on the same card
    float d12[2][4][2]{};
    const bool have12 = d3d12_positions(adapter.Get(), d12);
    auto show = [](const char* what, const float p[4][2]) {
        std::printf("    %s: pixel (0,0) %.2f,%.2f  (1,0) %.2f,%.2f  (0,1) %.2f,%.2f  (1,1) %.2f,%.2f\n", what,
                    p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1], p[3][0], p[3][1]);
    };
    std::printf("(v) SV_Position each pixel of the block at the origin was shaded with:\n");
    show("D3D11 NVAPI 2x1", pos21);
    show("D3D11 NVAPI 2x2", pos22);
    if (have12) { show("D3D12 2x1      ", d12[0]); show("D3D12 2x2      ", d12[1]); }
    else std::printf("    D3D12 coarse shading not available for comparison\n");
    bool same = have12;
    for (int k = 0; k < 2 && have12; ++k)
        for (int i = 0; i < 4; ++i)
            for (int a = 0; a < 2; ++a) same = same && (k ? pos22 : pos21)[i][a] == d12[k][i][a];
    expect(!have12 || same, "(v) D3D11 coarse blocks are shaded at the same position as D3D12's (the fold's kept pixel holds on both APIs)");

    std::printf(g_failed ? "FAIL: %d check(s)\n" : "PASS: D3D11 coarse shading through NVAPI\n", g_failed);
    return g_failed ? 1 : 0;
}
