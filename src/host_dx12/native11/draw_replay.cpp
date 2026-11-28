// Draw replay on the native DX11 route: see draw_replay.hpp. Mirrors observe/producers.cpp "Draw
// replay" (DX12) step by step; where D3D11 differs it says so.
#include "core/replay_depth.hpp"
#include "host_dx12/native11/draw_replay.hpp"
#include "host_dx12/native11/input_provider.hpp"

#include "core/replay_budget.hpp"
#include "core/replay_movers.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/diag/vram_ledger.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace na::native11::draw_replay {
namespace {
using Microsoft::WRL::ComPtr;

enum : uint32_t { kCreateVertexShader = 12 }; // ID3D11Device (d3d11.h order)
using PFN_CreateVS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
PFN_CreateVS g_create_vs = nullptr;
std::atomic<ID3D11Device*> g_hooked{nullptr};
std::mutex g_install_mu;
bool g_installed = false;

// The same per-step mix as DX12 (producers.cpp rp_mix): the identity only needs to be stable.
uint64_t Mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h; }
uint64_t Fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

// A game vertex shader: its bytecode and the stream-output twin made from it (DX12: SoEntry/SoTwin).
struct Entry {
    std::vector<uint8_t> bytes;
    uint64_t hash = 0;
    ComPtr<ID3D11GeometryShader> twin;
    uint8_t state = 0; // 0 not asked, 1 queued, 2 built, 3 failed
};
// Like the DX12 replay worker, the idle waiter can outlive CRT teardown.
std::mutex& g_shaders_m = *new std::mutex();
std::condition_variable& g_shaders_cv = *new std::condition_variable();
std::unordered_map<const void*, std::shared_ptr<Entry>>& g_shaders = *new std::unordered_map<const void*, std::shared_ptr<Entry>>();
std::deque<std::shared_ptr<Entry>>& g_queue = *new std::deque<std::shared_ptr<Entry>>();
bool g_worker = false;
ComPtr<ID3D11Device>& g_device = *new ComPtr<ID3D11Device>(); // the device the twins and buffers are made on

enum : uint32_t { kReplayed, kVerts, kTwinQueued, kTwinBuilding, kTwinFailed, kNoShader, kOverflow, kNoted, kBuilt, kBuildFailed,
                  kSealed, kPacked, kPackFailed, kNotSampled, kDrawTooLarge, kFrameFull, kMoverDraws, kMoverVerts, kMoverLeftOut,
                  kProbeVerts, kStatCount };
std::atomic<uint64_t> g_stat[kStatCount];
void count(uint32_t k, uint64_t n = 1) { g_stat[k].fetch_add(n, std::memory_order_relaxed); }
// Each gate is counted (DX12 names where D3D11 has the same thing).
enum : uint32_t { kGateDeferred, kGateIndirect, kGateOtherDepth, kGateQuery, kGateViewport, kGateTopology, kGateStages, kGateStreamOut,
                  kGatePredicate, kGateVsUav, kGateNoDepthWrite, kGateDepthMatched, kGateSealed, kGateNoDevice, kGateCount };
std::atomic<uint64_t> g_gate[kGateCount];
void gate(uint32_t k) { g_gate[k].fetch_add(1, std::memory_order_relaxed); }

std::atomic<bool> g_on{false}, g_movers_on{false};
std::atomic<uint64_t> g_budget{0};
std::atomic<uint32_t> g_threshold_seen{na::replay_budget::kFull};
constexpr uint32_t kRotateFrames = 240, kProbeShare = 4, kProbeFrames = 24; // as DX12 (kRpRotateFrames, kRpProbeShare, kRpProbeFrames)

// The frame being written (the game's immediate context, its thread) and what was learned.
std::mutex g_m;
struct Accumulating {
    uint64_t frame = 0, verts = 0;
    bool sealed = false;
    std::vector<Record> draws;
} g_acc;
na::replay_budget::FrameBudget g_rpb;
na::replay_movers::Table g_table;
uint64_t g_learn_clock = 0;
std::atomic<uint64_t> g_movers{0}, g_suspects{0}, g_promoted{0}, g_let_go{0}, g_reports{0};
ComPtr<ID3D11Buffer>& g_so = *new ComPtr<ID3D11Buffer>();
ComPtr<ID3D11ShaderResourceView>& g_so_view = *new ComPtr<ID3D11ShaderResourceView>();
ComPtr<ID3D11ComputeShader>& g_pack = *new ComPtr<ID3D11ComputeShader>();
bool g_buffers_tried = false;

