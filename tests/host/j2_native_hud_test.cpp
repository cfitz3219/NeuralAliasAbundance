// Render-small native-resolution HUD (j2/native_hud) and the HUD-mask fallback fix:
//  1. Pure helpers: formats, grey values, scene shape, viewport/scissor scaling.
//  2. HUD-mask classify kernel (Sol HUD-R2): a HUD element that disappears over a FLAT
//     background loses its HUD evidence; flag 4 forgets old evidence.
//  3. End to end on a real D3D12 device with the observer installed (WARP on CI): the game
//     draws a scene and three HUD draws (alpha "over", additive, opaque) into a render-size
//     "proxy"; the HUD draws must land in the native layers (not the proxy), the game's own
//     targets, viewports and scissors must be handed back, and the layer composite over the
//     scene must match the same draws made directly at native size.
#include "test_support.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/observe/producers.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nh = na::j2::native_hud;
using test::ComPtr;

namespace {
void __cdecl sink(const na_event_batch*, void*) {}

bool upload(test::Device& d, ID3D12Resource* tex, const void* pixels, uint32_t bpp, D3D12_RESOURCE_STATES after)
{
    ID3D12Device* dev = d.device.Get();
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto up = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) return false;
    uint8_t* p = nullptr;
    if (FAILED(up->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(p + fp.Offset + y * fp.Footprint.RowPitch, static_cast<const uint8_t*>(pixels) + size_t(y) * desc.Width * bpp, size_t(desc.Width) * bpp);
    up->Unmap(0, nullptr);
    test::List l;
    if (!l.Create(dev)) return false;
    D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    src.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    l.list->ResourceBarrier(1, &b);
    if (FAILED(l.list->Close())) return false;
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    return true;
}
// Reads a texture (in `state`, returned to it) into tightly packed rows.
bool readback(test::Device& d, ID3D12Resource* tex, D3D12_RESOURCE_STATES state, uint32_t bpp, std::vector<uint8_t>& out)
{
    ID3D12Device* dev = d.device.Get();
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    test::List l;
    if (!rb || !l.Create(dev)) return false;
    auto b = test::Transition(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    l.list->ResourceBarrier(1, &b);
    if (FAILED(l.list->Close())) return false;
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    out.resize(size_t(desc.Width) * desc.Height * bpp);
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(out.data() + size_t(y) * desc.Width * bpp, p + fp.Offset + y * fp.Footprint.RowPitch, size_t(desc.Width) * bpp);
    rb->Unmap(0, nullptr);
    return true;
}

void helpers()
{
    CHECK(nh::TypelessOf(DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_R8G8B8A8_TYPELESS);
    CHECK(nh::TypelessOf(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == DXGI_FORMAT_B8G8R8A8_TYPELESS);
    CHECK(nh::TypelessOf(DXGI_FORMAT_R10G10B10A2_UNORM) == DXGI_FORMAT_R10G10B10A2_TYPELESS);
    CHECK(nh::TypelessOf(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_TYPELESS);
    CHECK(nh::TypelessOf(DXGI_FORMAT_R11G11B10_FLOAT) == DXGI_FORMAT_UNKNOWN);
    CHECK(std::fabs(nh::GreyFor(DXGI_FORMAT_R8G8B8A8_UNORM) - 128.f / 255.f) < 1e-6f);
    CHECK(std::fabs(nh::GreyFor(DXGI_FORMAT_R16G16B16A16_FLOAT) - 1.f) < 1e-6f);
    CHECK(nh::GreyFor(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) < 0.25f); // linear value of the stored grey
    CHECK(nh::SceneShaped(true, 0, 3, 1));        // full-screen triangle, opaque
    CHECK(nh::SceneShaped(true, 0, 6, 1));        // quad
    CHECK(!nh::SceneShaped(true, 1, 3, 1));       // blended: HUD
    {
        const float vp[6] = {0, 0, 64, 32, 0, 1}, half[6] = {0, 0, 32, 32, 0, 1};
        const int32_t all[4] = {0, 0, 64, 32}, part[4] = {0, 0, 64, 16};
        CHECK(nh::CoversTarget(vp, all, 64, 32));
        CHECK(!nh::CoversTarget(half, all, 64, 32));
        CHECK(!nh::CoversTarget(vp, part, 64, 32));
        CHECK(!nh::CoversTarget(nullptr, all, 64, 32));
    }
    CHECK(!nh::SceneShaped(true, 0, 600, 1));     // geometry: HUD
    CHECK(!nh::SceneShaped(true, 0, 6, 40));      // instanced quads (glyphs): HUD
    CHECK(!nh::SceneShaped(false, 0, 3, 1));      // unknown blending: never taken for the scene
    const float vp[6] = {10.f, 4.f, 64.f, 32.f, 0.f, 1.f};
    float o[6];
    nh::ScaleViewport(vp, 1.5f, 1.5f, o);
    CHECK(o[0] == 15.f && o[1] == 6.f && o[2] == 96.f && o[3] == 48.f && o[4] == 0.f && o[5] == 1.f);
    const int32_t sc[4] = {8, 4, 41, 21};
    int32_t r[4];
    nh::ScaleScissor(sc, 1.5f, 1.5f, 96, 48, r);
    CHECK(r[0] == 12 && r[1] == 6 && r[2] == 62 && r[3] == 32); // grows outward, never loses a covered pixel
    const int32_t big[4] = {-5, -5, 100, 100};
    nh::ScaleScissor(big, 1.5f, 1.5f, 96, 48, r);
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 96 && r[3] == 48);
}

int classify(test::Device& d)
{
    ID3D12Device* dev = d.device.Get();
    constexpr uint32_t W = 16, H = 16;
    std::vector<uint32_t> cur(W * H), prev(W * H);
    std::vector<float> evidence(W * H, 1.f), motion(W * H * 2, 0.f);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            cur[y * W + x] = 0xFF808080u;                                              // flat background now
            prev[y * W + x] = (x >= 4 && x < 12 && y >= 4 && y < 12) ? 0xFFFFFFFFu : 0xFF808080u; // a HUD square was there
        }
    auto tcur = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto tprev = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto tmv = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto tev = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(tcur && tprev && tmv && tev);
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    REQUIRE(upload(d, tcur.Get(), cur.data(), 4, NPSR) && upload(d, tprev.Get(), prev.data(), 4, NPSR) &&
            upload(d, tmv.Get(), motion.data(), 8, NPSR) && upload(d, tev.Get(), evidence.data(), 4, UAV));
    for (uint32_t flags : {0u, 4u}) {
        test::List l;
        REQUIRE(l.Create(dev));
        REQUIRE(na::j2::RecordHudClassifyForTests(d.queue.Get(), l.list.Get(), tcur.Get(), tprev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                                  tmv.Get(), tev.Get(), W, H, flags));
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        std::vector<uint8_t> raw;
        REQUIRE(readback(d, tev.Get(), UAV, 4, raw));
        float inside = 0.f, outside = 0.f;
        std::memcpy(&inside, raw.data() + (8 * W + 8) * 4, 4);
        std::memcpy(&outside, raw.data() + (1 * W + 1) * 4, 4);
        std::printf("classify flags=%u: vanished HUD %.3f, untouched flat %.3f\n", flags, inside, outside);
        if (flags == 0) {
            CHECK(inside < 0.75f);   // was 1.0 forever before the fix (contrast gate)
            CHECK(outside > 0.99f);  // nothing changed there: evidence kept
        } else {
            CHECK(inside == 0.f);    // forgotten, then counted against
            CHECK(outside == 0.f);
        }
    }
    return 0;
}

constexpr char kDraw[] = R"(
cbuffer C : register(b0) { float4 color; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
float4 ps() : SV_Target { return color; })";
enum class Blend { Opaque, Over, Add };
ComPtr<ID3D12PipelineState> pipeline(ID3D12Device* dev, ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps, Blend b, DXGI_FORMAT dsv)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    rt.BlendEnable = b != Blend::Opaque;
    rt.SrcBlend = b == Blend::Over ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
    rt.DestBlend = b == Blend::Over ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = dsv;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    return pso;
}
struct Hud { Blend blend; float color[4]; int32_t rect[4]; };
// Scene colour, then the HUD (render pixels).
constexpr float kScene[4] = {0.2f, 0.4f, 0.6f, 1.f};
const Hud kHud[3] = {{Blend::Over, {1.f, 0.2f, 0.1f, 0.6f}, {8, 4, 40, 20}},
                     {Blend::Add, {0.1f, 0.1f, 0.3f, 1.f}, {24, 12, 56, 28}},
                     {Blend::Opaque, {0.9f, 0.9f, 0.9f, 1.f}, {0, 26, 10, 32}}};

int end_to_end(test::Device& d)
{
    ID3D12Device* dev = d.device.Get();
    nh::Install(); // before the observer sees any recording (raster snapshots latch at Reset)
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), true));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof cfg;
    REQUIRE(na_observe_start(&cfg, sink, nullptr) == NA_OK);
    constexpr uint32_t RW = 64, RH = 32, OW = 96, OH = 48;
    constexpr DXGI_FORMAT F = DXGI_FORMAT_R8G8B8A8_UNORM, DF = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    constexpr auto RTS = D3D12_RESOURCE_STATE_RENDER_TARGET, NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto proxy = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto other = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = test::CreateTexture(dev, RW, RH, DF, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto ref = test::CreateTexture(dev, OW, OH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto picture = test::CreateTexture(dev, OW, OH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto out = test::CreateTexture(dev, OW, OH, F, UAV, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(proxy && other && depth && ref && picture && out);
    nh::Layer layer;
    CHECK(!nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer)); // learns the proxy

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs))) && SUCCEEDED(dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&dsvs))));
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    ID3D12Resource* targets[4] = {proxy.Get(), other.Get(), ref.Get(), picture.Get()};
    for (int i = 0; i < 4; ++i) {
        rtv[i] = rtvs->GetCPUDescriptorHandleForHeapStart();
        rtv[i].ptr += SIZE_T(i) * inc;
        dev->CreateRenderTargetView(targets[i], nullptr, rtv[i]);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
    dev->CreateDepthStencilView(depth.Get(), nullptr, dsv);

    ComPtr<ID3DBlob> vs, ps, err, blob;
    REQUIRE(SUCCEEDED(D3DCompile(kDraw, sizeof kDraw - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)));
    REQUIRE(SUCCEEDED(D3DCompile(kDraw, sizeof kDraw - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err)));
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants = {0, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC rsd{1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
    ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))));
    ComPtr<ID3D12PipelineState> with_depth[3], plain[3];
    for (int b = 0; b < 3; ++b) {
        with_depth[b] = pipeline(dev, root.Get(), vs.Get(), ps.Get(), Blend(b), DF);
        plain[b] = pipeline(dev, root.Get(), vs.Get(), ps.Get(), Blend(b), DXGI_FORMAT_UNKNOWN);
        REQUIRE(with_depth[b] && plain[b]);
    }

    // The game's frame: scene into the proxy (no depth), HUD with its own depth target, then a
    // later pass on another target that relies on the game's viewport and scissor still being set.
    test::List game;
    REQUIRE(game.Create(dev));
    ID3D12GraphicsCommandList* l = game.list.Get();
    REQUIRE(SUCCEEDED(l->Close()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    const float black[4] = {0, 0, 0, 1};
    l->ClearRenderTargetView(rtv[1], black, 0, nullptr);
    l->SetGraphicsRootSignature(root.Get());
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT full{0, 0, float(RW), float(RH), 0, 1};
    const D3D12_RECT all{0, 0, LONG(RW), LONG(RH)};
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    l->RSSetViewports(1, &full);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, kScene, 0);
    l->DrawInstanced(3, 1, 0, 0); // the composite: stays in the proxy
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
    l->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv);
    for (const Hud& h : kHud) {
        const D3D12_RECT r{h.rect[0], h.rect[1], h.rect[2], h.rect[3]};
        l->RSSetScissorRects(1, &r);
        l->SetPipelineState(with_depth[int(h.blend)].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, h.color, 0);
        l->DrawInstanced(3, 1, 0, 0);
    }
    const float white[4] = {1, 1, 1, 1};
    l->OMSetRenderTargets(1, &rtv[1], FALSE, nullptr); // viewport/scissor: still the game's last ones
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, white, 0);
    l->DrawInstanced(3, 1, 0, 0);
    REQUIRE(SUCCEEDED(l->Close()));
    ID3D12CommandList* lists[] = {l};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);

    const nh::Stats st = nh::GetStats(); // before TakeFrame: this frame's counts are in the totals
    const bool got = nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer);
    std::printf("native HUD: layer=%d %s\n", got ? 1 : 0, nh::StatusLine().c_str());
    CHECK(got);
    CHECK(st.scene_draws == 1);
    CHECK(st.redirected == 3);
    CHECK(st.forwarded == 0);
    CHECK(st.restore_failed == 0);
    REQUIRE(got && layer.n0 && layer.n1);

    std::vector<uint8_t> px;
    REQUIRE(readback(d, proxy.Get(), RTS, 4, px));
    const auto at = [&](const std::vector<uint8_t>& v, uint32_t w, uint32_t x, uint32_t y, int c) { return int(v[(size_t(y) * w + x) * 4 + c]); };
    // DLSS's input: the scene only, where every HUD draw would have been.
    CHECK(std::abs(at(px, RW, 20, 10, 0) - 51) <= 1 && std::abs(at(px, RW, 40, 20, 2) - 153) <= 1 && std::abs(at(px, RW, 5, 28, 0) - 51) <= 1);
    REQUIRE(readback(d, other.Get(), RTS, 4, px));
    CHECK(at(px, RW, 5, 28, 0) == 255); // the game's scissor came back (not our native-size one)
    CHECK(at(px, RW, 12, 28, 0) == 0);
    CHECK(at(px, RW, 5, 20, 0) == 0);

    // Reference: the same frame drawn directly at native size; picture: the scene alone
    // at native size (what DLSS would hand the composite).
    test::List rl;
    REQUIRE(rl.Create(dev));
    ID3D12GraphicsCommandList* r = rl.list.Get();
    r->ClearRenderTargetView(rtv[2], kScene, 0, nullptr);
    r->ClearRenderTargetView(rtv[3], kScene, 0, nullptr);
    r->SetGraphicsRootSignature(root.Get());
    r->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT nfull{0, 0, float(OW), float(OH), 0, 1};
    r->OMSetRenderTargets(1, &rtv[2], FALSE, nullptr);
    r->RSSetViewports(1, &nfull);
    for (const Hud& h : kHud) {
        int32_t s[4];
        nh::ScaleScissor(h.rect, float(OW) / RW, float(OH) / RH, OW, OH, s);
        const D3D12_RECT rr{s[0], s[1], s[2], s[3]};
        r->RSSetScissorRects(1, &rr);
        r->SetPipelineState(plain[int(h.blend)].Get());
        r->SetGraphicsRoot32BitConstants(0, 4, h.color, 0);
        r->DrawInstanced(3, 1, 0, 0);
    }
    D3D12_RESOURCE_BARRIER b[3] = {test::Transition(picture.Get(), RTS, NPSR), test::Transition(layer.n0, RTS, NPSR),
                                   test::Transition(layer.n1, RTS, NPSR)};
    r->ResourceBarrier(3, b);
    REQUIRE(na::j2::RecordHudLayerForTests(d.queue.Get(), r, picture.Get(), F, layer.n0, layer.n1, layer.view, layer.grey, layer.srgb,
                                           out.Get(), OW, OH));
    for (auto& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    r->ResourceBarrier(3, b);
    REQUIRE(SUCCEEDED(r->Close()));
    ID3D12CommandList* rls[] = {r};
    d.queue->ExecuteCommandLists(1, rls);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    std::vector<uint8_t> want, have;
    REQUIRE(readback(d, ref.Get(), RTS, 4, want) && readback(d, out.Get(), UAV, 4, have));
    int worst = 0;
    uint32_t wx = 0, wy = 0;
    for (uint32_t y = 0; y < OH; ++y)
        for (uint32_t x = 0; x < OW; ++x)
            for (int c = 0; c < 3; ++c) {
                const int e = std::abs(at(want, OW, x, y, c) - at(have, OW, x, y, c));
                if (e > worst) { worst = e; wx = x; wy = y; }
            }
    std::printf("native HUD composite vs direct native draw: worst %d/255 at %u,%u (over %d,%d,%d; add+over %d,%d,%d)\n", worst, wx, wy,
                at(have, OW, 20, 10, 0), at(have, OW, 20, 10, 1), at(have, OW, 20, 10, 2),
                at(have, OW, 45, 22, 0), at(have, OW, 45, 22, 1), at(have, OW, 45, 22, 2));
    CHECK(worst <= 4);

    // Second frame, Anno's pause menu: after a HUD draw, an opaque full-screen draw (the blurred
    // scene copy) covers everything; it must go to the proxy and restart the layers, and later
    // HUD draws still go native.
    const float back[4] = {0.f, 0.6f, 0.2f, 1.f};
    nh::SetCoverRule(true); // opt-in ([present] hud_cover=1): the rule under test
    REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    l->SetGraphicsRootSignature(root.Get());
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    l->RSSetViewports(1, &full);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, kScene, 0);
    l->DrawInstanced(3, 1, 0, 0); // composite
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
    l->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv);
    {
        const D3D12_RECT hr{kHud[0].rect[0], kHud[0].rect[1], kHud[0].rect[2], kHud[0].rect[3]};
        l->RSSetScissorRects(1, &hr);
        l->SetPipelineState(with_depth[int(kHud[0].blend)].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, kHud[0].color, 0);
        l->DrawInstanced(3, 1, 0, 0); // HUD: native
    }
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, back, 0);
    l->DrawInstanced(3, 1, 0, 0); // pause background: covers the screen
    l->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv);
    {
        const D3D12_RECT hr{kHud[2].rect[0], kHud[2].rect[1], kHud[2].rect[2], kHud[2].rect[3]};
        l->RSSetScissorRects(1, &hr);
        l->SetPipelineState(with_depth[int(kHud[2].blend)].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, kHud[2].color, 0);
        l->DrawInstanced(3, 1, 0, 0); // menu panel: native
    }
    REQUIRE(SUCCEEDED(l->Close()));
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    const nh::Stats s2 = nh::GetStats();
    nh::Layer layer2;
    const bool got2 = nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer2);
    std::printf("native HUD pause frame: layer=%d %s\n", got2 ? 1 : 0, nh::StatusLine().c_str());
    CHECK(got2);
    CHECK(s2.scene_draws - st.scene_draws == 1);
    CHECK(s2.redirected - st.redirected == 2);
    CHECK(s2.covered - st.covered == 1);
    REQUIRE(got2 && layer2.n0);
    REQUIRE(readback(d, proxy.Get(), RTS, 4, px));
    CHECK(std::abs(at(px, RW, 20, 10, 1) - 153) <= 1 && at(px, RW, 20, 10, 0) == 0); // the background, in DLSS's input
    REQUIRE(readback(d, layer2.n0, RTS, 4, px));
    CHECK(at(px, OW, 30, 15, 0) == 0 && at(px, OW, 30, 15, 1) == 0); // the covered HUD draw is gone from the layer
    CHECK(std::abs(at(px, OW, 5, 44, 0) - 230) <= 1);                 // the later panel is there, at native size
    nh::SetCoverRule(false);

    // Third frame, Anno's menu text: a UI clipping mask (a depth/stencil-only draw, no colour
    // target) on the HUD's depth before the frame's first HUD draw, then text tested against it.
    // The mask must reach the native depths too, or the text fails the stencil test there.
    ComPtr<ID3D12PipelineState> mask_pso, masked_pso;
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = pd.DepthStencilState.StencilWriteMask = 0xFF;
        D3D12_DEPTH_STENCILOP_DESC write{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
        pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = write;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 0;
        pd.DSVFormat = DF;
        pd.SampleDesc.Count = 1;
        REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&mask_pso))));
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        auto& rt = pd.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.BlendEnable = TRUE; // blended: HUD
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pd.DepthStencilState.StencilWriteMask = 0;
        D3D12_DEPTH_STENCILOP_DESC test_eq{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
        pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = test_eq;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = F;
        REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&masked_pso))));
    }
    const float text[4] = {0.8f, 0.4f, 0.f, 1.f};
    REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    l->SetGraphicsRootSignature(root.Get());
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    l->RSSetViewports(1, &full);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, kScene, 0);
    l->DrawInstanced(3, 1, 0, 0); // composite
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
    l->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    l->OMSetStencilRef(1);
    {
        const D3D12_RECT mr{kHud[0].rect[0], kHud[0].rect[1], kHud[0].rect[2], kHud[0].rect[3]};
        l->RSSetScissorRects(1, &mr);
        l->SetPipelineState(mask_pso.Get());
        l->DrawInstanced(3, 1, 0, 0); // the mask: stencil 1 inside (8,4)-(40,20)
    }
    l->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(masked_pso.Get());
    l->SetGraphicsRoot32BitConstants(0, 4, text, 0);
    l->DrawInstanced(3, 1, 0, 0); // the text: only where the mask is
    REQUIRE(SUCCEEDED(l->Close()));
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    const nh::Stats s3 = nh::GetStats();
    nh::Layer layer3;
    const bool got3 = nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer3);
    std::printf("native HUD mask frame: layer=%d %s\n", got3 ? 1 : 0, nh::StatusLine().c_str());
    CHECK(got3);
    CHECK(s3.masks - s2.masks == 1);
    CHECK(s3.redirected - s2.redirected == 1);
    CHECK(s3.restore_failed == s2.restore_failed);
    REQUIRE(got3 && layer3.n0);
    REQUIRE(readback(d, layer3.n0, RTS, 4, px));
    CHECK(std::abs(at(px, OW, 30, 15, 0) - 204) <= 1 && std::abs(at(px, OW, 30, 15, 1) - 102) <= 1); // inside the mask (native 12,6-60,30)
    CHECK(at(px, OW, 80, 40, 0) == 0 && at(px, OW, 5, 3, 0) == 0);                                   // outside: nothing
    REQUIRE(readback(d, proxy.Get(), RTS, 4, px));
    CHECK(std::abs(at(px, RW, 50, 25, 0) - 51) <= 1); // the proxy keeps the picture (the text went native)
    na_observe_stop();
    return 0;
}
} // namespace

int main()
{
    helpers();
    char why[1024] = {};
    const bool compiled = na::j2::CompileHudShadersForTests(why, sizeof why);
    if (!compiled) std::printf("HUD shaders: %s\n", why);
    CHECK(compiled);
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_native_hud_test");
    if (classify(d)) return 1;
    if (end_to_end(d)) return 1;
    // The native layers are observed objects held by statics: like the game host at exit,
    // retirement must not run against the observer's already-destroyed tables.
    na::observe::SetProcessExiting();
    return test::finish("j2_native_hud_test");
}
