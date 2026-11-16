#include "host_dx12/settings.hpp"
#include "host_dx12/gpu/services.hpp"
// J2 live DLAA on the DX12 host with no scene camera (VARIANTS camera/optional,
// motion/measured-total, depth/direction-from-depth-writes), against the fake NGX runtime on a
// real D3D12 device (WARP-capable). The scene is a real depth-tested, depth-writing draw whose
// picture slides two pixels a frame, and it carries a camera block the finder accepts (the layout
// of j2_live_test), so "not looked for" is a choice of the setting and not an empty scene.
//
// What is checked:
//  1. [j2] camera = 0: nothing camera-related happens (no matrix is rewritten, no camera frame is
//     counted, the game's camera bytes stay as the game wrote them). The first frame waits once for
//     the depth direction; from the second it is proven from the scene's own depth writes
//     (GREATER_EQUAL + write, cleared to 0: larger is nearer). Frames are delivered with measured
//     motion, DLSS gets the depth direction and a viewport sample, and its history carries on.
//  2. [j2] camera = 1 with the camera there again: the camera takes over.
//  3. [j2] camera = 1 and the camera disappears from the scene: delivery goes on (held for a few
//     frames as before, then measured, with the raster sample moved to the viewports). No frame
//     waits for the camera.
//  4. [j2] camera = 2: a frame without a camera waits for one, as before 2026-11-16.
// Where this build or device cannot measure motion at all (the flow engine's shaders need DXC at
// build time), frames without a camera are left as the game drew them with that reason, nothing is
// latched, and the camera phases still deliver: the test says which of the two it saw.
#include "test_support.hpp"
#include <d3dcompiler.h>
#include "fake_ngx_runtime.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/observe/producers.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
void __cdecl Sink(const na_event_batch*, void*) {}

std::vector<ID3D12CommandList*> g_forwarded;
void __stdcall RawForward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) { q->ExecuteCommandLists(n, lists); }
void __stdcall Forward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    g_forwarded.assign(lists, lists + n);
    na::nr::Execute(q, n, lists, &RawForward); // the host's own forward seam (recording leases complete here)
}

using M = std::array<double, 16>;
M Mul(const M& a, const M& b)
{
    M o{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) o[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    return o;
}
bool Inverse(const M& m, M& inv)
{
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { a[r][c] = m[r * 4 + c]; a[r][c + 4] = r == c; }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        if (std::abs(a[p][c]) < 1e-12) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const double d = a[c][c];
        for (double& v : a[c]) v /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) inv[r * 4 + c] = a[r][c + 4];
    return true;
}
// Reverse-Z perspective times a rotated, translated view (the camera of j2_live_test).
M Camera(double x)
{
    const double n = 0.5, f = 5000.0, sy = 1.0 / std::tan(0.45), sx = sy / 2.0;
    const M p{sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, n / (n - f), n * f / (f - n), 0, 0, 1, 0};
    const double c = std::cos(0.3), s = std::sin(0.3);
    M v{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    const double pos[3] = {x, 20, -40};
    for (int r = 0; r < 3; ++r) v[r * 4 + 3] = -(v[r * 4] * pos[0] + v[r * 4 + 1] * pos[1] + v[r * 4 + 2] * pos[2]);
    return Mul(p, v);
}
void Store(uint8_t* dst, const M& m)
{
    for (int i = 0; i < 16; ++i) { const float f = static_cast<float>(m[i]); std::memcpy(dst + i * 4, &f, 4); }
}
} // namespace