void Worker()
{
    for (;;) {
        std::shared_ptr<Entry> e;
        ComPtr<ID3D11Device> dev;
        {
            std::unique_lock<std::mutex> lk(g_shaders_m);
            g_shaders_cv.wait(lk, [] { return !g_queue.empty(); });
            e = std::move(g_queue.front());
            g_queue.pop_front();
            dev = g_device;
        }
        // The vertex shader's own bytecode as a geometry stage with stream output and no rasterised
        // stream: the VS's SV_Position of every expanded vertex, 16 bytes each (DX12: the SO twin).
        static const D3D11_SO_DECLARATION_ENTRY decl{0, "SV_Position", 0, 0, 4, 0};
        static const UINT stride = 16;
        ComPtr<ID3D11GeometryShader> twin;
        HRESULT hr = E_FAIL;
        if (dev) {
            OwnWorkScope own;
            hr = dev->CreateGeometryShaderWithStreamOutput(e->bytes.data(), e->bytes.size(), &decl, 1, &stride, 1,
                                                           D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &twin);
        }
        const bool ok = SUCCEEDED(hr) && twin;
        count(ok ? kBuilt : kBuildFailed);
        if (!ok) na::eventlog::Emit("draw_replay11_twin_failed", "hr=0x%08x vs=0x%llx", static_cast<unsigned>(hr),
                                    static_cast<unsigned long long>(e->hash));
        std::lock_guard<std::mutex> lk(g_shaders_m);
        e->twin = std::move(twin);
        e->state = ok ? 2 : 3;
        e->bytes.clear(); // the twin is made: the bytecode is not needed again
        e->bytes.shrink_to_fit();
    }
}

HRESULT STDMETHODCALLTYPE CreateVertexShader_d(ID3D11Device* dev, const void* code, SIZE_T length, ID3D11ClassLinkage* linkage,
                                               ID3D11VertexShader** out)
{
    const HRESULT hr = g_create_vs(dev, code, length, linkage, out);
    if (SUCCEEDED(hr) && out && *out && code && length && !linkage && dev == g_hooked.load(std::memory_order_acquire) && !IsOwnWork())
        NoteVertexShader(*out, code, length);
    return hr;
}

// The frame's position buffer (stream output and a typed view for the pack) and the pack program.
bool EnsureBuffers(ID3D11Device* dev)
{
    if (g_so) return true;
    if (g_buffers_tried) return false;
    g_buffers_tried = true;
    OwnWorkScope own;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kCapacity * 16u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R32_UINT; // Read the stream-output words without floating point conversion.
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.FirstElement = 0;
    sv.Buffer.NumElements = kCapacity * 4u;
    static constexpr char kPackShader[] = R"(
Buffer<uint> words : register(t0);
RWTexture2D<unorm float4> packed : register(u0);
[numthreads(64, 1, 1)] void main(uint3 p : SV_DispatchThreadID) {
    uint width, height;
    packed.GetDimensions(width, height);
    uint word = words[p.y * width + p.x];
    uint4 bytes = uint4(word, word >> 8, word >> 16, word >> 24) & 255u;
    packed[p.xy] = float4(bytes) / 255.0;
}
)";
    ComPtr<ID3DBlob> code;
    ComPtr<ID3D11Buffer> so;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11ComputeShader> pack;
    const bool ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &so)) && SUCCEEDED(dev->CreateShaderResourceView(so.Get(), &sv, &view)) &&
        SUCCEEDED(D3DCompile(kPackShader, sizeof kPackShader - 1, "Replay positions", nullptr, nullptr, "main", "cs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr)) &&
        SUCCEEDED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &pack));
    na::eventlog::Emit("draw_replay11_buffers", "ok=%d bytes=%u", ok ? 1 : 0, bd.ByteWidth);
    if (!ok) return false;
    g_so = std::move(so); g_so_view = std::move(view); g_pack = std::move(pack);
    na::vram::Set("draw_replay_dx11_stream", na::vram::SizeOf(g_so.Get()), "draw replay is on: the frame's stream-output positions");
    return true;
}

template <class T> struct Held { // releases what a Get call handed back
    T* p = nullptr;
    ~Held() { if (p) p->Release(); }
};

} // namespace

