#include "host_dx12/settings.hpp"
#include "host_dx12/gpu/services.hpp"
// J2 live DLAA end to end against a fake NGX runtime on a real D3D12 device
// (WARP-capable): discovery picks -> scene list detection -> camera from the
// scene list's root constant buffer (ViewProjection at +80, inverse at +144,
// as in Anno 1800) -> camera motion -> DLSS evaluate -> result copied back.
// Checks: one NeuralAlias list inserted right after the scene list; DLSS gets
// the scene colour, an R32F depth copy, motion vectors and a separate output;
// reverse-Z detected; first frame resets, the next does not; the camera offset
// is learned; no selection leaves the batch untouched.
#include "test_support.hpp"
#include <d3dcompiler.h>
#include "fake_ngx_runtime.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/host_observe.h"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/hud_picture.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/native_present/size_spoof.hpp"

#include <array>
#include <memory>
#include <cmath>
#include <cstring>
#include <vector>
#include <functional>
#include <thread>

namespace {
void __cdecl Sink(const na_event_batch*, void*) {}

// Exercise a concurrent game Close between reconstruction's Before and After.
// Test-only CPU synchronization; no GPU wait or runtime scheduling seam added.
class CloseDuringDispatch final : public na::observe::ShaderOperationHook {
public:
    ID3D12GraphicsCommandList* target=nullptr;
    uint64_t pipeline=0;
    std::function<void()> close;
    unsigned closed=0;
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* l,na::observe::ShaderCall& call) noexcept override {
        if(l==target && call.kind==na::observe::ShaderKind::Dispatch &&
           na::observe::ListPipelineId(l)==pipeline && close){
            auto fn=std::exchange(close,{});
            std::thread worker(std::move(fn));worker.join();++closed;
        }
        return {};
    }
    void After(ID3D12GraphicsCommandList*,const na::observe::ShaderCall&,na::observe::ShaderScope&) noexcept override {}
};

std::vector<ID3D12CommandList*> g_forwarded;
ID3D12CommandQueue* g_queue = nullptr;
void __stdcall RawForward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists){q->ExecuteCommandLists(n,lists);}
// SE5 runs (100 frames, 30+ inline deliveries): also the overlay's DLSS completion
// bookkeeping around game submissions (overlay/submission_tracking.hpp), without
// which inline evaluations are never seen submitted and tracking fills up.
bool g_track_dlss = false;
void __stdcall Forward(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    g_forwarded.assign(lists, lists + n);
    if (g_track_dlss) na::dlss::BeforeExecute(q, n, lists);
    // Same mandatory forward seam as overlay::ForwardThroughNr, including when
    // NR is disabled. It publishes real GPU completion for recording leases.
    na::nr::Execute(q,n,lists,&RawForward);
    if (g_track_dlss) na::dlss::OnExecuted(q, n, lists);
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
// A planar reflection's camera (water, mirror): the main camera mirrored in y = 0
// (view-projection times the reflection diag(1, -1, 1, 1)); opposite winding.
M Reflection(double x)
{
    const M r{1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return Mul(Camera(x), r);
}
void Store(uint8_t* dst, const M& m)
{
    for (int i = 0; i < 16; ++i) { const float f = static_cast<float>(m[i]); std::memcpy(dst + i * 4, &f, 4); }
}
float Half(uint16_t h)
{
    const uint32_t e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return (h & 0x8000) ? -v : v;
}
// RGBA16F or RGBA8 texel (x, y) of `tex` (in COMMON); the GPU is idle afterwards.
std::array<float, 4> ReadTexel(test::Device& d, ID3D12Resource* tex, uint32_t x, uint32_t y)
{
    const na::observe::HostWorkScope diagnostic; // private test copy restores COMMON; not game state loss
    ID3D12Device* dev = d.device.Get();
    const D3D12_RESOURCE_DESC td = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    test::List l;
    std::array<float, 4> out{-1, -1, -1, -1};
    if (!rb || !l.Create(dev)) return out;
    D3D12_RESOURCE_BARRIER b = test::Transition(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    l.list->ResourceBarrier(1, &b);
    l.list->Close();
    ID3D12CommandList* ls[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, ls);
    d.WaitIdle();
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p))) || !p) return out;
    const auto* row=p+fp.Offset+static_cast<size_t>(y)*fp.Footprint.RowPitch;
    if(td.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || td.Format==DXGI_FORMAT_R8G8B8A8_UNORM) {
        for (unsigned i=0;i<4;++i) out[i]=row[x*4+i]/255.f;
    } else {
        const auto* t=reinterpret_cast<const uint16_t*>(row)+x*4;
        for (unsigned i=0;i<4;++i) out[i]=Half(t[i]);
    }
    rb->Unmap(0, nullptr);
    return out;
}
// Sol 6a48746: the fake evaluation writes a recognisable, frame-varying colour
// into its actual NGX output (in UNORDERED_ACCESS during evaluation), so J2's
// normal write-back has to carry it to the game's image.
ID3D12Device* g_pattern_dev = nullptr;
test::ComPtr<ID3D12DescriptorHeap> g_pattern_gpu, g_pattern_cpu;
float g_pattern[4] = {0, 0, 0, 1};
int g_pattern_writes = 0;
void WritePattern(ID3D12GraphicsCommandList* l, const std::map<std::string, test::FakeParams::Value>& params)
{
    auto* out = static_cast<ID3D12Resource*>(fake::P(params, NVSDK_NGX_Parameter_Output));
    if (!out || !g_pattern_dev) return;
    if (!g_pattern_gpu) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(g_pattern_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_pattern_gpu)))) return;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_pattern_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_pattern_cpu)))) return;
    }
    auto f = out->GetDesc().Format;
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == DXGI_FORMAT_R16G16B16A16_TYPELESS) f = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format = f; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_pattern_dev->CreateUnorderedAccessView(out, nullptr, &u, g_pattern_gpu->GetCPUDescriptorHandleForHeapStart());
    g_pattern_dev->CreateUnorderedAccessView(out, nullptr, &u, g_pattern_cpu->GetCPUDescriptorHandleForHeapStart());
    ID3D12DescriptorHeap* heaps[] = {g_pattern_gpu.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->ClearUnorderedAccessViewFloat(g_pattern_gpu->GetGPUDescriptorHandleForHeapStart(), g_pattern_cpu->GetCPUDescriptorHandleForHeapStart(), out, g_pattern, 0, nullptr);
    ++g_pattern_writes;
}
} // namespace

