// HUD by un-blending (j2/hud_unblend), on a real D3D12 device with the observer installed
// (WARP on CI). Sniper's shape: the game draws its scene and then its UI into a render-size
// canvas and hands the canvas to the back buffer; here the hand-off is a CopyResource (exact).
//  1. The UI draws are left in the canvas (the game's picture is complete) AND replayed into
//     the render-size layers L0/L1 and the native layers N0/N1; the game's targets, viewport
//     and scissor come back for its own draw.
//  2. The original pre-HUD scene is captured, including behind opaque text, and the GPU
//     verifies both final-picture identity and the actual per-pixel blend model.
//  3. The native composite over the scene matches the same UI drawn directly at native size.
//  4. Actual full clears invalidate previous contributions; a viewport is not pixel coverage.
//  5. Mixed world/HUD batches rebind their real world input. Hidden final dependencies are
//     learned from completed GPU pixels and checked again on every enhanced frame.
#include "test_support.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/hud_unblend.hpp"
#include "host_dx12/j2/hud_picture.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"

#include <d3dcompiler.h>
#include <d3d12sdklayers.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nh = na::j2::native_hud;
namespace ub = na::j2::hud_unblend;
using test::ComPtr;

namespace {
void __cdecl sink(const na_event_batch*, void*) {}
void submit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    {na::nr::native::Submission submitted(queue, count, lists);queue->ExecuteCommandLists(count, lists);}
    nh::AfterSubmission(queue,count,lists);
}

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
    submit(d.queue.Get(), 1, lists);
    d.WaitIdle();
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    out.resize(size_t(desc.Width) * desc.Height * bpp);
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(out.data() + size_t(y) * desc.Width * bpp, p + fp.Offset + y * fp.Footprint.RowPitch, size_t(desc.Width) * bpp);
    rb->Unmap(0, nullptr);
    return true;
}

constexpr char kDraw[] = R"(
cbuffer C : register(b0) { float4 color; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
float4 ps() : SV_Target { return color; })";
enum class Blend { Opaque, Over, Add };
ComPtr<ID3D12PipelineState> pipeline(ID3D12Device* dev, ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps, Blend b)
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
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    return pso;
}
struct Hud { Blend blend; float color[4]; int32_t rect[4]; };
constexpr float kScene[4] = {0.2f, 0.4f, 0.6f, 1.f};
const Hud kHud[3] = {{Blend::Over, {1.f, 0.2f, 0.1f, 0.6f}, {8, 4, 40, 20}},
                     {Blend::Add, {0.1f, 0.1f, 0.3f, 1.f}, {24, 12, 56, 28}},
                     {Blend::Opaque, {0.9f, 0.9f, 0.9f, 1.f}, {0, 26, 10, 32}}};
constexpr uint32_t RW = 64, RH = 32, OW = 96, OH = 48;
constexpr DXGI_FORMAT F = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr auto RTS = D3D12_RESOURCE_STATE_RENDER_TARGET, NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
int at(const std::vector<uint8_t>& v, uint32_t w, uint32_t x, uint32_t y, int c) { return int(v[(size_t(y) * w + x) * 4 + c]); }
float half_value(uint16_t value)
{
    const int exponent = (value >> 10) & 31, fraction = value & 1023;
    const float magnitude = exponent ? std::ldexp(1.f + float(fraction) / 1024.f, exponent - 15) : std::ldexp(float(fraction), -24);
    return value & 0x8000 ? -magnitude : magnitude;
}
ComPtr<ID3D12Resource> zero_motion(test::Device& d)
{
    const na::observe::HostWorkScope host;
    auto* dev=d.device.Get();auto texture=test::CreateTexture(dev,RW,RH,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if(!texture)return {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;const auto desc=texture->GetDesc();dev->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto upload=test::CreateBuffer(dev,bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* data=nullptr;
    if(!upload||FAILED(upload->Map(0,nullptr,&data)))return {};std::memset(data,0,size_t(bytes));upload->Unmap(0,nullptr);
    test::List copy;if(!copy.Create(dev))return {};
    D3D12_TEXTURE_COPY_LOCATION to{texture.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}},from{upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};
    from.PlacedFootprint=fp;copy.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);auto b=test::Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,NPSR);copy.list->ResourceBarrier(1,&b);
    if(FAILED(copy.list->Close()))return {};ID3D12CommandList* lists[]={copy.list.Get()};submit(d.queue.Get(),1,lists);d.WaitIdle();return texture;
}

int verdict_logic()
{
    // 30 agreeing frames: identity; 30 mismatching: holding; 30 agreeing: identity again.
    for (int i = 0; i < 30; ++i) ub::ReportCheck(1000, 100, 100, 3, 2);
    CHECK(ub::GetStats().verdict == 1 && ub::GetStats().rate_id == 1000);
    for (int i = 0; i < 30; ++i) ub::ReportCheck(1000, 100, 10, 100, 2);
    CHECK(ub::GetStats().verdict == 2 && ub::GetStats().rate_dec == 1000);
    for (int i = 0; i < 30; ++i) ub::ReportCheck(1000, 10, 10, 0, 0); // too few covered pixels: undecided, keeps holding
    CHECK(ub::GetStats().verdict == 2);
    for (int i = 0; i < 30; ++i) ub::ReportCheck(1000, 100, 95, 0, 0);
    CHECK(ub::GetStats().verdict == 1);
    return 0;
}

