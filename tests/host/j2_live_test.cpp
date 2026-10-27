// J2 live DLAA end to end against a fake NGX runtime on a real D3D12 device
// (WARP-capable): discovery picks -> scene list detection -> camera from the
// scene list's root constant buffer (ViewProjection at +80, inverse at +144,
// as in Anno 1800) -> camera motion -> DLSS evaluate -> result copied back.
// Checks: one NeuralAlias list inserted right after the scene list; DLSS gets
// the scene colour, an R32F depth copy, motion vectors and a separate output;
// reverse-Z detected; first frame resets, the next does not; the camera offset
// is learned; no selection leaves the batch untouched.
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
ID3D12CommandQueue* g_queue = nullptr;
void __stdcall Forward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    g_forwarded.assign(lists, lists + n);
    q->ExecuteCommandLists(n, lists);
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

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0); // progress survives a timeout kill
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_live_test");
    ID3D12Device* dev = d.device.Get();
    g_queue = d.queue.Get();
    std::printf("step: install producers\n");
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
    na::dlss::SetInitOwnOverrideForTests([](ID3D12Device* device, const wchar_t*) { return na::dlss::Attach(device, fake::MakeApi()); });

    constexpr uint32_t W = 64, H = 32;
    auto color = test::CreateTexture(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto cb = test::CreateBuffer(dev, 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    REQUIRE(color && depth && cb);
    test::ComPtr<ID3D12DescriptorHeap> rtvh, dsvh;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh))));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh))));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart(), dsv = dsvh->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color.Get(), nullptr, rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(depth.Get(), &dvd, dsv);
    // Root signature: a root CBV (b0, junk constants) and a descriptor table with
    // one CBV (b1): Anno binds its camera through a table, so the test does too.
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
    test::ComPtr<ID3D12DescriptorHeap> cbvh;
    {
        D3D12_DESCRIPTOR_HEAP_DESC chd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&cbvh))));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cv{cb->GetGPUVirtualAddress() + 512, 256};
        dev->CreateConstantBufferView(&cv, cbvh->GetCPUDescriptorHandleForHeapStart());
    }
    uint8_t* cpu = nullptr;
    REQUIRE(SUCCEEDED(cb->Map(0, nullptr, reinterpret_cast<void**>(&cpu))) && cpu); // persistent, like a game's upload ring
    const D3D12_GPU_VIRTUAL_ADDRESS va = cb->GetGPUVirtualAddress();

    test::List scene, consumer;
    REQUIRE(scene.Create(dev) && consumer.Create(dev));
    REQUIRE(SUCCEEDED(scene.list->Close()) && SUCCEEDED(consumer.list->Close()));
    const auto frame = [&](double cam_x) {
        d.WaitIdle();
        // Camera block at 512 (VP at +80, inverse at +144), a junk block at 1024.
        std::memset(cpu, 0, 4096);
        const M vp = Camera(cam_x);
        M inv{};
        Inverse(vp, inv);
        Store(cpu + 512 + 80, vp);
        Store(cpu + 512 + 144, inv);
        for (int i = 0; i < 64; ++i) cpu[1024 + i] = static_cast<uint8_t>(i * 7);
        scene.Reset();
        D3D12_RESOURCE_BARRIER in[2] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                        test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE)};
        scene.list->ResourceBarrier(2, in);
        scene.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        const float clear[4] = {0.2f, 0.3f, 0.4f, 1};
        scene.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        scene.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        scene.list->SetGraphicsRootSignature(rs.Get());
        ID3D12DescriptorHeap* heaps[] = {cbvh.Get()};
        scene.list->SetDescriptorHeaps(1, heaps);
        for (int i = 0; i < 3; ++i) scene.list->SetGraphicsRootConstantBufferView(0, va + 1024); // junk: most bound
        scene.list->SetGraphicsRootDescriptorTable(1, cbvh->GetGPUDescriptorHandleForHeapStart()); // camera (table CBV)
        D3D12_RESOURCE_BARRIER out = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        scene.list->ResourceBarrier(1, &out);
        REQUIRE(SUCCEEDED(scene.list->Close()));
        consumer.Reset();
        D3D12_RESOURCE_BARRIER back[2] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                          test::Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COMMON)};
        consumer.list->ResourceBarrier(2, back);
        REQUIRE(SUCCEEDED(consumer.list->Close()));
        ID3D12CommandList* lists[] = {scene.list.Get(), consumer.list.Get()};
        g_forwarded.clear();
        std::printf("step: execute (camera x %.1f)\n", cam_x);
        na::j2::Execute(d.queue.Get(), 2, lists, &Forward);
        const auto js = na::j2::GetStats();
        const auto ds = na::dlss::GetStats();
        std::printf("step: forwarded %zu | j2 frames %llu sel %llu scene %llu state %llu cam %llu busy %llu dlss %llu \"%s\" | dlss evals %llu rejected %llu \"%s\"\n",
                    g_forwarded.size(), (unsigned long long)js.frames, (unsigned long long)js.no_selection, (unsigned long long)js.no_scene_list,
                    (unsigned long long)js.no_state, (unsigned long long)js.no_camera, (unsigned long long)js.busy, (unsigned long long)js.dlss_failed,
                    js.reason, (unsigned long long)ds.evaluates, (unsigned long long)ds.rejected, ds.reason);
        d.WaitIdle();
        return 0;
    };

    na::j2::SetEnabled(true);
    na::j2::SetJitter(true); // J1 is off by default in the game; the tests exercise it
    // No discovery picks yet: untouched.
    na::observe::SetLiveSelection(0, 0);
    frame(0);
    CHECK(g_forwarded.size() == 2);
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()), na::observe::IdOf(color.Get()));
    const int evals0 = fake::evals;
    frame(0);
    auto st = na::j2::GetStats();
    std::printf("frame 1: forwarded %zu, frames %llu, camera offset %llu, reason \"%s\"\n", g_forwarded.size(),
                static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.camera_offset), st.reason);
    CHECK(g_forwarded.size() == 3 && g_forwarded[0] == scene.list.Get() && g_forwarded[2] == consumer.list.Get());
    CHECK(fake::evals == evals0 + 1);
    // DLSS reads J2's plain copy of the scene colour (slice 0 / mip 0) and writes a separate output.
    void* in_color = fake::P(fake::last_eval, NVSDK_NGX_Parameter_Color);
    void* out = fake::P(fake::last_eval, NVSDK_NGX_Parameter_Output);
    CHECK(in_color && in_color != color.Get());
    CHECK(out && out != color.Get() && out != in_color);
    CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_Depth) && fake::P(fake::last_eval, NVSDK_NGX_Parameter_Depth) != depth.Get()); // R32F copy
    CHECK(fake::P(fake::last_eval, NVSDK_NGX_Parameter_MotionVectors) != nullptr);
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1);
    CHECK((fake::I(fake::last_create, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0);
    CHECK((fake::I(fake::last_create, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags) & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0);
    CHECK(st.frames == 1 && st.camera_offset == 80 && st.later_passes == 0);

    // A second matching batch in the same game frame is not another DLAA frame.
    // Nor does an unrelated/unknown queue's presentation advance this view.
    na::j2::OnPresent(nullptr);
    frame(0.25);
    CHECK(g_forwarded.size() == 2 && fake::evals == evals0 + 1);
    CHECK(na::j2::GetStats().frames == 1 && na::j2::GetStats().duplicate_submissions == 1);

    na::j2::OnPresent(d.queue.Get());
    const auto cuts_before_failure = na::j2::GetStats().cuts;
    fake::eval_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
    frame(500); // would be a camera cut, but this DLSS attempt never submits
    CHECK(g_forwarded.size() == 2);
    CHECK(na::j2::GetStats().frames == 1 && na::j2::GetStats().cuts == cuts_before_failure);
    // Recover the deliberately failed fake backend without resetting J2 history.
    na::dlss::Shutdown();
    fake::eval_result = NVSDK_NGX_Result_Success;
    REQUIRE(na::dlss::Attach(dev, fake::MakeApi()));
    frame(0.5); // camera moved a little: same history
    st = na::j2::GetStats();
    std::printf("frame 2: forwarded %zu, frames %llu, cuts %llu, reset %d\n", g_forwarded.size(), static_cast<unsigned long long>(st.frames),
                static_cast<unsigned long long>(st.cuts), fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset));
    CHECK(g_forwarded.size() == 3 && st.frames == 2 && fake::evals == evals0 + 3);
    CHECK(st.cuts == cuts_before_failure); // the failed camera x=500 did not enter history
    // The recreated backend resets its own feature; the next normal frame does not.
    na::j2::OnPresent(d.queue.Get());
    frame(1.0);
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
    // J1: this frame ran jittered (the previous one produced DLAA). DLSS receives a
    // Halton sample inside the pixel, and the game's camera block holds the jittered copy;
    // no history reset above means the tracker removed exactly that sample.
    st = na::j2::GetStats();
    const double jx = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X), jy = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_Y);
    std::printf("jittered frame: %.4f %.4f, camera matrices replaced %llu\n", jx, jy, static_cast<unsigned long long>(st.jitter_writes));
    CHECK((jx != 0.0 || jy != 0.0) && std::fabs(jx) < 0.5 && std::fabs(jy) < 0.5);
    CHECK(st.jitter_writes >= 2); // ViewProjection and its inverse
    {
        uint8_t orig[64];
        Store(orig, Camera(1.0));
        CHECK(std::memcmp(cpu + 512 + 80, orig, 64) != 0);
    }
    na::j2::OnPresent(d.queue.Get());
    frame(500); // teleport: history reset
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1);
    na::j2::OnPresent(d.queue.Get());
    na::observe::SetLiveSelection(0, 0);
    frame(500.5); // an unprocessed game frame must not advance camera history
    const auto before_gap = na::j2::GetStats().frames;
    na::j2::OnPresent(d.queue.Get());
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()), na::observe::IdOf(color.Get()));
    frame(501);
    CHECK(na::j2::GetStats().frames == before_gap + 1);
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1);
    // Debug views (motion / fine motion / depth) record and close cleanly.
    for (int dv = 1; dv <= 3; ++dv) {
        na::j2::SetDebugView(dv);
        na::j2::OnPresent(d.queue.Get());
        frame(501.0 + dv * 0.1);
        const auto ds = na::j2::GetStats();
        std::printf("debug view %d: \"%s\"\n", dv, ds.reason);
        CHECK(std::strncmp(ds.reason, "Stopped", 7) != 0);
    }
    na::j2::SetDebugView(0);
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    na::j2::SetEnabled(false);
    d.WaitIdle();
    na::j2::Shutdown();
    na::dlss::Shutdown(); // GPU idle (WaitIdle above): release the fake runtime's features before exit
    na::dlss::SetInitOwnOverrideForTests(nullptr);
    na_observe_stop(); // the transport thread must not outlive main
    return test::finish("j2_live_test");
}
