// The scene sizes come from native DX11 gameplay traces. Synthetic colors make the returned
// current frame, channel order, destination extent and one-pixel HUD independently observable.
// Waiting and mapping below are restricted to this offline test.
#include "test_support.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/native_d3d11/shared_resources.hpp"
#include "host_dx12/native11/screen_check.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace na::native_d3d11;

namespace
{
constexpr UINT outW = 3440, outH = 1440;
constexpr char shader[] =
    "float4 vs(uint i:SV_VertexID):SV_Position{float2 p=float2((i<<1)&2,i&2);return "
    "float4(p*float2(2,-2)+float2(-1,1),0,1);}"
    "Texture2D<float4> image:register(t0);float4 ps(float4 p:SV_Position):SV_Target{return image.Load(int3(p.xy,0));}"
    "struct V{float4 p:SV_Position;float2 uv:TEXCOORD;};"
    "V worldVs(uint i:SV_VertexID){V "
    "v;v.uv=float2((i<<1)&2,i&2);v.p=float4(v.uv*float2(2,-2)+float2(-1,1),.5,1);return v;}"
    "float4 halves(float x){return x>.5?float4(.88,.06,.06,1):float4(.06,.06,.88,1);}"
    "float4 world0(V i):SV_Target{return halves(i.uv.x);}"
    "float4 world1(V i):SV_Target{return halves(1-i.uv.x);}"
    "SamplerState sampler0:register(s0);float4 nativeScale(V i):SV_Target{return image.SampleLevel(sampler0,i.uv,0);}";

// The game owns this native-sized pair and final screen for the entire test, through all mode changes.
// The fixture supplies the already identified world draw. Arbitrary game draws cannot use that assumption.
struct Game
{
    ComPtr<ID3D11Texture2D> color, depth, screen;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> pixel[2];
    UINT64 nativeInvocations = 0;
};

int DepthTarget(ID3D11Device *dev, UINT w, UINT h, ComPtr<ID3D11Texture2D> &depth, ComPtr<ID3D11DepthStencilView> &view)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    REQUIRE(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &depth)));
    D3D11_DEPTH_STENCIL_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_D32_FLOAT;
    v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    REQUIRE(SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), &v, &view)));
    return 0;
}

int CreateGame(ID3D11Device *dev, Game &g)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = outW;
    d.Height = outH;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    REQUIRE(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g.color)));
    REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(g.color.Get(), nullptr, &g.target)));
    REQUIRE(DepthTarget(dev, outW, outH, g.depth, g.depthView) == 0);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    REQUIRE(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g.screen)));
    ComPtr<ID3DBlob> code;
    REQUIRE(SUCCEEDED(
        D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "worldVs", "vs_5_0", 0, 0, &code, nullptr)));
    REQUIRE(SUCCEEDED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.vs)));
    for (UINT i = 0; i < 2; ++i)
    {
        code.Reset();
        REQUIRE(SUCCEEDED(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, i ? "world1" : "world0",
                                     "ps_5_0", 0, 0, &code, nullptr)));
        REQUIRE(
            SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.pixel[i])));
    }
    return 0;
}

int DrawWorld(ID3D11Device *dev, ID3D11DeviceContext *ctx, const Game &g, UINT frame, UINT w, UINT h,
              ID3D11RenderTargetView *target, ID3D11DepthStencilView *z, UINT64 &calls)
{
    const D3D11_VIEWPORT full{0, 0, float(outW), float(outH), 0, 1}, reduced{0, 0, float(w), float(h), 0, 1};
    ctx->OMSetRenderTargets(1, g.target.GetAddressOf(), g.depthView.Get());
    ctx->RSSetViewports(1, &full);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    ctx->PSSetShader(g.pixel[frame].Get(), nullptr, 0);
    ctx->OMSetRenderTargets(1, &target, z);
    ctx->RSSetViewports(1, &reduced);
    ctx->ClearDepthStencilView(z, D3D11_CLEAR_DEPTH, 1.f, 0);
    D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    ComPtr<ID3D11Query> query;
    REQUIRE(SUCCEEDED(dev->CreateQuery(&desc, &query)));
    ctx->Begin(query.Get());
    ctx->Draw(3, 0);
    ctx->End(query.Get());
    ctx->OMSetRenderTargets(1, g.target.GetAddressOf(), g.depthView.Get());
    ctx->RSSetViewports(1, &full);
    ctx->Flush();
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    const ULONGLONG deadline = GetTickCount64() + 5000;
    HRESULT hr;
    while ((hr = ctx->GetData(query.Get(), &stats, sizeof(stats), 0)) == S_FALSE && GetTickCount64() < deadline)
        Sleep(1);
    REQUIRE(hr == S_OK);
    calls = stats.PSInvocations;
    REQUIRE(calls >= UINT64(w) * h);
    return 0;
}

