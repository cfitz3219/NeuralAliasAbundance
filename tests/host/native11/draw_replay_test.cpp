// Draw replay on the native DX11 route (native11/draw_replay.hpp), on a real D3D11 device (hardware,
// else WARP, so the hosted runner runs it). It checks, with no game and no DLSS:
//   - the device hook keeps the vertex shader the game creates, and the twin is built from it;
//   - a qualifying scene draw is replayed with stream output: the packed positions are the clip
//     positions the vertex shader made, in the draw's expanded (index) order;
//   - the game's state is given back (no geometry stage, no stream-output target);
//   - the gates: not the scene depth, the game's own stream output, a draw after the hand-over,
//     a draw over half the budget;
//   - the same draw in two frames has the same identity (what the DX12 rasteriser matches on).
#include "host_dx12/native11/draw_replay.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace dr = na::native11::draw_replay;

namespace {
int g_failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)
#define REQUIRE(c) do { if (!(c)) { std::printf("FATAL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
}

int main()
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    bool warp = false;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        warp = true;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
            std::printf("native11_draw_replay_test: SKIP (no DX11 device)\n");
            return 77;
        }
    }
    std::printf("device: %s\n", warp ? "WARP" : "hardware");
    REQUIRE(dr::Install(dev.Get()));

    constexpr UINT w = 32, h = 24;
    const char shader[] = R"(