bool Install(void* device)
{
    std::lock_guard<std::mutex> g(g_install_mu);
    auto* dev = static_cast<ID3D11Device*>(device);
    // The twin builder calls device methods from a worker.
    if (!dev || (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)) return false;
    {
        std::lock_guard<std::mutex> frame_lock(g_m);
        std::lock_guard<std::mutex> lk(g_shaders_m);
        if (g_device.Get() != dev) {
            g_device = dev;
            g_shaders.clear();
            g_queue.clear();
            g_acc = {};
            g_table.Clear();
            g_so.Reset();
            g_so_view.Reset();
            g_pack.Reset();
            g_buffers_tried = false;
        }
    }
    if (g_installed) {
        g_hooked.store(dev, std::memory_order_release);
        return true;
    }
    if (!na::hooking::Initialize()) return false;
    bool ok = na::hooking::BeginInstallWait("native11_draw_replay");
    ok = ok && na::hooking::PrepareVTableSlot(dev, kCreateVertexShader, reinterpret_cast<void*>(&CreateVertexShader_d),
                                              reinterpret_cast<void**>(&g_create_vs));
    if (ok) ok = na::hooking::CommitInstall();
    else na::hooking::AbandonInstall();
    g_installed = ok;
    if (ok) g_hooked.store(dev, std::memory_order_release);
    na::eventlog::Emit("draw_replay11", "install ok=%d", ok ? 1 : 0);
    return ok;
}

void NoteVertexShader(void* shader, const void* bytecode, size_t length)
{
    if (!shader || !bytecode || !length) return;
    auto e = std::make_shared<Entry>();
    const auto* p = static_cast<const uint8_t*>(bytecode);
    e->bytes.assign(p, p + length);
    e->hash = Fnv(p, length);
    count(kNoted);
    std::lock_guard<std::mutex> lk(g_shaders_m);
    g_shaders[shader] = std::move(e); // a new shader at a reused address replaces the old copy
}

void Configure(bool on, uint64_t budget, bool movers)
{
    g_on.store(on, std::memory_order_relaxed);
    g_budget.store(budget, std::memory_order_relaxed);
    if (g_movers_on.exchange(movers, std::memory_order_relaxed) != movers && !movers) {
        std::lock_guard<std::mutex> lock(g_m); // turned off: what was learned goes with it (as DX12)
        g_table.Clear();
        g_movers.store(0, std::memory_order_relaxed);
        g_suspects.store(0, std::memory_order_relaxed);
    }
}
bool Active() { return g_on.load(std::memory_order_relaxed); }

void ReleaseBuffers()
{
    if (g_on.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_so) return;
    g_so.Reset();
    g_so_view.Reset();
    g_buffers_tried = false; // made again the next time replay is on
    g_acc = {};
    na::vram::Set("draw_replay_dx11_stream", 0, "draw replay has been off for a few seconds: its stream-output buffer is released");
}