int end_to_end(test::Device& d)
{
    ID3D12Device* dev = d.device.Get();
    nh::Install(); // the hooks; hud_unblend takes them over
    ub::SetConfigured(true);
    REQUIRE(ub::Enabled());
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), true));
    REQUIRE(na::observe::InstallListResetTracking(dev, na::nr::native::Reset));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof cfg;
    REQUIRE(na_observe_start(&cfg, sink, nullptr) == NA_OK);
    auto proxy = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto canvas = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto other = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto ref = test::CreateTexture(dev, OW, OH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto picture = test::CreateTexture(dev, OW, OH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto out = test::CreateTexture(dev, OW, OH, F, UAV, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto scene = test::CreateTexture(dev, RW, RH, F, UAV, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto counts = test::CreateBuffer(dev, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    REQUIRE(proxy && canvas && other && ref && picture && out && scene && counts);
    nh::Layer layer;
    CHECK(!nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer, d.queue.Get())); // learns the picture's size and format

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> rtvs;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs))));
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[5];
    ID3D12Resource* targets[5] = {proxy.Get(), canvas.Get(), other.Get(), ref.Get(), picture.Get()};
    for (int i = 0; i < 5; ++i) {
        rtv[i] = rtvs->GetCPUDescriptorHandleForHeapStart();
        rtv[i].ptr += SIZE_T(i) * inc;
        dev->CreateRenderTargetView(targets[i], nullptr, rtv[i]);
    }
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
    ComPtr<ID3D12PipelineState> pso[3];
    for (int b = 0; b < 3; ++b) { pso[b] = pipeline(dev, root.Get(), vs.Get(), ps.Get(), Blend(b)); REQUIRE(pso[b]); }

    // The game's frame: scene into the canvas (full-screen, opaque), the UI into it, the canvas
    // copied to the back buffer, then a pass on another target that relies on the game's
    // viewport and scissor still being set.
    test::List game;
    REQUIRE(game.Create(dev));
    ID3D12GraphicsCommandList* l = game.list.Get();
    REQUIRE(SUCCEEDED(l->Close()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    const D3D12_VIEWPORT full{0, 0, float(RW), float(RH), 0, 1};
    const D3D12_RECT all{0, 0, LONG(RW), LONG(RH)};
    const float black[4] = {0, 0, 0, 1};
    auto frame = [&](bool cover_after_ui, bool canvas_overlay = false) {
        l->ClearRenderTargetView(rtv[2], black, 0, nullptr);
        l->SetGraphicsRootSignature(root.Get());
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        l->OMSetRenderTargets(1, &rtv[1], FALSE, nullptr);
        l->RSSetViewports(1, &full);
        l->RSSetScissorRects(1, &all);
        l->ClearRenderTargetView(rtv[1],kScene,0,nullptr); // known complete scene, not guessed vertex coverage
        for (int i = 0; i < 3; ++i) {
            const Hud& h = kHud[i];
            const D3D12_RECT r{h.rect[0], h.rect[1], h.rect[2], h.rect[3]};
            l->RSSetScissorRects(1, &r);
            l->SetPipelineState(pso[int(h.blend)].Get());
            l->SetGraphicsRoot32BitConstants(0, 4, h.color, 0);
            l->DrawInstanced(3, 1, 0, 0);
            if (cover_after_ui && i == 0) { // a pause backdrop: covers everything drawn so far
                l->ClearRenderTargetView(rtv[1],kScene,0,nullptr);
            }
        }
        if (canvas_overlay) { // a blended full-screen triangle (how a UI canvas is laid on): UI, replayed
            const float veil[4] = {0.1f, 0.1f, 0.1f, 0.3f};
            l->RSSetScissorRects(1, &all);
            l->SetPipelineState(pso[int(Blend::Over)].Get());
            l->SetGraphicsRoot32BitConstants(0, 4, veil, 0);
            l->DrawInstanced(3, 1, 0, 0);
        }
        D3D12_RESOURCE_BARRIER b[2] = {test::Transition(canvas.Get(), RTS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                       test::Transition(proxy.Get(), RTS, D3D12_RESOURCE_STATE_COPY_DEST)};
        l->ResourceBarrier(2, b);
        l->CopyResource(proxy.Get(), canvas.Get()); // the hand-off: the picture is now complete
        for (auto& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
        l->ResourceBarrier(2, b);
        const float white[4] = {1, 1, 1, 1};
        l->OMSetRenderTargets(1, &rtv[2], FALSE, nullptr); // viewport/scissor: still the game's last ones
        l->SetPipelineState(pso[0].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, white, 0);
        l->DrawInstanced(3, 1, 0, 0);
    };
    frame(false);
    REQUIRE(SUCCEEDED(l->Close()));
    ID3D12CommandList* lists[] = {l};
    submit(d.queue.Get(), 1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);

    ub::Stats st = ub::GetStats();
    std::printf("%s\n", ub::StatusLine().c_str());
    CHECK(st.replayed == 4);
    CHECK(st.blended == 2);
    CHECK(st.scene == 0);
    CHECK(st.forwarded == 0 && st.other_target == 0); // unrelated target has independent layers
    CHECK(st.world == 0);
    CHECK(st.restore_failed == 0);
    const bool got = nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer, d.queue.Get());
    std::printf("after selection: %s\n", ub::StatusLine().c_str());
    CHECK(got);
    REQUIRE(got && layer.unblend && layer.compose && layer.n0 && layer.n1 && layer.l0 && layer.l1);
    CHECK(layer.view == F);
    CHECK(!layer.srgb);

    std::vector<uint8_t> px, first_l0;
    REQUIRE(readback(d, layer.l0, RTS, 4, first_l0));
    REQUIRE(readback(d, proxy.Get(), RTS, 4, px));
    // The game's own picture is complete: the UI is in it.
    CHECK(std::abs(at(px, RW, 20, 10, 0) - int(std::lround((0.6f * 1.f + 0.4f * 0.2f) * 255))) <= 1);
    CHECK(std::abs(at(px, RW, 5, 28, 0) - 230) <= 1); // 0.9 * 255 = 229.5: the tie rounds either way per GPU
    REQUIRE(readback(d, other.Get(), RTS, 4, px));
    CHECK(at(px, RW, 5, 28, 0) == 255); // the game's scissor came back (not our native-size one)
    CHECK(at(px, RW, 12, 28, 0) == 0);
    CHECK(at(px, RW, 5, 20, 0) == 0);

    // The un-blend pass: the scene under the UI from the final picture alone.
    {
        test::List u;
        REQUIRE(u.Create(dev));
        ID3D12GraphicsCommandList* r = u.list.Get();
        D3D12_RESOURCE_BARRIER b[3] = {test::Transition(proxy.Get(), RTS, NPSR), test::Transition(layer.l0, RTS, NPSR),
                                       test::Transition(layer.l1, RTS, NPSR)};
        r->ResourceBarrier(3, b);
        REQUIRE(na::j2::RecordHudUnblendForTests(d.queue.Get(), r, proxy.Get(), F, layer.l0, layer.l1, layer.view, layer.grey, layer.srgb,
                                                 scene.Get(), RW, RH, counts.Get()));
        for (auto& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
        r->ResourceBarrier(3, b);
        REQUIRE(SUCCEEDED(r->Close()));
        ID3D12CommandList* ul[] = {r};
        d.queue->ExecuteCommandLists(1, ul);
        d.WaitIdle();
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
        REQUIRE(readback(d, scene.Get(), UAV, 4, px));
        int worst = 0;
        uint32_t wx = 0, wy = 0;
        for (uint32_t y = 0; y < RH; ++y)
            for (uint32_t x = 0; x < RW; ++x) {
                const bool opaque = x < 10 && y >= 26; // the opaque UI: keeps the picture (covered natively)
                for (int c = 0; c < 3; ++c) {
                    const int want = opaque ? 230 : int(std::lround(kScene[c] * 255));
                    const int e = std::abs(at(px, RW, x, y, c) - want);
                    if (e > worst) { worst = e; wx = x; wy = y; }
                }
            }
        std::printf("un-blended scene vs the scene: worst %d/255 at %u,%u (under over %d,%d,%d; under add+over %d,%d,%d)\n", worst, wx, wy,
                    at(px, RW, 20, 10, 0), at(px, RW, 20, 10, 1), at(px, RW, 20, 10, 2), at(px, RW, 30, 15, 0), at(px, RW, 30, 15, 1), at(px, RW, 30, 15, 2));
        // Bound: P, L0 and L1 are each stored to 8 bits (+-0.5), (P - L0) / m with m = 0.4 scales
        // that to +-2.5, and m itself carries +-1/128 of grey (~2 % of a value near 150, ~3):
        // about 5.5 in the add+over corner. NVIDIA gives 2, WARP 4.
        CHECK(worst <= 6);
        uint32_t v[5] = {};
        void* ptr = nullptr;
        REQUIRE(SUCCEEDED(counts->Map(0, nullptr, &ptr)) && ptr);
        std::memcpy(v, ptr, sizeof v);
        counts->Unmap(0, nullptr);
        std::printf("self-check: ui=%u opaque=%u match id=%u dec=%u enc=%u\n", v[0], v[1], v[2], v[3], v[4]);
        CHECK(v[1] == 60);          // the opaque UI rectangle, 10 x 6
        CHECK(v[2] == v[1]);        // the picture equals L0 there: the hand-off was an identity
        CHECK(v[3] < v[1] && v[4] < v[1]);
        CHECK(v[0] >= 60 + 32 * 16); // plus the "over" rectangle at least
    }
    // Reference: the same UI drawn directly at native size over the scene; the composite of the
    // native layers over the scene alone must match it.
    {
        test::List rl;
        REQUIRE(rl.Create(dev));
        ID3D12GraphicsCommandList* r = rl.list.Get();
        r->ClearRenderTargetView(rtv[3], kScene, 0, nullptr);
        r->ClearRenderTargetView(rtv[4], kScene, 0, nullptr);
        r->SetGraphicsRootSignature(root.Get());
        r->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT nfull{0, 0, float(OW), float(OH), 0, 1};
        r->OMSetRenderTargets(1, &rtv[3], FALSE, nullptr);
        r->RSSetViewports(1, &nfull);
        for (const Hud& h : kHud) {
            int32_t s[4];
            nh::ScaleScissor(h.rect, float(OW) / RW, float(OH) / RH, OW, OH, s);
            const D3D12_RECT rr{s[0], s[1], s[2], s[3]};
            r->RSSetScissorRects(1, &rr);
            r->SetPipelineState(pso[int(h.blend)].Get());
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
        submit(d.queue.Get(), 1, rls);
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
        std::printf("native composite vs direct native draw: worst %d/255 at %u,%u\n", worst, wx, wy);
        CHECK(worst <= 4);
    }
    // The production current-picture path supplies the real PRE-HUD scene to
    // DLSS, including behind opaque text. It does not divide a zero coefficient
    // and accidentally leave opaque HUD pixels in DLSS's input.
    {
        const na::observe::HostWorkScope owned;
        REQUIRE(layer.picture && layer.clean_allowed);
        auto motion = test::CreateTexture(dev, RW, RH, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        REQUIRE(motion);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes = 0;
        const auto md = motion->GetDesc(); dev->GetCopyableFootprints(&md, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto upload = test::CreateBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        REQUIRE(upload); void* zero = nullptr; REQUIRE(SUCCEEDED(upload->Map(0, nullptr, &zero)));
        std::memset(zero, 0, size_t(bytes)); upload->Unmap(0, nullptr);
        test::List calc; REQUIRE(calc.Create(dev)); auto* c = calc.list.Get();
        D3D12_TEXTURE_COPY_LOCATION dest{motion.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}},
                                    src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        src.PlacedFootprint = footprint; c->CopyTextureRegion(&dest, 0, 0, 0, &src, nullptr);
        auto transition = test::Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NPSR); c->ResourceBarrier(1, &transition);
        transition = test::Transition(proxy.Get(), RTS, NPSR); c->ResourceBarrier(1, &transition);
        REQUIRE(layer.picture->Prepare(c, proxy.Get(), F, motion.Get(), layer.n0, layer.n1, layer.view, layer.grey,
                                        layer.srgb, layer.clean_allowed, F));
        transition = test::Transition(picture.Get(), RTS, D3D12_RESOURCE_STATE_COPY_SOURCE); c->ResourceBarrier(1, &transition);
        transition = test::Transition(out.Get(), UAV, D3D12_RESOURCE_STATE_COPY_DEST); c->ResourceBarrier(1, &transition);
        c->CopyResource(out.Get(), picture.Get());
        transition = test::Transition(picture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, RTS); c->ResourceBarrier(1, &transition);
        transition = test::Transition(out.Get(), D3D12_RESOURCE_STATE_COPY_DEST, UAV); c->ResourceBarrier(1, &transition);
        REQUIRE(layer.picture->Composite(c, out.Get()));
        transition = test::Transition(proxy.Get(), NPSR, RTS); c->ResourceBarrier(1, &transition);
        REQUIRE(SUCCEEDED(c->Close())); ID3D12CommandList* calculations[] = {c}; submit(d.queue.Get(), 1, calculations); d.WaitIdle();
        bool identity = false, clean = false; nh::Picture::ProofReceipt receipt;
        REQUIRE(layer.picture->ProofAfterCompletion(identity, clean, &receipt));
        CHECK(identity && clean && receipt.matched == RW * RH);
        std::vector<uint8_t> clean_input; REQUIRE(readback(d, layer.picture->Input(), NPSR, 8, clean_input));
        float maximum = 0;
        for (uint32_t y = 0; y < RH; ++y) for (uint32_t x = 0; x < RW; ++x) for (int channel = 0; channel < 3; ++channel) {
            uint16_t encoded = 0; std::memcpy(&encoded, clean_input.data() + (size_t(y) * RW + x) * 8 + channel * 2, 2);
            const float value = half_value(encoded); CHECK(std::isfinite(value));
            maximum = std::max(maximum, std::abs(value - kScene[channel]));
        }
        std::printf("actual DLSS input, including opaque HUD pixels: scene error %.6f\n", maximum);
        CHECK(maximum < .002f);
        std::vector<uint8_t> reference, actual; REQUIRE(readback(d, ref.Get(), RTS, 4, reference) && readback(d, out.Get(), UAV, 4, actual));
        int error = 0; for (size_t i = 0; i < reference.size(); ++i) if (i % 4 != 3) error = std::max(error, std::abs(int(reference[i]) - int(actual[i])));
        std::printf("current-picture composition vs native reference: %d/255\n", error); CHECK(error <= 4);
        test::List repeat;REQUIRE(repeat.Create(dev));
        auto repeated=layer.picture->ForkSnapshot(repeat.list.Get());REQUIRE(repeated&&!repeated->Prepared());
        transition=test::Transition(proxy.Get(),RTS,NPSR);repeat.list->ResourceBarrier(1,&transition);
        REQUIRE(repeated->Prepare(repeat.list.Get(),proxy.Get(),F,motion.Get(),layer.n0,layer.n1,layer.view,layer.grey,layer.srgb,layer.clean_allowed,F));
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);repeat.list->ResourceBarrier(1,&transition);
        REQUIRE(SUCCEEDED(repeat.list->Close()));ID3D12CommandList* repeated_work[]={repeat.list.Get()};submit(d.queue.Get(),1,repeated_work);d.WaitIdle();
        std::vector<uint8_t> repeated_input;REQUIRE(readback(d,repeated->Input(),NPSR,8,repeated_input));
        CHECK(repeated_input==clean_input&&repeated->Input()!=layer.picture->Input());
    }
    // Second frame: an opaque full-screen draw after the first UI draw restarts the layers;
    // the two UI draws after it are what the frame ends with.
    REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    frame(true);
    REQUIRE(SUCCEEDED(l->Close()));
    submit(d.queue.Get(), 1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    st = ub::GetStats();
    CHECK(st.replayed == 8);
    CHECK(st.cleared_after == 1);
    CHECK(st.scene == 0);
    nh::Layer second;
    CHECK(nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &second, d.queue.Get()));
    st = ub::GetStats();
    CHECK(st.last_draws == 2 && st.last_covered == 0);
    CHECK(st.hud_frames == 2 && st.composed == 2);
    // The verdict holds the layers back (compose=false) while the self-check reports a mismatch.
    for (int i = 0; i < 30; ++i) ub::ReportCheck(1000, 100, 0, 100, 0);
    REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    frame(false);
    REQUIRE(SUCCEEDED(l->Close()));
    submit(d.queue.Get(), 1, lists);
    d.WaitIdle();
    nh::Layer third;
    CHECK(nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &third, d.queue.Get()));
    CHECK(third.unblend && third.compose && third.picture); // current GPU proof supersedes a stale window verdict
    CHECK(ub::GetStats().held == 0);
    std::printf("%s\n", ub::StatusLine().c_str());
    // A blended full-screen draw after the UI (a canvas laid on with one triangle) is replayed
    // with the UI, not left out as image processing (SE5 2026-11-05: menus were missing).
    {
        const uint64_t replayed0 = ub::GetStats().replayed, cover0 = ub::GetStats().cover_blend;
        REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
        frame(false, true);
        REQUIRE(SUCCEEDED(l->Close()));
        submit(d.queue.Get(), 1, lists);
        d.WaitIdle();
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
        CHECK(ub::GetStats().replayed == replayed0 + 5);
        CHECK(ub::GetStats().cover_blend == cover0 + 1);
        nh::Layer fourth;
        CHECK(nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &fourth, d.queue.Get()));
        CHECK(fourth.unblend && fourth.l0 && fourth.l1);
    }
    // A later opaque pass and veil on an unrelated target must not steal the
    // layers of the canvas actually copied to proxy. Verify the recovered pixels.
    {
        const ub::Stats before = ub::GetStats();
        REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
        frame(false);
        const float veil[4] = {0.1f, 0.1f, 0.1f, 0.3f};
        l->OMSetRenderTargets(1, &rtv[2], FALSE, nullptr);
        l->RSSetViewports(1, &full);
        l->RSSetScissorRects(1, &all);
        l->SetPipelineState(pso[0].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, kScene, 0);
        l->DrawInstanced(3, 1, 0, 0); // post-processing: the picture is in `other` now
        l->SetPipelineState(pso[int(Blend::Over)].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, veil, 0);
        l->DrawInstanced(3, 1, 0, 0); // the canvas laid on: UI, on the new target
        REQUIRE(SUCCEEDED(l->Close()));
        submit(d.queue.Get(), 1, lists);
        d.WaitIdle();
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
        const ub::Stats after = ub::GetStats();
        CHECK(after.retargeted == before.retargeted);
        CHECK(after.other_target == before.other_target);
        CHECK(after.replayed == before.replayed + 6);
        nh::Layer fifth;
        CHECK(nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &fifth, d.queue.Get()));
        CHECK(ub::GetStats().last_draws == 3);
        CHECK(fifth.unblend && fifth.l0 && fifth.l1);
        std::vector<uint8_t> actual, low_pixels, high_pixels;
        REQUIRE(readback(d, proxy.Get(), RTS, 4, actual) && readback(d, fifth.l0, RTS, 4, low_pixels) && readback(d, fifth.l1, RTS, 4, high_pixels));
        const float m = float(at(high_pixels, RW, 0, 0, 0) - at(low_pixels, RW, 0, 0, 0)) / 128.f;
        const float recovered = float(at(actual, RW, 0, 0, 0) - at(low_pixels, RW, 0, 0, 0)) / (255.f * m);
        std::printf("unrelated target isolation: recovered %.6f expected %.6f\n", recovered, kScene[0]);
        CHECK(std::abs(recovered - kScene[0]) < .02f);
    }
    // Source writes AFTER the handoff cannot change that handoff's layers.
    // Two subsequent whole-picture copies preserve it, and HUD drawn directly
    // on the last destination composes with the transferred contribution.
    {
        REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
        frame(false);
        const float erased_source[4] = {0.9f, 0.1f, 0.5f, 1.f};
        l->ClearRenderTargetView(rtv[1], erased_source, 0, nullptr); // canvas is NOT bound
        D3D12_RESOURCE_BARRIER chain[2] = {test::Transition(proxy.Get(), RTS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                          test::Transition(other.Get(), RTS, D3D12_RESOURCE_STATE_COPY_DEST)};
        l->ResourceBarrier(2, chain); l->CopyResource(other.Get(), proxy.Get());
        for (auto& v : chain) std::swap(v.Transition.StateBefore, v.Transition.StateAfter);
        l->ResourceBarrier(2, chain);
        chain[0] = test::Transition(other.Get(), RTS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        chain[1] = test::Transition(proxy.Get(), RTS, D3D12_RESOURCE_STATE_COPY_DEST);
        l->ResourceBarrier(2, chain); l->CopyResource(proxy.Get(), other.Get());
        for (auto& v : chain) std::swap(v.Transition.StateBefore, v.Transition.StateAfter);
        l->ResourceBarrier(2, chain);
        const float veil[4] = {0.1f, 0.1f, 0.1f, 0.3f};
        l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
        l->RSSetViewports(1, &full); l->RSSetScissorRects(1, &all);
        l->SetPipelineState(pso[int(Blend::Over)].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, veil, 0); l->DrawInstanced(3, 1, 0, 0);
        REQUIRE(SUCCEEDED(l->Close())); submit(d.queue.Get(), 1, lists); d.WaitIdle();
        nh::Layer final_layer;
        REQUIRE(nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &final_layer, d.queue.Get()));
        CHECK(ub::GetStats().last_draws == 4);
        std::vector<uint8_t> actual, low_pixels, high_pixels;
        REQUIRE(readback(d, proxy.Get(), RTS, 4, actual) && readback(d, final_layer.l0, RTS, 4, low_pixels) &&
                readback(d, final_layer.l1, RTS, 4, high_pixels));
        float worst = 0;
        for (int c = 0; c < 3; ++c) {
            const float m = float(at(high_pixels, RW, 0, 0, c) - at(low_pixels, RW, 0, 0, c)) / 128.f;
            REQUIRE(m > .1f);
            const float recovered = float(at(actual, RW, 0, 0, c) - at(low_pixels, RW, 0, 0, c)) / (255.f * m);
            worst = std::max(worst, std::abs(recovered - kScene[c]));
        }
        std::printf("frozen source, copy chain and late HUD: scene error %.6f\n", worst);
        CHECK(worst < .02f);
        REQUIRE(final_layer.picture && final_layer.clean_allowed);
        std::vector<uint8_t> original_scene;
        REQUIRE(readback(d, final_layer.picture->Scene(), NPSR, 4, original_scene));
        int scene_error = 0;
        for (size_t p = 0; p < original_scene.size(); ++p) if (p % 4 != 3)
            scene_error = std::max(scene_error, std::abs(int(original_scene[p]) - int(std::lround(kScene[p % 4] * 255))));
        CHECK(scene_error <= 1); // late HUD must inherit the PRE-HUD scene, not snapshot earlier text
        std::vector<uint8_t> preserved;
        REQUIRE(readback(d, layer.l0, RTS, 4, preserved));
        CHECK(preserved == first_l0); // owner retained beyond the three-Present ring
    }
    // Off: nothing is replayed.
    const auto before_off = ub::GetStats().replayed;
    ub::SetEnabled(false);
    REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    frame(false);
    REQUIRE(SUCCEEDED(l->Close()));
    submit(d.queue.Get(), 1, lists);
    d.WaitIdle();
    CHECK(ub::GetStats().replayed == before_off);
    ub::SetEnabled(true);
    {
        // UNORM clipping makes two-point coefficients non-affine. Reject only
        // the affected pixels; preserve the correctly separated rest of frame.
        REQUIRE(SUCCEEDED(game.alloc->Reset())&&SUCCEEDED(l->Reset(game.alloc.Get(),nullptr)));
        frame(false);const float add[4]={.7f,.7f,.7f,1};const D3D12_RECT clip{0,0,4,4};
        l->OMSetRenderTargets(1,&rtv[0],FALSE,nullptr);l->RSSetScissorRects(1,&clip);l->SetPipelineState(pso[int(Blend::Add)].Get());
        l->SetGraphicsRoot32BitConstants(0,4,add,0);l->DrawInstanced(3,1,0,0);
        REQUIRE(SUCCEEDED(l->Close()));submit(d.queue.Get(),1,lists);d.WaitIdle();nh::Layer clipped;
        REQUIRE(nh::TakeFrame(proxy.Get(),RW,RH,OW,OH,&clipped,d.queue.Get())&&clipped.picture);
        auto motion=zero_motion(d);REQUIRE(motion);test::List calc;REQUIRE(calc.Create(dev));const na::observe::HostWorkScope host;
        auto b=test::Transition(proxy.Get(),RTS,NPSR);calc.list->ResourceBarrier(1,&b);
        REQUIRE(clipped.picture->Prepare(calc.list.Get(),proxy.Get(),F,motion.Get(),clipped.n0,clipped.n1,clipped.view,clipped.grey,clipped.srgb,clipped.clean_allowed,F));
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);calc.list->ResourceBarrier(1,&b);
        REQUIRE(SUCCEEDED(calc.list->Close()));ID3D12CommandList* check[]={calc.list.Get()};submit(d.queue.Get(),1,check);d.WaitIdle();
        bool identity=false,clean=false;nh::Picture::ProofReceipt proof;REQUIRE(clipped.picture->ProofAfterCompletion(identity,clean,&proof));
        CHECK(!identity&&!clean&&proof.matched<RW*RH&&proof.matched>RW*RH/2&&proof.picture_matched==RW*RH);
        std::vector<uint8_t> mask;REQUIRE(readback(d,clipped.picture->Mask(),NPSR,2,mask));uint16_t protected_value=0,scene_value=0;
        std::memcpy(&protected_value,mask.data(),2);std::memcpy(&scene_value,mask.data()+((RH-1)*RW+RW-1)*2,2);
        CHECK(half_value(protected_value)>.9f&&half_value(scene_value)==0);
    }
    // Record a future frame into the same game resources before submitting
    // the current one. Actual queue order, not the last CPU recording, owns
    // the presented scene and HUD. Repeat the closed lists in reverse order.
    REQUIRE(SUCCEEDED(game.alloc->Reset())&&SUCCEEDED(l->Reset(game.alloc.Get(),nullptr)));
    frame(false);REQUIRE(SUCCEEDED(l->Close()));
    test::List future;REQUIRE(future.Create(dev));
    REQUIRE(SUCCEEDED(future.list->Close())&&SUCCEEDED(future.list->Reset(future.alloc.Get(),nullptr)));
    auto* next=future.list.Get();const float next_scene[4]={.7f,.3f,.1f,1.f};
    next->ClearRenderTargetView(rtv[1],next_scene,0,nullptr);next->SetGraphicsRootSignature(root.Get());
    next->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);next->OMSetRenderTargets(1,&rtv[1],FALSE,nullptr);
    next->RSSetViewports(1,&full);next->RSSetScissorRects(1,&all);next->SetPipelineState(pso[int(Blend::Over)].Get());
    next->SetGraphicsRoot32BitConstants(0,4,kHud[0].color,0);next->DrawInstanced(3,1,0,0);
    constexpr char small_vertex[]=R"(
