// J2 on Anno's shape (the run that crashed, 2026-10-26): the path the game takes,
// not the convenient one.
//   - submissions go through ExecuteBegin/End around J2, as the overlay does;
//   - the scene colour is a 2-slice ARRAY with a MIP CHAIN (RGBA16F, 4 mips), the
//     scene drawn into slice 1 while slice 0 holds something else and the scene list never
//     transitions colour or depth: their states are carried from an earlier
//     submission (the known-state store);
//   - the camera sits in a 768-byte DESCRIPTOR-TABLE CBV at +336, its inverse NOT
//     adjacent (+464: Anno run 7 found the camera but J1 never jittered, because
//     the inverse was assumed to sit 64 bytes away);
//   - a depth pre-pass in an EARLIER batch, with its own root signature (a root
//     CBV), binds its own copy of the camera: J1 must jitter both identically.
// The DLAA list must close (no latch), DLSS gets the frame, the result lands in
// mip 0, J1 jitters the next frame, and the device survives.
#include "test_support.hpp"
#include "fake_ngx_runtime.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/producers.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
void __cdecl Sink(const na_event_batch*, void*) {}
std::vector<ID3D12CommandList*> g_forwarded;
void __stdcall Forward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    g_forwarded.assign(lists, lists + n);
    q->ExecuteCommandLists(n, lists); // queue not hooked in this test: the plain runtime call
}
// The overlay's sequence around a game submission.
void Submit(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, bool j2)
{
    const na::observe::ExecuteToken t = na::observe::ExecuteBegin(q, n, lists);
    if (j2) na::j2::Execute(q, n, lists, &Forward);
    else q->ExecuteCommandLists(n, lists);
    na::observe::ExecuteEnd(t, q, n, lists);
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
// Reverse-Z perspective (infinite-ish far) times a rotated, translated view.
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

int main(int argc, char**)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool msaa = argc > 1; // CTest runs the same submission path with AA Off and x2.
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_anno_shape_test");
    ID3D12Device* dev = d.device.Get();
    ID3D12CommandQueue* q = d.queue.Get();
    REQUIRE(na::observe::InstallProducers(dev, q, /*hook_queue_execute=*/false));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
    na::dlss::SetInitOwnOverrideForTests([](ID3D12Device* device, const wchar_t*) { return na::dlss::Attach(device, fake::MakeApi()); });

    constexpr uint32_t W = 64, H = 32;
    test::ComPtr<ID3D12Resource> color;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = W;
        td.Height = H;
        td.DepthOrArraySize = 2; // an array (Anno run 6: "multisampled or an array")
        td.MipLevels = msaa ? 1 : 4;        // and a mip chain (Anno run 4: the rejected list)
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = msaa ? 2 : 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color))));
    }
    auto depth = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if (msaa) {
        depth.Reset();
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto dd = color->GetDesc();
        dd.DepthOrArraySize = 1; dd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&depth))));
    }
    auto cb = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(depth && cb);
    test::ComPtr<ID3D12DescriptorHeap> rtvh, dsvh, cbvh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh))));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh))));
    D3D12_DESCRIPTOR_HEAP_DESC chd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&cbvh))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart(), dsv = dsvh->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rvd{};
    rvd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rvd.Texture2DArray.ArraySize = 1;
    rvd.Texture2DArray.FirstArraySlice = 1; // the scene is drawn into SLICE 1 (mip 0): subresource 4
    if (msaa) {
        rvd = {}; rvd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        rvd.Texture2DMSArray.ArraySize = 1; rvd.Texture2DMSArray.FirstArraySlice = 1;
    }
    dev->CreateRenderTargetView(color.Get(), &rvd, rtv);
    // Slice 0 holds something else (red) that DLAA must never touch.
    test::ComPtr<ID3D12DescriptorHeap> rtvh0;
    D3D12_DESCRIPTOR_HEAP_DESC hd0{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd0, IID_PPV_ARGS(&rtvh0))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtvh0->GetCPUDescriptorHandleForHeapStart();
    {
        D3D12_RENDER_TARGET_VIEW_DESC v0 = rvd;
        if (msaa) v0.Texture2DMSArray.FirstArraySlice = 0; else v0.Texture2DArray.FirstArraySlice = 0;
        dev->CreateRenderTargetView(color.Get(), &v0, rtv0);
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dvd.ViewDimension = msaa ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(depth.Get(), &dvd, dsv);
    constexpr uint32_t kBlock = 1024, kVp = 336;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv{cb->GetGPUVirtualAddress() + kBlock, 768};
    dev->CreateConstantBufferView(&cv, cbvh->GetCPUDescriptorHandleForHeapStart());
    test::ComPtr<ID3D12RootSignature> rs;
    {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 1;
        p.DescriptorTable.pDescriptorRanges = &range;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{1, &p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        test::ComPtr<ID3DBlob> blob, err;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
        REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))));
    }
    test::ComPtr<ID3D12RootSignature> rs_pre; // the pre-pass's own root signature: one root CBV
    {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd{1, &p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        test::ComPtr<ID3DBlob> blob, err;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
        REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_pre))));
    }
    constexpr uint32_t kPre = 2048;
    uint8_t* cpu = nullptr;
    REQUIRE(SUCCEEDED(cb->Map(0, nullptr, reinterpret_cast<void**>(&cpu))) && cpu);

    // An earlier submission leaves colour in RENDER_TARGET and depth in DEPTH_WRITE.
    test::List setup, scene, consumer, prepass;
    REQUIRE(setup.Create(dev) && scene.Create(dev) && consumer.Create(dev) && prepass.Create(dev));
    REQUIRE(SUCCEEDED(setup.list->Close()));
    {
        setup.Reset(); // recorded after a Reset, like a game's reused list
        D3D12_RESOURCE_BARRIER in[2] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                        test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE)};
        setup.list->ResourceBarrier(2, in);
        const float red[4] = {1, 0, 0, 1};
        setup.list->ClearRenderTargetView(rtv0, red, 0, nullptr);
        REQUIRE(SUCCEEDED(setup.list->Close()));
        ID3D12CommandList* l[] = {setup.list.Get()};
        Submit(q, 1, l, false);
        d.WaitIdle();
    }
    REQUIRE(SUCCEEDED(scene.list->Close()) && SUCCEEDED(consumer.list->Close()) && SUCCEEDED(prepass.list->Close()));
    na::j2::SetEnabled(true);
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()), na::observe::IdOf(color.Get()));
    const auto frame = [&](double cam_x) {
        d.WaitIdle();
        std::memset(cpu, 0, 4096);
        const M vp = Camera(cam_x);
        M inv{};
        Inverse(vp, inv);
        for (int i = 0; i < 300; ++i) cpu[kBlock + i] = static_cast<uint8_t>(i * 13); // other constants before the camera
        Store(cpu + kBlock + kVp, vp);
        Store(cpu + kBlock + kVp + 128, inv);
        Store(cpu + kPre + kVp, vp); // the pre-pass's own copy of the camera
        Store(cpu + kPre + kVp + 128, inv);
        prepass.Reset(); // earlier batch: depth only, its own root signature
        prepass.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        prepass.list->SetGraphicsRootSignature(rs_pre.Get());
        prepass.list->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress() + kPre);
        REQUIRE(SUCCEEDED(prepass.list->Close()));
        {
            ID3D12CommandList* pl[] = {prepass.list.Get()};
            Submit(q, 1, pl, true);
        }
        scene.Reset(); // no colour/depth transitions in the scene list: carried state
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float clear[4] = {0.2f, 0.3f, 0.4f, 1};
        scene.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        scene.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        ID3D12DescriptorHeap* heaps[] = {cbvh.Get()};
        scene.list->SetDescriptorHeaps(1, heaps);
        scene.list->SetGraphicsRootSignature(rs.Get());
        for (int i = 0; i < 3; ++i) scene.list->SetGraphicsRootDescriptorTable(0, cbvh->GetGPUDescriptorHandleForHeapStart());
        REQUIRE(SUCCEEDED(scene.list->Close()));
        consumer.Reset(); // a post pass reads the colour, then hands it back as a render target
        D3D12_RESOURCE_BARRIER rd = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        consumer.list->ResourceBarrier(1, &rd);
        D3D12_RESOURCE_BARRIER back = test::Transition(color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        consumer.list->ResourceBarrier(1, &back);
        REQUIRE(SUCCEEDED(consumer.list->Close()));
        ID3D12CommandList* lists[] = {scene.list.Get(), consumer.list.Get()};
        g_forwarded.clear();
        Submit(q, 2, lists, true);
        na::j2::OnPresent(q);
        d.WaitIdle();
        const auto ps = na::observe::GetProducerStats();
        std::printf("producers: known disabled %u unknown_inflight %u entries %llu unhooked_queues %llu clears %llu sample_known %llu\n",
                    ps.known_store_disabled, ps.known_unknown_inflight, (unsigned long long)ps.known_entries, (unsigned long long)ps.unhooked_queues,
                    (unsigned long long)ps.known_state_clears, (unsigned long long)ps.sample_known_state);
        const auto js = na::j2::GetStats();
        std::printf("frame x=%.1f: forwarded %zu, frames %llu, state %llu, camera %llu, offset %llu, jitter writes %llu, \"%s\"\n", cam_x,
                    g_forwarded.size(), (unsigned long long)js.frames, (unsigned long long)js.no_state, (unsigned long long)js.no_camera,
                    (unsigned long long)js.camera_offset, (unsigned long long)js.jitter_writes, js.reason);
        return 0;
    };
    const int evals0 = fake::evals;
    REQUIRE(frame(0) == 0);
    auto st = na::j2::GetStats();
    CHECK(g_forwarded.size() == 3 && st.frames == 1 && fake::evals == evals0 + 1);
    CHECK(st.camera_offset == kVp);
    CHECK(std::strncmp(st.reason, "Stopped", 7) != 0); // the list closed: no latch
    REQUIRE(frame(0.5) == 0);
    st = na::j2::GetStats();
    REQUIRE(st.frames == 2 && fake::evals == evals0 + 2);
    for (const char* key : {NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth}) {
        auto* input = static_cast<ID3D12Resource*>(fake::P(fake::last_eval, key));
        REQUIRE(input);
        const auto input_desc = input->GetDesc();
        CHECK(input_desc.SampleDesc.Count == 1 && input_desc.MipLevels == 1 && input_desc.DepthOrArraySize == 1);
        CHECK(input_desc.Width == W && input_desc.Height == H);
    }
    CHECK(st.jitter_writes >= 2);
    CHECK(st.camera_inv_delta == 128 && st.camera_learned_hits >= 1);
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
    const double jx = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X), jy = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_Y);
    CHECK((jx != 0.0 || jy != 0.0) && std::fabs(jx) < 0.5 && std::fabs(jy) < 0.5);
    {
        uint8_t orig[64];
        Store(orig, Camera(0.5));
        CHECK(std::memcmp(cpu + kPre + kVp, orig, 64) != 0);                // the earlier pre-pass batch was jittered...
        CHECK(std::memcmp(cpu + kPre + kVp, cpu + kBlock + kVp, 64) == 0); // ...exactly like the scene's camera
    }
    // Content: the result went into slice 1 (the fake DLSS leaves its output zero),
    // and slice 0 kept its red.
    {
        d.WaitIdle();
        test::ComPtr<ID3D12Resource> readable = color;
        const UINT scene_sub = msaa ? 1u : 4u;
        if (msaa) {
            auto desc = color->GetDesc(); desc.SampleDesc.Count = 1; desc.Alignment = 0;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            readable.Reset();
            REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&readable))));
        }
        const D3D12_RESOURCE_DESC cdsc = readable->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[5]{};
        UINT64 total = 0;
        dev->GetCopyableFootprints(&cdsc, 0, scene_sub + 1, 0, fp, nullptr, nullptr, &total);
        auto rb = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        REQUIRE(rb);
        test::List c;
        REQUIRE(c.Create(dev));
        if (msaa) {
            D3D12_RESOURCE_BARRIER b[] = {
                test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
                test::Transition(readable.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST)};
            c.list->ResourceBarrier(2, b);
            for (UINT sub : {0u, 1u}) c.list->ResolveSubresource(readable.Get(), sub, color.Get(), sub, cdsc.Format);
            for (auto& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
            c.list->ResourceBarrier(2, b);
        }
        D3D12_RESOURCE_BARRIER tb = test::Transition(readable.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        c.list->ResourceBarrier(1, &tb);
        for (UINT sub : {0u, scene_sub}) {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = readable.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = sub;
            dst.pResource = rb.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp[sub];
            c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        tb = test::Transition(readable.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        c.list->ResourceBarrier(1, &tb);
        REQUIRE(SUCCEEDED(c.list->Close()));
        ID3D12CommandList* cl[] = {c.list.Get()};
        q->ExecuteCommandLists(1, cl);
        d.WaitIdle();
        uint8_t* p = nullptr;
        REQUIRE(SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p))));
        uint16_t s0[4], s1[4];
        std::memcpy(s0, p + fp[0].Offset + fp[0].Footprint.RowPitch * 5 + 8 * 7, 8);
        std::memcpy(s1, p + fp[scene_sub].Offset + fp[scene_sub].Footprint.RowPitch * 5 + 8 * 7, 8);
        rb->Unmap(0, nullptr);
        std::printf("slice 0 pixel %04X %04X %04X %04X, slice 1 pixel %04X %04X %04X %04X\n", s0[0], s0[1], s0[2], s0[3], s1[0], s1[1], s1[2], s1[3]);
        CHECK(s0[0] == 0x3C00 && s0[1] == 0 && s0[2] == 0); // red (1.0 in half float) untouched
        CHECK(s1[0] == 0 && s1[1] == 0 && s1[2] == 0);      // the scene's clear (0.2,0.3,0.4) was replaced by the DLAA result
    }
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    na::j2::SetEnabled(false);
    d.WaitIdle();
    na::j2::Shutdown();
    na::dlss::Shutdown();
    na::dlss::SetInitOwnOverrideForTests(nullptr);
    na_observe_stop();
    return test::finish("j2_anno_shape_test");
}