void AfterDraw(ID3D11DeviceContext* c, const DrawFacts& facts, const DrawArgs& args)
{
    if (!c || !g_on.load(std::memory_order_relaxed)) return;
    if (facts.deferred) { gate(kGateDeferred); return; }
    if (args.indirect) { gate(kGateIndirect); return; }
    if (!facts.scene_depth) { gate(kGateOtherDepth); return; }
    if (facts.query_active) { gate(kGateQuery); return; }
    std::lock_guard<std::mutex> lock(g_m);
    if (g_acc.frame != facts.frame) { g_acc.frame = facts.frame; g_acc.verts = 0; g_acc.sealed = false; g_acc.draws.clear(); }
    if (g_acc.sealed) { gate(kGateSealed); return; }
    const float* vp = facts.viewport;
    if (facts.viewports != 1 || !vp || vp[0] != 0.f || vp[1] != 0.f || vp[4] != 0.f || vp[5] != 1.f ||
        !(vp[2] > 0.f && vp[2] <= 16384.f && vp[3] > 0.f && vp[3] <= 16384.f)) { gate(kGateViewport); return; }
    Record rec{};
    rec.width = static_cast<uint32_t>(vp[2]);
    rec.height = static_cast<uint32_t>(vp[3]);
    if (static_cast<float>(rec.width) != vp[2] || static_cast<float>(rec.height) != vp[3]) { gate(kGateViewport); return; }
    ComPtr<ID3D11Device> dev;
    c->GetDevice(&dev);
    {
        std::lock_guard<std::mutex> lk(g_shaders_m);
        if (!g_device || g_device.Get() != dev.Get()) { gate(kGateNoDevice); return; }
        if (!g_worker) { g_worker = true; std::thread(Worker).detach(); }
    }
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    c->IAGetPrimitiveTopology(&topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) { gate(kGateTopology); return; }
    {
        Held<ID3D11GeometryShader> gs; Held<ID3D11HullShader> hs; Held<ID3D11DomainShader> ds;
        c->GSGetShader(&gs.p, nullptr, nullptr);
        c->HSGetShader(&hs.p, nullptr, nullptr);
        c->DSGetShader(&ds.p, nullptr, nullptr);
        if (gs.p || hs.p || ds.p) { gate(kGateStages); return; }
    }
    {
        ID3D11Buffer* targets[D3D11_SO_BUFFER_SLOT_COUNT]{};
        c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, targets);
        bool bound = false;
        for (auto* target : targets) if (target) {
            bound = true;
            target->Release();
        }
        if (bound) { gate(kGateStreamOut); return; }
    }
    {
        Held<ID3D11Predicate> predicate;
        BOOL value = FALSE;
        c->GetPredication(&predicate.p, &value);
        if (predicate.p) { gate(kGatePredicate); return; }
    }
    {
        // D3D11.1 lets every stage see the output UAVs: running the vertex shader twice must not
        // write through one twice (DX12: kGateVsUav).
        ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT]{};
        const UINT slots = dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
            D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
        c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, slots, uavs);
        bool any = false;
        for (auto* u : uavs) if (u) { any = true; u->Release(); }
        if (any) { gate(kGateVsUav); return; }
    }
    {
        // The draw writes the scene depth, or is opaque geometry tested EQUAL / LESS_EQUAL /
        // GREATER_EQUAL against a depth already written (a depth pre-pass), as DX12 admits.
        Held<ID3D11DepthStencilState> dss;
        UINT ref = 0;
        c->OMGetDepthStencilState(&dss.p, &ref);
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; d.DepthFunc = D3D11_COMPARISON_LESS; // the default state
        if (dss.p) dss.p->GetDesc(&d);
        const bool written = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL && !facts.depth_read_only;
        bool blended = false;
        if (!written) {
            Held<ID3D11BlendState> bs;
            FLOAT factor[4]{};
            UINT mask = 0;
            c->OMGetBlendState(&bs.p, factor, &mask);
            if (bs.p) {
                D3D11_BLEND_DESC b{};
                bs.p->GetDesc(&b);
                for (UINT i = 0; i < (b.IndependentBlendEnable ? 8u : 1u); ++i) blended = blended || b.RenderTarget[i].BlendEnable != FALSE;
            }
        }
        // The same rule as DX12's recipe and gate (core/replay_depth.hpp).
        const auto use = na::replay::DepthUseOf(d.DepthEnable != FALSE, written, blended, static_cast<uint32_t>(d.DepthFunc));
        if (use == na::replay::DepthUse::No) { gate(kGateNoDepthWrite); return; }
        if (use == na::replay::DepthUse::Matched) gate(kGateDepthMatched);
    }
    Held<ID3D11VertexShader> vs;
    c->VSGetShader(&vs.p, nullptr, nullptr);
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lk(g_shaders_m);
        const auto it = vs.p ? g_shaders.find(vs.p) : g_shaders.end();
        if (it != g_shaders.end()) entry = it->second;
    }
    if (!entry) { count(kNoShader); return; } // a shader we never saw created
    // The draw's identity, as DX12: its shader (and input layout), counts and the buffers it reads.
    {
        Held<ID3D11InputLayout> layout;
        c->IAGetInputLayout(&layout.p);
        uint64_t h = Mix(0, reinterpret_cast<uintptr_t>(vs.p));
        h = Mix(h, entry->hash); h = Mix(h, reinterpret_cast<uintptr_t>(layout.p));
        h = Mix(h, args.count); h = Mix(h, args.instances); h = Mix(h, args.indexed);
        h = Mix(h, args.start); h = Mix(h, static_cast<uint32_t>(args.base_vertex)); h = Mix(h, args.start_instance);
        ID3D11Buffer* buffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
        UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{}, offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
        c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, buffers, strides, offsets);
        uint64_t vb = 0;
        for (UINT i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++i) {
            if (!buffers[i]) continue;
            vb = Mix(vb, i); vb = Mix(vb, reinterpret_cast<uintptr_t>(buffers[i])); vb = Mix(vb, offsets[i]); vb = Mix(vb, strides[i]);
            buffers[i]->Release();
        }
        if (args.indexed) {
            Held<ID3D11Buffer> ib;
            DXGI_FORMAT format{};
            UINT offset = 0;
            c->IAGetIndexBuffer(&ib.p, &format, &offset);
            vb = Mix(vb, reinterpret_cast<uintptr_t>(ib.p)); vb = Mix(vb, offset); vb = Mix(vb, format);
        }
        rec.sig = h; rec.vb = vb;
    }
    {
        // The game's scissor when it scissors, else the whole viewport (DX12 always has one).
        Held<ID3D11RasterizerState> rs;
        c->RSGetState(&rs.p);
        D3D11_RASTERIZER_DESC r{};
        if (rs.p) rs.p->GetDesc(&r);
        rec.scissor[0] = 0; rec.scissor[1] = 0;
        rec.scissor[2] = static_cast<int32_t>(rec.width); rec.scissor[3] = static_cast<int32_t>(rec.height);
        if (r.ScissorEnable) {
            D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            c->RSGetScissorRects(&n, rects);
            if (n < 1) { gate(kGateViewport); return; }
            rec.scissor[0] = rects[0].left; rec.scissor[1] = rects[0].top;
            rec.scissor[2] = rects[0].right; rec.scissor[3] = rects[0].bottom;
        }
    }
    const uint64_t verts = uint64_t(args.count / 3) * 3 * (args.instances ? args.instances : 1);
    if (!verts) return;
    if (const uint64_t budget = g_budget.load(std::memory_order_relaxed)) {
        namespace rb = na::replay_budget;
        const bool movers = g_movers_on.load(std::memory_order_relaxed);
        const rb::Choice choice = rb::Choose(g_rpb, facts.frame, budget, movers, movers && g_table.IsMover(rec.sig, rec.vb), rec.sig, rec.vb, verts,
                                             {kRotateFrames, kProbeShare, kProbeFrames});
        g_threshold_seen.store(g_rpb.threshold, std::memory_order_relaxed);
        switch (choice) {
        case rb::Choice::MoverLeftOut: count(kMoverLeftOut); return;
        case rb::Choice::MoverAdmit: count(kMoverDraws); count(kMoverVerts, verts); break;
        case rb::Choice::DrawTooLarge: count(kDrawTooLarge); return;
        case rb::Choice::NotSampled: count(kNotSampled); return;
        case rb::Choice::FrameFull: count(kFrameFull); return;
        case rb::Choice::Admit: if (movers) count(kProbeVerts, verts); break;
        }
    }
    ID3D11GeometryShader* twin = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_shaders_m);
        if (entry->state == 0) {
            entry->state = 1;
            g_queue.push_back(entry);
            g_shaders_cv.notify_one();
            count(kTwinQueued);
            return;
        }
        if (entry->state == 1) { count(kTwinBuilding); return; }
        if (entry->state == 3) { count(kTwinFailed); return; }
        twin = entry->twin.Get();
    }
    if (!EnsureBuffers(dev.Get())) { count(kOverflow); return; }
    if (verts > kCapacity - g_acc.verts) { count(kOverflow); return; }
    {
        // The draw again, its vertex shader's positions streamed out at this draw's place in the
        // frame's buffer, nothing rasterised. The game had no geometry stage and no stream-output
        // target bound (gates above): both are put back empty.
        OwnWorkScope own;
        ID3D11Buffer* target = g_so.Get();
        const UINT offset = static_cast<UINT>(g_acc.verts * 16u);
        c->SOSetTargets(1, &target, &offset);
        c->GSSetShader(twin, nullptr, 0);
        const UINT instances = args.instances ? args.instances : 1;
        if (args.indexed) c->DrawIndexedInstanced(args.count, instances, args.start, args.base_vertex, args.start_instance);
        else c->DrawInstanced(args.count, instances, args.start, args.start_instance);
        c->GSSetShader(nullptr, nullptr, 0);
        c->SOSetTargets(0, nullptr, nullptr);
    }
    rec.first = static_cast<uint32_t>(g_acc.verts);
    rec.verts = static_cast<uint32_t>(verts);
    g_acc.verts += verts;
    g_acc.draws.push_back(rec);
    count(kReplayed);
    count(kVerts, verts);
}