// Two draws read the same reduced source with the same shader and output format.
// Only one intermediate survives to the screen. The existing Referee witness must distinguish them.
int QualifyReaders(ID3D11Device *dev, ID3D11DeviceContext *ctx, const Game &game, ID3D11Texture2D *scene)
{
    using Witness = na::native11::ScreenCheck;
    Witness witness;
    const auto sceneId = reinterpret_cast<uintptr_t>(scene), depthId = reinterpret_cast<uintptr_t>(game.depth.Get());
    witness.SetScene(sceneId, depthId);
    D3D11_TEXTURE2D_DESC desc{};
    game.screen->GetDesc(&desc);
    ComPtr<ID3D11Texture2D> destinations[2];
    ComPtr<ID3D11RenderTargetView> targets[2];
    for (UINT i = 0; i < 2; ++i)
    {
        REQUIRE(SUCCEEDED(dev->CreateTexture2D(&desc, nullptr, &destinations[i])));
        REQUIRE(SUCCEEDED(dev->CreateRenderTargetView(destinations[i].Get(), nullptr, &targets[i])));
    }
    ComPtr<ID3D11ShaderResourceView> src;
    REQUIRE(SUCCEEDED(dev->CreateShaderResourceView(scene, nullptr, &src)));
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3D11PixelShader> ps;
    REQUIRE(SUCCEEDED(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "nativeScale", "ps_5_0", 0, 0,
                                 &bytecode, nullptr)));
    REQUIRE(SUCCEEDED(dev->CreatePixelShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &ps)));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> sampler;
    REQUIRE(SUCCEEDED(dev->CreateSamplerState(&sd, &sampler)));
    Witness::Reader readers[2]{};
    for (UINT i = 0; i < 2; ++i)
    {
        readers[i].output = reinterpret_cast<uintptr_t>(destinations[i].Get());
        readers[i].shader = reinterpret_cast<uintptr_t>(ps.Get());
        readers[i].format = desc.Format;
    }
    const D3D11_VIEWPORT vp{0, 0, float(outW), float(outH), 0, 1};
    for (UINT f = 1; f <= 100 && !witness.Verified(sceneId, depthId); ++f)
    {
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(game.vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT i = 0; i < 2; ++i)
        {
            auto mark = witness.Begin(ctx, readers[i], scene, DXGI_FORMAT_R8G8B8A8_UNORM, f, 1);
            ctx->OMSetRenderTargets(1, targets[i].GetAddressOf(), nullptr);
            ctx->PSSetShaderResources(0, 1, src.GetAddressOf());
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView *clear = nullptr;
            ctx->PSSetShaderResources(0, 1, &clear);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            if (mark.source)
                Witness::Restore(ctx, mark);
        }
        ctx->CopyResource(game.screen.Get(), destinations[1].Get());
        witness.Present(ctx, game.screen.Get(), f, 1, false);
        ctx->Flush();
        Sleep(2); // Only this offline fixture paces the asynchronous screen samples.
    }
    std::printf("Scaled reader challenge: decoy rejected %llu, visible reader selected %u\n",
                static_cast<unsigned long long>(witness.Refuted()), unsigned(witness.Matches(readers[1])));
    REQUIRE(witness.Refuted() && witness.Matches(readers[1]) && !witness.Matches(readers[0]));
    return 0;
}

