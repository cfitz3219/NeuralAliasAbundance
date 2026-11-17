#include "host_dx12/capture/referee_gpu.hpp"
#include "host_dx12/capture/referee_shader.hpp"
#include "core/referee.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/settings.hpp"

#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace na::referee::gpu {
namespace {

constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
enum Img : uint32_t { kPrev, kInput, kOutput, kScreen, kMotion, kImages };
enum class Phase { Idle, PreRecorded, PreSubmitted, MainRecorded, MainSubmitted, Computing, Stopped };
constexpr uint32_t kTokenPre = 1, kTokenMain = 2;
constexpr ULONGLONG kStuckMs = 3000; // a half-taken sample is given up after this

struct Copy {
    ComPtr<ID3D12Resource> tex;   // the referee's own texture, NON_PIXEL_SHADER_RESOURCE between lists
    D3D12_RESOURCE_DESC desc{};
};

struct State {
    std::mutex m;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> thumb, tiles, points;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> results, readback;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_LIST_TYPE list_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    bool init_tried = false, init_ok = false;
    Copy img[kImages];
    // Textures a list we did not submit may still write (inline route): never released.
    std::vector<ComPtr<ID3D12Resource>> kept;
    bool tainted = false;
    // Replaced textures, released once the next sample's reduction has completed.
    std::vector<ComPtr<ID3D12Resource>> retire;
    Phase phase = Phase::Idle;
    ULONGLONG phase_at = 0, next_at = 0, window_at = 0;
    uint64_t pre_ordinal = 0, main_ordinal = 0;
    float pre_jitter[2]{}, main_jitter[2]{}, mv_scale[2]{1, 1};
    bool main_closed = false;
    UINT64 pre_fence = 0, main_fence = 0, compute_fence = 0;
    ComPtr<ID3D12CommandQueue> pre_queue, main_queue;
    bool done_screen = false, done_tiles = false, done_pair = false;
    float done_jitter_change = 0;
    na::referee::Window window;
    std::vector<float> data = std::vector<float>(na::referee::kResultFloats, 0.f);
    Stats stats;
};
State& g = *new State; // never destroyed: its D3D12 objects must not be released by the CRT at exit

void set_phase(Phase p) { g.phase = p; g.phase_at = GetTickCount64(); }
bool enabled() { return na::settings::Get().referee_enabled.load(std::memory_order_relaxed) != 0; }
ULONGLONG every_ms()
{
    const uint32_t v = na::settings::Get().referee_every_ms.load(std::memory_order_relaxed);
    return v < 100 ? 100 : v;
}
void stop(const char* why)
{
    set_phase(Phase::Stopped);
    g.stats.stopped = true;
    g.stats.running = false;
    std::snprintf(g.stats.why, sizeof g.stats.why, "%s", why);
    na::eventlog::Emit("referee_state", "stopped why=\"%s\"", why);
}

DXGI_FORMAT view_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    default: return f;
    }
}
bool readable(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_UNKNOWN:
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS:
        return false;
    default: return true;
    }
}

bool init(ID3D12Device* dev)
{
    if (g.init_tried) return g.init_ok && g.device.Get() == dev;
    g.init_tried = true;
    g.device = dev;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kImages, 0, 0, 0};
    D3D12_ROOT_PARAMETER p[3]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants = {0, 0, 12};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable = {1, &range};
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[2].Descriptor = {0, 0};
    D3D12_ROOT_SIGNATURE_DESC rd{3, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g.root)))) {
        stop("the referee's root signature could not be made");
        return false;
    }
    const char* entries[3] = {"thumb", "tiles", "points"};
    ComPtr<ID3D12PipelineState>* psos[3] = {&g.thumb, &g.tiles, &g.points};
    for (int i = 0; i < 3; ++i) {
        ComPtr<ID3DBlob> cs;
        err.Reset();
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "referee.hlsl", nullptr, nullptr, entries[i], "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err))) {
            stop("the referee's card programs did not compile");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g.root.Get();
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(psos[i]->ReleaseAndGetAddressOf())))) {
            stop("the referee's card programs could not be made");
            return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kImages, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const UINT64 bytes = UINT64(na::referee::kResultFloats) * 4;
    auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& r) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    };
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.heap))) ||
        !buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, g.results) ||
        !buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, g.readback) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
        stop("the referee's buffers could not be made");
        return false;
    }
    g.init_ok = true;
    g.stats.running = true;
    g.window_at = GetTickCount64();
    na::eventlog::Emit("referee_state", "started every_ms=%llu (watches only: picture, result, motion, jitter)",
                       static_cast<unsigned long long>(every_ms()));
    return true;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