cbuffer C : register(b0) { float4 shift; }
float4 vs(float2 p : POS) : SV_Position { return float4(p + shift.xy, .5, 1); }
float4 ps() : SV_Target { return 1; }
)";
    ComPtr<ID3DBlob> vs_code, ps_code;
    REQUIRE(SUCCEEDED(D3DCompile(shader, sizeof shader - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs_code, nullptr)));
    REQUIRE(SUCCEEDED(D3DCompile(shader, sizeof shader - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps_code, nullptr)));
    const uint64_t noted_before = dr::GetStats().noted;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    REQUIRE(SUCCEEDED(dev->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs)));
    REQUIRE(SUCCEEDED(dev->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps)));
    CHECK(dr::GetStats().noted == noted_before + 1); // the device hook kept it
    const D3D11_INPUT_ELEMENT_DESC element{"POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> layout;
    REQUIRE(SUCCEEDED(dev->CreateInputLayout(&element, 1, vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &layout)));
    const float quad[8] = {-.5f, -.65f, -.5f, .65f, .6f, .65f, .6f, -.65f};
    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Buffer> vb, ib, cb, game_so;
    bd.ByteWidth = sizeof quad; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{quad};
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, &data, &vb)));
    bd.ByteWidth = sizeof indices; bd.BindFlags = D3D11_BIND_INDEX_BUFFER; data.pSysMem = indices;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, &data, &ib)));
    bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb)));
    bd.ByteWidth = 1024; bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    REQUIRE(SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &game_so)));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth, color;
    REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &depth)));
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &color)));
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11RenderTargetView> rtv;
    REQUIRE(SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv)));
    REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(color.Get(), nullptr, &rtv)));
    // The pack's target: one row holds this test's positions.
    td.Width = dr::kPackWidth; td.Height = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> packed, staging;
    REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &packed)));
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    REQUIRE(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging)));
    ComPtr<ID3D11UnorderedAccessView> packed_view;
    REQUIRE(SUCCEEDED(dev->CreateUnorderedAccessView(packed.Get(), nullptr, &packed_view)));

    const D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    const float* vp_words = &vp.TopLeftX;
    // One frame of the "game": the quad drawn into the scene depth, then the provider's AfterDraw.
    const auto draw = [&](uint64_t frame, float shift, bool scene) {
        const float s[4] = {shift, 0, 0, 0};
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, s, 0, 0);
        ID3D11Buffer* vbs[] = {vb.Get()};
        const UINT stride = 8, offset = 0;
        ctx->IASetInputLayout(layout.Get());
        ctx->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
        ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = {cb.Get()};
        ctx->VSSetConstantBuffers(0, 1, cbs);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
        ctx->OMSetRenderTargets(1, rtvs, dsv.Get());
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        ctx->DrawIndexed(6, 0, 0);
        dr::DrawFacts f;
        f.frame = frame;
        f.scene_depth = scene;
        f.viewport = vp_words;
        f.viewports = 1;
        dr::AfterDraw(ctx.Get(), f, {true, false, 6, 1, 0, 0, 0});
    };
    const auto read_positions = [&](float out[6][4]) {
        ctx->CopyResource(staging.Get(), packed.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        std::memcpy(out, m.pData, 6 * 16);
        ctx->Unmap(staging.Get(), 0);
        return true;
    };

    dr::Configure(true, 0, false);
    // Frame 1: the first sight of the shader queues its twin; nothing is replayed yet.
    draw(1, 0, true);
    CHECK(dr::GetStats().twin_queued == 1);
    CHECK(dr::Seal(1).draws.empty());
    for (int i = 0; i < 200 && !dr::GetStats().built && !dr::GetStats().build_failed; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(dr::GetStats().built == 1 && dr::GetStats().build_failed == 0);

    // Frame 2: replayed. A draw into another depth is not.
    draw(2, 0, true);
    draw(2, 0, false);
    {
        ComPtr<ID3D11GeometryShader> gs;
        ComPtr<ID3D11Buffer> so;
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ctx->SOGetTargets(1, &so);
        CHECK(!gs && !so); // the game's state is given back
    }
    const dr::Sealed two = dr::Seal(2);
    REQUIRE(two.draws.size() == 1);
    const dr::Record r2 = two.draws[0];
    CHECK(two.verts == 6 && r2.first == 0 && r2.verts == 6 && r2.width == w && r2.height == h);
    CHECK(r2.scissor[0] == 0 && r2.scissor[1] == 0 && r2.scissor[2] == int32_t(w) && r2.scissor[3] == int32_t(h));
    std::printf("gates after frame 2: %s\n", dr::GetStats().gates);
    CHECK(std::strstr(dr::GetStats().gates, "other_depth=1") != nullptr);
    // After the hand-over the frame's draws are not the scene any more.
    draw(2, 0, true);
    CHECK(std::strstr(dr::GetStats().gates, "after_handover=1") != nullptr);
    REQUIRE(dr::Pack(ctx.Get(), packed_view.Get(), two.verts));
    float p[6][4]{};
    REQUIRE(read_positions(p));
    for (int i = 0; i < 6; ++i) {
        const int k = indices[i];
        const bool same = std::fabs(p[i][0] - quad[2 * k]) < 1e-6f && std::fabs(p[i][1] - quad[2 * k + 1]) < 1e-6f &&
                          std::fabs(p[i][2] - .5f) < 1e-6f && std::fabs(p[i][3] - 1.f) < 1e-6f;
        if (!same) std::printf("position %d: %f %f %f %f\n", i, p[i][0], p[i][1], p[i][2], p[i][3]);
        CHECK(same);
    }

    // Frame 3: the same draw moved. Same identity; the positions moved with it.
    draw(3, .25f, true);
    const dr::Sealed three = dr::Seal(3);
    REQUIRE(three.draws.size() == 1);
    CHECK(three.draws[0].sig == r2.sig && three.draws[0].vb == r2.vb);
    REQUIRE(dr::Pack(ctx.Get(), packed_view.Get(), three.verts));
    REQUIRE(read_positions(p));
    CHECK(std::fabs(p[0][0] - (quad[0] + .25f)) < 1e-6f && std::fabs(p[5][0] - (quad[6] + .25f)) < 1e-6f);

    // Frame 4: the game streams out itself: left alone.
    {
        ID3D11Buffer* targets[] = {game_so.Get()};
        const UINT offsets[] = {0};
        ctx->SOSetTargets(1, targets, offsets);
        draw(4, 0, true);
        ctx->SOSetTargets(0, nullptr, nullptr);
        CHECK(dr::Seal(4).draws.empty());
        CHECK(std::strstr(dr::GetStats().gates, "stream_out=1") != nullptr);
    }
    // Frame 5: a budget of 8 vertices: a 6-vertex draw is over half of it.
    dr::Configure(true, 8, false);
    draw(5, 0, true);
    CHECK(dr::Seal(5).draws.empty() && dr::GetStats().draw_too_large == 1);
    // Off: nothing is replayed.
    dr::Configure(false, 0, false);
    const uint64_t replayed = dr::GetStats().replayed;
    draw(6, 0, true);
    CHECK(dr::Seal(6).draws.empty() && dr::GetStats().replayed == replayed);

    const auto st = dr::GetStats();
    std::printf("noted %llu built %llu replayed %llu verts %llu packed %llu gates {%s}\n", (unsigned long long)st.noted,
                (unsigned long long)st.built, (unsigned long long)st.replayed, (unsigned long long)st.vertices,
                (unsigned long long)st.packed, st.gates);
    std::printf("native11_draw_replay_test: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), g_failures ? 1 : 0); // the twin worker thread never ends
    return g_failures ? 1 : 0;
}