Sealed Seal(uint64_t frame)
{
    Sealed out;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_acc.frame != frame) { g_acc.frame = frame; g_acc.verts = 0; g_acc.draws.clear(); }
    g_acc.sealed = true;
    out.frame = frame;
    out.verts = g_acc.verts;
    out.draws.swap(g_acc.draws);
    if (!out.draws.empty()) count(kSealed);
    return out;
}

bool Pack(ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* target, uint64_t verts)
{
    std::lock_guard<std::mutex> lock(g_m);
    if (!ctx || !target || !verts || verts > kCapacity || !g_pack || !g_so_view) { count(kPackFailed); return false; }
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    if (device.Get() != g_device.Get()) { count(kPackFailed); return false; }
    OwnWorkScope own;
    ComPtr<ID3D11ComputeShader> previous;
    ComPtr<ID3D11ShaderResourceView> previous_srv;
    ComPtr<ID3D11UnorderedAccessView> previous_uav;
    ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
    UINT n = D3D11_SHADER_MAX_INTERFACES;
    ctx->CSGetShader(&previous, instances, &n);
    ctx->CSGetShaderResources(0, 1, &previous_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &previous_uav);
    ID3D11ShaderResourceView* srv = g_so_view.Get();
    ctx->CSSetShader(g_pack.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    ctx->Dispatch(kPackWidth / 64u, PackRows(verts), 1);
    srv = previous_srv.Get();
    ID3D11UnorderedAccessView* uav = previous_uav.Get();
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetShader(previous.Get(), instances, n);
    for (UINT i = 0; i < n; ++i) if (instances[i]) instances[i]->Release();
    count(kPacked);
    return true;
}

void Learn(const Observation* seen, size_t n)
{
    if (!seen || !n || !g_movers_on.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_m);
    ++g_learn_clock;
    for (size_t i = 0; i < n; ++i) g_table.Report(seen[i].sig, seen[i].vb, seen[i].visible, seen[i].apart, g_learn_clock);
    g_reports.fetch_add(n, std::memory_order_relaxed);
    g_movers.store(g_table.Movers(), std::memory_order_relaxed);
    g_suspects.store(g_table.Suspects(), std::memory_order_relaxed);
    g_promoted.store(g_table.Promoted(), std::memory_order_relaxed);
    g_let_go.store(g_table.LetGo(), std::memory_order_relaxed);
}

Stats GetStats()
{
    Stats o;
    const auto v = [](uint32_t k) { return g_stat[k].load(std::memory_order_relaxed); };
    o.noted = v(kNoted); o.built = v(kBuilt); o.build_failed = v(kBuildFailed); o.replayed = v(kReplayed); o.vertices = v(kVerts);
    o.twin_queued = v(kTwinQueued); o.twin_building = v(kTwinBuilding); o.twin_failed = v(kTwinFailed); o.no_shader = v(kNoShader);
    o.overflow = v(kOverflow); o.sealed = v(kSealed); o.packed = v(kPacked); o.pack_failed = v(kPackFailed);
    o.not_sampled = v(kNotSampled); o.draw_too_large = v(kDrawTooLarge); o.frame_full = v(kFrameFull);
    o.budget = g_budget.load(std::memory_order_relaxed); o.budget_threshold = g_threshold_seen.load(std::memory_order_relaxed);
    o.on = g_on.load(std::memory_order_relaxed); o.movers_on = g_movers_on.load(std::memory_order_relaxed);
    o.movers = g_movers.load(); o.suspects = g_suspects.load(); o.promoted = g_promoted.load(); o.let_go = g_let_go.load();
    o.mover_draws = v(kMoverDraws); o.mover_verts = v(kMoverVerts); o.movers_left_out = v(kMoverLeftOut); o.probe_verts = v(kProbeVerts);
    o.reports = g_reports.load();
    static const char* const names[kGateCount] = {"deferred", "indirect", "other_depth", "query", "viewport", "topology", "stages", "stream_out",
        "predicated", "vs_uav", "no_depth_write", "admitted_depth_tested", "after_handover", "no_device"};
    int used = 0;
    for (uint32_t i = 0; i < kGateCount && used < static_cast<int>(sizeof o.gates) - 1; ++i) {
        const uint64_t n = g_gate[i].load(std::memory_order_relaxed);
        if (!n) continue;
        const int w = std::snprintf(o.gates + used, sizeof o.gates - used, "%s%s=%llu", used ? " " : "", names[i], static_cast<unsigned long long>(n));
        if (w < 0) break;
        used = (std::min)(used + w, static_cast<int>(sizeof o.gates) - 1);
    }
    return o;
}

} // namespace na::native11::draw_replay
