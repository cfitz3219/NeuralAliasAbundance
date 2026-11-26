// The HUD-free picture on DX12 end to end, through the real observation hooks and J2's live HUD code
// (j2/live_dlaa.cpp): a D3D12 "game" draws its picture and then an interface, in each of the two
// shapes, with live Frame Generation wanted; J2 traces one frame, learns the shape by itself and from
// then on makes the HUD-free picture inside the game's own recording.
//  --snapshot : hudfree/snapshot-before-interface. An opaque pass into the shown picture, then six
//               blended interface draws on it.
//  --sheet    : hudfree/sheet-redraw. Six blended interface draws into a screen-size sheet, then ONE
//               final draw composites the picture and the sheet into the shown picture.
// Checks: the variant learned; the HUD-free picture equals the picture without interface byte for byte;
// the paste rule restores every interface pixel of the real frame exactly onto another picture.
#include "test_support.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/settings.hpp"
#include "core/live_frame_gen.hpp"

#include <d3dcompiler.h>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using test::ComPtr;

namespace {
void __cdecl Sink(const na_event_batch*, void*) {}
void __stdcall Forward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) { q->ExecuteCommandLists(n, lists); }

constexpr UINT W = 160, H = 96;
constexpr char kShaders[] = R"(
cbuffer K : register(b0) { float4 color; uint mode; };
Texture2D sheet : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 pattern(float2 p) { return float4(frac(p.x / 37.0), frac(p.y / 23.0), 0.25 + 0.5 * frac((p.x + p.y) / 91.0), 1); }
float4 ps(float4 p : SV_Position) : SV_Target {
    if (mode == 1) return pattern(p.xy);                                   // the picture
    if (mode == 2) { float4 s = sheet.Load(int3(p.xy, 0)); return float4(pattern(p.xy).rgb * (1 - s.a) + s.rgb, 1); } // the final composite
    return color;                                                           // interface (premultiplied when into the sheet)
}
)";
const D3D12_RECT kHud[6] = {{8, 6, 50, 20}, {60, 10, 120, 24}, {20, 40, 70, 60}, {90, 50, 150, 70}, {5, 75, 40, 90}, {100, 78, 155, 92}};
const float kHudColor[6][4] = {{0.9f, 0.8f, 0.1f, 0.6f}, {0.1f, 0.9f, 0.3f, 0.5f}, {1, 1, 1, 0.8f}, {0.8f, 0.2f, 0.2f, 0.7f}, {0.2f, 0.4f, 0.9f, 0.6f}, {1, 0.5f, 0, 0.9f}};

struct K { float color[4]; uint32_t mode; uint32_t pad[3]; };