// Our texture for `src` (same size and format), made or remade when needed.
bool ensure(Img which, ID3D12Resource* src)
{
    const D3D12_RESOURCE_DESC s = src->GetDesc();
    if (s.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || s.SampleDesc.Count != 1 || !readable(s.Format)) return false;
    Copy& c = g.img[which];
    if (c.tex && c.desc.Width == s.Width && c.desc.Height == s.Height && c.desc.Format == s.Format) return true;
    if (c.tex) (g.tainted ? g.kept : g.retire).push_back(c.tex);
    c.tex.Reset();
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = s.Width;
    d.Height = s.Height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = s.Format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, NPSR, nullptr, IID_PPV_ARGS(&c.tex)))) return false;
    c.desc = d;
    return true;
}

// src (in `state`) -> our texture, in `l`. Both end in the states they started in.
void copy(ID3D12GraphicsCommandList* l, Img which, ID3D12Resource* src, D3D12_RESOURCE_STATES state)
{
    ID3D12Resource* dst = g.img[which].tex.Get();
    D3D12_RESOURCE_BARRIER b[3]{};
    UINT n = 0;
    if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b[n++].UAV.pResource = src;
    }
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) b[n++] = transition(src, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b[n++] = transition(dst, NPSR, D3D12_RESOURCE_STATE_COPY_DEST);
    l->ResourceBarrier(n, b);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = src;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = dst;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    n = 0;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) b[n++] = transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    b[n++] = transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, NPSR);
    l->ResourceBarrier(n, b);
}

void emit_window(bool force)
{
    const ULONGLONG now = GetTickCount64();
    const uint32_t log_s = na::settings::Get().referee_log_s.load(std::memory_order_relaxed);
    if (!g.window.samples || (!force && now - g.window_at < ULONGLONG(log_s < 1 ? 1 : log_s) * 1000)) return;
    char line[2048];
    na::referee::Format(line, sizeof line, g.window, na::referee::Judge(g.window));
    na::eventlog::Emit("referee", "%s", line);
    ++g.stats.logged;
    g.window = na::referee::Window();
    g.window_at = now;
}

// A finished reduction: read, score, add to the window. Never waits.
void collect()
{
    if (g.phase != Phase::Computing || g.fence->GetCompletedValue() < g.compute_fence) return;
    void* p = nullptr;
    D3D12_RANGE r{0, SIZE_T(na::referee::kResultFloats) * 4};
    if (SUCCEEDED(g.readback->Map(0, &r, &p)) && p) {
        std::memcpy(g.data.data(), p, SIZE_T(na::referee::kResultFloats) * 4);
        D3D12_RANGE none{0, 0};
        g.readback->Unmap(0, &none);
        na::referee::Sample s;
        s.data = g.data.data();
        s.have_screen = g.done_screen;
        s.tiles_comparable = g.done_tiles;
        s.have_motion = g.done_pair;
        s.jitter_change = g.done_jitter_change;
        g.window.Add(na::referee::Score(s));
        ++g.stats.samples;
        if (g.done_pair) ++g.stats.pairs;
        if (g.done_screen) ++g.stats.screens;
    } else {
        ++g.stats.failed;
    }
    g.retire.clear();
    set_phase(Phase::Idle);
    g.next_at = GetTickCount64() + every_ms();
}

