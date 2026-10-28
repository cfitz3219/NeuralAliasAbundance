#include "host_dx12/settings.hpp"
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
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/observe/producers.hpp"

#include <d3dcompiler.h>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <windows.h>

namespace {
void __cdecl Sink(const na_event_batch*, void*) {}
std::vector<ID3D12CommandList*> g_forwarded;
void __stdcall PlainExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) { q->ExecuteCommandLists(n, lists); }
long FileSize(const std::wstring& path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}
std::string ReadText(const std::wstring& path)
{
    std::string out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}
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
M Proj()
{
    const double n = 0.5, f = 5000.0, sy = 1.0 / std::tan(0.45), sx = sy / 2.0;
    return M{sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, n / (n - f), n * f / (f - n), 0, 0, 1, 0};
}
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

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const UINT samples = argc > 1 ? UINT(std::atoi(argv[1])) : 1;
    const UINT depth_samples = argc > 2 ? UINT(std::atoi(argv[2])) : samples;
    const bool msaa = samples > 1;
    const bool real_ngx = argc > 3 && std::strcmp(argv[3], "--real") == 0;
    // --planes: depth's two planes (depth, stencil) are transitioned by separate
    // per-subresource barriers, as Sniper Elite Resistance does.
    const bool planes = argc > 3 && std::strcmp(argv[3], "--planes") == 0;
    // --split: the scene is drawn across two submissions per frame (Sniper Elite
    // Resistance); DLAA must follow the second one, before the post pass.
    const bool split = argc > 3 && std::strcmp(argv[3], "--split") == 0;
    // --inline: the scene and its post-processing are recorded in ONE command list
    // (Sniper Elite Resistance); DLAA must be recorded inside it, before the post
    // pass binds the colour for reading, and the game's state handed back.
    const bool inline_copy = argc > 3 && std::strcmp(argv[3], "--inline-copy") == 0;
    const bool inline_record_first = argc > 3 && std::strcmp(argv[3], "--inline-record-first") == 0;
    // --auto-inline / --auto-between: no override; the route selector must choose the
    // route from the frame structure (inline for scene+post in one list, between for
    // the Anno shape) after its learning frames.
    const bool auto_inline = argc > 3 && std::strcmp(argv[3], "--auto-inline") == 0;
    const bool auto_between = argc > 3 && std::strcmp(argv[3], "--auto-between") == 0;
    const bool inline_list = auto_inline || inline_record_first || inline_copy || (argc > 3 && std::strcmp(argv[3], "--inline") == 0);
    // auto-between needs an OBSERVABLE consumer (a copy of the colour): a barrier-only
    // hand-back is not evidence of a list boundary (Astra R3).
    const bool post_reader = argc > 3 && !real_ngx && !planes && !split && !inline_list;
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_anno_shape_test");
    if (real_ngx) {
        test::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        REQUIRE(SUCCEEDED(d.factory->EnumAdapterByLuid(d.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))));
        REQUIRE(SUCCEEDED(adapter->GetDesc1(&desc)));
        if (desc.VendorId != 0x10DE) { printf("SKIP: real DLAA needs NVIDIA hardware\n"); return test::kSkip; }
    }
    ID3D12Device* dev = d.device.Get();
    ID3D12CommandQueue* q = d.queue.Get();
    REQUIRE(na::observe::InstallProducers(dev, q, /*hook_queue_execute=*/false));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof(cfg);
    REQUIRE(na_observe_start(&cfg, &Sink, nullptr) == NA_OK);
    if (!real_ngx) na::dlss::SetInitOwnOverrideForTests([](ID3D12Device* device, const wchar_t*) { return na::dlss::Attach(device, fake::MakeApi()); });

    constexpr uint32_t W = 64, H = 32;
    for (auto pair : {std::make_pair(DXGI_FORMAT_R16G16B16A16_FLOAT,samples), std::make_pair(DXGI_FORMAT_D32_FLOAT_S8X24_UINT,depth_samples)}) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{pair.first,pair.second,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
        REQUIRE(SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&levels,sizeof levels)));
        if (!levels.NumQualityLevels) { printf("SKIP: hardware does not support sample count %u\n",pair.second); return test::kSkip; }
    }
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
        td.SampleDesc.Count = samples;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color))));
    }
    auto depth = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if (depth_samples > 1) {
        depth.Reset();
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto dd = color->GetDesc();
        dd.DepthOrArraySize = 1; dd.MipLevels = 1; dd.SampleDesc.Count = depth_samples; dd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
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
    dvd.ViewDimension = depth_samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
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
        // A mip-mapped static sampler: J2's texture mip bias must re-serialise the root signature cleanly.
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_ANISOTROPIC;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        ss.MaxAnisotropy = 8;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        na::observe::SetSamplerMipBias(-1.0f);
        D3D12_ROOT_SIGNATURE_DESC rd{1, &p, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        test::ComPtr<ID3DBlob> blob, err;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
        REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))));
        uint64_t biased = 0, biased_static = 0;
        na::observe::MipBiasCounts(&biased, &biased_static);
        CHECK(biased_static >= 1); // the static sampler got the bias and the rebuilt root signature was accepted
        // ...and a mip-mapped sampler descriptor, but never a comparison (shadow) sampler.
        test::ComPtr<ID3D12DescriptorHeap> sh;
        D3D12_DESCRIPTOR_HEAP_DESC shd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&sh))));
        D3D12_SAMPLER_DESC sd{};
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sd.MaxLOD = D3D12_FLOAT32_MAX;
        dev->CreateSampler(&sd, sh->GetCPUDescriptorHandleForHeapStart());
        uint64_t after = 0;
        na::observe::MipBiasCounts(&after, nullptr);
        CHECK(after == biased + 1);
        sd.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS;
        D3D12_CPU_DESCRIPTOR_HANDLE h2 = sh->GetCPUDescriptorHandleForHeapStart();
        h2.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        dev->CreateSampler(&sd, h2);
        uint64_t after2 = 0;
        na::observe::MipBiasCounts(&after2, nullptr);
        CHECK(after2 == after); // shadow comparison sampler untouched
        // A clamped linear sampler (screen-space post-processing) is left alone too.
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        dev->CreateSampler(&sd, h2);
        uint64_t after3 = 0;
        na::observe::MipBiasCounts(&after3, nullptr);
        CHECK(after3 == after2);
        // ...while an anisotropic one is biased whatever its addressing.
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = 8;
        dev->CreateSampler(&sd, h2);
        uint64_t after4 = 0;
        na::observe::MipBiasCounts(&after4, nullptr);
        CHECK(after4 == after3 + 1);
        // Aim for a total: a game's own -1.5 is already past -1 (left alone); its -0.5
        // is brought to -1 (biased); the native values are recorded either way.
        na::observe::NativeBiasStats n0;
        na::observe::NativeMipBiases(&n0);
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd.MaxAnisotropy = 1;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sd.MipLODBias = -1.5f;
        dev->CreateSampler(&sd, h2);
        uint64_t after5 = 0;
        na::observe::MipBiasCounts(&after5, nullptr);
        CHECK(after5 == after4);
        sd.MipLODBias = -0.5f;
        dev->CreateSampler(&sd, h2);
        uint64_t after6 = 0;
        na::observe::MipBiasCounts(&after6, nullptr);
        CHECK(after6 == after5 + 1);
        na::observe::NativeBiasStats n1;
        na::observe::NativeMipBiases(&n1);
        CHECK(n1.negative == n0.negative + 2);
        CHECK(n1.min <= -1.5f);
        sd.MipLODBias = 0.0f;
        na::observe::SetSamplerMipBias(0.0f);
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
    // A downstream consumer reads the scene then rebinds its RTV/depth. That
    // final bind must not move DLAA after the read. GPU copy stands in for the
    // consumer's filtering; readback below checks what the consumer received.
    auto post_color = test::CreateTexture(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(post_color);
    test::ComPtr<ID3D12DescriptorHeap> post_heap;
    test::ComPtr<ID3D12RootSignature> post_root;
    if (post_reader || inline_list) {
        REQUIRE(!msaa);
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&chd, IID_PPV_ARGS(&post_heap))));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MipLevels = 1; srv.Texture2DArray.FirstArraySlice = 1; srv.Texture2DArray.ArraySize = 1;
        dev->CreateShaderResourceView(color.Get(), &srv, post_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER p{}; p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = {1, &range};
        D3D12_ROOT_SIGNATURE_DESC rd{1, &p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        test::ComPtr<ID3DBlob> blob, error;
        REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)));
        REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&post_root))));
    }
    // Inline mode: the post pass draws into its own target (another resource).
    test::ComPtr<ID3D12PipelineState> post_pso;
    test::ComPtr<ID3D12Resource> post_rt;
    test::ComPtr<ID3D12DescriptorHeap> post_rtvh;
    D3D12_CPU_DESCRIPTOR_HANDLE post_rtv{};
    if (inline_list) {
        post_rt = test::CreateTexture(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        REQUIRE(post_rt);
        D3D12_DESCRIPTOR_HEAP_DESC phd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&phd, IID_PPV_ARGS(&post_rtvh))));
        post_rtv = post_rtvh->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(post_rt.Get(), nullptr, post_rtv);
        const char* shader = R"(