int main()
{
    na::gpu::SetBackendForTests(na::gpu::TestBackend::Native);
    setvbuf(stdout, nullptr, _IONBF, 0); // progress survives a timeout kill
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_no_camera_test");
    ID3D12Device* dev = d.device.Get();
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    // As in the game: the measuring pass keeps its working memory on recording leases, and a list
    // Reset starts a new one (without this it reports "recording lifetime unavailable").
    REQUIRE(na::observe::InstallListResetTracking(dev, na::nr::OnCommandListReset));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
    na::dlss::SetInitOwnOverrideForTests([](ID3D12Device* device, const wchar_t*) { return na::dlss::Attach(device, fake::MakeApi()); });

    // The smallest picture the flow engine accepts is 256 x 64.
    constexpr uint32_t W = 256, H = 128;
    constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R16G16B16A16_FLOAT, kDepthView = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    auto color = test::CreateTexture(dev, W, H, kColor, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto cb = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(color && depth && cb);
    test::ComPtr<ID3D12DescriptorHeap> rtvh, dsvh, cbvh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh))));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart(), dsv = dsvh->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color.Get(), nullptr, rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = kDepthView;
    dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(depth.Get(), &dvd, dsv);
    // Root signature: a root CBV (b0: the draw's own constants) and a table with one CBV (b1: the
    // camera block, bound the way j2_live_test binds it).
    test::ComPtr<ID3D12RootSignature> rs;
    {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, 0};
        D3D12_ROOT_PARAMETER p[2]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &range;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        test::ComPtr<ID3DBlob> blob, err;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
        REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC chd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&cbvh))));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cv{cb->GetGPUVirtualAddress() + 512, 256};
        dev->CreateConstantBufferView(&cv, cbvh->GetCPUDescriptorHandleForHeapStart());
    }
    // The scene's one pipeline: tests the depth with GREATER_EQUAL and writes it (a reversed-depth
    // game), and paints a blocky pattern that `shift` slides across the picture.
    test::ComPtr<ID3D12PipelineState> pso;
    {
        constexpr char code[] = R"(cbuffer C : register(b0) { float4 other[4]; float2 shift; };
float4 VS(uint i : SV_VertexID) : SV_Position { float2 uv = float2((i << 1) & 2, i & 2); return float4(uv.x * 2 - 1, 1 - uv.y * 2, .5, 1); }
float4 PS(float4 p : SV_Position) : SV_Target
{
    float2 q = floor((p.xy + shift) / 4);
    float n = frac(sin(dot(q, float2(12.9898, 78.233))) * 43758.5453);
    return float4(n, frac(n * 7), frac(n * 13), 1);
})";
        test::ComPtr<ID3DBlob> vs, ps, err;
        REQUIRE(SUCCEEDED(D3DCompile(code, sizeof code - 1, nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)));
        REQUIRE(SUCCEEDED(D3DCompile(code, sizeof code - 1, nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err)));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 15;
        pd.SampleMask = UINT_MAX;
        pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kColor;
        pd.DSVFormat = kDepthView;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))));
    }
    uint8_t* cpu = nullptr;
    REQUIRE(SUCCEEDED(cb->Map(0, nullptr, reinterpret_cast<void**>(&cpu))) && cpu); // persistent, like a game's upload ring
    const D3D12_GPU_VIRTUAL_ADDRESS va = cb->GetGPUVirtualAddress();

    test::List scene, consumer;
    REQUIRE(scene.Create(dev) && consumer.Create(dev));
    REQUIRE(SUCCEEDED(scene.list->Close()) && SUCCEEDED(consumer.list->Close()));
    bool camera_in_scene = true;
    uint8_t camera_bytes[64]{}; // the camera matrix as the game wrote it this frame
    int number = 0;
    // One game frame: Present, the scene list (clear, the draw), the list that reads the scene.
    const auto frame = [&](const char* what) {
        d.WaitIdle();
        if (number) na::j2::OnPresent(d.queue.Get());
        ++number;
        const double cam_x = 0.5 * number; // the camera moves every frame: it is never "still"
        std::memset(cpu, 0, 4096);
        if (camera_in_scene) { // camera block at 512: ViewProjection at +80, its inverse at +144
            const M vp = Camera(cam_x);
            M inv{};
            Inverse(vp, inv);
            Store(cpu + 512 + 80, vp);
            Store(cpu + 512 + 144, inv);
        }
        std::memcpy(camera_bytes, cpu + 512 + 80, 64);
        for (int i = 0; i < 64; ++i) cpu[1024 + i] = static_cast<uint8_t>(i * 7); // the draw's block: other constants...
        const float shift[2] = {2.0f * static_cast<float>(number), 0.0f};          // ...and where the pattern is this frame
        std::memcpy(cpu + 1024 + 64, shift, sizeof shift);
        scene.Reset();
        ID3D12GraphicsCommandList* l = scene.list.Get();
        D3D12_RESOURCE_BARRIER in[2] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                        test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE)};
        l->ResourceBarrier(2, in);
        l->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float clear[4] = {0.2f, 0.3f, 0.4f, 1};
        l->ClearRenderTargetView(rtv, clear, 0, nullptr);
        l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        l->SetGraphicsRootSignature(rs.Get());
        ID3D12DescriptorHeap* heaps[] = {cbvh.Get()};
        l->SetDescriptorHeaps(1, heaps);
        l->SetGraphicsRootConstantBufferView(0, va + 1024);
        l->SetGraphicsRootDescriptorTable(1, cbvh->GetGPUDescriptorHandleForHeapStart()); // camera (table CBV)
        l->SetPipelineState(pso.Get());
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(W), static_cast<LONG>(H)};
        l->RSSetViewports(1, &viewport);
        l->RSSetScissorRects(1, &scissor);
        l->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER out = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        l->ResourceBarrier(1, &out);
        REQUIRE(SUCCEEDED(l->Close()));
        consumer.Reset();
        D3D12_RESOURCE_BARRIER back[2] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                          test::Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON)};
        consumer.list->ResourceBarrier(2, back);
        REQUIRE(SUCCEEDED(consumer.list->Close()));
        ID3D12CommandList* lists[] = {scene.list.Get(), consumer.list.Get()};
        g_forwarded.clear();
        const int evals = fake::evals;
        na::j2::Execute(d.queue.Get(), 2, lists, &Forward);
        d.WaitIdle();
        const auto s = na::j2::GetStats();
        const bool ran = fake::evals > evals;
        std::printf("frame %2d %-22s forwarded %zu dlss %d reset %d jitter %+.3f %+.3f | camera %llu measured %llu (no pair %llu why %u, cut %llu, unavailable %llu, camera back %llu) held %llu waiting %llu | depth counting %u near %u verdict %u greater %u less %u clears %u unproven %llu | \"%s\"\n",
                    number, what, g_forwarded.size(), ran ? 1 : 0, ran ? fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) : -1,
                    ran ? fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X) : 0.0, ran ? fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_Y) : 0.0,
                    static_cast<unsigned long long>(s.supplied_camera), static_cast<unsigned long long>(s.supplied_measured),
                    static_cast<unsigned long long>(s.measured_no_pair), s.measured_last_no_pair, static_cast<unsigned long long>(s.measured_cut_resets),
                    static_cast<unsigned long long>(s.measured_unavailable), static_cast<unsigned long long>(s.cut_after_measured),
                    static_cast<unsigned long long>(s.camera_held), static_cast<unsigned long long>(s.no_camera),
                    s.depth_evidence ? 1u : 0u, s.depth_near, s.depth_verdict, s.depth_writes_greater, s.depth_writes_less, s.depth_clears,
                    static_cast<unsigned long long>(s.depth_unproven), s.reason);
        return 0;
    };
    const auto delivered = [&] { return g_forwarded.size() == 3 && g_forwarded[0] == scene.list.Get() && g_forwarded[2] == consumer.list.Get(); };
    const auto untouched = [&] { return g_forwarded.size() == 2 && g_forwarded[0] == scene.list.Get() && g_forwarded[1] == consumer.list.Get(); };
    const auto camera_as_written = [&] { return std::memcmp(cpu + 512 + 80, camera_bytes, 64) == 0; };

    na::j2::SetEnabled(true);
    na::settings::Get().j2_route.store(1); // the fixed between-list route, as in j2_live_test
    na::j2::SetJitter(true);
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()), na::observe::IdOf(color.Get()));

    // 1. The camera is never looked for.
    na::settings::Get().j2_camera.store(0);
    int evals = fake::evals;
    REQUIRE(frame("camera off: first") == 0);
    auto st = na::j2::GetStats();
    // Nothing was counted while this frame's draws were recorded: the direction is not guessed.
    CHECK(untouched() && fake::evals == evals);
    CHECK(st.depth_evidence && st.depth_near == 0 && st.depth_unproven == 1);
    CHECK(std::strstr(st.reason, "not proven") != nullptr);
    REQUIRE(frame("camera off: second") == 0);
    st = na::j2::GetStats();
    // Proven from the scene's own writes: GREATER_EQUAL + write, cleared to 0 = larger is nearer.
    CHECK(st.depth_near == 2 && st.depth_verdict == 0 && st.depth_writes_greater >= 1 && st.depth_writes_less == 0 && st.depth_clears >= 1);
    CHECK(st.depth_unproven == 1);
    const bool can_measure = st.measured_unavailable == 0;
    {
        const auto fs = na::j2::compute_flow::GetStats();
        std::printf("measured motion on this build and device: %s | measuring pass: pictures %llu pairs %llu unavailable %llu \"%s\"\n",
                    can_measure ? "available" : "NOT available (frames without a camera are left as drawn)",
                    static_cast<unsigned long long>(fs.total_frames), static_cast<unsigned long long>(fs.total_pairs),
                    static_cast<unsigned long long>(fs.unavailable), fs.reason);
    }
    if (can_measure) {
        // The first picture has nothing to be measured against: DLSS starts its history on it.
        CHECK(delivered() && fake::evals == evals + 1);
        CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1);
        CHECK((fake::I(fake::last_create, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0);
        CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_MotionVectors) != nullptr);
        CHECK(st.supplied_measured == 1);
    } else {
        CHECK(untouched() && fake::evals == evals && st.measured_unavailable == 1);
        CHECK(std::strstr(st.reason, "could not be measured") != nullptr);
    }
    CHECK(camera_as_written());
    int kept = 0, sampled = 0;
    for (int i = 0; i < 6; ++i) {
        evals = fake::evals;
        REQUIRE(frame("camera off") == 0);
        CHECK(camera_as_written());
        if (!can_measure) { CHECK(untouched() && fake::evals == evals); continue; }
        CHECK(delivered() && fake::evals == evals + 1);
        if (fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0) ++kept;
        const double jx = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X), jy = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_Y);
        if (jx != 0.0 || jy != 0.0) { ++sampled; CHECK(std::fabs(jx) < 0.5 && std::fabs(jy) < 0.5); }
    }
    st = na::j2::GetStats();
    // Whichever way the frames went, no camera was involved: none supplied motion, none was waited
    // for, no matrix of the game's was replaced, and the depth direction was never taken back.
    CHECK(st.supplied_camera == 0 && st.no_camera == 0 && st.jitter_writes == 0 && st.camera_held == 0);
    CHECK(st.depth_near == 2 && st.depth_unproven == 1 && st.depth_flips == 0);
    if (can_measure) {
        CHECK(st.supplied_measured == 7);
        std::printf("camera off: %d of 6 later frames carried the DLSS history on, %d had a viewport sample\n", kept, sampled);
        CHECK(kept >= 1);    // measured pairs keep the history: not a reset every frame
        CHECK(sampled >= 1); // the raster sample reaches the picture and DLSS without a camera
    } else {
        CHECK(st.supplied_measured == 0 && st.measured_unavailable == 7);
    }

    // 2. The camera may be used again, and it is there: it takes over.
    na::settings::Get().j2_camera.store(1);
    const uint64_t measured_before = st.supplied_measured;
    for (int i = 0; i < 4; ++i) {
        evals = fake::evals;
        REQUIRE(frame("camera used when found") == 0);
        CHECK(delivered() && fake::evals == evals + 1);
    }
    st = na::j2::GetStats();
    CHECK(st.camera_offset == 80);
    CHECK(st.supplied_camera >= 3 && st.supplied_camera + (st.supplied_measured - measured_before) == 4);
    // With measured frames before it, the frame the camera comes back on is measured too (its
    // tracker has no previous camera yet); without, the camera starts as it always did.
    CHECK(st.supplied_measured - measured_before == (can_measure ? 1u : 0u));
    CHECK(st.cut_after_measured == (can_measure ? 1u : 0u));
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0); // by the fourth frame the history carries on
    CHECK(st.depth_camera_disagrees == 0);                            // the camera and the depth writes say the same
    CHECK(st.no_camera == 0);

    // 3. The camera disappears from the scene while it may be used: delivery goes on.
    camera_in_scene = false;
    const uint64_t camera_before = st.supplied_camera, measured_mid = st.supplied_measured, unavailable_mid = st.measured_unavailable;
    int ran = 0;
    for (int i = 0; i < 10; ++i) {
        evals = fake::evals;
        REQUIRE(frame("camera gone from scene") == 0);
        if (fake::evals == evals + 1 && delivered()) ++ran;
        else CHECK(untouched() && fake::evals == evals);
    }
    st = na::j2::GetStats();
    std::printf("camera gone: %d of 10 frames delivered (%llu with the held camera, %llu measured, %llu could not be measured)\n", ran,
                static_cast<unsigned long long>(st.supplied_camera - camera_before), static_cast<unsigned long long>(st.supplied_measured - measured_mid),
                static_cast<unsigned long long>(st.measured_unavailable - unavailable_mid));
    CHECK(st.no_camera == 0);     // no frame waited for the camera
    CHECK(st.camera_held >= 1);   // held for a few frames first, as before
    CHECK(st.supplied_camera - camera_before >= 1 && st.supplied_camera - camera_before <= 4);
    if (can_measure) {
        CHECK(ran == 10);
        CHECK(st.supplied_measured - measured_mid >= 6);
        CHECK(st.measured_unavailable == unavailable_mid);
        // With no matrix to carry the raster sample, the viewports carry it from the second such frame.
        CHECK(fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X) != 0.0 || fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_Y) != 0.0);
    } else {
        CHECK(st.supplied_measured == measured_mid && st.measured_unavailable - unavailable_mid >= 6);
    }
    CHECK(st.depth_near == 2); // still the proven direction, kept through the frames that had a camera

    // 4. A camera is required (the behaviour before 2026-11-16): without one the frame waits.
    na::settings::Get().j2_camera.store(2);
    evals = fake::evals;
    REQUIRE(frame("camera required, none") == 0);
    st = na::j2::GetStats();
    CHECK(untouched() && fake::evals == evals);
    CHECK(st.no_camera == 1 && std::strncmp(st.reason, "Waiting for the camera", 22) == 0);
    CHECK(!st.depth_evidence && st.depth_near == 0); // nothing is counted while the camera's answer is the only one used
    camera_in_scene = true;
    const uint64_t camera_last = st.supplied_camera, measured_last = st.supplied_measured;
    for (int i = 0; i < 3; ++i) {
        evals = fake::evals;
        REQUIRE(frame("camera required, there") == 0);
        CHECK(delivered() && fake::evals == evals + 1);
    }
    st = na::j2::GetStats();
    CHECK(st.supplied_camera == camera_last + 3 && st.supplied_measured == measured_last);
    CHECK(st.no_camera == 1 && !st.depth_evidence);
    CHECK(std::strncmp(st.reason, "Stopped", 7) != 0);

    na::settings::Get().j2_camera.store(1);
    {
        const auto fs = na::j2::compute_flow::GetStats();
        std::printf("measuring pass at the end: pictures %llu pairs %llu scene cuts %llu unavailable %llu no pair by reason 1..7 = %llu %llu %llu %llu %llu %llu %llu \"%s\"\n",
                    static_cast<unsigned long long>(fs.total_frames), static_cast<unsigned long long>(fs.total_pairs), static_cast<unsigned long long>(fs.total_cuts),
                    static_cast<unsigned long long>(fs.unavailable), static_cast<unsigned long long>(fs.no_pair[1]), static_cast<unsigned long long>(fs.no_pair[2]),
                    static_cast<unsigned long long>(fs.no_pair[3]), static_cast<unsigned long long>(fs.no_pair[4]), static_cast<unsigned long long>(fs.no_pair[5]),
                    static_cast<unsigned long long>(fs.no_pair[6]), static_cast<unsigned long long>(fs.no_pair[7]), fs.reason);
    }
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    na::j2::SetEnabled(false);
    d.WaitIdle();
    na::j2::Shutdown();
    na::dlss::Shutdown(); // GPU idle (WaitIdle above): release the fake runtime's features before exit
    na::dlss::SetInitOwnOverrideForTests(nullptr);
    na_observe_stop(); // the transport thread must not outlive main
    na::observe::SetProcessExiting(); // same exit contract as the host before static GPU-owner teardown
    return test::finish("j2_no_camera_test");
}