float4 vs(uint id:SV_VertexID):SV_Position{float2 v=float2((id<<1)&2,id&2)*.125+.25;return float4(v*float2(2,-2)+float2(-1,1),.5,1);})";
    ComPtr<ID3DBlob> small_code;REQUIRE(SUCCEEDED(D3DCompile(small_vertex,sizeof small_vertex-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&small_code,&err)));
    auto quad_pso=pipeline(dev,root.Get(),small_code.Get(),ps.Get(),Blend::Opaque);REQUIRE(quad_pso);
    next->SetPipelineState(quad_pso.Get());next->SetGraphicsRoot32BitConstants(0,4,kHud[2].color,0);next->DrawInstanced(3,1,0,0);
    D3D12_RESOURCE_BARRIER handoff[]={test::Transition(canvas.Get(),RTS,D3D12_RESOURCE_STATE_COPY_SOURCE),test::Transition(proxy.Get(),RTS,D3D12_RESOURCE_STATE_COPY_DEST)};
    next->ResourceBarrier(2,handoff);next->CopyResource(proxy.Get(),canvas.Get());
    for(auto& t:handoff)std::swap(t.Transition.StateBefore,t.Transition.StateAfter);next->ResourceBarrier(2,handoff);
    REQUIRE(SUCCEEDED(next->Close()));
    const auto validate=[&](const float* expected){
        nh::Layer chosen;if(!nh::TakeFrame(proxy.Get(),RW,RH,OW,OH,&chosen,d.queue.Get())||!chosen.picture)return false;
        std::vector<uint8_t> scene_pixels;if(!readback(d,chosen.picture->Scene(),NPSR,4,scene_pixels))return false;
        for(size_t i=0;i<scene_pixels.size();++i)if(i%4!=3&&std::abs(int(scene_pixels[i])-int(std::lround(expected[i%4]*255)))>1)return false;
        return true;
    };
    submit(d.queue.Get(),1,lists);d.WaitIdle();CHECK(validate(kScene));
    ID3D12CommandList* later[]={next};submit(d.queue.Get(),1,later);d.WaitIdle();CHECK(validate(next_scene));
    CHECK(ub::GetStats().last_draws==2); // small opaque geometry did not erase the earlier veil
    ID3D12CommandList* reversed[]={next,l};submit(d.queue.Get(),2,reversed);d.WaitIdle();CHECK(validate(kScene));
    std::printf("render-ahead and repeated reverse queue order: exact submitted scene retained\n");
    return 0;
}