int Exercise(test::Device &d, ID3D11Device *native, ID3D11DeviceContext *ctx, const Game &game, UINT w, UINT h,
             na::dlss::Quality level)
{
    SharedFences fences;
    REQUIRE(SUCCEEDED(fences.Create(native, d.device.Get(), d.queue.Get())));
    SharedTexture color, depth, motion, output;
    REQUIRE(SUCCEEDED(color.Create(native, d.device.Get(), {w, h, DXGI_FORMAT_R8G8B8A8_UNORM})));
    REQUIRE(SUCCEEDED(depth.Create(native, d.device.Get(), {w, h, DXGI_FORMAT_R32_FLOAT})));
    REQUIRE(SUCCEEDED(motion.Create(native, d.device.Get(), {w, h, DXGI_FORMAT_R16G16_FLOAT})));
    REQUIRE(SUCCEEDED(output.Create(native, d.device.Get(), {outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM, true})));
    ComPtr<ID3D11RenderTargetView> worldTarget;
    ComPtr<ID3D11DepthStencilView> worldDepthView;
    ComPtr<ID3D11Texture2D> worldDepth;
    REQUIRE(SUCCEEDED(native->CreateRenderTargetView(color.Native(), nullptr, &worldTarget)));
    REQUIRE(DepthTarget(native, w, h, worldDepth, worldDepthView) == 0);
    ComPtr<ID3D11ShaderResourceView> resultView;
    REQUIRE(SUCCEEDED(native->CreateShaderResourceView(output.Native(), nullptr, &resultView)));

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = outW;
    desc.Height = outH;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    auto screen = game.screen;
    ComPtr<ID3D11Texture2D> before, after;
    ComPtr<ID3D11RenderTargetView> screenView;
    REQUIRE(SUCCEEDED(native->CreateRenderTargetView(screen.Get(), nullptr, &screenView)));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;
    REQUIRE(SUCCEEDED(native->CreateTexture2D(&desc, nullptr, &before)));
    REQUIRE(SUCCEEDED(native->CreateTexture2D(&desc, nullptr, &after)));
    ComPtr<ID3DBlob> vsCode, psCode;
    REQUIRE(SUCCEEDED(
        D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsCode, nullptr)));
    REQUIRE(SUCCEEDED(
        D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psCode, nullptr)));
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    REQUIRE(SUCCEEDED(native->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)));
    REQUIRE(SUCCEEDED(native->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)));
    const D3D11_VIEWPORT vp{0, 0, float(outW), float(outH), 0, 1};
    std::vector<uint32_t> zero(size_t(w) * h, 0);
    ctx->UpdateSubresource(motion.Native(), 0, nullptr, zero.data(), w * 4, 0);

    const UINT stripeX = outW - 83;
    const uint32_t hud = 0xff12c8fa; // literal BGRA bytes
    const std::vector<uint32_t> stripe(outH, hud);
    for (UINT frame = 0; frame < 2; ++frame)
    {
        UINT64 calls = 0;
        REQUIRE(DrawWorld(native, ctx, game, frame, w, h, worldTarget.Get(), worldDepthView.Get(), calls) == 0);
        const double observed = double(calls) / game.nativeInvocations;
        const double expected = double(w) * h / (double(outW) * outH);
        REQUIRE(std::abs(observed - expected) < 0.02);
        ctx->CopyResource(depth.Native(), worldDepth.Get());
        uint64_t ready = 0, done = 0;
        REQUIRE(SUCCEEDED(fences.SignalNative(&ready)) && SUCCEEDED(fences.WaitEffects(ready)));
        test::List list;
        REQUIRE(list.Create(d.device.Get()));
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER barriers[] = {
            test::Transition(color.Effects(), D3D12_RESOURCE_STATE_COMMON, read),
            test::Transition(depth.Effects(), D3D12_RESOURCE_STATE_COMMON, read),
            test::Transition(motion.Effects(), D3D12_RESOURCE_STATE_COMMON, read),
            test::Transition(output.Effects(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list.list->ResourceBarrier(4, barriers);
        na::dlss::EvalDesc e{};
        e.color = color.Effects();
        e.depth = depth.Effects();
        e.motion = motion.Effects();
        e.output = output.Effects();
        e.render_w = w;
        e.render_h = h;
        e.out_w = outW;
        e.out_h = outH;
        e.quality = level;
        e.reset = true;
        e.auto_exposure = true;
        e.preset = 13;
        REQUIRE(na::dlss::Evaluate(list.list.Get(), e));
        for (auto &barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list.list->ResourceBarrier(4, barriers);
        REQUIRE(SUCCEEDED(list.list->Close()));
        ID3D12CommandList *submitted[] = {list.list.Get()};
        d.queue->ExecuteCommandLists(1, submitted);
        na::dlss::OnExecuted(d.queue.Get(), 1, submitted);
        REQUIRE(SUCCEEDED(fences.SignalEffects(&done)) && SUCCEEDED(fences.WaitNative(done)));

        // Full-size native compositor, followed by the game's independent HUD draw equivalent.
        ctx->OMSetRenderTargets(1, screenView.GetAddressOf(), nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        if (frame == 0)
        {
            // Negative control: native pixel coordinates point outside a reduced input. Confirm
            // the missing right edge before supplying a full-sized source to the identical draw.
            ComPtr<ID3D11ShaderResourceView> smallView;
            REQUIRE(SUCCEEDED(native->CreateShaderResourceView(color.Native(), nullptr, &smallView)));
            ctx->PSSetShaderResources(0, 1, smallView.GetAddressOf());
            ctx->Draw(3, 0);
            ctx->CopyResource(before.Get(), screen.Get());
            D3D11_MAPPED_SUBRESOURCE bad{};
            REQUIRE(SUCCEEDED(ctx->Map(before.Get(), 0, D3D11_MAP_READ, 0, &bad)));
            auto *row =
                reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(bad.pData) + (outH / 3) * bad.RowPitch);
            const bool lostRight = (row[outW - 33] & 0xffffffu) == 0 && (row[32] & 0xffffffu) != 0;
            ctx->Unmap(before.Get(), 0);
            REQUIRE(lostRight);
        }
        ctx->PSSetShaderResources(0, 1, resultView.GetAddressOf());
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView *unbound = nullptr;
        ctx->PSSetShaderResources(0, 1, &unbound);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->CopyResource(before.Get(), screen.Get());
        D3D11_BOX box{stripeX, 0, 0, stripeX + 1, outH, 1};
        ctx->UpdateSubresource(screen.Get(), 0, &box, stripe.data(), 4, 0);
        ctx->CopyResource(after.Get(), screen.Get());
        D3D11_MAPPED_SUBRESOURCE pre{}, post{};
        REQUIRE(SUCCEEDED(ctx->Map(before.Get(), 0, D3D11_MAP_READ, 0, &pre)));
        REQUIRE(SUCCEEDED(ctx->Map(after.Get(), 0, D3D11_MAP_READ, 0, &post)));
        size_t hudPixels = 0, changedOutside = 0, quadrantSize[4]{}, matching[4]{};
        for (UINT y = 0; y < outH; ++y)
        {
            const auto *a =
                reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(pre.pData) + size_t(y) * pre.RowPitch);
            const auto *b = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(post.pData) +
                                                               size_t(y) * post.RowPitch);
            for (UINT x = 0; x < outW; ++x)
            {
                if (x == stripeX)
                    hudPixels += (b[x] == hud && a[x] != hud);
                else
                    changedOutside += (a[x] != b[x]);
                const UINT q = (x >= outW / 2) + 2 * (y >= outH / 2);
                const int r = (a[x] >> 16) & 255, blueByte = a[x] & 255;
                const bool wantRed = (x >= outW / 2) != (frame != 0);
                ++quadrantSize[q];
                matching[q] += wantRed ? r > blueByte + 60 : blueByte > r + 60;
            }
        }
        ctx->Unmap(after.Get(), 0);
        ctx->Unmap(before.Get(), 0);
        // NGX can draw its enabled diagnostic text over part of the lower left quadrant.
        // Check every pixel's dominant color and require coverage in every quadrant.
        double lowest = 100.0;
        for (UINT q = 0; q != 4; ++q)
            lowest = std::min(lowest, 100.0 * matching[q] / quadrantSize[q]);
        std::printf("%ux%u -> %ux%u frame %u: PS invocations %llu (%.2f%% of native), lowest quadrant match %.3f%%, "
                    "HUD %zu/%u, changed outside HUD %zu\n",
                    w, h, outW, outH, frame, static_cast<unsigned long long>(calls), observed * 100, lowest, hudPixels,
                    outH, changedOutside);
        REQUIRE(lowest > 98.0 && hudPixels == outH && changedOutside == 0);
        d.WaitIdle();
    }
    REQUIRE(QualifyReaders(native, ctx, game, color.Native()) == 0);
    return 0;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    test::Device d;
    if (!test::CreateDevice(d, false))
        return test::kSkip;
    ComPtr<IDXGIAdapter1> adapter;
    REQUIRE(SUCCEEDED(d.factory->EnumAdapterByLuid(d.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))));
    DXGI_ADAPTER_DESC1 desc{};
    REQUIRE(SUCCEEDED(adapter->GetDesc1(&desc)));
    if (desc.VendorId != 0x10de)
        return test::kSkip;
    std::printf("Native DX11 / D3D12 device: %ls\n", desc.Description);
    ComPtr<ID3D11Device> native;
    ComPtr<ID3D11DeviceContext> ctx;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                        D3D11_SDK_VERSION, &native, nullptr, &ctx)));
    const wchar_t *cache = argc > 1 ? argv[1] : L".";
    CreateDirectoryW(cache, nullptr);
    REQUIRE(na::dlss::InitOwn(d.device.Get(), cache));
    Game game;
    REQUIRE(CreateGame(native.Get(), game) == 0);
    REQUIRE(DrawWorld(native.Get(), ctx.Get(), game, 0, outW, outH, game.target.Get(), game.depthView.Get(),
                      game.nativeInvocations) == 0);
    std::printf("Native world baseline: %llu PS invocations\n",
                static_cast<unsigned long long>(game.nativeInvocations));
    for (const auto mode : {na::dlss::Quality::MaxQuality, na::dlss::Quality::Balanced,
                            na::dlss::Quality::MaxPerformance, na::dlss::Quality::MaxQuality})
    {
        na::dlss::OptimalSettings optimal{};
        REQUIRE(na::dlss::QueryOptimal(outW, outH, mode, &optimal));
        REQUIRE(Exercise(d, native.Get(), ctx.Get(), game, optimal.render_w, optimal.render_h, mode) == 0);
    }
    REQUIRE(Exercise(d, native.Get(), ctx.Get(), game, 2408, 1008, na::dlss::Quality::MaxQuality) == 0);
    na::dlss::Shutdown();
    return 0;
}