// The reduction, on our own list, on the presenting queue.
void compute(ID3D12CommandQueue* queue, ID3D12Resource* presented, ExecuteFn exec)
{
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    if (!g.alloc || g.list_type != type) {
        g.alloc.Reset();
        g.list.Reset();
        if (FAILED(g.device->CreateCommandAllocator(type, IID_PPV_ARGS(&g.alloc))) ||
            FAILED(g.device->CreateCommandList(0, type, g.alloc.Get(), nullptr, IID_PPV_ARGS(&g.list)))) {
            stop("the referee's command list could not be made");
            return;
        }
        g.list->Close();
        g.list_type = type;
    }
    if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc.Get(), nullptr))) { ++g.stats.failed; set_phase(Phase::Idle); return; }
    ID3D12GraphicsCommandList* l = g.list.Get();
    const bool screen = presented && ensure(kScreen, presented);
    if (screen) {
        D3D12_RESOURCE_BARRIER b[2] = {transition(presented, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                       transition(g.img[kScreen].tex.Get(), NPSR, D3D12_RESOURCE_STATE_COPY_DEST)};
        l->ResourceBarrier(2, b);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = presented;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = g.img[kScreen].tex.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        b[0] = transition(presented, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        b[1] = transition(g.img[kScreen].tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NPSR);
        l->ResourceBarrier(2, b);
    }
    // Views: a missing image is stood in for by the input (never read: its pass is not run).
    const UINT inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kImages; ++i) {
        const Copy& c = (g.img[i].tex && (i != kScreen || screen)) ? g.img[i] : g.img[kInput];
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = view_format(c.desc.Format);
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Texture2D.MipLevels = 1;
        g.device->CreateShaderResourceView(c.tex.Get(), &v, h);
        h.ptr += inc;
    }
    ID3D12DescriptorHeap* heaps[] = {g.heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g.root.Get());
    l->SetComputeRootDescriptorTable(1, g.heap->GetGPUDescriptorHandleForHeapStart());
    l->SetComputeRootUnorderedAccessView(2, g.results->GetGPUVirtualAddress());
    D3D12_RESOURCE_BARRIER rb = transition(g.results.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->ResourceBarrier(1, &rb);
    struct K { uint32_t src, base, w, h, mw, mh, pad0, pad1; float sx, sy, djx, djy; };
    auto size = [](Img which, uint32_t& sw_, uint32_t& sh_) { sw_ = uint32_t(g.img[which].desc.Width); sh_ = g.img[which].desc.Height; };
    auto run = [&](ID3D12PipelineState* pso, const K& k, UINT gx, UINT gy) {
        l->SetPipelineState(pso);
        l->SetComputeRoot32BitConstants(0, 12, &k, 0);
        l->Dispatch(gx, gy, 1);
    };
    using namespace na::referee;
    K k{};
    const Img thumbs[3] = {kInput, kOutput, kScreen};
    const Block thumb_blocks[3] = {kThumbInput, kThumbOutput, kThumbScreen}, tile_blocks[3] = {kTileInput, kTileOutput, kTileScreen};
    uint32_t ow = 0, oh = 0, sw = 0, shh = 0;
    size(kOutput, ow, oh);
    if (screen) size(kScreen, sw, shh);
    const bool tiles = screen && ow == sw && oh == shh;
    for (int i = 0; i < 3; ++i) {
        if (thumbs[i] == kScreen && !screen) continue;
        k = {};
        k.src = thumbs[i];
        size(thumbs[i], k.w, k.h);
        k.base = BlockOffset(thumb_blocks[i]);
        run(g.thumb.Get(), k, (kThumbW + 7) / 8, (kThumbH + 7) / 8);
        if (tiles) {
            k.base = BlockOffset(tile_blocks[i]);
            run(g.tiles.Get(), k, kTiles, 1);
        }
    }
    uint32_t pw = 0, ph = 0, iw = 0, ih = 0;
    size(kPrev, pw, ph);
    size(kInput, iw, ih);
    const bool pair = g.img[kPrev].tex && g.img[kMotion].tex && pw == iw && ph == ih && g.main_ordinal == g.pre_ordinal + 1;
    if (pair) {
        k = {};
        k.src = kInput;
        k.base = BlockOffset(kPoints);
        k.w = iw; k.h = ih;
        size(kMotion, k.mw, k.mh);
        k.sx = g.mv_scale[0]; k.sy = g.mv_scale[1];
        k.djx = g.main_jitter[0] - g.pre_jitter[0];
        k.djy = g.main_jitter[1] - g.pre_jitter[1];
        run(g.points.Get(), k, (kGridW + 7) / 8, (kGridH + 5) / 6);
    }
    D3D12_RESOURCE_BARRIER after[2]{};
    after[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    after[0].UAV.pResource = g.results.Get();
    after[1] = transition(g.results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->ResourceBarrier(2, after);
    l->CopyBufferRegion(g.readback.Get(), 0, g.results.Get(), 0, UINT64(kResultFloats) * 4);
    rb = transition(g.results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    l->ResourceBarrier(1, &rb);
    if (FAILED(l->Close())) { ++g.stats.failed; set_phase(Phase::Idle); return; }
    // The evaluation's copies may have run on another queue: this queue waits for them on the card.
    if (g.pre_queue && g.pre_queue.Get() != queue && g.pre_fence) queue->Wait(g.fence.Get(), g.pre_fence);
    if (g.main_queue && g.main_queue.Get() != queue && g.main_fence) queue->Wait(g.fence.Get(), g.main_fence);
    ID3D12CommandList* one[] = {l};
    exec(queue, 1, one);
    g.compute_fence = g.fence_next++;
    queue->Signal(g.fence.Get(), g.compute_fence);
    g.done_screen = screen;
    g.done_tiles = tiles;
    g.done_pair = pair;
    g.done_jitter_change = std::hypot(g.main_jitter[0] - g.pre_jitter[0], g.main_jitter[1] - g.pre_jitter[1]);
    if (!pair) ++g.stats.unpaired;
    set_phase(Phase::Computing);
}

// A half-taken sample that went nowhere is given up (its lists were submitted, discarded, or are
// kept by `tainted`, so the textures stay valid).
void watchdog()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g.phase_at < kStuckMs) return;
    switch (g.phase) {
    case Phase::PreRecorded: case Phase::MainRecorded:
        g.tainted = true; // neither submitted nor discarded: it may still run
        [[fallthrough]];
    case Phase::PreSubmitted: case Phase::MainSubmitted:
        ++g.stats.unpaired;
        set_phase(Phase::Idle);
        g.next_at = now + every_ms();
        break;
    default: break;
    }
}

} // namespace

bool Wanted()
{
    if (!enabled()) return false;
    std::lock_guard<std::mutex> lock(g.m);
    // Collected here too: a route that hands over the presented image only after an evaluation it
    // recorded (DX11) would otherwise never read a finished reduction.
    if (g.init_ok && g.phase == Phase::Computing) { collect(); emit_window(false); }
    if (g.phase == Phase::Idle) return GetTickCount64() >= g.next_at;
    return g.phase == Phase::PreSubmitted;
}

uint32_t Record(ID3D12GraphicsCommandList* l, const Evaluation& e)
{
    if (!l || !e.color || !e.output || e.owns_present || !enabled()) return 0;
    std::lock_guard<std::mutex> lock(g.m);
    if (g.phase == Phase::Stopped) return 0;
    watchdog();
    const bool due = g.phase == Phase::Idle && GetTickCount64() >= g.next_at;
    const bool restart = g.phase == Phase::PreSubmitted && (e.present_ordinal != g.pre_ordinal + 1 || e.reset) &&
                         e.present_ordinal != g.pre_ordinal;
    if (due || restart) {
        ComPtr<ID3D12Device> dev;
        if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev))) || !init(dev.Get())) return 0;
        if (!ensure(kPrev, e.color)) { ++g.stats.failed; return 0; }
        copy(l, kPrev, e.color, e.color_state);
        g.pre_ordinal = e.present_ordinal;
        g.pre_jitter[0] = e.jitter_x;
        g.pre_jitter[1] = e.jitter_y;
        g.pre_fence = 0;
        g.pre_queue.Reset();
        if (restart) ++g.stats.restarted;
        ++g.stats.pre;
        set_phase(Phase::PreRecorded);
        return kTokenPre;
    }
    if (g.phase != Phase::PreSubmitted || e.present_ordinal != g.pre_ordinal + 1) return 0;
    if (!ensure(kInput, e.color) || !ensure(kOutput, e.output) || (e.motion && !ensure(kMotion, e.motion))) { ++g.stats.failed; return 0; }
    copy(l, kInput, e.color, e.color_state);
    copy(l, kOutput, e.output, e.output_state);
    if (e.motion) copy(l, kMotion, e.motion, e.motion_state);
    else g.img[kMotion].tex.Reset();
    g.main_ordinal = e.present_ordinal;
    g.main_jitter[0] = e.jitter_x;
    g.main_jitter[1] = e.jitter_y;
    g.mv_scale[0] = e.mv_scale_x;
    g.mv_scale[1] = e.mv_scale_y;
    g.main_closed = false;
    g.main_fence = 0;
    g.main_queue.Reset();
    ++g.stats.main;
    set_phase(Phase::MainRecorded);
    return kTokenMain;
}