int main(int argc,char** argv)
{
    const bool native_scale=argc>1&&std::strstr(argv[1],"--native-scale")!=nullptr;
    const bool native_merged=argc>1&&std::strstr(argv[1],"merged")!=nullptr;
    const bool native_late=argc>1&&std::strstr(argv[1],"late")!=nullptr;
    const bool native_composite=argc>1&&std::strstr(argv[1],"composite")!=nullptr;
    const bool native_graphics=argc>1&&std::strstr(argv[1],"graphics")!=nullptr;
    // SE5 shape (p49436): the pass writing the presented image has bindings the
    // trace cannot snapshot (here an unbounded table), and the picture is not SDR.
    const bool native_se5=argc>1&&std::strstr(argv[1],"se5")!=nullptr;
    // p51016: the presenting pass is a compute dispatch reading a runtime-indexed bindless
    // array (unreadable): the crossing is learned only through the screen probe, and the
    // decoy upscale earlier in the frame must be refuted first.
    const bool native_bindless=native_se5&&argc>1&&std::strstr(argv[1],"bindless")!=nullptr;
    // Sol 7fec1c4: the presented image is a real DXGI swap-chain buffer, created by DXGI
    // (never through our creation hooks) and first met through CreateRenderTargetView.
    const bool native_dxgi=native_se5&&argc>1&&std::strstr(argv[1],"dxgi")!=nullptr;
    // 0cfeff9 run: the presenting draw reads its picture where no root table shows it
    // (SE5 pso 785 shows only a 128x128 input). Emulated: the game's image through a
    // Texture2DArray view, which the trace does not count as a whole output-size input.
    const bool native_unseen=native_se5&&argc>1&&std::strstr(argv[1],"unseen")!=nullptr;
    // f1d0594/de8f0b3 runs: one pipeline draws several full-viewport, no-blend quads onto the
    // back buffer (only one reads the 3440x1440 canvas), and the canvas base is written by a
    // DRAW into a UAV from the render-size image (SE5 pso 660/799). Plus the compute decoy.
    const bool native_session=native_se5&&argc>1&&std::strstr(argv[1],"session")!=nullptr;
    // p11148 frame dumps: the game fills the upscale dispatch's descriptor table AFTER the
    // Dispatch is recorded (stale ring content at record time). Emulated: the table holds a
    // decoy UAV and 1x1 dummies while the list records; the real views are copied in after
    // Close, before ExecuteCommandLists. The trace and the delivery must resolve the table
    // at submission (late=1/N in the trace line), and DLSS still reaches the screen.
    const bool native_latebind=native_se5&&argc>1&&std::strstr(argv[1],"latebind")!=nullptr;
    const bool native_rotate=native_latebind&&std::strstr(argv[1],"rotate")!=nullptr;
    const bool native_layout_change=native_latebind&&std::strstr(argv[1],"layout-change")!=nullptr;
    const bool native_inherited=native_latebind&&std::strstr(argv[1],"inherited")!=nullptr;
    const bool native_close_race=native_latebind&&std::strstr(argv[1],"close-race")!=nullptr;
    const bool native_close_before=native_close_race&&std::strstr(argv[1],"close-before")!=nullptr;
    const bool nvrhi=argc>1&&std::strcmp(argv[1],"--nvrhi")==0;
    if(nvrhi&&!na::gpu::GetStats().built)return test::kSkip;
    na::gpu::SetBackendForTests(nvrhi?na::gpu::TestBackend::Nvrhi:na::gpu::TestBackend::Native);
    setvbuf(stdout, nullptr, _IONBF, 0); // progress survives a timeout kill
    test::ComPtr<ID3D12Debug> debug;
    if(native_late && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_live_test");
    ID3D12Device* dev = d.device.Get();
    g_queue = d.queue.Get();
    std::printf("step: install producers\n");
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), /*hook_queue_execute=*/true));
    if(native_scale)na::j2::EnableNativeScaleBridge(true);
    // Every native-scale variant uses recording leases, as does production.
    // A Reset must create a new receipt, including the short direct-pass tests.
    if(native_scale) REQUIRE(na::observe::InstallListResetTracking(dev,na::nr::OnCommandListReset));
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
        D3D12_DESCRIPTOR_HEAP_DESC chd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&cbvh))));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cv{cb->GetGPUVirtualAddress() + 512, 256};
        dev->CreateConstantBufferView(&cv, cbvh->GetCPUDescriptorHandleForHeapStart());
        // Second descriptor: another camera's block at 1536 (same layout), used by the
        // several-cameras-in-one-frame case.
        D3D12_CONSTANT_BUFFER_VIEW_DESC cv2{cb->GetGPUVirtualAddress() + 1536, 256};
        D3D12_CPU_DESCRIPTOR_HANDLE h2 = cbvh->GetCPUDescriptorHandleForHeapStart();
        h2.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        dev->CreateConstantBufferView(&cv2, h2);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE other_cam_table = cbvh->GetGPUDescriptorHandleForHeapStart();
    other_cam_table.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    uint8_t* cpu = nullptr;
    REQUIRE(SUCCEEDED(cb->Map(0, nullptr, reinterpret_cast<void**>(&cpu))) && cpu); // persistent, like a game's upload ring
    const D3D12_GPU_VIRTUAL_ADDRESS va = cb->GetGPUVirtualAddress();

    test::List scene, consumer;
    REQUIRE(scene.Create(dev) && consumer.Create(dev));
    REQUIRE(SUCCEEDED(scene.list->Close()) && SUCCEEDED(consumer.list->Close()));
    const auto frame = [&](double cam_x, ID3D12Resource* touched = nullptr, bool other_camera = false) {
        d.WaitIdle();
        // Camera block at 512 (VP at +80, inverse at +144), a junk block at 1024.
        std::memset(cpu, 0, 4096);
        const M vp = Camera(cam_x);
        M inv{};
        Inverse(vp, inv);
        Store(cpu + 512 + 80, vp);
        Store(cpu + 512 + 144, inv);
        for (int i = 0; i < 64; ++i) cpu[1024 + i] = static_cast<uint8_t>(i * 7);
        if (other_camera) { // a water reflection's camera: the main camera mirrored in the plane y = 0
            const M ov = Reflection(cam_x);
            M oinv{};
            Inverse(ov, oinv);
            Store(cpu + 1536 + 80, ov);
            Store(cpu + 1536 + 144, oinv);
        }
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
        if (other_camera) // bound through the camera slot first and more often than the main camera
            for (int i = 0; i < 2; ++i) scene.list->SetGraphicsRootDescriptorTable(1, other_cam_table);
        scene.list->SetGraphicsRootDescriptorTable(1, cbvh->GetGPUDescriptorHandleForHeapStart()); // camera (table CBV)
        D3D12_RESOURCE_BARRIER out = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        scene.list->ResourceBarrier(1, &out);
        if (touched) { // a texture the game also uses this frame (P4-4: its stretch destination), left in COMMON
            D3D12_RESOURCE_BARRIER t[2] = {test::Transition(touched, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                           test::Transition(touched, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON)};
            scene.list->ResourceBarrier(1, &t[0]);
            scene.list->ResourceBarrier(1, &t[1]);
        }
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
    na::settings::Get().j2_route.store(1); // fixed between-list route: frame-1 expectations (selector tested separately)
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

    if(native_scale){
        // Actual native upscale operation; no SetUpscaleTarget, manual inline
        // callback, destination injection or test-only bridge matcher. The
        // existing operation shadow must find the source/destination itself.
        constexpr uint32_t OW=96,OH=48;
        const auto format=native_late?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto view_format=native_late?DXGI_FORMAT_R8G8B8A8_UNORM:format;
        const auto flags=D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto native=test::CreateTexture(dev,OW,OH,format,D3D12_RESOURCE_STATE_COMMON,flags);
        REQUIRE(native);
        test::ComPtr<ID3D12Resource> late, decoy, presented, aux;
        test::ComPtr<IDXGISwapChain1> witness_chain;HWND witness_window=nullptr;
        if (native_late) {
            late=test::CreateTexture(dev,W,H,format,D3D12_RESOURCE_STATE_COMMON,flags);
            decoy=test::CreateTexture(dev,OW,OH,format,D3D12_RESOURCE_STATE_COMMON,flags);
            // SE5: the presented image is FP16 (p49436 swap chain format 10).
            if(native_dxgi){
                witness_window=test::CreateTestWindow(L"Backbuffer provenance witness",OW,OH);
                DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=OW;sd.Height=OH;sd.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;sd.SampleDesc.Count=1;
                sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
                REQUIRE(witness_window&&SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(),witness_window,&sd,nullptr,nullptr,&witness_chain)));
                REQUIRE(SUCCEEDED(witness_chain->GetBuffer(0,IID_PPV_ARGS(&presented))));
                CHECK(!na::observe::IdOf(presented.Get())); // unknown until its first view
            }else            presented=test::CreateTexture(dev,OW,OH,native_se5?DXGI_FORMAT_R16G16B16A16_FLOAT:format,D3D12_RESOURCE_STATE_COMMON,
                native_composite?D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|(native_se5?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE)):D3D12_RESOURCE_FLAG_NONE);
            REQUIRE(late&&decoy&&presented);
            // SE5: a smaller auxiliary input of the upscale step (#13990 1146x480/10).
            if(native_se5){aux=test::CreateTexture(dev,W/2,H/2,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COMMON,flags);REQUIRE(aux);}
        }
        if(native_se5){
            // Normal output path: no debug picture, no sharpening; the fake
            // evaluation writes the pattern into its real output.
            g_pattern_dev=dev;fake::eval_output=&WritePattern;g_track_dlss=true;
            na::settings::Get().j2_sharpness.store(0);
        }
        test::ComPtr<ID3D12DescriptorHeap> heap,cpu_heap;
        test::ComPtr<ID3D12DescriptorHeap> late_rtvs;
        D3D12_RENDER_TARGET_VIEW_DESC late_view{};
        late_view.Format=view_format;late_view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        if(native_layout_change){
            D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
            REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&late_rtvs))));
            dev->CreateRenderTargetView(late.Get(),&late_view,late_rtvs->GetCPUDescriptorHandleForHeapStart());
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd_native{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,native_se5?16u:native_composite?7u:native_late?6u:2u,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd_native,IID_PPV_ARGS(&heap))));
        hd_native.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd_native,IID_PPV_ARGS(&cpu_heap))));
        const auto increment=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=view_format;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;
        dev->CreateShaderResourceView(native_late?late.Get():color.Get(),&sv,heap->GetCPUDescriptorHandleForHeapStart());
        auto cpu_uav=heap->GetCPUDescriptorHandleForHeapStart();cpu_uav.ptr+=increment;
        auto gpu_uav=heap->GetGPUDescriptorHandleForHeapStart();gpu_uav.ptr+=increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=sv.Format;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(native.Get(),nullptr,&uv,cpu_uav);
        dev->CreateUnorderedAccessView(native.Get(),nullptr,&uv,cpu_heap->GetCPUDescriptorHandleForHeapStart());
        test::ComPtr<ID3D12RootSignature> native_root;test::ComPtr<ID3D12PipelineState> native_pipeline;
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
        D3D12_ROOT_PARAMETER root_param{};root_param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;root_param.DescriptorTable={2,ranges};
        D3D12_ROOT_SIGNATURE_DESC root_desc{1,&root_param,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        test::ComPtr<ID3DBlob> serialized,error,shader;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)));
        REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&native_root))));
        const char hlsl[]=R"(Texture2D<float4> image:register(t0);RWTexture2D<float4> output:register(u0);
            [numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x<96&&p.y<48)output[p.xy]=image.Load(int3(p.xy*uint2(64,32)/uint2(96,48),0));})";
        REQUIRE(SUCCEEDED(D3DCompile(hlsl,sizeof hlsl,"native_stretch",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&error)));
        test::ComPtr<ID3D12DescriptorHeap> native_rtvs;
        if(native_graphics){
            test::ComPtr<ID3DBlob> vertex,pixel;
            const char graphics[]=R"(Texture2D<float4> image:register(t0);
                float4 vs(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
                float4 ps(float4 p:SV_Position):SV_Target{return image.Load(int3(uint2(p.xy)*uint2(64,32)/uint2(96,48),0));})";
            REQUIRE(SUCCEEDED(D3DCompile(graphics,sizeof graphics,"native_graphics",nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vertex,&error)));
            REQUIRE(SUCCEEDED(D3DCompile(graphics,sizeof graphics,"native_graphics",nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&pixel,&error)));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=native_root.Get();p.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};p.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
            p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
            p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            auto& blend=p.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&native_pipeline))));
            D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&native_rtvs))));dev->CreateRenderTargetView(native.Get(),nullptr,native_rtvs->GetCPUDescriptorHandleForHeapStart());
        }else{
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=native_root.Get();pipeline.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&native_pipeline))));
        }
        test::ComPtr<ID3D12PipelineState> tonemap;
        if (native_late) {
            const auto base=heap->GetCPUDescriptorHandleForHeapStart().ptr;
            auto hdr=sv;hdr.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            dev->CreateShaderResourceView(color.Get(),&hdr,{base+2*increment});
            dev->CreateUnorderedAccessView(late.Get(),nullptr,&uv,{base+3*increment});
            dev->CreateShaderResourceView(late.Get(),&sv,{base+4*increment});
            dev->CreateUnorderedAccessView(decoy.Get(),nullptr,&uv,{base+5*increment});
            const char convert[]=R"(Texture2D<float4> src:register(t0);RWTexture2D<float4> dst:register(u0);
                [numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){dst[p.xy]=sqrt(src.Load(int3(p.xy,0)));})";
            REQUIRE(SUCCEEDED(D3DCompile(convert,sizeof convert,"test_tonemap",nullptr,nullptr,"main","cs_5_0",0,0,&shader,&error)));
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=native_root.Get();desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&desc,IID_PPV_ARGS(&tonemap))));
        }
        test::ComPtr<ID3D12PipelineState> composite;
        test::ComPtr<ID3D12RootSignature> composite_root;
        test::ComPtr<ID3D12DescriptorHeap> composite_rtv;
        if(native_composite){
            const auto base=heap->GetCPUDescriptorHandleForHeapStart().ptr;
            dev->CreateShaderResourceView(native.Get(),&sv,{base+6*increment});
            root_param.DescriptorTable.NumDescriptorRanges=1;
            if(native_se5)ranges[0].NumDescriptors=UINT_MAX;
            serialized.Reset();
            REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)));
            REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&composite_root))));
            // SE5: an explicit 8-bit UNORM -> FP16 conversion (gamma 2.2 decode).
            const char pass[]=R"(Texture2D<float4> image:register(t0);
                float4 vs(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
                float4 ps(float4 p:SV_Position):SV_Target{float4 c=image.Load(int3(uint2(p.xy),0));
                #ifdef SE5
                if(p.x<4 && p.y<4)return float4(0,1,0,1);
                return float4(pow(c.rgb,2.2),c.a);
                #else
                return c;
                #endif
                })";
            const D3D_SHADER_MACRO se5_macro[]{{"SE5","1"},{nullptr,nullptr}};
            test::ComPtr<ID3DBlob> vertex,pixel;
            REQUIRE(SUCCEEDED(D3DCompile(pass,sizeof pass,"test_composite",native_se5?se5_macro:nullptr,nullptr,"vs","vs_5_0",0,0,&vertex,&error)));
            REQUIRE(SUCCEEDED(D3DCompile(pass,sizeof pass,"test_composite",native_se5?se5_macro:nullptr,nullptr,"ps","ps_5_0",0,0,&pixel,&error)));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature=composite_root.Get();
            desc.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};
            desc.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
            desc.SampleMask=UINT_MAX;desc.SampleDesc.Count=1;
            desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable=TRUE;
            desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            const auto present_format=native_se5?DXGI_FORMAT_R16G16B16A16_FLOAT:view_format;
            desc.NumRenderTargets=1;desc.RTVFormats[0]=present_format;
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&composite))));
            D3D12_DESCRIPTOR_HEAP_DESC rt_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2};
            REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rt_heap,IID_PPV_ARGS(&composite_rtv))));
            D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format=present_format;rtv_desc.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            dev->CreateRenderTargetView(presented.Get(),&rtv_desc,composite_rtv->GetCPUDescriptorHandleForHeapStart());
            if(native_dxgi){
                na::observe::OperationDescriptor observed;
                const bool resolved=na::observe::NativeDescriptor(composite_rtv->GetCPUDescriptorHandleForHeapStart(),observed);
                std::printf("dxgi RTV at creation: buffer=%llu resolved=%d\n",(unsigned long long)na::observe::IdOf(presented.Get()),resolved);
                CHECK(resolved && na::observe::IdOf(presented.Get())); // registered before the view's metadata was published
            }
            if(native_se5){
                // [1] RTV of the game's upscaled image (the overwrite case clears it).
                rtv_desc.Format=view_format;
                auto h=composite_rtv->GetCPUDescriptorHandleForHeapStart();h.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                dev->CreateRenderTargetView(native.Get(),&rtv_desc,h);
            }
        }
        // SE5 upscale step with a second, smaller input bound on alternating frames:
        // Tables at 7 (native, late, aux) and 10 (native, late, null). The
        // unbounded SRV range uses two statically declared registers.
        test::ComPtr<ID3D12RootSignature> se5_root;test::ComPtr<ID3D12PipelineState> se5_upscale;
        test::ComPtr<ID3D12DescriptorHeap> stage_heap;test::ComPtr<ID3D12Resource> dummy;
        if(native_se5){
            const auto base=heap->GetCPUDescriptorHandleForHeapStart().ptr;
            D3D12_SHADER_RESOURCE_VIEW_DESC fp=sv;fp.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            dev->CreateUnorderedAccessView(native.Get(),nullptr,&uv,{base+7*increment});
            dev->CreateShaderResourceView(late.Get(),&sv,{base+8*increment});
            dev->CreateShaderResourceView(aux.Get(),&fp,{base+9*increment});
            dev->CreateUnorderedAccessView(native.Get(),nullptr,&uv,{base+10*increment});
            dev->CreateShaderResourceView(late.Get(),&sv,{base+11*increment});
            dev->CreateShaderResourceView(nullptr,&fp,{base+12*increment});
            dev->CreateShaderResourceView(decoy.Get(),&sv,{base+13*increment});
            if(native_latebind){
                // Staging (CPU) heap: 0..2 the record-time dummies (decoy UAV, 1x1 SRVs),
                // 7..12 the real tables, copied into the shader-visible heap before submission.
                D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,16u,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
                REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&sd,IID_PPV_ARGS(&stage_heap))));
                dummy=test::CreateTexture(dev,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);REQUIRE(dummy);
                const auto sbase=stage_heap->GetCPUDescriptorHandleForHeapStart().ptr;
                D3D12_SHADER_RESOURCE_VIEW_DESC dv=sv;dv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                dev->CreateUnorderedAccessView(decoy.Get(),nullptr,&uv,{sbase});
                dev->CreateShaderResourceView(dummy.Get(),&dv,{sbase+1*increment});
                dev->CreateShaderResourceView(dummy.Get(),&dv,{sbase+2*increment});
                dev->CreateUnorderedAccessView(native.Get(),nullptr,&uv,{sbase+7*increment});
                dev->CreateShaderResourceView(late.Get(),&sv,{sbase+8*increment});
                dev->CreateShaderResourceView(aux.Get(),&fp,{sbase+9*increment});
                dev->CreateUnorderedAccessView(native.Get(),nullptr,&uv,{sbase+10*increment});
                dev->CreateShaderResourceView(late.Get(),&sv,{sbase+11*increment});
                dev->CreateShaderResourceView(nullptr,&fp,{sbase+12*increment});
            }
            D3D12_DESCRIPTOR_RANGE r2[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,0,0,1}};
            D3D12_ROOT_PARAMETER p2{};p2.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p2.DescriptorTable={2,r2};
            D3D12_ROOT_SIGNATURE_DESC d2{1,&p2,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
            serialized.Reset();
            REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&d2,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)));
            REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&se5_root))));
            const char up[]=R"(Texture2D<float4> image:register(t0);Texture2D<float4> aux:register(t1);RWTexture2D<float4> output:register(u0);
                [numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x<96&&p.y<48)output[p.xy]=image.Load(int3(p.xy*uint2(64,32)/uint2(96,48),0))+min(aux.Load(int3(0,0,0)),0);})";
            test::ComPtr<ID3DBlob> cs;
            REQUIRE(SUCCEEDED(D3DCompile(up,sizeof up,"se5_upscale",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&cs,&error)));
            D3D12_COMPUTE_PIPELINE_STATE_DESC c{};c.pRootSignature=se5_root.Get();c.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
            REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&c,IID_PPV_ARGS(&se5_upscale))));
        }
        // Bindless presenting dispatch: t0[] unbounded (runtime index from a root
        // constant) starting at descriptor 6 (the game's image; 13 = decoy), u0 at 15.
        test::ComPtr<ID3D12RootSignature> bindless_root;test::ComPtr<ID3D12PipelineState> bindless_present;
        if(native_bindless){
            const auto base=heap->GetCPUDescriptorHandleForHeapStart().ptr;
            D3D12_UNORDERED_ACCESS_VIEW_DESC pu{};pu.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;pu.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(presented.Get(),nullptr,&pu,{base+15*increment});
            D3D12_DESCRIPTOR_RANGE brs[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,UINT_MAX,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
            D3D12_ROOT_PARAMETER ps[3]{};
            ps[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;ps[0].DescriptorTable={1,&brs[0]};
            ps[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;ps[1].DescriptorTable={1,&brs[1]};
            ps[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;ps[2].Constants={0,0,1};
            D3D12_ROOT_SIGNATURE_DESC rd{3,ps,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
            serialized.Reset();
            REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)));
            REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&bindless_root))));
            const char bl[]=R"(Texture2D<float4> imgs[]:register(t0);RWTexture2D<float4> o:register(u0);cbuffer C:register(b0){uint idx;};
                [numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){if(p.x<96&&p.y<48){float4 c=imgs[NonUniformResourceIndex(idx)].Load(int3(p.xy,0));o[p.xy]=float4(pow(c.rgb,2.2),c.a);}})";
            test::ComPtr<ID3DBlob> cs;
            const HRESULT hr=D3DCompile(bl,sizeof bl,"bindless_present",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES,0,&cs,&error);
            if(FAILED(hr)&&error)std::printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));
            REQUIRE(SUCCEEDED(hr));
            D3D12_COMPUTE_PIPELINE_STATE_DESC c{};c.pRootSignature=bindless_root.Get();c.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
            REQUIRE(SUCCEEDED(dev->CreateComputePipelineState(&c,IID_PPV_ARGS(&bindless_present))));
        }
        const bool native_overwrite=native_se5&&argc>1&&std::strstr(argv[1],"overwrite")!=nullptr;
        // Negative: the upscale list is submitted BEFORE its scene producer (Sol's ordering fixture).
        const bool native_reverse=native_se5&&argc>1&&std::strstr(argv[1],"reverse")!=nullptr;
        const bool native_tail=native_se5&&argc>1&&std::strstr(argv[1],"tail")!=nullptr;
        // Negative: delivery works, then the game overwrites its image (in a list of its own,
        // between the upscale list and the presenting list).
        const bool native_replace=native_se5&&argc>1&&std::strstr(argv[1],"replace")!=nullptr;
        // Sol 480af25: the same overwrite only after 5 successful deliveries; a later
        // complete trace must invalidate the learned route (re-checks every 20 presents).
        const bool native_stale=native_se5&&argc>1&&std::strstr(argv[1],"stale")!=nullptr;
        unsigned stale_from=0;
        if(native_stale)na::j2::SetNativeTraceIntervalForTests(20);
        na::j2::SetQualityWarmupForTests(2); // the fixtures are short; the gate itself is exercised by the first two held frames
        test::List over;if(native_replace||native_stale){REQUIRE(over.Create(dev));REQUIRE(SUCCEEDED(over.list->Close()));}        // SE5: the presenting list is recorded ONCE (before any trace window) and
        // executed every frame; it writes the presented image twice: first from
        // the decoy, then from the game's upscaled image (the write that counts).
        test::List final_list;bool final_recorded=false;
        test::ComPtr<ID3D12PipelineState> unseen_composite,session_upscale,session_composite;test::ComPtr<ID3D12Resource> dot;
        if(native_session){
            const auto base=heap->GetCPUDescriptorHandleForHeapStart().ptr;
            dot=test::CreateTexture(dev,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_FLAG_NONE);REQUIRE(dot);
            D3D12_SHADER_RESOURCE_VIEW_DESC dv=sv;dv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;dev->CreateShaderResourceView(dot.Get(),&dv,{base+14*increment});
            const char up[]=R"(Texture2D<float4> image:register(t0);Texture2D<float4> aux:register(t1);RWTexture2D<float4> o:register(u0);
                float4 vs(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
                void ps(float4 p:SV_Position){uint2 q=uint2(p.xy);if(q.x<96&&q.y<48)o[q]=image.Load(int3(q*uint2(64,32)/uint2(96,48),0))+min(aux.Load(int3(0,0,0)),0);})";
            test::ComPtr<ID3DBlob> v,px,err;
            REQUIRE(SUCCEEDED(D3DCompile(up,sizeof up,"session_up",nullptr,nullptr,"vs","vs_5_0",0,0,&v,&err)));
            REQUIRE(SUCCEEDED(D3DCompile(up,sizeof up,"session_up",nullptr,nullptr,"ps","ps_5_0",0,0,&px,&err)));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};gd.pRootSignature=se5_root.Get();gd.VS={v->GetBufferPointer(),v->GetBufferSize()};gd.PS={px->GetBufferPointer(),px->GetBufferSize()};
            gd.SampleMask=UINT_MAX;gd.SampleDesc.Count=1;gd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;gd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;gd.RasterizerState.DepthClipEnable=TRUE;
            gd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;gd.NumRenderTargets=0;
            REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&gd,IID_PPV_ARGS(&session_upscale))));
            const char cp[]=R"(Texture2D<float4> image:register(t0);
                float4 vs(uint id:SV_VertexID):SV_Position{uint k=id%3;float2 p=float2((k<<1)&2,k&2);if(id>=3)p*=0.04;return float4(p*float2(2,-2)+float2(-1,1),0,1);}
                float4 ps(float4 p:SV_Position):SV_Target{float4 c=image.Load(int3(uint2(p.xy),0));return float4(pow(c.rgb,2.2),c.a);})";
            v.Reset();px.Reset();
            REQUIRE(SUCCEEDED(D3DCompile(cp,sizeof cp,"session_cp",nullptr,nullptr,"vs","vs_5_0",0,0,&v,&err)));
            REQUIRE(SUCCEEDED(D3DCompile(cp,sizeof cp,"session_cp",nullptr,nullptr,"ps","ps_5_0",0,0,&px,&err)));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cd{};cd.pRootSignature=composite_root.Get();cd.VS={v->GetBufferPointer(),v->GetBufferSize()};cd.PS={px->GetBufferPointer(),px->GetBufferSize()};
            cd.SampleMask=UINT_MAX;cd.SampleDesc.Count=1;cd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;cd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;cd.RasterizerState.DepthClipEnable=TRUE;
            cd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;cd.NumRenderTargets=1;cd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            cd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&cd,IID_PPV_ARGS(&session_composite))));
        }
        if(native_unseen){
            const auto base=heap->GetCPUDescriptorHandleForHeapStart().ptr;
            D3D12_SHADER_RESOURCE_VIEW_DESC av{};av.Format=view_format;av.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            av.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;av.Texture2DArray.MipLevels=1;av.Texture2DArray.ArraySize=1;
            dev->CreateShaderResourceView(native.Get(),&av,{base+14*increment});
            const char us[]=R"(Texture2DArray<float4> image:register(t0);
                float4 vs(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}
                float4 ps(float4 p:SV_Position):SV_Target{float4 c=image.Load(int4(uint2(p.xy),0,0));return float4(pow(c.rgb,2.2),c.a);})";
            test::ComPtr<ID3DBlob> v,px,err;
            REQUIRE(SUCCEEDED(D3DCompile(us,sizeof us,"unseen",nullptr,nullptr,"vs","vs_5_0",0,0,&v,&err)));
            REQUIRE(SUCCEEDED(D3DCompile(us,sizeof us,"unseen",nullptr,nullptr,"ps","ps_5_0",0,0,&px,&err)));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC gd{};gd.pRootSignature=composite_root.Get();gd.VS={v->GetBufferPointer(),v->GetBufferSize()};gd.PS={px->GetBufferPointer(),px->GetBufferSize()};
            gd.SampleMask=UINT_MAX;gd.SampleDesc.Count=1;gd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;gd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;gd.RasterizerState.DepthClipEnable=TRUE;
            gd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;gd.NumRenderTargets=1;gd.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            gd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&gd,IID_PPV_ARGS(&unseen_composite))));
        }
        if(native_se5){REQUIRE(final_list.Create(dev));REQUIRE(SUCCEEDED(final_list.list->Close()));}
        test::List post;REQUIRE(post.Create(dev));REQUIRE(SUCCEEDED(post.list->Close()));
        na::j2::SetNativeRenderExtent(W,H,OW,OH);na::j2::SetDebugView(native_se5?0:3); // 3: known black depth output, alpha1; SE5: the normal output
        const int before_native=fake::evals;
        const unsigned iterations=native_se5?100u:native_late?68u:3u;
        if(native_se5&&!native_bindless&&!native_unseen)na::j2::RequestDeliveryProbe();
        unsigned late_deliveries=0;
        const auto inherited_depth_state=[&](bool enter){
            test::List state_list;CHECK(state_list.Create(dev));
            CHECK(SUCCEEDED(state_list.list->Close()));state_list.Reset();
            auto b=test::Transition(depth.Get(),enter?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_DEPTH_WRITE,
                enter?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_COMMON);
            state_list.list->ResourceBarrier(1,&b);CHECK(SUCCEEDED(state_list.list->Close()));
            ID3D12CommandList* ls[]{state_list.list.Get()};d.queue->ExecuteCommandLists(1,ls);d.WaitIdle();
        };
        if(native_inherited)inherited_depth_state(true);
        test::List future;
        std::shared_ptr<CloseDuringDispatch> close_race;
        if(native_close_race){
            REQUIRE(future.Create(dev));REQUIRE(SUCCEEDED(future.list->Close()));
            close_race=std::make_shared<CloseDuringDispatch>();
            close_race->pipeline=na::observe::IdOf(se5_upscale.Get());
            na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,close_race);
        }
        const auto native_execute=[&](UINT count,ID3D12CommandList* const* ls){
            // The production overlay opens the observation scope BEFORE J2;
            // carried queue state is not available outside that scope.
            auto token=native_inherited?na::observe::ExecuteBegin(d.queue.Get(),count,ls):na::observe::ExecuteToken{};
            na::j2::Execute(d.queue.Get(),count,ls,&Forward);
            if(native_inherited)na::observe::ExecuteEnd(token,d.queue.Get(),count,ls);
        };
        for(unsigned iteration=0;iteration<iterations;++iteration){
            d.WaitIdle();na::j2::OnPresent(d.queue.Get());
            if((native_rotate && iteration==80) || (native_layout_change && (iteration==50 || iteration==80))){
                if(iteration==80)CHECK(late_deliveries>0);
                // A post-process input is replaced AFTER the route is learned,
                // between periodic traces. Same shader, destination and extent;
                // the late descriptor now names the current image, not its old id.
                late=test::CreateTexture(dev,W,H,format,D3D12_RESOURCE_STATE_COMMON,flags);REQUIRE(late);
                if(native_layout_change)dev->CreateRenderTargetView(late.Get(),&late_view,late_rtvs->GetCPUDescriptorHandleForHeapStart());
                const auto descriptor_base=heap->GetCPUDescriptorHandleForHeapStart().ptr;
                dev->CreateUnorderedAccessView(late.Get(),nullptr,&uv,{descriptor_base+3*increment});
                for(unsigned at:{0u,4u,8u,11u})dev->CreateShaderResourceView(late.Get(),&sv,{descriptor_base+at*increment});
                const auto sb=stage_heap->GetCPUDescriptorHandleForHeapStart().ptr;
                for(unsigned at:{8u,11u})dev->CreateShaderResourceView(late.Get(),&sv,{sb+at*increment});
            }
            g_pattern[0]=.2f+.1f*float(iteration%4);g_pattern[1]=.6f;g_pattern[2]=.4f;g_pattern[3]=1.f;
            const M vp_native=Camera(.1+iteration*.01);M inv_native{};Inverse(vp_native,inv_native);
            Store(cpu+512+80,vp_native);Store(cpu+512+144,inv_native);
            scene.Reset();
            D3D12_RESOURCE_BARRIER begin_native[2]{test::Transition(color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET),test::Transition(depth.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_DEPTH_WRITE)};
            scene.list->ResourceBarrier(native_inherited?1u:2u,begin_native);scene.list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
            const float grey_native[]{.3f,.3f,.3f,1};scene.list->ClearRenderTargetView(rtv,grey_native,0,nullptr);
            if(!native_inherited)scene.list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
            scene.list->SetGraphicsRootSignature(rs.Get());ID3D12DescriptorHeap* camera_heap[]{cbvh.Get()};scene.list->SetDescriptorHeaps(1,camera_heap);
            scene.list->SetGraphicsRootConstantBufferView(0,va+1024);scene.list->SetGraphicsRootDescriptorTable(1,cbvh->GetGPUDescriptorHandleForHeapStart());
            const auto source_read=native_graphics?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            D3D12_RESOURCE_BARRIER finish_native[2]{test::Transition(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,native_se5?D3D12_RESOURCE_STATE_COMMON:source_read),test::Transition(depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COMMON)};
            scene.list->ResourceBarrier(native_inherited?1u:2u,finish_native);
            ID3D12GraphicsCommandList* target_list=scene.list.Get();
            if(!native_merged){
                REQUIRE(SUCCEEDED(scene.list->Close()));
                if(native_close_race){
                    ID3D12CommandList* producer[]{scene.list.Get()};native_execute(1,producer);
                    // A future recording of the SAME scene closes while the
                    // current dispatch is forwarded, but is never submitted.
                    // Its CPU Close must not replace the established producer.
                    future.Reset();
                    if(iteration>=80){
                        future.list->ResourceBarrier(2,begin_native);
                        future.list->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
                        future.list->ClearRenderTargetView(rtv,grey_native,0,nullptr);
                        future.list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
                        future.list->SetGraphicsRootSignature(rs.Get());future.list->SetDescriptorHeaps(1,camera_heap);
                        future.list->SetGraphicsRootConstantBufferView(0,va+1024);
                        future.list->SetGraphicsRootDescriptorTable(1,cbvh->GetGPUDescriptorHandleForHeapStart());
                        future.list->ResourceBarrier(2,finish_native);
                        if(native_close_before){REQUIRE(SUCCEEDED(future.list->Close()));++close_race->closed;}
                        else {
                            close_race->target=post.list.Get();
                            close_race->close=[&]{CHECK(SUCCEEDED(future.list->Close()));};
                        }
                    }else REQUIRE(SUCCEEDED(future.list->Close()));
                }
                // Render ahead: this producer closes before the preceding
                // picture's Present; its separate post list records afterward.
                // Cover both not-yet-submitted and already-submitted producer.
                if(!native_se5&&(native_late || iteration==2)){ID3D12CommandList* producer[]{scene.list.Get()};na::j2::Execute(d.queue.Get(),1,producer,&Forward);}                if(!native_late && iteration)na::j2::OnPresent(d.queue.Get());
                post.Reset();target_list=post.list.Get();
            }
            const auto game_write=native_graphics?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            auto into_uav=test::Transition(native.Get(),D3D12_RESOURCE_STATE_COMMON,game_write);target_list->ResourceBarrier(1,&into_uav);
            if(native_se5){
                // The game's native work is legal even with a stale scene. The
                // reversed case must detect our dependency without invalid barriers.
                auto read=test::Transition(color.Get(),D3D12_RESOURCE_STATE_COMMON,source_read);
                target_list->ResourceBarrier(1,&read);
            }
            target_list->SetPipelineState(native_pipeline.Get());ID3D12DescriptorHeap* game_heap[]{heap.Get()};target_list->SetDescriptorHeaps(1,game_heap);
            if(native_graphics){
                target_list->SetGraphicsRootSignature(native_root.Get());target_list->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
                auto native_rtv=native_rtvs->GetCPUDescriptorHandleForHeapStart();target_list->OMSetRenderTargets(1,&native_rtv,FALSE,nullptr);
                const D3D12_VIEWPORT vp_full{0,0,float(OW),float(OH),0,1};const D3D12_RECT sc_full{0,0,LONG(OW),LONG(OH)};target_list->RSSetViewports(1,&vp_full);target_list->RSSetScissorRects(1,&sc_full);target_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);target_list->DrawInstanced(3,1,0,0);
                auto to_clear=test::Transition(native.Get(),game_write,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);target_list->ResourceBarrier(1,&to_clear);
            }else{
                target_list->SetComputeRootSignature(native_root.Get());
                const auto base=heap->GetGPUDescriptorHandleForHeapStart().ptr;
                if(native_late){
                    if(native_layout_change){
                        // Match the game's render-target-written post-process input.
                        // Its identity changes while the learned upscale shader stays.
                        auto rt=test::Transition(late.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
                        target_list->ResourceBarrier(1,&rt);
                        auto h=late_rtvs->GetCPUDescriptorHandleForHeapStart();
                        target_list->OMSetRenderTargets(1,&h,FALSE,nullptr);
                        target_list->ClearRenderTargetView(h,grey_native,0,nullptr);
                    }
                    auto write=test::Transition(late.Get(),native_layout_change?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    target_list->ResourceBarrier(1,&write);
                    target_list->SetPipelineState(tonemap.Get());
                    target_list->SetComputeRootDescriptorTable(0,{base+2*increment});
                    target_list->Dispatch(W/8,H/8,1);
                    auto read=test::Transition(late.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    target_list->ResourceBarrier(1,&read);
                    auto decoy_write=test::Transition(decoy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    target_list->ResourceBarrier(1,&decoy_write);
                    target_list->SetPipelineState(native_pipeline.Get());
                    target_list->SetComputeRootDescriptorTable(0,{base+4*increment});
                    target_list->Dispatch(12,6,1);
                    std::swap(decoy_write.Transition.StateBefore,decoy_write.Transition.StateAfter);
                    target_list->ResourceBarrier(1,&decoy_write);
                }
                if(native_session){
                    // The canvas base: a draw writing the output-size UAV from the render-size image.
                    target_list->SetGraphicsRootSignature(se5_root.Get());
                    target_list->SetPipelineState(session_upscale.Get());
                    target_list->SetGraphicsRootDescriptorTable(0,{base+(iteration%2?10u:7u)*increment});
                    target_list->OMSetRenderTargets(0,nullptr,FALSE,nullptr);
                    const D3D12_VIEWPORT vp{0,0,float(OW),float(OH),0,1};const D3D12_RECT sc{0,0,LONG(OW),LONG(OH)};
                    target_list->RSSetViewports(1,&vp);target_list->RSSetScissorRects(1,&sc);target_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    target_list->DrawInstanced(3,1,0,0);
                }else if(native_se5){
                    // The real upscale step, with the auxiliary input on even frames.
                    if(native_latebind){
                        // Record-time content of this frame's table: dummies (the game has not written it yet).
                        const auto cbase=heap->GetCPUDescriptorHandleForHeapStart().ptr;
                        dev->CopyDescriptorsSimple(3,{cbase+(iteration%2?10u:7u)*increment},stage_heap->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
                    target_list->SetComputeRootSignature(se5_root.Get());
                    target_list->SetPipelineState(se5_upscale.Get());
                    target_list->SetComputeRootDescriptorTable(0,{base+(iteration%2?10u:7u)*increment});
                }else target_list->SetComputeRootDescriptorTable(0,{base});
                if(!native_session)target_list->Dispatch(12,6,1);
            }
            // Later native HUD draw: green corner must survive reconstruction.
            D3D12_RESOURCE_BARRIER order{};order.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;order.UAV.pResource=native.Get();target_list->ResourceBarrier(1,&order);
            const float green[]{0,1,0,1};const D3D12_RECT hud_rect{0,0,4,4};
            if(!native_se5 || native_tail || (native_layout_change && iteration>=80))
                target_list->ClearUnorderedAccessViewFloat(gpu_uav,cpu_heap->GetCPUDescriptorHandleForHeapStart(),native.Get(),green,1,&hud_rect);
            if(native_stale&&late_deliveries>=5&&!stale_from)stale_from=iteration;
            if(native_overwrite){
                // Negative: the game overwrites its upscaled image before the final write reads it.
                auto to_rt=test::Transition(native.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET);
                target_list->ResourceBarrier(1,&to_rt);
                auto h=composite_rtv->GetCPUDescriptorHandleForHeapStart();h.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                const float blue[]{0,0,1,1};target_list->ClearRenderTargetView(h,blue,0,nullptr);
                std::swap(to_rt.Transition.StateBefore,to_rt.Transition.StateAfter);target_list->ResourceBarrier(1,&to_rt);
            }
            D3D12_RESOURCE_BARRIER end_native[2]{test::Transition(native.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON),test::Transition(color.Get(),source_read,D3D12_RESOURCE_STATE_COMMON)};
            if(native_se5){
                // The presenting list is separate (final_list, below).
                auto end_late=test::Transition(late.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
                target_list->ResourceBarrier(1,&end_late);
                target_list->ResourceBarrier(2,end_native);
            }else if(native_late){
                const auto native_read=native_composite?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COPY_SOURCE;
                const auto present_write=native_composite?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COPY_DEST;
                D3D12_RESOURCE_BARRIER copy[]{
                    test::Transition(native.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,native_read),
                    test::Transition(presented.Get(),D3D12_RESOURCE_STATE_COMMON,present_write)};
                target_list->ResourceBarrier(2,copy);
                if(native_composite){
                    test::ComPtr<ID3D12GraphicsCommandList4> render_pass;
                    REQUIRE(SUCCEEDED(target_list->QueryInterface(IID_PPV_ARGS(&render_pass))));
                    D3D12_RENDER_PASS_RENDER_TARGET_DESC attachment{};
                    attachment.cpuDescriptor=composite_rtv->GetCPUDescriptorHandleForHeapStart();
                    attachment.BeginningAccess.Type=D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
                    attachment.EndingAccess.Type=D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                    target_list->SetGraphicsRootSignature(composite_root.Get());
                    target_list->SetPipelineState(composite.Get());
                    const auto base=heap->GetGPUDescriptorHandleForHeapStart().ptr;
                    target_list->SetGraphicsRootDescriptorTable(0,{base+6*increment});
                    const D3D12_VIEWPORT vp{0,0,float(OW),float(OH),0,1};
                    const D3D12_RECT sc{0,0,LONG(OW),LONG(OH)};
                    target_list->RSSetViewports(1,&vp);target_list->RSSetScissorRects(1,&sc);
                    target_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    render_pass->BeginRenderPass(1,&attachment,nullptr,D3D12_RENDER_PASS_FLAG_NONE);
                    target_list->DrawInstanced(3,1,0,0);
                    render_pass->EndRenderPass();
                }else target_list->CopyResource(presented.Get(),native.Get());
                copy[0]=test::Transition(native.Get(),native_read,D3D12_RESOURCE_STATE_COMMON);
                std::swap(copy[1].Transition.StateBefore,copy[1].Transition.StateAfter);
                target_list->ResourceBarrier(2,copy);
                auto end_late=test::Transition(late.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
                target_list->ResourceBarrier(1,&end_late);
                target_list->ResourceBarrier(1,&end_native[1]);
            }else target_list->ResourceBarrier(2,end_native);
            REQUIRE(SUCCEEDED(target_list->Close()));
            if(native_latebind){
                // After Close, before submission: the game writes the real table.
                const auto cbase=heap->GetCPUDescriptorHandleForHeapStart().ptr;
                const auto sbase=stage_heap->GetCPUDescriptorHandleForHeapStart().ptr;
                const unsigned at=iteration%2?10u:7u;
                dev->CopyDescriptorsSimple(3,{cbase+at*increment},{sbase+at*increment},D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            ID3D12CommandList* native_lists[]{scene.list.Get(),post.list.Get()};
            if(native_se5){
                if(native_close_race){
                    native_execute(1,native_lists+1);
                }else if(native_reverse && iteration>=2){
                    native_execute(1,native_lists+1);
                    native_execute(1,native_lists);
                }else if(iteration%2){
                    native_execute(2,native_lists);
                }else{
                    native_execute(1,native_lists);
                    native_execute(1,native_lists+1);
                }
                if((native_replace&&late_deliveries>0)||(native_stale&&stale_from)){
                    // The game overwrites its upscaled image in a list of its own, before the presenting list.
                    over.Reset();
                    auto to_rt=test::Transition(native.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
                    over.list->ResourceBarrier(1,&to_rt);
                    auto h=composite_rtv->GetCPUDescriptorHandleForHeapStart();h.ptr+=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                    const float blue[]{0,0,1,1};over.list->ClearRenderTargetView(h,blue,0,nullptr);
                    std::swap(to_rt.Transition.StateBefore,to_rt.Transition.StateAfter);over.list->ResourceBarrier(1,&to_rt);
                    REQUIRE(SUCCEEDED(over.list->Close()));
                    ID3D12CommandList* ol[]{over.list.Get()};na::j2::Execute(d.queue.Get(),1,ol,&Forward);
                }
            }else if(!native_merged&&(native_late||iteration==2))na::j2::Execute(d.queue.Get(),1,native_lists+1,&Forward);            else na::j2::Execute(d.queue.Get(),native_merged?1u:2u,native_lists,&Forward);
            d.WaitIdle();
            if(native_se5&&iteration>=1){
                if(!final_recorded){
                    // Recorded once, after the first Present (the presented buffer is
                    // known) and long before any trace window; executed every frame.
                    final_list.Reset();
                    auto* fl=final_list.list.Get();
                    if(native_bindless){

                        D3D12_RESOURCE_BARRIER bb[2]{test::Transition(native.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                            test::Transition(presented.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
                        fl->ResourceBarrier(2,bb);
                        ID3D12DescriptorHeap* hh[]{heap.Get()};fl->SetDescriptorHeaps(1,hh);
                        fl->SetComputeRootSignature(bindless_root.Get());fl->SetPipelineState(bindless_present.Get());
                        const auto gbase=heap->GetGPUDescriptorHandleForHeapStart().ptr;
                        fl->SetComputeRootDescriptorTable(0,{gbase+6*increment});fl->SetComputeRootDescriptorTable(1,{gbase+15*increment});
                        fl->SetComputeRoot32BitConstant(2,0,0); // runtime index 0: the game's image
                        fl->Dispatch(12,6,1);
                        for(auto& x:bb)std::swap(x.Transition.StateBefore,x.Transition.StateAfter);
                        fl->ResourceBarrier(2,bb);
                        REQUIRE(SUCCEEDED(fl->Close()));final_recorded=true;
                    }
                }
                if(!final_recorded){
                    auto* fl=final_list.list.Get();
                    D3D12_RESOURCE_BARRIER b[3]{test::Transition(native.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                        test::Transition(decoy.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                        test::Transition(presented.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET)};
                    fl->ResourceBarrier(3,b);
                    ID3D12DescriptorHeap* hh[]{heap.Get()};fl->SetDescriptorHeaps(1,hh);
                    fl->SetGraphicsRootSignature(composite_root.Get());fl->SetPipelineState(composite.Get());
                    auto rt=composite_rtv->GetCPUDescriptorHandleForHeapStart();fl->OMSetRenderTargets(1,&rt,FALSE,nullptr);
                    if(native_dxgi){
                        na_object_id bound=0;na::observe::ListCurrentTargets(fl,&bound,nullptr);
                        std::printf("dxgi bound target=%llu actual buffer=%llu\n",(unsigned long long)bound,(unsigned long long)na::observe::IdOf(presented.Get()));
                        CHECK(bound && bound==na::observe::IdOf(presented.Get()));
                    }
                    const D3D12_VIEWPORT vp{0,0,float(OW),float(OH),0,1};const D3D12_RECT sc{0,0,LONG(OW),LONG(OH)};
                    fl->RSSetViewports(1,&vp);fl->RSSetScissorRects(1,&sc);fl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    const auto gbase=heap->GetGPUDescriptorHandleForHeapStart().ptr;
                    if(native_unseen){fl->SetPipelineState(unseen_composite.Get());fl->SetGraphicsRootDescriptorTable(0,{gbase+14*increment});fl->DrawInstanced(3,1,0,0);}
                    else if(native_session){
                        // One pipeline: the full-screen composite of the canvas, then two small quads
                        // (full viewport, no blend) reading a 1x1 texture.
                        fl->SetPipelineState(session_composite.Get());
                        fl->SetGraphicsRootDescriptorTable(0,{gbase+6*increment});fl->DrawInstanced(3,1,0,0);
                        fl->SetGraphicsRootDescriptorTable(0,{gbase+14*increment});fl->DrawInstanced(3,1,3,0);fl->DrawInstanced(3,1,3,0);
                    }
                    else{
                    fl->SetGraphicsRootDescriptorTable(0,{gbase+13*increment});fl->DrawInstanced(3,1,0,0); // first write: the decoy
                    fl->SetGraphicsRootDescriptorTable(0,{gbase+6*increment});fl->DrawInstanced(3,1,0,0);  // last write: the game's image
                    }
                    for(auto& x:b)std::swap(x.Transition.StateBefore,x.Transition.StateAfter);
                    fl->ResourceBarrier(3,b);
                    REQUIRE(SUCCEEDED(fl->Close()));final_recorded=true;
                }
                ID3D12CommandList* fls[]{final_list.list.Get()};
                na::j2::Execute(d.queue.Get(),1,fls,&Forward);
                d.WaitIdle();
            }
            st=na::j2::GetStats();
            if(native_se5){
                na::j2::NativeTracePresent(d.queue.Get(),presented.Get(),false);
                na::j2::NativeProbePresent(d.queue.Get(),presented.Get(),nullptr);
                if(native_bindless){
static uint64_t seen=0;char tr[2048];uint64_t n=0;na::j2::NativeTraceResult(tr,sizeof tr,&n);if(n!=seen){seen=n;const char* pv=std::strstr(tr,"provisional");std::printf("bindless iteration %u trace %llu: %.160s | probe %.200s\n",iteration,(unsigned long long)n,pv?pv:"(none)",na::j2::GetNativeBridgeStats().probe);}}
                d.WaitIdle();
                if(native_stale&&stale_from){
                    // Within 60 presents a re-verifying trace must have dropped the route.
                    if(iteration>stale_from+60)CHECK(st.native_deliveries==late_deliveries);
                    else late_deliveries=unsigned(st.native_deliveries);
                    continue;
                }
                if(st.native_deliveries>late_deliveries&&native_replace){late_deliveries=unsigned(st.native_deliveries);continue;}
                if(st.native_deliveries&&st.native_deliveries==late_deliveries)
                    std::printf("se5 iteration %u: no new delivery: j2 \"%s\" bridge \"%s\"\n",iteration,st.reason,na::j2::GetNativeBridgeStats().reason);
                const bool learning_changed_input=native_layout_change && iteration>=80 && iteration<=84 && st.native_deliveries==late_deliveries;
                if(st.native_deliveries && !learning_changed_input){
                    CHECK(!native_overwrite && !native_reverse && !native_tail);                    CHECK(st.native_deliveries==++late_deliveries);
                    // The evaluation's own pixels, through J2's normal write-back, the game's HUD,
                    // and the game's explicit UNORM -> FP16 conversion into the presented image.
                    const auto q=[](float v){return std::round(v*255.f)/255.f;};
                    const auto world=ReadTexel(d,presented.Get(),OW-1,OH-1),hud=ReadTexel(d,presented.Get(),1,1);
                    const auto game=ReadTexel(d,native.Get(),OW-1,OH-1);
                    std::printf("se5 iteration %u: pattern %.3f %.3f %.3f native %.3f %.3f %.3f presented %.4f %.4f %.4f hud %.2f %.2f %.2f aux %s\n",iteration,
                        g_pattern[0],g_pattern[1],g_pattern[2],game[0],game[1],game[2],world[0],world[1],world[2],hud[0],hud[1],hud[2],iteration%2?"no":"yes");
                    (void)q;
                    for(int c=0;c<3;++c){
                        CHECK(std::fabs(game[c]-g_pattern[c])<1.f/255.f+.001f);          // 8-bit store of the pattern
                        CHECK(std::fabs(world[c]-std::pow(game[c],2.2f))<.003f);           // the game's conversion of it
                    }
                }else{
                    if(late_deliveries)std::printf("se5 iteration %u: no delivery: j2 \"%s\" bridge \"%s\"\n",iteration,st.reason,na::j2::GetNativeBridgeStats().reason);
                    CHECK(fake::evals==before_native+int(late_deliveries));
                    if((native_reverse || native_tail) && final_recorded){
                        const auto world=ReadTexel(d,presented.Get(),OW-1,OH-1);
                        const auto game=ReadTexel(d,native.Get(),OW-1,OH-1);
                        CHECK(!st.active && st.frames==1);
                        for(int c=0;c<3;++c){
                            CHECK(std::fabs(game[c]-std::sqrt(.3f))<.005f);
                            CHECK(std::fabs(world[c]-std::pow(game[c],2.2f))<.003f);
                        }
                    }                    if(native_overwrite&&final_recorded){
                        const auto world=ReadTexel(d,presented.Get(),OW-1,OH-1);
                        CHECK(world[0]==0&&world[1]==0&&world[2]==1);
                    }
                }
                continue;
            }
            if(native_late){
                na::j2::NativeTracePresent(d.queue.Get(),presented.Get(),!native_se5);
                if(st.native_deliveries){
                    CHECK(st.native_deliveries==++late_deliveries);
                    CHECK(st.active&&st.render_w==W&&st.render_h==H&&st.out_w==OW&&st.out_h==OH);
                    const auto p=ReadTexel(d,presented.Get(),OW-1,OH-1),hud=ReadTexel(d,presented.Get(),1,1);
                    const auto untouched=ReadTexel(d,decoy.Get(),OW-1,OH-1);
                    CHECK(p[0]==0 && p[3]==1 && hud[1]==1 && untouched[0]>.5f);
                }else CHECK(fake::evals==before_native);
                continue;
            }
            std::printf("native scale %s iteration%u: evaluations=%d %ux%u->%ux%u active=%u reason=%s\n",native_merged?"merged":"separate",iteration,fake::evals-before_native,st.render_w,st.render_h,st.out_w,st.out_h,st.active,st.reason);
            CHECK(st.active&&st.render_w==W&&st.render_h==H&&st.out_w==OW&&st.out_h==OH);
            const auto corner_native=ReadTexel(d,native.Get(),OW-1,OH-1),hud_native=ReadTexel(d,native.Get(),1,1);
            CHECK(corner_native[0]==0&&corner_native[3]==1);CHECK(hud_native[1]==1);
        }
        if(native_late){
            char trace[2048];uint64_t traces=0;na::j2::NativeTraceResult(trace,sizeof trace,&traces);
            std::printf("learned native Quality deliveries %u, traces %llu: %s\n",late_deliveries,(unsigned long long)traces,trace);
            const auto nbs=na::j2::GetNativeBridgeStats();
            if(native_se5)std::printf("se5 delivery: delivered %llu first %llu best_streak %llu order_refused %llu later_written %llu probe{%s} refusals{%s}\n",
                (unsigned long long)nbs.delivered,(unsigned long long)nbs.first_delivery_frame,(unsigned long long)nbs.best_streak,
                (unsigned long long)nbs.order_refused,(unsigned long long)nbs.later_written,nbs.probe,nbs.refusals);
            if(native_session){
                std::printf("se5 session: deliveries %u trace{%.600s}\n",late_deliveries,trace);
                CHECK(std::strstr(trace,"partial_lookpast=")!=nullptr && std::strstr(trace,"crossing=[draw")!=nullptr && std::strstr(trace,"learned=0")==nullptr);
                CHECK(late_deliveries>=2);
            }else if(native_unseen){
                std::printf("se5 unseen: deliveries %u probe{%s} trace{%.400s}\n",late_deliveries,nbs.probe,trace);
                CHECK(std::strstr(nbs.probe,"verified=1")!=nullptr && std::strstr(nbs.probe,"refuted=1")!=nullptr);
                CHECK(late_deliveries>=2);
            }else if(native_bindless){
                // Unreadable presenting dispatch: the decoy crossing was refuted by the
                // screen probe, the game's upscale step confirmed, then delivered.
                std::printf("se5 bindless: deliveries %u probe{%s} trace{%s}\n",late_deliveries,nbs.probe,trace);
                CHECK(std::strstr(trace,"presented_writer=none")!=nullptr && std::strstr(trace,"refuted=1")!=nullptr);
                CHECK(std::strstr(nbs.probe,"verified=1")!=nullptr && std::strstr(nbs.probe,"refuted=1")!=nullptr);
                CHECK(late_deliveries>=2);
            }else if(native_stale){
                std::printf("se5 stale: overwrite from iteration %u, deliveries %u, stages{%s}\n",stale_from,late_deliveries,nbs.stages);
                CHECK(stale_from>0 && late_deliveries>=5);
                CHECK(std::strstr(trace,"invalidations=0 ")==nullptr && std::strstr(trace,"learned=0")!=nullptr);
            }else if(native_replace){
                // Delivered and submitted, but the game's later write replaced it: the probe says so.
                CHECK(late_deliveries>=2);

            }else if(false){ // the delivery probe is a fallback only now (no automatic delivery session)
            }else if(native_se5){
                // The fixture arms the delivery probe (RequestDeliveryProbe); a confirmed verdict is a
                // valid outcome, an unresolved one too (the probe is not a delivery condition).
                CHECK(std::strstr(nbs.probe,"verdict=")!=nullptr);
            }
            if(native_overwrite){
                // No false confirmation: nothing learned, nothing evaluated or delivered.
                CHECK(late_deliveries==0 && fake::evals==before_native && std::strstr(trace,"learned=0")!=nullptr);
            }else if(native_reverse || native_tail){
                CHECK(late_deliveries==0 && fake::evals==before_native && std::strstr(trace,"handoff=[")!=nullptr);
                const auto nb=na::j2::GetNativeBridgeStats();
                CHECK(nb.after_skipped>0);
                CHECK(std::strstr(nb.reason,native_reverse?"not submitted before":"no list boundary")!=nullptr);            }else{
                if(native_se5&&!native_tail)CHECK(nbs.delivered==late_deliveries && nbs.order_refused==0 && (late_deliveries<2||nbs.best_streak>=2));
                if(native_se5){
                    // Learned through the once-recorded presenting list (cached, last write),
                    // with the typed UNORM -> FP16 handoff named.
                    // (stale: the last trace is the invalidating one, without a crossing)
                    if(!native_stale&&!native_bindless&&!native_unseen&&!native_session)CHECK(std::strstr(trace,"handoff=[")!=nullptr && std::strstr(trace,"transfer=shader_defined")!=nullptr);
                    if(!native_bindless&&!native_unseen)CHECK(std::strstr(trace,"cached=0 ")==nullptr);
                    // Late-bound table: exactly the upscale dispatch changed between recording
                    // and submission; with tables written up front nothing changes.
                    std::printf("se5 late tables: %s\n",std::strstr(trace," late=")?std::strstr(trace," late="):"(absent)");
                    CHECK(std::strstr(trace,native_latebind?" late=1/":" late=0/")!=nullptr);
                    if(native_latebind){
                        const auto nl=na::j2::GetNativeBridgeStats();
                        std::printf("se5 latebind: staged %llu changed %llu same %llu resolved %llu inline %llu\n",(unsigned long long)nl.st_late_staged,
                            (unsigned long long)nl.st_late_changed,(unsigned long long)nl.st_late_same,(unsigned long long)nl.st_late_resolved,(unsigned long long)nl.st_late_inline);
                        // Every delivery came through a late-bound route: inline with the input chosen by
                        // this recording's writes, or staged and resolved at submission (never by a stale table).
                        CHECK(nl.st_late_inline+nl.st_late_resolved>=late_deliveries && nl.st_late_same==0);
                        if(native_layout_change){
                            CHECK(nl.st_late_inline>0);
                            CHECK(nl.st_late_changed<=nl.st_late_staged && nl.st_late_staged-nl.st_late_changed<=5);
                        }else CHECK(nl.st_late_changed==nl.st_late_staged);
                    }
                    CHECK(g_pattern_writes>=int(late_deliveries));
                    // Deliveries on frames with the auxiliary input were counted.
                    const auto nb=na::j2::GetNativeBridgeStats();
                    std::printf("se5 stretch: candidates %llu recorded %llu aux %llu\n",(unsigned long long)nb.st_candidates,(unsigned long long)nb.st_recorded,(unsigned long long)nb.st_aux);
                    // (the inline late-bound route picks its input without the table, so it counts no aux)
                    if(!(native_latebind&&nb.st_late_inline>0))CHECK(nb.st_aux>0 && nb.st_aux<nb.st_candidates);
                }
                CHECK(late_deliveries>=2 && fake::evals==before_native+int(late_deliveries));
            }
        }else CHECK(fake::evals==before_native+3);
        CHECK(dev->GetDeviceRemovedReason()==S_OK);
        if(native_close_race){
            CHECK(close_race->closed==iterations-80);
            na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,{});
        }
        if(witness_window){final_list.list.Reset();presented.Reset();witness_chain.Reset();DestroyWindow(witness_window);}
        fake::eval_output=nullptr;
        if(native_inherited){
            CHECK(na::j2::GetNativeBridgeStats().snap_depth_unknown>0);
            inherited_depth_state(false);
        }
        if(native_se5 && std::strstr(na::j2::GetNativeBridgeStats().stages,"learned=1")){
            na::j2::SetNativeRenderExtent(W-1,H,OW,OH);
            na::j2::NativeTracePresent(d.queue.Get(),presented.Get(),false);
            CHECK(std::strstr(na::j2::GetNativeBridgeStats().stages,"learned=0")!=nullptr);
        }
        // Restore DLAA while the last post list remains closed/replayable. GPU
        // completion, rather than requiring its future Reset, permits resizing.
        na::j2::SetNativeRenderExtent(W,H,W,H);na::j2::OnPresent(d.queue.Get());frame(.5);
        st=na::j2::GetStats();CHECK(st.active&&st.render_w==W&&st.render_h==H&&st.out_w==W&&st.out_h==H);
        if(!native_reverse)CHECK(fake::evals==before_native+(native_late?int(late_deliveries):3)+1);
        test::ComPtr<ID3D12InfoQueue> info;
        if (debug && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&info)))) {
            for (UINT64 i=0;i<info->GetNumStoredMessages();++i) {
                SIZE_T size=0;
                info->GetMessage(i,nullptr,&size);
                std::vector<uint8_t> storage(size);
                auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
                    std::printf("D3D12 error %u: %s\n",unsigned(message->ID),message->pDescription);
                    CHECK(false);
                }
            }
        }
        na::j2::SetEnabled(false);d.WaitIdle();na::j2::Shutdown();na::dlss::Shutdown();na::dlss::SetInitOwnOverrideForTests(nullptr);na_observe_stop();na::observe::SetProcessExiting();
        return test::finish("j2_native_scale_test");
    }

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
    const auto sharp_before = na::j2::GetStats().sharpened_frames;
    const auto saved_sharpness = na::settings::Get().j2_sharpness.exchange(75);
    frame(0.5); // camera moved a little: same history
    na::settings::Get().j2_sharpness.store(saved_sharpness);
    st = na::j2::GetStats();
    CHECK(st.sharpened_frames == sharp_before + 1 && st.nis_frames == 0);
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
    frame(500); // teleport: held for one frame (could be another camera for a frame)...
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
    na::j2::OnPresent(d.queue.Get());
    frame(500.01); // ...and confirmed by the next camera: history reset
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1);
    na::j2::OnPresent(d.queue.Get());
    na::observe::SetLiveSelection(0, 0);
    frame(500.5); // an unprocessed game frame must not advance camera history...
    const auto before_gap = na::j2::GetStats().frames;
    na::j2::OnPresent(d.queue.Get());
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()), na::observe::IdOf(color.Get()));
    frame(500.6);
    CHECK(na::j2::GetStats().frames == before_gap + 1);
    // ...but it does not reset either: DLSS's history and the tracker's previous
    // camera are both the last evaluated frame, so motion stays correct.
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
    // A long gap (more than 4 unprocessed game frames) starts over.
    na::observe::SetLiveSelection(0, 0);
    for (int k = 0; k < 6; ++k) { na::j2::OnPresent(d.queue.Get()); frame(500.7); }
    na::j2::OnPresent(d.queue.Get());
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()), na::observe::IdOf(color.Get()));
    frame(501);
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

    // Several valid cameras in one frame (Sniper Elite 5: a water reflection's camera, the sky's;
    // any engine: shadow, environment, scope cameras). The other camera is bound in the learned
    // camera slot first and more often; J2 must stay on the camera continuing the last main one:
    // no history reset, the other block never jittered.
    {
        const auto before = na::j2::GetStats();
        for (int k = 1; k <= 3; ++k) {
            na::j2::OnPresent(d.queue.Get());
            frame(501.3 + k * 0.1, nullptr, true);
            CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
            uint8_t other[64];
            Store(other, Reflection(501.3 + k * 0.1));
            CHECK(std::memcmp(cpu + 1536 + 80, other, 64) == 0); // J1 left the other camera alone
        }
        const auto after = na::j2::GetStats();
        std::printf("several cameras: frames +%llu cuts +%llu contested +%llu alternatives +%llu jitter_slot_other +%llu \"%s\"\n",
                    (unsigned long long)(after.frames - before.frames), (unsigned long long)(after.cuts - before.cuts),
                    (unsigned long long)(after.camera_contested - before.camera_contested),
                    (unsigned long long)(after.camera_alternatives - before.camera_alternatives),
                    (unsigned long long)(after.jitter_slot_other_camera - before.jitter_slot_other_camera), after.reason);
        CHECK(after.frames == before.frames + 3);
        CHECK(after.cuts == before.cuts);
        CHECK(after.camera_contested >= before.camera_contested + 3);
        CHECK(after.camera_alternatives >= before.camera_alternatives + 3); // the first-ranked block was not chosen
    }

    // P4-4 Stage 1: upscaling into a distinct full-size destination (slider route).
    // The depth debug view gives a known output (depth cleared to 0 -> black, alpha 1),
    // so the readback shows that J2's full-size result reached every corner of `dest`.
    {
        constexpr uint32_t OW = 96, OH = 48; // 1.5x: DLSS Quality
        auto dest = test::CreateTexture(dev, OW, OH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto ldr = test::CreateTexture(dev, OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        REQUIRE(dest && ldr);
        {   // fill dest with grey so a missing copy shows
            test::ComPtr<ID3D12DescriptorHeap> h;
            D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&h))));
            dev->CreateRenderTargetView(dest.Get(), nullptr, h->GetCPUDescriptorHandleForHeapStart());
            test::List c;
            REQUIRE(c.Create(dev));
            D3D12_RESOURCE_BARRIER b = test::Transition(dest.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            c.list->ResourceBarrier(1, &b);
            const float grey[4] = {0.5f, 0.5f, 0.5f, 0.5f};
            c.list->ClearRenderTargetView(h->GetCPUDescriptorHandleForHeapStart(), grey, 0, nullptr);
            b = test::Transition(dest.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            c.list->ResourceBarrier(1, &b);
            REQUIRE(SUCCEEDED(c.list->Close()));
            ID3D12CommandList* ls[] = {c.list.Get()};
            d.queue->ExecuteCommandLists(1, ls);
            d.WaitIdle();
        }
        const auto before = ReadTexel(d, dest.Get(), OW - 1, OH - 1);
        CHECK(std::fabs(before[0] - 0.5f) < 0.01f);

        na::j2::SetDebugView(3);
        na::j2::SetUpscaleTarget({na::observe::IdOf(dest.Get()), 0, 0, 0});
        const auto up0 = na::j2::GetStats().upscaled_frames;
        const int ev = fake::evals;
        na::j2::OnPresent(d.queue.Get());
        frame(503, dest.Get());
        st = na::j2::GetStats();
        std::printf("upscale: %ux%u -> %ux%u, upscaled %llu, phases %u, note \"%s\", reason \"%s\"\n", st.render_w, st.render_h, st.out_w, st.out_h,
                    static_cast<unsigned long long>(st.upscaled_frames), st.jitter_phases, st.upscale_note, st.reason);
        CHECK(fake::evals == ev + 1 && st.upscaled_frames == up0 + 1);
        CHECK(st.render_w == W && st.render_h == H && st.out_w == OW && st.out_h == OH);
        CHECK(st.jitter_phases == 18); // ceil(8 * 1.5^2)
        CHECK(fake::U(fake::last_create, NVSDK_NGX_Parameter_Width) == W && fake::U(fake::last_create, NVSDK_NGX_Parameter_Height) == H);
        CHECK(fake::U(fake::last_create, NVSDK_NGX_Parameter_OutWidth) == OW && fake::U(fake::last_create, NVSDK_NGX_Parameter_OutHeight) == OH);
        CHECK(fake::I(fake::last_create, NVSDK_NGX_Parameter_PerfQualityValue) == static_cast<int>(na::dlss::Quality::MaxQuality));
        CHECK(fake::U(fake::last_eval, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) == W);
        void* o = fake::P(fake::last_eval, NVSDK_NGX_Parameter_Output);
        CHECK(o != dest.Get() && o != color.Get()); // J2's own full-size target, then copied
        CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 1); // new target: history restarts
        const auto corner = ReadTexel(d, dest.Get(), OW - 1, OH - 1), origin = ReadTexel(d, dest.Get(), 0, 0);
        std::printf("upscale dest: corner %.3f %.3f %.3f %.3f, origin %.3f %.3f %.3f %.3f\n", corner[0], corner[1], corner[2], corner[3],
                    origin[0], origin[1], origin[2], origin[3]);
        CHECK(corner[0] == 0.0f && corner[3] == 1.0f && origin[0] == 0.0f && origin[3] == 1.0f);

        // Active extent smaller than the allocation (render at the top left): 48x24 -> 96x48.
        na::j2::SetUpscaleTarget({na::observe::IdOf(dest.Get()), 0, 48, 24});
        na::j2::OnPresent(d.queue.Get());
        frame(503.1, dest.Get());
        st = na::j2::GetStats();
        CHECK(st.render_w == 48 && st.render_h == 24 && st.out_w == OW && st.upscaled_frames == up0 + 2);
        CHECK(st.jitter_phases == 32);
        CHECK(fake::U(fake::last_eval, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) == 48);
        CHECK(fake::I(fake::last_create, NVSDK_NGX_Parameter_PerfQualityValue) == static_cast<int>(na::dlss::Quality::MaxPerformance));
        // Jitter stays inside a render pixel and the next frame keeps history.
        na::j2::OnPresent(d.queue.Get());
        frame(503.2, dest.Get());
        CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
        CHECK(std::fabs(fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X)) < 0.5);

        // A destination J2 cannot use (different format) falls back to DLAA, with a reason.
        const auto refused0 = na::j2::GetStats().upscale_refused;
        na::j2::SetUpscaleTarget({na::observe::IdOf(ldr.Get()), 0, 0, 0});
        na::j2::OnPresent(d.queue.Get());
        frame(503.3, ldr.Get());
        st = na::j2::GetStats();
        std::printf("refused: \"%s\", out %ux%u\n", st.upscale_note, st.out_w, st.out_h);
        CHECK(st.upscale_refused == refused0 + 1 && st.upscale_note[0] != 0);
        CHECK(st.out_w == W && st.out_h == H && st.upscaled_frames == up0 + 3);
        CHECK(fake::U(fake::last_create, NVSDK_NGX_Parameter_OutWidth) == W);
        na::j2::SetUpscaleTarget({});

        // P4-4 Layer2 physical-input override (Astra's no-slider route). J2 evaluates private
        // companions (not observed, states supplied) into the snapshot's full-size output; the
        // logical ids stay the identity. Provided / Unavailable / Failed each get one outcome.
        {
            auto pc = test::CreateTexture(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
            auto pd = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            REQUIRE(pc && pd);
            struct Ctx {
                int mode = 0; // 0 provide, 1 unavailable, 2 provide with the wrong logical id
                std::shared_ptr<na::j2::PhysicalInput> payload;
                int outcomes = 0;
                uint64_t last_gen = 0;
                na::j2::PhysicalOutcome last = na::j2::PhysicalOutcome::Failed;
                uint64_t seen_color = 0, seen_depth = 0;
            } ctx;
            ctx.payload = std::make_shared<na::j2::PhysicalInput>();
            auto& pi = *ctx.payload;
            pi.generation = 7;
            pi.color = pc.Get();
            pi.depth = pd.Get();
            pi.color_state = pi.depth_state = pi.output_state = D3D12_RESOURCE_STATE_COMMON;
            pi.render_w = 40;
            pi.render_h = 20;
            pi.output = dest.Get();
            pi.owner = std::shared_ptr<void>(pc.Get(), [](void*) {}); // the test owns pc/pd/dest
            na::j2::PhysicalInputProvider prov;
            prov.ctx = &ctx;
            prov.lookup = [](void* c, uint64_t lc, uint64_t ld, ID3D12GraphicsCommandList*, std::shared_ptr<const na::j2::PhysicalInput>* out) {
                auto& x = *static_cast<Ctx*>(c);
                x.seen_color = lc;
                x.seen_depth = ld;
                if (x.mode == 1) return na::j2::PhysicalLookup::Unavailable;
                x.payload->logical_color = x.mode == 2 ? lc + 1 : lc;
                x.payload->logical_depth = ld;
                *out = x.payload;
                return na::j2::PhysicalLookup::Provided;
            };
            prov.outcome = [](void* c, uint64_t gen, na::j2::PhysicalOutcome r, const char*) {
                auto& x = *static_cast<Ctx*>(c);
                ++x.outcomes;
                x.last_gen = gen;
                x.last = r;
            };
            na::j2::SetPhysicalInputProvider(prov);
            const auto st0 = na::j2::GetStats();
            int ev2 = fake::evals;
            na::j2::OnPresent(d.queue.Get());
            frame(504);
            st = na::j2::GetStats();
            std::printf("physical: evaluated %llu, %ux%u -> %ux%u, outcomes %d, reason \"%s\"\n", (unsigned long long)st.physical_evaluated,
                        st.render_w, st.render_h, st.out_w, st.out_h, ctx.outcomes, st.reason);
            CHECK(ctx.seen_color == na::observe::IdOf(color.Get()) && ctx.seen_depth == na::observe::IdOf(depth.Get())); // logical identity
            CHECK(fake::evals == ev2 + 1 && st.physical_evaluated == st0.physical_evaluated + 1);
            CHECK(ctx.outcomes == 1 && ctx.last == na::j2::PhysicalOutcome::Evaluated && ctx.last_gen == 7);
            CHECK(st.render_w == 40 && st.render_h == 20 && st.out_w == OW && st.out_h == OH);
            const auto pcorner = ReadTexel(d, dest.Get(), OW - 1, OH - 1);
            CHECK(pcorner[0] == 0.0f && pcorner[3] == 1.0f); // written into the snapshot's output
            // Redirected but unavailable: nothing evaluated, the owner is told.
            ctx.mode = 1;
            ev2 = fake::evals;
            na::j2::OnPresent(d.queue.Get());
            frame(504.1);
            st = na::j2::GetStats();
            CHECK(fake::evals == ev2 && ctx.outcomes == 2 && ctx.last == na::j2::PhysicalOutcome::Unavailable);
            CHECK(st.physical_unavailable == st0.physical_unavailable + 1);
            // Provided but naming other logical ids: refused, reported Failed, never drawn as DLAA.
            ctx.mode = 2;
            ev2 = fake::evals;
            na::j2::OnPresent(d.queue.Get());
            frame(504.2);
            st = na::j2::GetStats();
            CHECK(fake::evals == ev2 && ctx.outcomes == 3 && ctx.last == na::j2::PhysicalOutcome::Failed);
            CHECK(st.physical_failed == st0.physical_failed + 1);
            na::j2::SetPhysicalInputProvider({});
            d.WaitIdle();
        }
        na::j2::SetDebugView(0);
        CHECK(na::j2::JitterPhasesFor(64, 64) == 16 && na::j2::JitterPhasesFor(10, 100) == 64);
        CHECK(std::fabs(na::j2::UpscaleMipBias(64, 64) + 1.0f) < 1e-6f && std::fabs(na::j2::UpscaleMipBias(50, 100) + 2.0f) < 1e-6f);
        d.WaitIdle();
    }
    // Exercise the PRODUCTION pre-HUD upscaler callback with real deferred J2
    // depth/motion acquisition. The model is fake; native replay mechanics have
    // their actual indexed/stripped-shader GPU coverage in j2_native_hud_test.
    {
        namespace presenter=na::present;namespace hud=na::j2::native_hud;
        HWND window=test::CreateTestWindow(L"pre-HUD callback",128,64);REQUIRE(window);
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=128;desc.Height=64;desc.BufferCount=2;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        test::ComPtr<IDXGISwapChain1> chain;REQUIRE(SUCCEEDED(d.factory->CreateSwapChainForHwnd(d.queue.Get(),window,&desc,nullptr,nullptr,&chain)));
        presenter::Originals originals{};originals.get_buffer=[](IDXGISwapChain* sc,UINT index,REFIID riid,void** out)->HRESULT{return sc->GetBuffer(index,riid,out);};
        presenter::SetOriginals(originals);presenter::SetRenderScale(.5f,&na::j2::PresentUpscale,nullptr);presenter::SetEnabled(true);
        REQUIRE(na::native_present::Attach(window,{128,64},{64,32}));
        test::ComPtr<ID3D12Resource> proxy;HRESULT hr=E_FAIL;
        REQUIRE(presenter::HandleGetBuffer(chain.Get(),0,IID_PPV_ARGS(&proxy),&hr)&&SUCCEEDED(hr)&&presenter::Scaling());
        na::j2::SetRenderSmall(true);na::j2::OnPresent(d.queue.Get());
        const int before=fake::evals;REQUIRE(frame(505)==0);CHECK(fake::evals==before); // deferred, no early NGX
        constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,R=D3D12_RESOURCE_STATE_RENDER_TARGET;
        auto native=test::CreateTexture(dev,W*2,H*2,DXGI_FORMAT_R16G16B16A16_FLOAT,S,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto n0=test::CreateTexture(dev,W*2,H*2,DXGI_FORMAT_R16G16B16A16_FLOAT,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto n1=test::CreateTexture(dev,W*2,H*2,DXGI_FORMAT_R16G16B16A16_FLOAT,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);REQUIRE(native&&n0&&n1);
        auto picture=hud::Picture::Create(dev,color->GetDesc(),W*2,H*2);REQUIRE(picture);
        test::ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hud_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hud_heap_desc,IID_PPV_ARGS(&heap))));auto handle=heap->GetCPUDescriptorHandleForHeapStart();const UINT inc=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        test::List seed,replay;REQUIRE(seed.Create(dev)&&replay.Create(dev));const float black[4]{},grey[4]={1,1,1,0},marker[4]={.25f,.5f,.75f,1};
        dev->CreateRenderTargetView(n0.Get(),nullptr,handle);seed.list->ClearRenderTargetView(handle,black,0,nullptr);handle.ptr+=inc;
        dev->CreateRenderTargetView(n1.Get(),nullptr,handle);seed.list->ClearRenderTargetView(handle,grey,0,nullptr);handle.ptr+=inc;
        REQUIRE(picture->SnapshotScene(seed.list.Get(),color.Get(),D3D12_RESOURCE_STATE_COMMON)&&picture->SnapshotFull(seed.list.Get(),color.Get(),D3D12_RESOURCE_STATE_COMMON));
        REQUIRE(SUCCEEDED(seed.list->Close()));ID3D12CommandList* seeds[]={seed.list.Get()};d.queue->ExecuteCommandLists(1,seeds);d.WaitIdle();
        dev->CreateRenderTargetView(native.Get(),nullptr,handle);auto barrier=test::Transition(native.Get(),D3D12_RESOURCE_STATE_PRESENT,R);replay.list->ResourceBarrier(1,&barrier);
        replay.list->ClearRenderTargetView(handle,marker,0,nullptr);barrier=test::Transition(native.Get(),R,S);replay.list->ResourceBarrier(1,&barrier);REQUIRE(SUCCEEDED(replay.list->Close()));
        hud::Layer layer;layer.n0=n0.Get();layer.n1=n1.Get();layer.view=DXGI_FORMAT_R16G16B16A16_FLOAT;layer.grey=1;layer.picture=picture;layer.clean_allowed=true;layer.owner=picture;
        layer.recording_identity=std::make_shared<na::capture::j2::Identity>();
        REQUIRE(na::j2::PreHudUpscale(d.queue.Get(),color.Get(),W,H,native.Get(),W*2,H*2,layer,replay.list.Get()));d.WaitIdle();
        CHECK(fake::evals==before+1&&layer.recording_identity->evaluation_succeeded);
        CHECK(fake::U(fake::last_create,NVSDK_NGX_Parameter_OutWidth)==W*2);
        CHECK(!na::j2::PreHudUpscale(d.queue.Get(),color.Get(),W,H,native.Get(),W*2,H*2,layer,replay.list.Get())); // guides consumed once
        test::List handback;REQUIRE(handback.Create(dev));barrier=test::Transition(native.Get(),S,D3D12_RESOURCE_STATE_COMMON);handback.list->ResourceBarrier(1,&barrier);
        REQUIRE(SUCCEEDED(handback.list->Close()));ID3D12CommandList* finish[]={handback.list.Get()};d.queue->ExecuteCommandLists(1,finish);d.WaitIdle();
        const auto pixel=ReadTexel(d,native.Get(),W,H);CHECK(pixel[0]==.25f&&pixel[1]==.5f&&pixel[2]==.75f&&pixel[3]==1);
        // Actual production branch for a UI-only texture: NGX consumes the
        // independent world snapshot, and glyph replay gets a separate seed.
        na::j2::OnPresent(d.queue.Get());const int world_before=fake::evals;REQUIRE(frame(505.1)==0);CHECK(fake::evals==world_before);
        auto ui_source=test::CreateTexture(dev,W,H,DXGI_FORMAT_R16G16B16A16_FLOAT,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto ui_native=test::CreateTexture(dev,W*2,H*2,DXGI_FORMAT_R16G16B16A16_FLOAT,S,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);REQUIRE(ui_source&&ui_native);
        auto ui_picture=hud::Picture::Create(dev,ui_source->GetDesc(),W*2,H*2),world_picture=hud::Picture::Create(dev,color->GetDesc(),W*2,H*2);REQUIRE(ui_picture&&world_picture);
        test::List ui_seed,ui_replay;REQUIRE(ui_seed.Create(dev)&&ui_replay.Create(dev));
        dev->CreateRenderTargetView(ui_source.Get(),nullptr,handle);ui_seed.list->ClearRenderTargetView(handle,black,0,nullptr);
        REQUIRE(ui_picture->SnapshotScene(ui_seed.list.Get(),ui_source.Get(),R)&&ui_picture->SnapshotFull(ui_seed.list.Get(),ui_source.Get(),R));
        REQUIRE(world_picture->SnapshotScene(ui_seed.list.Get(),color.Get(),D3D12_RESOURCE_STATE_COMMON));
        barrier=test::Transition(native.Get(),D3D12_RESOURCE_STATE_COMMON,S);ui_seed.list->ResourceBarrier(1,&barrier);
        REQUIRE(SUCCEEDED(ui_seed.list->Close()));ID3D12CommandList* initial[]={ui_seed.list.Get()};d.queue->ExecuteCommandLists(1,initial);d.WaitIdle();
        dev->CreateRenderTargetView(ui_native.Get(),nullptr,handle);barrier=test::Transition(ui_native.Get(),D3D12_RESOURCE_STATE_PRESENT,R);ui_replay.list->ResourceBarrier(1,&barrier);
        ui_replay.list->ClearRenderTargetView(handle,marker,0,nullptr);barrier=test::Transition(ui_native.Get(),R,S);ui_replay.list->ResourceBarrier(1,&barrier);
        barrier=test::Transition(native.Get(),D3D12_RESOURCE_STATE_PRESENT,S);ui_replay.list->ResourceBarrier(1,&barrier);REQUIRE(SUCCEEDED(ui_replay.list->Close()));
        hud::Layer separate=layer;separate.picture=ui_picture;separate.reconstruction=world_picture;separate.ui_output=ui_native.Get();separate.ui_view=DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto old_shift=na::settings::Get().j2_jitter_shift.exchange(0);
        REQUIRE(na::j2::PreHudUpscale(d.queue.Get(),color.Get(),W,H,native.Get(),W*2,H*2,separate,ui_replay.list.Get()));d.WaitIdle();
        CHECK(fake::evals==world_before+1&&fake::P(fake::last_eval,NVSDK_NGX_Parameter_Color)==world_picture->Scene());
        na::settings::Get().j2_jitter_shift.store(old_shift);
        test::List ui_handback;REQUIRE(ui_handback.Create(dev));D3D12_RESOURCE_BARRIER returned[]={test::Transition(native.Get(),S,D3D12_RESOURCE_STATE_COMMON),test::Transition(ui_native.Get(),S,D3D12_RESOURCE_STATE_COMMON)};
        ui_handback.list->ResourceBarrier(2,returned);REQUIRE(SUCCEEDED(ui_handback.list->Close()));ID3D12CommandList* returned_lists[]={ui_handback.list.Get()};d.queue->ExecuteCommandLists(1,returned_lists);d.WaitIdle();
        const auto ui_pixel=ReadTexel(d,ui_native.Get(),W,H);CHECK(ui_pixel[0]==.25f&&ui_pixel[1]==.5f&&ui_pixel[2]==.75f&&ui_pixel[3]==1);
        // Production automatic route: a shadow native/check replay MUST NOT
        // evaluate NGX or consume this frame's guides. Exhausting all four HUD
        // methods MUST still permit an ordinary Present DLSS evaluation.
        hud::Install();hud::SetAutomatic(true);na::j2::OnPresent(d.queue.Get());REQUIRE(frame(505.2)==0);
        const int automatic_before=fake::evals;
        auto auto_picture=hud::Picture::Create(dev,color->GetDesc(),W*2,H*2);REQUIRE(auto_picture);
        test::List auto_seed;REQUIRE(auto_seed.Create(dev));
        REQUIRE(auto_picture->SnapshotScene(auto_seed.list.Get(),color.Get(),D3D12_RESOURCE_STATE_COMMON)&&auto_picture->SnapshotFull(auto_seed.list.Get(),color.Get(),D3D12_RESOURCE_STATE_COMMON));
        barrier=test::Transition(native.Get(),D3D12_RESOURCE_STATE_COMMON,S);auto_seed.list->ResourceBarrier(1,&barrier);
        REQUIRE(SUCCEEDED(auto_seed.list->Close()));ID3D12CommandList* auto_seeds[]={auto_seed.list.Get()};d.queue->ExecuteCommandLists(1,auto_seeds);d.WaitIdle();
        hud::Layer auto_layer=layer;auto_layer.picture=auto_picture;auto_layer.owner=auto_picture;
        REQUIRE(na::j2::PreHudUpscale(d.queue.Get(),color.Get(),W,H,native.Get(),W*2,H*2,auto_layer,replay.list.Get()));d.WaitIdle();
        CHECK(fake::evals==automatic_before&&na::j2::Enabled());
        hud::AutomaticFrame(proxy.Get(),d.queue.Get(),W,H,W*2,H*2,false);
        for(int i=0;i<364;++i)hud::AutomaticFrame(proxy.Get(),d.queue.Get(),W,H,W*2,H*2,true);
        CHECK(hud::AutomaticMethod()==na::j2::hud_routes::Method::Off);
        auto ordinary=test::CreateTexture(dev,W*2,H*2,DXGI_FORMAT_R8G8B8A8_UNORM,S,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);REQUIRE(ordinary);
        na::j2::OnPresent(d.queue.Get());
        REQUIRE(na::j2::PresentUpscale(d.queue.Get(),proxy.Get(),W,H,ordinary.Get(),W*2,H*2,false,nullptr));d.WaitIdle();
        CHECK(fake::evals==automatic_before+1&&na::j2::Enabled());
        CHECK(hud::AutomaticMethod()==na::j2::hud_routes::Method::Off);
        std::printf("automatic HUD exhaustion: ordinary DLSS evaluated once; shadow replay evaluated zero times\n");
        hud::SetAutomatic(false);
        na::j2::SetRenderSmall(false);proxy.Reset();d.WaitIdle();REQUIRE(presenter::Shutdown());presenter::SetEnabled(false);presenter::SetRenderScale(1,nullptr,nullptr);na::native_present::Detach(window);chain.Reset();DestroyWindow(window);
    }
    if(nvrhi){na::gpu::Collect();CHECK(na::gpu::GetStats().submits>=2);CHECK(na::gpu::GetStats().errors==0);}
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    na::j2::SetEnabled(false);
    d.WaitIdle();
    na::j2::Shutdown();
    na::dlss::Shutdown(); // GPU idle (WaitIdle above): release the fake runtime's features before exit
    na::dlss::SetInitOwnOverrideForTests(nullptr);
    na_observe_stop(); // the transport thread must not outlive main
    na::observe::SetProcessExiting(); // same exit contract as the host before static GPU-owner teardown
    return test::finish("j2_live_test");
}