int mixed_canvas(test::Device& d,bool srgb_source=false)
{
    // One batch copies the world and writes opaque HUD at the same time.
    // Output alpha is always1 with SRC_ALPHA/INV_SRC_ALPHA enabled: the old
    // two-background replays both contained the world (L0 == L1 everywhere).
    auto* dev=d.device.Get();
    auto world=test::CreateTexture(dev,RW,RH,srgb_source?DXGI_FORMAT_R8G8B8A8_TYPELESS:F,RTS,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto canvas=test::CreateTexture(dev,RW,RH,F,RTS,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto output=test::CreateTexture(dev,RW,RH,F,RTS,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(world&&canvas&&output);
    ComPtr<ID3D12DescriptorHeap> rtvs,views;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))));
    auto w=rtvs->GetCPUDescriptorHandleForHeapStart(),c=w,o=w;
    c.ptr+=dev->GetDescriptorHandleIncrementSize(hd.Type);o.ptr+=2*dev->GetDescriptorHandleIncrementSize(hd.Type);
    D3D12_RENDER_TARGET_VIEW_DESC world_rtv{};world_rtv.Format=F;world_rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(world.Get(),&world_rtv,w);dev->CreateRenderTargetView(canvas.Get(),nullptr,c);dev->CreateRenderTargetView(output.Get(),nullptr,o);
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views))));
    auto handle=views->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC world_srv{};world_srv.Format=srgb_source?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:F;world_srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    world_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;world_srv.Texture2D.MipLevels=1;
    dev->CreateShaderResourceView(world.Get(),&world_srv,handle);
    handle.ptr+=dev->GetDescriptorHandleIncrementSize(hd.Type);dev->CreateShaderResourceView(canvas.Get(),nullptr,handle);
    constexpr char shader[]=R"(