Texture2DArray<float4> image : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position { float2 p=float2((id<<1)&2,id&2); return float4(p*2-1,0,1); }
float4 PS(float4 pos : SV_Position) : SV_Target { return image.Load(int4(int2(pos.xy),0,0)); }
)";
        test::ComPtr<ID3DBlob> vs, ps, err;
        REQUIRE(SUCCEEDED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)));
        REQUIRE(SUCCEEDED(D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err)));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = post_root.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.SampleMask = UINT_MAX; pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&post_pso))));
    }
    uint8_t* cpu = nullptr;
    REQUIRE(SUCCEEDED(cb->Map(0, nullptr, reinterpret_cast<void**>(&cpu))) && cpu);

    // An earlier submission leaves colour in RENDER_TARGET and depth in DEPTH_WRITE.
    test::List setup, scene, consumer, prepass, scene2;
    REQUIRE(setup.Create(dev) && scene.Create(dev) && consumer.Create(dev) && prepass.Create(dev) && scene2.Create(dev));
    REQUIRE(SUCCEEDED(scene2.list->Close()));
    REQUIRE(SUCCEEDED(setup.list->Close()));
    {
        setup.Reset(); // recorded after a Reset, like a game's reused list
        D3D12_RESOURCE_BARRIER in[3] = {test::Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                        test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE)};
        if (planes) { // plane 0 (depth) and plane 1 (stencil) as separate subresources
            const D3D12_RESOURCE_DESC ddsc = depth->GetDesc();
            const UINT per_plane = UINT(ddsc.MipLevels ? ddsc.MipLevels : 1) * ddsc.DepthOrArraySize;
            in[1].Transition.Subresource = 0;
            in[2] = test::Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            in[2].Transition.Subresource = per_plane; // first subresource of plane 1
        }
        setup.list->ResourceBarrier(planes ? 3 : 2, in);
        const float red[4] = {1, 0, 0, 1};
        setup.list->ClearRenderTargetView(rtv0, red, 0, nullptr);
        REQUIRE(SUCCEEDED(setup.list->Close()));
        ID3D12CommandList* l[] = {setup.list.Get()};
        Submit(q, 1, l, false);
        d.WaitIdle();
    }
    REQUIRE(SUCCEEDED(scene.list->Close()) && SUCCEEDED(consumer.list->Close()) && SUCCEEDED(prepass.list->Close()));
    na::settings::Get().j2_inline.store(inline_list && !auto_inline);
    // Fixed-route tests force between-list insertion so their frame-1 expectations hold;
    // the auto modes exercise the selector.
    na::settings::Get().j2_route.store((auto_inline || auto_between || inline_list) ? 0u : 1u);
    na::j2::SetEnabled(true);
    na::j2::SetJitter(true); // J1 is off by default in the game; the tests exercise it
    na::observe::SetLiveSelection(na::observe::IdOf(depth.Get()), na::observe::IdOf(color.Get()));
    // Auto modes also exercise the J2 evidence capture: 3 consecutive evaluations, each
    // with its own slot, joined by the presented image (a stand-in in PRESENT state).
    const bool capture = auto_inline || auto_between;
    test::ComPtr<ID3D12Resource> presented;
    std::wstring cap_dir;
    if (capture) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC pd{};
        pd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        pd.Width = W; pd.Height = H; pd.DepthOrArraySize = 1; pd.MipLevels = 1;
        pd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        REQUIRE(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &pd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&presented))));
        wchar_t tmp[MAX_PATH];
        REQUIRE(GetTempPathW(MAX_PATH, tmp) > 0);
        cap_dir = std::wstring(tmp) + L"na_j2_capture_" + std::to_wstring(GetCurrentProcessId()) + (auto_inline ? L"_inline" : L"_between");
        na::capture::j2::Request(3, cap_dir.c_str(), 0);
    }
    const auto frame = [&](double cam_x, bool unpatched_prepass = false) {
        d.WaitIdle();
        const int frame_evals = fake::evals;
        if (inline_list) {
            // Pooled list objects alternate between an early write-only job and
            // the final scene/post job. Learning by object identity fails here.
            std::swap(scene, scene2);
            scene2.Reset();
            scene2.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            const float early[4] = {0, 0, 0, 1};
            scene2.list->ClearRenderTargetView(rtv, early, 0, nullptr);
            REQUIRE(SUCCEEDED(scene2.list->Close()));
            ID3D12CommandList* first[] = {scene2.list.Get()};
            Submit(q, 1, first, true);
        }
        std::memset(cpu, 0, 4096);
        const M vp = Camera(cam_x);
        M inv{};
        Inverse(vp, inv);
        for (int i = 0; i < 300; ++i) cpu[kBlock + i] = static_cast<uint8_t>(i * 13); // other constants before the camera
        Store(cpu + kBlock + kVp, vp);
        Store(cpu + kBlock + kVp + 128, inv);
        Store(cpu + kBlock + kVp + 192, Proj()); // the camera's own projection, in the same block
        Store(cpu + kPre + kVp, vp); // the pre-pass's own copy of the camera
        Store(cpu + kPre + kVp + 128, inv);
        prepass.Reset(); // earlier batch: depth only, its own root signature
        prepass.list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        prepass.list->SetGraphicsRootSignature(rs_pre.Get());
        prepass.list->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress() + kPre);
        REQUIRE(SUCCEEDED(prepass.list->Close()));
        if (!inline_record_first || unpatched_prepass) {
            if (unpatched_prepass) std::memset(cpu + kPre + kVp, 0, 64);
            ID3D12CommandList* pl[] = {prepass.list.Get()};
            Submit(q, 1, pl, true);
            if (unpatched_prepass) {
                d.WaitIdle(); // test-only: finish the unrecognized camera batch
                Store(cpu + kPre + kVp, vp);
            }
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
        if (inline_list) { // ...then, in the SAME list, post-processing reads the colour
            D3D12_RESOURCE_BARRIER rd2 = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            scene.list->ResourceBarrier(1, &rd2);
            scene.list->OMSetRenderTargets(1, &post_rtv, FALSE, nullptr); // the post pass's own target
            ID3D12DescriptorHeap* ph[] = {post_heap.Get()};
            scene.list->SetDescriptorHeaps(1, ph);
            scene.list->SetGraphicsRootSignature(post_root.Get());
            scene.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VIEWPORT viewport{0, 0, float(W), float(H), 0, 1};
            D3D12_RECT scissor{0, 0, LONG(W), LONG(H)};
            scene.list->RSSetViewports(1, &viewport);
            scene.list->RSSetScissorRects(1, &scissor);
            const int evals_before_read = fake::evals;
            scene.list->SetGraphicsRootDescriptorTable(0, post_heap->GetGPUDescriptorHandleForHeapStart()); // binding alone is not a read
            REQUIRE(fake::evals == evals_before_read);
            scene.list->SetPipelineState(post_pso.Get()); // shader chosen AFTER the table
            scene.list->DrawInstanced(3, 1, 0, 0); // intermediate actual read
            REQUIRE(fake::evals == evals_before_read); // must NEVER process the incomplete scene
            std::swap(rd2.Transition.StateBefore, rd2.Transition.StateAfter);
            scene.list->ResourceBarrier(1, &rd2);
            scene.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            scene.list->ClearRenderTargetView(rtv, clear, 0, nullptr); // actual later scene write
            std::swap(rd2.Transition.StateBefore, rd2.Transition.StateAfter);
            scene.list->ResourceBarrier(1, &rd2);
            scene.list->OMSetRenderTargets(1, &post_rtv, FALSE, nullptr);
            scene.list->SetGraphicsRootDescriptorTable(0, post_heap->GetGPUDescriptorHandleForHeapStart()); // final scene binding
            // Legacy boundary matcher: injected at the final read transition. Consumer anchor
            // (P4-0D): injected just before the final actual consumer (below). Never twice.
            REQUIRE(fake::evals <= evals_before_read + 1);
            if (inline_copy) {
                D3D12_RESOURCE_BARRIER cbars[] = {
                    test::Transition(color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    test::Transition(post_rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST)};
                scene.list->ResourceBarrier(2, cbars);
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                from.pResource = color.Get(); from.SubresourceIndex = 4;
                to.pResource = post_rt.Get();
                scene.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                for (auto& b : cbars) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                scene.list->ResourceBarrier(2, cbars);
            } else scene.list->DrawInstanced(3, 1, 0, 0); // final actual read
            REQUIRE(fake::evals == evals_before_read + (na::j2::GetStats().inline_mode ? 1 : 0)); // exactly one, before the final consumer ran
            std::swap(rd2.Transition.StateBefore, rd2.Transition.StateAfter);
            scene.list->ResourceBarrier(1, &rd2);
        }
        REQUIRE(SUCCEEDED(scene.list->Close()));
        consumer.Reset(); // a post pass reads the colour, then hands it back as a render target
        D3D12_RESOURCE_BARRIER rd = test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        consumer.list->ResourceBarrier(1, &rd);
        D3D12_RESOURCE_BARRIER back = test::Transition(color.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        consumer.list->ResourceBarrier(1, &back);
        if (post_reader) {
            ID3D12DescriptorHeap* heaps2[] = {post_heap.Get()};
            consumer.list->SetDescriptorHeaps(1, heaps2);
            consumer.list->SetGraphicsRootSignature(post_root.Get());
            consumer.list->SetGraphicsRootDescriptorTable(0, post_heap->GetGPUDescriptorHandleForHeapStart());
            D3D12_RESOURCE_BARRIER b[] = {
                test::Transition(color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                test::Transition(post_color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
            consumer.list->ResourceBarrier(2, b);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = color.Get(); src.SubresourceIndex = 4; dst.pResource = post_color.Get();
            consumer.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            for (auto& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
            consumer.list->ResourceBarrier(2, b);
            consumer.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
        REQUIRE(SUCCEEDED(consumer.list->Close()));
        g_forwarded.clear();
        if (inline_list) {
            if (inline_record_first && !unpatched_prepass) {
                ID3D12CommandList* pl[] = {prepass.list.Get()};
                Submit(q, 1, pl, true); // cannot change the sample already recorded by NGX
            }
            ID3D12CommandList* one[] = {scene.list.Get()};
            Submit(q, 1, one, true);
            if (fake::evals > frame_evals) {
                const double jx = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X);
                const double jy = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_Y);
                uint8_t original[64]; Store(original, vp);
                const auto f = [](const uint8_t* p, size_t i) { float v; std::memcpy(&v, p + i * 4, 4); return double(v); };
                for (size_t block : {size_t(kBlock), size_t(kPre)}) {
                    const auto* raster = cpu + block + kVp;
                    const double dx = (f(raster, 3) / f(raster, 15) - f(original, 3) / f(original, 15)) * W / 2;
                    const double dy = -(f(raster, 7) / f(raster, 15) - f(original, 7) / f(original, 15)) * H / 2;
                    REQUIRE(std::abs(dx - jx) < 0.001 && std::abs(dy - jy) < 0.001);
                }
                if (unpatched_prepass) REQUIRE(jx == 0 && jy == 0);
                else if (inline_record_first && cam_x >= 0.5) REQUIRE(jx != 0 || jy != 0);
            }
        } else if (split) {
            scene2.Reset(); // more of the scene in a second submission (same camera, same targets)
            scene2.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            scene2.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); // weapon-style depth clear: same image
            ID3D12DescriptorHeap* heaps3[] = {cbvh.Get()};
            scene2.list->SetDescriptorHeaps(1, heaps3);
            scene2.list->SetGraphicsRootSignature(rs.Get());
            scene2.list->SetGraphicsRootDescriptorTable(0, cbvh->GetGPUDescriptorHandleForHeapStart());
            REQUIRE(SUCCEEDED(scene2.list->Close()));
            ID3D12CommandList* first[] = {scene.list.Get()};
            Submit(q, 1, first, true);
            ID3D12CommandList* second[] = {scene2.list.Get(), consumer.list.Get()};
            g_forwarded.clear();
            Submit(q, 2, second, true);
        } else {
            ID3D12CommandList* lists[] = {scene.list.Get(), consumer.list.Get()};
            Submit(q, 2, lists, true);
        }
        na::j2::OnPresent(q);
        if (capture) na::capture::j2::OnPresent(q, presented.Get(), &PlainExecute);
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
    const auto evaluations = [&]() -> uint64_t { return real_ngx ? na::dlss::GetStats().evaluates : uint64_t(fake::evals); };
    const uint64_t evals0 = evaluations();
    if (auto_inline || auto_between) {
        for (int f = 0; f < 7; ++f) REQUIRE(frame(0.25 * f) == 0);
        const auto as = na::j2::GetStats();
        std::printf("auto: route %u state %u mode %u gen %u matches %u scene frames %llu, frames %llu (inline %llu), \"%s\" / \"%s\"\n",
                    as.route.route, as.route.state, as.route.mode, as.route.generation, as.route.matches,
                    (unsigned long long)as.route.scene_frames, (unsigned long long)as.frames, (unsigned long long)as.inline_frames,
                    as.route.reason, as.reason);
        CHECK(as.route.mode == 0);                               // no override
        CHECK(as.route.route == (auto_inline ? 2u : 1u));        // chosen from the structure
        CHECK(as.route.state == 1 && as.route.generation == 1);  // learned once, no oscillation
        CHECK(as.frames >= 2 && as.frames <= 4);                 // not before the learning frames
        if (auto_inline) CHECK(as.inline_frames == as.frames && g_forwarded.size() == 1);
        else CHECK(as.inline_frames == 0 && g_forwarded.size() == 3);
        CHECK(as.route.contribution_established == 0);           // lineage not observed: unknown
        CHECK(dev->GetDeviceRemovedReason() == S_OK);
        // Evidence capture: written only after each slot's fences completed.
        d.WaitIdle();
        na::capture::j2::Shutdown();
        const auto cs = na::capture::j2::GetStats();
        std::printf("capture: recorded %u submitted %u written %u busy %u discarded %u retired %u no-present %u errors %u\n",
                    cs.recorded, cs.submitted, cs.written, cs.dropped_busy, cs.discarded, cs.retired, cs.without_present, cs.errors);
        CHECK(cs.recorded == 3 && cs.submitted == 3 && cs.written == 3);
        CHECK(cs.without_present == 0 && cs.errors == 0 && cs.retired == 0);
        uint64_t prev_ordinal = 0;
        for (int i = 0; i < 3; ++i) {
            wchar_t base[32];
            swprintf_s(base, L"\\%05d-", i);
            const std::wstring b = cap_dir + base;
            for (const wchar_t* plane : {L"color", L"depth", L"motion", L"dlssout", L"present"})
                CHECK(FileSize(b + plane + L".raw") > 0);
            const std::string meta = ReadText(b + L"meta.json");
            CHECK(meta.find(auto_inline ? "\"route\": 2," : "\"route\": 1,") != std::string::npos);
            CHECK(meta.find("\"frame_closed\": true") != std::string::npos);
            const size_t at = meta.find("\"present_ordinal\": ");
            CHECK(at != std::string::npos);
            const uint64_t ordinal = at == std::string::npos ? 0 : std::strtoull(meta.c_str() + at + 19, nullptr, 10);
            if (i) CHECK(ordinal == prev_ordinal + 1); // consecutive frames, one slot each
            prev_ordinal = ordinal;
        }
        CHECK(FileSize(cap_dir + L"\\00003-meta.json") < 0); // bounded: exactly the requested frames
        return test::finish(auto_inline ? "j2_anno_shape_test (auto-inline)" : "j2_anno_shape_test (auto-between)");
    }
    if (inline_list) {
        // The copy case runs long enough for the consumer anchor's stability gate (16
        // identical signatures) to hand over from the boundary matcher.
        const int frames = inline_copy ? 22 : 4;
        for (int f = 0; f < frames; ++f) REQUIRE(frame(0.25 * (f % 4)) == 0);
        if (inline_record_first) REQUIRE(frame(1.0, true) == 0);
        const auto is = na::j2::GetStats();
        std::printf("inline: mode %u, candidates %llu, recorded %llu, frames %llu (inline %llu), refused %llu, evals %llu, forwarded %zu, \"%s\"\n",
                    is.inline_mode, (unsigned long long)is.inline_candidates, (unsigned long long)is.inline_recorded,
                    (unsigned long long)is.frames, (unsigned long long)is.inline_frames, (unsigned long long)is.inline_refused,
                    (unsigned long long)(evaluations() - evals0), g_forwarded.size(), is.reason);
        std::printf("anchor: mode %u matched unique %llu pair %llu copy %llu legacy %llu skips %llu why \"%s\"\n", is.consumer_mode,
                    (unsigned long long)is.consumer_matched[1], (unsigned long long)is.consumer_matched[2],
                    (unsigned long long)is.consumer_matched[3], (unsigned long long)is.consumer_matched[4],
                    (unsigned long long)is.consumer_skips, is.consumer_why);
        // The intermediate and final post draws share a PSO AND producer (clears): ambiguous,
        // so the boundary matcher stays in use. A copy consumer anchors on its destination.
        if (inline_copy) CHECK(is.consumer_mode == 3 && is.consumer_matched[3] >= 1 && is.consumer_matched[4] >= 14 &&
                               is.consumer_streak >= 16 && is.consumer_fallbacks == 0); // boundary matcher first, anchor once stable
        else CHECK(is.consumer_mode == 4 && is.consumer_matched[4] == is.inline_recorded);
        CHECK(is.inline_target == 3); // intermediate transition + draw, then final transition
        CHECK(is.inline_mode == 1);                  // detected on the first frame
        CHECK(is.inline_frames >= 2);                // frames 2.. ran DLAA inside the game's list
        CHECK(is.inline_recorded == is.inline_frames);
        CHECK(evaluations() >= evals0 + 2);
        CHECK(g_forwarded.size() == 1);              // no J2 list was inserted
        CHECK(is.inline_refused == 0);
        CHECK(is.jitter_writes >= 2);
        CHECK(dev->GetDeviceRemovedReason() == S_OK); // the game's list (with our work and the restored state) ran
        return test::finish("j2_anno_shape_test (inline)");
    }
    if (split) {
        for (int f = 0; f < 4; ++f) REQUIRE(frame(0.25 * f) == 0);
        const auto ss = na::j2::GetStats();
        std::printf("split: frames %llu, scene batches %u (max %u), waited %llu, after DLAA %llu, last forward %zu lists\n",
                    (unsigned long long)ss.frames, ss.scene_batches, ss.scene_batches_max, (unsigned long long)ss.waited_batches,
                    (unsigned long long)ss.post_scene_batches, g_forwarded.size());
        CHECK(ss.scene_batches == 2);
        CHECK(ss.post_scene_batches == 1);          // only the first frame (nothing learned yet) was early
        CHECK(ss.waited_batches >= 3);              // frames 2..4 skip the first scene submission...
        CHECK(ss.frames == 4 && evaluations() == evals0 + 4);
        CHECK(g_forwarded.size() == 3 && g_forwarded[0] == scene2.list.Get() && g_forwarded[2] == consumer.list.Get()); // ...and follow the second
        return test::finish("j2_anno_shape_test (split)");
    }
    REQUIRE(frame(0) == 0);
    auto st = na::j2::GetStats();
    CHECK(g_forwarded.size() == 3 && st.frames == 1 && evaluations() == evals0 + 1);
    CHECK(st.camera_offset == kVp);
    CHECK(std::strncmp(st.reason, "Stopped", 7) != 0); // the list closed: no latch
    REQUIRE(frame(0.5) == 0);
    st = na::j2::GetStats();
    REQUIRE(st.frames == 2 && evaluations() == evals0 + 2);
    CHECK(g_forwarded.size() == 3 && g_forwarded[0] == scene.list.Get() && g_forwarded[2] == consumer.list.Get());
    if (post_reader) CHECK(std::strstr(st.reason, "before a later scene-colour reader") != nullptr);
    if (!real_ngx) for (const char* key : {NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth}) {
        auto* input = static_cast<ID3D12Resource*>(fake::P(fake::last_eval, key));
        REQUIRE(input);
        const auto input_desc = input->GetDesc();
        CHECK(input_desc.SampleDesc.Count == 1 && input_desc.MipLevels == 1 && input_desc.DepthOrArraySize == 1);
        CHECK(input_desc.Width == W && input_desc.Height == H);
    }
    CHECK(st.jitter_writes >= 2);
    CHECK(st.camera_inv_delta == 128 && st.camera_learned_hits >= 1);
    CHECK(st.camera_proj_delta == 192 && st.jitter_proj_writes >= 1); // the separate projection is jittered too
    if (!real_ngx) {
    CHECK(fake::I(fake::last_eval, NVSDK_NGX_Parameter_Reset) == 0);
    const double jx = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_X), jy = fake::F(fake::last_eval, NVSDK_NGX_Parameter_Jitter_Offset_Y);
    CHECK((jx != 0.0 || jy != 0.0) && std::fabs(jx) < 0.5 && std::fabs(jy) < 0.5);
    }
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
            if (post_reader && sub == scene_sub) { src.pResource = post_color.Get(); src.SubresourceIndex = 0; }
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
        if (real_ngx) {
            for (UINT channel = 0; channel < 3; ++channel) CHECK((s1[channel] & 0x7c00) != 0x7c00); // finite output
            CHECK(s1[0] || s1[1] || s1[2]); // NVIDIA produced non-black scene output
        } else CHECK(s1[0] == 0 && s1[1] == 0 && s1[2] == 0);      // the scene's clear (0.2,0.3,0.4) was replaced by the DLAA result
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