std::vector<uint8_t> readback(test::Device& d, ID3D12Resource* tex, D3D12_RESOURCE_STATES state)
{
    test::List l;
    if (!l.Create(d.device.Get())) return {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    const auto desc = tex->GetDesc();
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = test::CreateBuffer(d.device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto b = test::Transition(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l.list->ResourceBarrier(1, &b);
    l.list->Close();
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    std::vector<uint8_t> out(size_t(W) * H * 4);
    uint8_t* m = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
    for (UINT y = 0; y < H; ++y) std::memcpy(out.data() + size_t(y) * W * 4, m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, W * 4);
    rb->Unmap(0, nullptr);
    return out;
}
std::vector<float> floats(const std::vector<uint8_t>& rgba)
{
    std::vector<float> f(rgba.size() / 4 * 3);
    for (size_t i = 0; i < rgba.size() / 4; ++i) for (int c = 0; c < 3; ++c) f[i * 3 + c] = rgba[i * 4 + c] / 255.f;
    return f;
}
} // namespace

int main(int argc, char** argv)
{
    const bool sheet_shape = argc > 1 && std::strcmp(argv[1], "--sheet") == 0;
    setvbuf(stdout, nullptr, _IONBF, 0);
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("fg_hud_free12_test");
    ID3D12Device* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
    na::j2::SetLiveFrameGeneration(true);
    na::settings::Get().fg_enabled.store(1); // x2 on: the HUD-free picture is wanted

    // Pipelines: root constants (b0) + one SRV table (t0).
    ComPtr<ID3D12RootSignature> rs;
    {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER p[2]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; p[0].Constants = {0, 0, 8};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; p[1].DescriptorTable = {1, &range};
        D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, err;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
        REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))));
    }
    ComPtr<ID3DBlob> vsb, psb, err;
    REQUIRE(SUCCEEDED(D3DCompile(kShaders, sizeof kShaders - 1, "hud12", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)));
    REQUIRE(SUCCEEDED(D3DCompile(kShaders, sizeof kShaders - 1, "hud12", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err)));
    auto pso = [&](int blend) { // 0 opaque, 1 alpha, 2 premultiplied
        D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
        g.pRootSignature = rs.Get();
        g.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
        g.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
        g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (blend) {
            auto& t = g.BlendState.RenderTarget[0];
            t.BlendEnable = TRUE;
            t.SrcBlend = blend == 1 ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE; t.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; t.BlendOp = D3D12_BLEND_OP_ADD;
            t.SrcBlendAlpha = D3D12_BLEND_ONE; t.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; t.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        g.SampleMask = UINT_MAX;
        g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; g.RasterizerState.DepthClipEnable = TRUE;
        g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        g.NumRenderTargets = 1; g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; g.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> p;
        dev->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&p));
        return p;
    };
    auto opaque = pso(0), alpha = pso(1), premul = pso(2);
    REQUIRE(opaque && alpha && premul);

    auto shown = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto sheet = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto reference = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(shown && sheet && reference);
    ComPtr<ID3D12DescriptorHeap> rtvh, srvh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh))));
    D3D12_DESCRIPTOR_HEAP_DESC sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srvh))));
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv_at = [&](int i) { auto h = rtvh->GetCPUDescriptorHandleForHeapStart(); h.ptr += SIZE_T(i) * inc; return h; };
    dev->CreateRenderTargetView(shown.Get(), nullptr, rtv_at(0));
    dev->CreateRenderTargetView(sheet.Get(), nullptr, rtv_at(1));
    dev->CreateRenderTargetView(reference.Get(), nullptr, rtv_at(2));
    dev->CreateShaderResourceView(sheet.Get(), nullptr, srvh->GetCPUDescriptorHandleForHeapStart());

    test::List game;
    REQUIRE(game.Create(dev));
    game.list->Close();
    const D3D12_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
    const D3D12_RECT full{0, 0, LONG(W), LONG(H)};
    auto setup = [&](ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE rt) {
        l->SetGraphicsRootSignature(rs.Get());
        ID3D12DescriptorHeap* heaps[] = {srvh.Get()};
        l->SetDescriptorHeaps(1, heaps);
        l->SetGraphicsRootDescriptorTable(1, srvh->GetGPUDescriptorHandleForHeapStart());
        l->OMSetRenderTargets(1, &rt, FALSE, nullptr);
        l->RSSetViewports(1, &vp);
        l->RSSetScissorRects(1, &full);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    auto draw = [&](ID3D12GraphicsCommandList* l, ID3D12PipelineState* p, const float* color, uint32_t mode, const D3D12_RECT* scissor) {
        K k{{color ? color[0] : 0, color ? color[1] : 0, color ? color[2] : 0, color ? color[3] : 0}, mode, {}};
        l->SetPipelineState(p);
        l->SetGraphicsRoot32BitConstants(0, 8, &k, 0);
        l->RSSetScissorRects(1, scissor ? scissor : &full);
        l->DrawInstanced(3, 1, 0, 0);
    };
    // The reference: the picture alone.
    {
        game.Reset();
        auto b = test::Transition(reference.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        game.list->ResourceBarrier(1, &b);
        setup(game.list.Get(), rtv_at(2));
        draw(game.list.Get(), opaque.Get(), nullptr, 1, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        game.list->ResourceBarrier(1, &b);
        game.list->Close();
        ID3D12CommandList* l[] = {game.list.Get()};
        d.queue->ExecuteCommandLists(1, l);
        d.WaitIdle();
    }
    const auto expected = readback(d, reference.Get(), D3D12_RESOURCE_STATE_COMMON);

    std::vector<uint8_t> real, hudfree;
    bool got = false;
    for (int frame = 0; frame < 40 && !got; ++frame) {
        game.Reset();
        ID3D12GraphicsCommandList* l = game.list.Get();
        auto bs = test::Transition(shown.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        l->ResourceBarrier(1, &bs);
        if (sheet_shape) {
            auto bh = test::Transition(sheet.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            l->ResourceBarrier(1, &bh);
            const float none[4] = {};
            l->ClearRenderTargetView(rtv_at(1), none, 0, nullptr);
            setup(l, rtv_at(1));
            for (int i = 0; i < 6; ++i) {
                float c[4] = {kHudColor[i][0] * kHudColor[i][3], kHudColor[i][1] * kHudColor[i][3], kHudColor[i][2] * kHudColor[i][3], kHudColor[i][3]};
                draw(l, premul.Get(), c, 0, &kHud[i]);
            }
            auto br = test::Transition(sheet.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            l->ResourceBarrier(1, &br);
            setup(l, rtv_at(0));
            draw(l, opaque.Get(), nullptr, 2, nullptr); // the one final draw
            auto bc = test::Transition(sheet.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            l->ResourceBarrier(1, &bc);
        } else {
            setup(l, rtv_at(0));
            draw(l, opaque.Get(), nullptr, 1, nullptr);               // the picture straight onto the shown picture
            for (int i = 0; i < 6; ++i) draw(l, alpha.Get(), kHudColor[i], 0, &kHud[i]); // the interface on it
        }
        std::swap(bs.Transition.StateBefore, bs.Transition.StateAfter);
        l->ResourceBarrier(1, &bs);
        REQUIRE(SUCCEEDED(l->Close()));
        ID3D12CommandList* lists[] = {l};
        na::j2::Execute(d.queue.Get(), 1, lists, &Forward);
        d.WaitIdle();
        na::j2::NativeTracePresent(d.queue.Get(), shown.Get(), true);
        const char* why = nullptr;
        if (ID3D12Resource* hf = na::j2::LiveHudFree(&why)) {
            real = readback(d, shown.Get(), D3D12_RESOURCE_STATE_COMMON);
            hudfree = readback(d, hf, D3D12_RESOURCE_STATE_RENDER_TARGET);
            got = true;
        } else if (frame % 4 == 0) std::printf("  frame %d: %s\n", frame, why ? why : "?");
    }
    const auto st = na::j2::GetLiveHudStats();
    std::printf("%s: learned=%u method \"%s\"\n  verdict: %s\n", sheet_shape ? "sheet" : "snapshot", st.learned, st.method, st.verdict);
    CHECK(got);
    if (got) {
        CHECK(std::strstr(st.method, sheet_shape ? "hudfree/sheet-redraw" : "hudfree/snapshot-before-interface") != nullptr);
        size_t differ = 0;
        for (size_t i = 0; i < expected.size(); ++i) differ += (i % 4 != 3) && expected[i] != hudfree[i];
        auto fr = floats(real), fh = floats(hudfree);
        std::vector<float> gen(fr.size(), 0.33f);
        na::fglive::PasteHud(fr.data(), fh.data(), gen.data(), W, H, na::fglive::kPictureSdr);
        size_t hud = 0, wrong = 0;
        for (size_t p = 0; p < size_t(W) * H; ++p) {
            if (!na::fglive::HudPixel(&fr[p * 3], &fh[p * 3], na::fglive::kPictureSdr)) continue;
            ++hud;
            for (int c = 0; c < 3; ++c) wrong += gen[p * 3 + c] != fr[p * 3 + c];
        }
        std::printf("  HUD-free == picture without interface: %zu differing bytes; interface pixels %zu, wrong after the paste %zu\n", differ, hud, wrong);
        CHECK(differ == 0);
        CHECK(hud > 500 && wrong == 0);
    }
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    return test::finish(sheet_shape ? "fg_hud_free12_test --sheet" : "fg_hud_free12_test --snapshot");
}