Texture2D<float4> image:register(t0);SamplerState s:register(s0);
struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};
V vs(uint id:SV_VertexID){V v;v.uv=float2(((id%3)<<1)&2,(id%3)&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),.5,1);return v;}
float4 ps(V v):SV_Target{float3 a=image.SampleLevel(s,v.uv,0).rgb;
 if(v.uv.x>.25&&v.uv.x<.5&&v.uv.y>.25&&v.uv.y<.75)a=float3(.9,.1,.2);return float4(a,1);}
float4 copy(V v):SV_Target{return image.SampleLevel(s,v.uv,0);})";
    ComPtr<ID3DBlob> vs,ps,copy,code,error;
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof shader-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof shader-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error)));
    REQUIRE(SUCCEEDED(D3DCompile(shader,sizeof shader-1,nullptr,nullptr,nullptr,"copy","ps_5_0",0,0,&copy,&error)));
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_ROOT_PARAMETER param{};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;param.DescriptorTable={1,&range};
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC desc{1,&param,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&code,&error)));
    ComPtr<ID3D12RootSignature> root;REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&root))));
    auto ui=pipeline(dev,root.Get(),vs.Get(),ps.Get(),Blend::Over),tail=pipeline(dev,root.Get(),vs.Get(),copy.Get(),Blend::Opaque);
    REQUIRE(ui&&tail);
    nh::Layer unused;nh::TakeFrame(output.Get(),RW,RH,OW,OH,&unused,d.queue.Get());
    test::List game;REQUIRE(game.Create(dev));
    REQUIRE(SUCCEEDED(game.list->Close())&&SUCCEEDED(game.list->Reset(game.alloc.Get(),nullptr)));
    auto* l=game.list.Get();const float black[4]={0,0,0,0};
    l->ClearRenderTargetView(w,kScene,0,nullptr);l->ClearRenderTargetView(c,black,0,nullptr);
    auto b=test::Transition(world.Get(),RTS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);l->ResourceBarrier(1,&b);
    l->SetGraphicsRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]={views.Get()};l->SetDescriptorHeaps(1,heaps);
    auto table=views->GetGPUDescriptorHandleForHeapStart();l->SetGraphicsRootDescriptorTable(0,table);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->OMSetRenderTargets(1,&c,FALSE,nullptr);
    const D3D12_VIEWPORT viewport{0,0,float(RW),float(RH),0,1};const D3D12_RECT scissor{0,0,LONG(RW),LONG(RH)};
    l->RSSetViewports(1,&viewport);l->RSSetScissorRects(1,&scissor);l->SetPipelineState(ui.Get());
    l->DrawInstanced(18,1,0,0); // batched world + HUD, not a six-vertex heuristic
    b=test::Transition(canvas.Get(),RTS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);l->ResourceBarrier(1,&b);
    table.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);l->SetGraphicsRootDescriptorTable(0,table);
    l->OMSetRenderTargets(1,&o,FALSE,nullptr);l->SetPipelineState(tail.Get());l->DrawInstanced(3,1,0,0);
    REQUIRE(SUCCEEDED(l->Close()));ID3D12CommandList* lists[]={l};submit(d.queue.Get(),1,lists);d.WaitIdle();
    nh::Layer separated;const bool found=nh::TakeFrame(output.Get(),RW,RH,OW,OH,&separated,d.queue.Get());
    std::printf("mixed selection: %s removed=%08lx\n",ub::StatusLine().c_str(),dev->GetDeviceRemovedReason());
    REQUIRE(found);
    REQUIRE(separated.picture&&separated.clean_allowed);
    std::vector<uint8_t> actual,low,high,source;
    REQUIRE(readback(d,output.Get(),RTS,4,actual)&&readback(d,separated.l0,RTS,4,low)&&readback(d,separated.l1,RTS,4,high)&&
            readback(d,separated.picture->Scene(),NPSR,4,source));
    int maximum=0;
    for(UINT y=0;y<RH;++y)for(UINT x=0;x<RW;++x)for(int ch=0;ch<3;++ch){
        CHECK(std::abs(at(source,RW,x,y,ch)-int(std::lround(kScene[ch]*255)))<=1);
        const float source_value=srgb_source?std::pow((kScene[ch]+.055f)/1.055f,2.4f):kScene[ch];
        const float prediction=at(low,RW,x,y,ch)+(at(high,RW,x,y,ch)-at(low,RW,x,y,ch))*source_value/nh::GreyFor(F);
        maximum=std::max(maximum,int(std::lround(std::abs(prediction-at(actual,RW,x,y,ch)))));
    }
    CHECK(maximum<=2);
    CHECK(at(high,RW,0,0,0)-at(low,RW,0,0,0)>120); // world is destination-dependent now
    CHECK(at(high,RW,20,16,0)==at(low,RW,20,16,0)); // actual opaque HUD remains independent
    std::printf("mixed world/HUD batch and opaque final draw: prediction %d/255; source_srgb=%d substitutions=%llu\n",maximum,srgb_source,(unsigned long long)ub::GetStats().world_substitutions);
    ComPtr<ID3D12QueryHeap> queries;D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION,1,0};
    REQUIRE(SUCCEEDED(dev->CreateQueryHeap(&query_desc,IID_PPV_ARGS(&queries))));
    for(unsigned frame=0;frame<3;++frame){
        REQUIRE(SUCCEEDED(game.alloc->Reset())&&SUCCEEDED(l->Reset(game.alloc.Get(),nullptr)));
        b=test::Transition(world.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|NPSR);l->ResourceBarrier(1,&b);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
        b=test::Transition(canvas.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,RTS);l->ResourceBarrier(1,&b);
        l->ClearRenderTargetView(c,black,0,nullptr);
        l->SetGraphicsRootSignature(root.Get());l->SetDescriptorHeaps(1,heaps);l->SetGraphicsRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart());
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->OMSetRenderTargets(1,&c,FALSE,nullptr);
        l->RSSetViewports(1,&viewport);l->RSSetScissorRects(1,&scissor);l->SetPipelineState(ui.Get());l->DrawInstanced(18,1,0,0);
        b=test::Transition(canvas.Get(),RTS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);l->ResourceBarrier(1,&b);
        // Query-protected final draw cannot be privately replayed. It still
        // invalidates the output's older association. Discover its actual
        // source from pixels, just as for a hidden final shader dependency.
        table=views->GetGPUDescriptorHandleForHeapStart();table.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        l->SetGraphicsRootDescriptorTable(0,table);l->SetPipelineState(tail.Get());l->OMSetRenderTargets(1,&o,FALSE,nullptr);
        l->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,0);l->DrawInstanced(3,1,0,0);l->EndQuery(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,0);
        const auto closed=l->Close();
        if(FAILED(closed)){
            ComPtr<ID3D12InfoQueue> messages;dev->QueryInterface(IID_PPV_ARGS(&messages));
            if(messages)for(UINT64 m=0;m<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++m){
                SIZE_T count=0;messages->GetMessage(m,nullptr,&count);std::vector<uint8_t> memory(count);auto* message=reinterpret_cast<D3D12_MESSAGE*>(memory.data());
                if(SUCCEEDED(messages->GetMessage(m,message,&count))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::printf("mixed DX12 error: %s\n",message->pDescription);
            }
        }
        REQUIRE(SUCCEEDED(closed));submit(d.queue.Get(),1,lists);d.WaitIdle();
        nh::Layer learned;const bool selected=nh::TakeFrame(output.Get(),RW,RH,OW,OH,&learned,d.queue.Get());
        if(frame<2){
            CHECK(!selected);REQUIRE(learned.probe);
            test::List verify;REQUIRE(verify.Create(dev));const na::observe::HostWorkScope host;
            b=test::Transition(output.Get(),RTS,NPSR);verify.list->ResourceBarrier(1,&b);
            REQUIRE(learned.probe->Probe(verify.list.Get(),output.Get(),F));
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);verify.list->ResourceBarrier(1,&b);
            REQUIRE(SUCCEEDED(verify.list->Close()));ID3D12CommandList* verification[]={verify.list.Get()};submit(d.queue.Get(),1,verification);d.WaitIdle();
            bool same=false,clean=false;nh::Picture::ProofReceipt receipt;REQUIRE(learned.probe->ProofAfterCompletion(same,clean,&receipt));
            CHECK(same&&receipt.matched==RW*RH);
            nh::ReportCanvasProbe(learned.probe_source,learned.probe_generation,same,receipt.tested,receipt.matched,receipt.maximum_error);
        }else{
            REQUIRE(selected&&learned.picture&&learned.clean_allowed);
            CHECK(learned.association_source==na::observe::IdOf(canvas.Get()));
            auto motion=zero_motion(d);REQUIRE(motion);test::List calc;REQUIRE(calc.Create(dev));const na::observe::HostWorkScope host;
            b=test::Transition(output.Get(),RTS,NPSR);calc.list->ResourceBarrier(1,&b);
            REQUIRE(learned.picture->Prepare(calc.list.Get(),output.Get(),F,motion.Get(),learned.n0,learned.n1,learned.view,learned.grey,learned.srgb,learned.clean_allowed,F));
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);calc.list->ResourceBarrier(1,&b);
            REQUIRE(SUCCEEDED(calc.list->Close()));ID3D12CommandList* validation[]={calc.list.Get()};submit(d.queue.Get(),1,validation);d.WaitIdle();
            bool identity=false,clean=false;nh::Picture::ProofReceipt proof;REQUIRE(learned.picture->ProofAfterCompletion(identity,clean,&proof));
            CHECK(identity&&clean&&proof.matched==RW*RH&&proof.picture_matched==RW*RH);
            std::vector<uint8_t> input;REQUIRE(readback(d,learned.picture->Input(),NPSR,8,input));
            float worst=0;
            for(size_t pixel=0;pixel<RW*RH;++pixel)for(int ch=0;ch<3;++ch){uint16_t value=0;std::memcpy(&value,input.data()+pixel*8+ch*2,2);
                const float expected=srgb_source?std::pow((kScene[ch]+.055f)/1.055f,2.4f):kScene[ch];worst=std::max(worst,std::abs(half_value(value)-expected));}
            CHECK(worst<.002f);std::printf("mixed-canvas actual DLSS input: source_srgb=%d error %.6f\n",srgb_source,worst);
        }
    }
    CHECK(ub::GetStats().learned_frames>0);
    std::printf("hidden final dependency: discovered from completed GPU pixels, selected after two matching frames\n");
    return 0;
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    char why[1024] = {};
    const bool compiled = na::j2::CompileHudShadersForTests(why, sizeof why);
    if (!compiled) std::printf("HUD shaders: %s\n", why);
    CHECK(compiled);
    if (verdict_logic()) return 1;
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_hud_unblend_test");
    struct ExitObserver {~ExitObserver(){na::observe::SetProcessExiting();}} exit_observer;
    ComPtr<ID3D12InfoQueue> info;
    d.device->QueryInterface(IID_PPV_ARGS(&info));
    ComPtr<ID3D12InfoQueue1> immediate;d.device->QueryInterface(IID_PPV_ARGS(&immediate));DWORD callback=0;
    if(immediate)immediate->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY,D3D12_MESSAGE_SEVERITY severity,D3D12_MESSAGE_ID,const char* text,void*){
        if(severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::printf("DX12 ERROR: %s\n",text);
    },D3D12_MESSAGE_CALLBACK_FLAG_NONE,nullptr,&callback);
    if (end_to_end(d)||mixed_canvas(d)||mixed_canvas(d,true)) return 1;
    if (info) {
        UINT errors = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T bytes = 0;
            info->GetMessage(i, nullptr, &bytes);
            std::vector<uint8_t> data(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            if (SUCCEEDED(info->GetMessage(i, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                ++errors;
                std::printf("DX12 ERROR: %s\n", message->pDescription);
            }
        }
        CHECK(errors == 0);
    }
    na::observe::SetProcessExiting();
    if(immediate)immediate->UnregisterMessageCallback(callback);
    return test::finish("j2_hud_unblend_test");
}