void Submitted(uint32_t token, ID3D12CommandQueue* queue)
{
    if (!token || !queue) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (token == kTokenPre && g.phase == Phase::PreRecorded) {
        g.pre_fence = g.fence_next++;
        queue->Signal(g.fence.Get(), g.pre_fence);
        g.pre_queue = queue;
        set_phase(Phase::PreSubmitted);
    } else if (token == kTokenMain && g.phase == Phase::MainRecorded) {
        g.main_fence = g.fence_next++;
        queue->Signal(g.fence.Get(), g.main_fence);
        g.main_queue = queue;
        set_phase(Phase::MainSubmitted);
    }
}

void Discard(uint32_t token, bool may_still_run)
{
    if (!token) return;
    std::lock_guard<std::mutex> lock(g.m);
    const bool ours = (token == kTokenPre && g.phase == Phase::PreRecorded) || (token == kTokenMain && g.phase == Phase::MainRecorded);
    if (!ours) return;
    if (may_still_run) g.tainted = true; // its copies may still land: these textures are never released
    ++g.stats.discarded;
    set_phase(Phase::Idle);
    g.next_at = GetTickCount64() + 50;
}

void OnFrameClosed(uint64_t present_ordinal)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.phase == Phase::MainSubmitted && present_ordinal >= g.main_ordinal) g.main_closed = true;
}

void OnPresent(ID3D12CommandQueue* queue, ID3D12Resource* presented, ExecuteFn exec)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.init_ok || g.phase == Phase::Stopped) return;
    collect();
    if (g.phase == Phase::MainSubmitted && g.main_closed && queue && exec) compute(queue, presented, exec);
    else watchdog();
    emit_window(false);
    if (!enabled() && g.window.samples) emit_window(true);
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.stats;
}

} // namespace na::referee::gpu
