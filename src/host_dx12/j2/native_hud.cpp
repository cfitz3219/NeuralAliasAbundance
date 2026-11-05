#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/hud_picture.hpp"
#include "host_dx12/j2/hud_unblend.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/nr/native_fg.hpp"

#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/shader_operation.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/observe/shader_dimensions.hpp"
#include "host_dx12/scale/scoped_bindings.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace na::j2::native_hud {
namespace {
using Microsoft::WRL::ComPtr;

constexpr uint32_t kProxies = 8;     // learned proxies (a chain has 2-4; old ones age out)
constexpr uint32_t kViews = 4;       // render-target view formats per layer (typed, sRGB, ...)
constexpr uint64_t kGraveFrames = 8; // presents before a replaced layer is released

std::atomic<bool> g_installed{false}, g_on{true}, g_cover{false};
std::atomic<PreHudRenderer> g_test_renderer{nullptr};
struct CapturedPicture;
struct ProbeCapture;
struct NativeReplay;

struct Proxy {
    na_object_id id = 0;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t used = 0;           // last present (age-out order)
    // This frame (until TakeFrame):
    bool hud = false;            // the HUD has started: every later draw into the proxy is HUD
    bool cleared = false;        // the layers were cleared for this frame
    na_object_id mirrored = 0;   // the game depth target our native depths currently mirror
    na_object_id hud_ds = 0;     // the depth target the HUD used last frame (kept across frames)
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    uint32_t scene = 0, redirected = 0, forwarded = 0;
    uint32_t depth_draws = 0, blended = 0, covered = 0, world = 0, masks = 0; // of `redirected`: with a depth target / with RT0 blending
    // H1b (COMMS b07e083 / Astra 16bbca8): an UPSTREAM target is one the game composes the whole
    // picture in (scene composite, then its HUD) before handing it to the back buffer in one
    // full-screen copy or draw. It is learned from structure and then handled exactly like the
    // back buffer: its HUD draws go to the native layers, so the copy carries the scene only.
    bool upstream = false;
    bool preserve = false, clean = true, pixel_proved = false;
    std::shared_ptr<Picture> picture;
    std::vector<na::nr::native::RecordingLease> picture_uses;
    std::shared_ptr<CapturedPicture> output; // exact handoff into this back-buffer identity
    std::shared_ptr<ProbeCapture> probe,full_snapshot;
    std::shared_ptr<NativeReplay> replay;
    na_object_id source = 0;     // this frame: the single picture-sized input of the opaque full-screen hand-off into this proxy
    bool source_ambiguous = false;
    bool source_copy = false;    // that hand-off was a whole-resource Copy* (a transfer), not a draw (processing)
    // Upstream only: presents in a row whose hand-off did not come from it (current lineage,
    // Astra 20cddb8 H1b point 3), and paused = its HUD draws stay in it (the original full
    // picture is preserved) until the hand-off comes from it again.
    uint32_t lineage_miss = 0;
    bool paused = false;
};
struct Clear { D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH; float depth = 1.f; uint8_t stencil = 0; };
struct Layers {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> n[2];
    uint32_t w = 0, h = 0;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> rtv; // [2*i + layer]: view format i
    DXGI_FORMAT rtv_format[kViews] = {};
    uint32_t rtv_count = 0;
    UINT rtv_inc = 0;
    ComPtr<ID3D12Resource> d[2];
    uint32_t dw = 0, dh = 0;
    DXGI_FORMAT d_format = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> dsv; // [2*i + layer]
    DXGI_FORMAT dsv_format[kViews] = {};
    uint32_t dsv_count = 0;
    UINT dsv_inc = 0;
};

std::mutex g_m;
Proxy g_proxy[kProxies];
uint64_t g_frame = 0;
Layers g_l;
struct LayerStorage { Layers layers; std::vector<na::nr::native::RecordingLease> uses; };
struct CapturedPicture {
    std::shared_ptr<Picture> picture;
    std::shared_ptr<LayerStorage> owner;
    std::vector<na::nr::native::RecordingLease> uses;
    DXGI_FORMAT view=DXGI_FORMAT_UNKNOWN;
    bool clean=false;uint32_t draws=0,forwarded=0;
    std::shared_ptr<NativeReplay> replay;
    ComPtr<ID3D12Resource> final;
    ID3D12GraphicsCommandList* writer=nullptr;
    uint64_t writer_id=0,recording=0,order=0;
    na::nr::native::RecordingLease final_use;
    bool published=false,needs_full=false;
};
// CPU recording is not submission order. A later recording must not erase the
// immutable packet whose final write is about to execute/present.
std::vector<std::shared_ptr<CapturedPicture>> g_pending_outputs;
std::unordered_map<uint64_t,std::shared_ptr<CapturedPicture>> g_submitted_outputs;
struct NativeReplay {
    ID3D12Device* device=nullptr; // native/check textures retain it
    ComPtr<ID3D12Resource> native;
    ComPtr<ID3D12Resource> check,tail_native,tail_check;
    ComPtr<ID3D12DescriptorHeap> tail_rtv;
    std::shared_ptr<na::scale::ShaderDescriptorArena> tail_arena=std::make_shared<na::scale::ShaderDescriptorArena>();
    std::shared_ptr<const na::scale::PreparedBindings> tail_bindings[2];
    std::unordered_map<uint64_t,std::shared_ptr<const na::scale::GpuConstants>> tail_constants;
    bool tail=false,tail_snapshot=false;
    ComPtr<ID3D12DescriptorHeap> rtv;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::shared_ptr<Picture> picture;
    bool ui_only=false;
    uint64_t world_source=0;
    std::shared_ptr<Picture> world_picture;
    ComPtr<ID3D12Resource> world_native;
    struct Source {ID3D12GraphicsCommandList* list=nullptr;uint64_t recording=0,id=0;};
    std::shared_ptr<na::capture::j2::Identity> identity=std::make_shared<na::capture::j2::Identity>();
    std::vector<Source> sources;
    std::vector<na::nr::native::RecordingLease> uses;
    bool closed=false,submitted=false,complete=true,attempted=false,restart=false;
    struct Read {uint64_t id=0;uint32_t sub=0,state=0;na::observe::StateLookup lookup=na::observe::StateLookup::Absent;};
    std::vector<Read> reads;
    ComPtr<ID3D12CommandQueue> queue;
    UINT rtv_inc=0;
    uint32_t rw=0,rh=0,ow=0,oh=0,draws=0;
    DXGI_FORMAT view=DXGI_FORMAT_UNKNOWN;
    void Prefix(){
        D3D12_RESOURCE_BARRIER b[3]{};for(auto& t:b)t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition={native.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};
        b[1].Transition={picture->Scene(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE};
        b[2].Transition={check.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
        list->ResourceBarrier(3,b);list->CopyResource(check.Get(),picture->Scene());
        std::swap(b[1].Transition.StateBefore,b[1].Transition.StateAfter);
        b[2].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b[2].Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(2,b+1);
    }
};
std::vector<std::shared_ptr<NativeReplay>> g_replay_pool;
struct ProbeCapture {
    std::shared_ptr<Picture> picture;
    ComPtr<ID3D12Resource> source_resource;
    na::nr::native::RecordingLease use;
    uint64_t source=0,generation=0,write=0;
    ID3D12GraphicsCommandList* list=nullptr;uint64_t recording=0;
};
// Discovered from actual colour writes, independently of final SRV reflection.
// A canvas is only a probe candidate until the GPU compares the exact pictures.
struct Canvas {
    uint64_t id=0,generation=0,last_write=0,last_2d=0,used=0,probed=0;
    uint32_t rw=0,rh=0,ow=0,oh=0,blended=0,proofs=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    ID3D12GraphicsCommandList* writer=nullptr;uint64_t writer_id=0,recording=0;
    std::shared_ptr<ProbeCapture> snapshot;
};
std::vector<Canvas> g_canvases;
std::vector<std::shared_ptr<ProbeCapture>> g_probe_pool;
std::unordered_map<uint64_t,D3D12_RESOURCE_DESC> g_canvas_shapes;
uint64_t g_canvas_generation=0,g_draw_order=0;
std::shared_ptr<LayerStorage> g_layer_owner;
std::vector<std::shared_ptr<LayerStorage>> g_layer_pool;
std::vector<std::shared_ptr<Picture>> g_picture_pool;
std::shared_ptr<Picture> picture_locked(ID3D12Device* dev,const D3D12_RESOURCE_DESC& desc,uint32_t ow,uint32_t oh)
{
    g_picture_pool.erase(std::remove_if(g_picture_pool.begin(),g_picture_pool.end(),[&](const auto& p){
        return p.use_count()==1&&!p->Matches(dev,desc,ow,oh);
    }),g_picture_pool.end());
    for(const auto& p:g_picture_pool)if(p.use_count()==1&&p->Matches(dev,desc,ow,oh)){p->BeginCycle();return p;}
    auto p=Picture::Create(dev,desc,ow,oh);if(p)g_picture_pool.push_back(p);return p;
}
// The source commands and our Present commands can overlap. A count of
// Presents is not permission to overwrite a previous frame's native layers.
bool retain_layers_locked(ID3D12GraphicsCommandList* l,const Proxy& proxy)
{
    if (!g_layer_owner) {
        g_layer_pool.erase(std::remove_if(g_layer_pool.begin(),g_layer_pool.end(),[&](const auto& p){
            return p.use_count()==1&&(p->layers.w!=proxy.ow||p->layers.h!=proxy.oh||p->layers.typeless!=TypelessOf(proxy.format))&&
                std::all_of(p->uses.begin(),p->uses.end(),[](const auto& use){return use.Retired();});
        }),g_layer_pool.end());
        for (const auto& p : g_layer_pool) if (p.use_count()==1 &&
            std::all_of(p->uses.begin(),p->uses.end(),[](const auto& use){return use.Retired();})) {
            g_layer_owner=p;g_layer_owner->uses.clear();g_l=std::move(p->layers);break;
        }
        if (!g_layer_owner) { g_layer_owner=std::make_shared<LayerStorage>();g_layer_pool.push_back(g_layer_owner); }
    }
    auto lease=na::nr::native::RetainRecording(l,g_layer_owner);
    if (!lease) return false;
    if (std::none_of(g_layer_owner->uses.begin(),g_layer_owner->uses.end(),[&](const auto& v){return v.SameRecording(lease);}))
        g_layer_owner->uses.push_back(lease);
    return true;
}
struct Grave { uint64_t frame; ComPtr<ID3D12Resource> r; };
std::vector<Grave> g_grave;
std::unordered_map<na_object_id, Clear> g_clears;            // the game's last depth clear per target
std::unordered_map<na_object_id, D3D12_RESOURCE_DESC> g_ds;  // depth targets' descriptions
Stats g_stats;
// H1b learning: the hand-off source seen on consecutive presented frames without any HUD of its own.
struct Candidate { na_object_id id = 0; uint32_t frames = 0; } g_cand;
constexpr uint32_t kPromoteFrames = 3, kDemoteFrames = 3;
bool g_layers_cleared = false;    // this frame's layers were cleared (by the back buffer or its upstream: once, never between them)

// The bind the recording thread made last (set by the raster listener, read by the draw hook).
struct Bind { ID3D12GraphicsCommandList* list = nullptr; uint64_t recording = 0; na_object_id proxy = 0, mask_ds = 0, canvas = 0; };
constexpr uint32_t kEarlyMasks = 64; // depth-only draws mirrored per frame before the HUD's first draw
thread_local Bind t_bind;

// Frame-structure trace (2026-11-04): one whole frame every ~30 s (the first after 300 presents),
// at most kTraces per session: every draw outside the scene-depth passes (those are only counted
// per target), every whole-picture copy, in record order, with target size/format, depth use,
// blending, sampled textures and the camera binding. It answers where a game draws its HUD and
// how many hops (copy or processing draw, format changes) lie between that and the back buffer.
constexpr uint32_t kTraceLines = 1500, kTraces = 40;
std::atomic<bool> g_tracing{false};
uint64_t g_trace_next = 300;
uint32_t g_traces = 0, g_trace_lines = 0, g_trace_dispatches = 0, g_trace_dropped = 0;
std::unordered_map<na_object_id, uint32_t> g_trace_scene; // draws on the scene depth, per target
std::string g_trace_frame, g_trace_out;                 // the frame being traced / finished traces

Proxy* find_locked(na_object_id id)
{
    if (!id) return nullptr;
    for (auto& p : g_proxy)
        if (p.id == id) return &p;
    return nullptr;
}
Proxy* observe_canvas_locked(Canvas& c){
    if(auto* p=find_locked(c.id))return p;
    Proxy* slot=nullptr;
    for(auto& p:g_proxy)if(!p.id){slot=&p;break;}
    if(!slot)for(auto& p:g_proxy)if(!p.upstream&&p.used+8<g_frame&&(!slot||p.used<slot->used))slot=&p;
    if(!slot)for(auto& p:g_proxy)if(p.upstream&&!p.picture&&!p.replay&&!p.hud&&p.used+2<g_frame&&(!slot||p.used<slot->used))slot=&p;
    if(!slot)return nullptr;
    if(slot->upstream)++g_stats.upstream_dropped;
    *slot={};slot->id=c.id;slot->rw=c.rw;slot->rh=c.rh;slot->ow=c.ow;slot->oh=c.oh;slot->format=c.format;
    slot->upstream=slot->preserve=true;slot->used=g_frame;++g_stats.upstream_learned;
    return slot; // observational: original game draw is always forwarded
}

bool is_srgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
DXGI_FORMAT clear_view_of(DXGI_FORMAT typeless)
{
    switch (typeless) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
float stored_grey(DXGI_FORMAT typeless) // exactly representable: what the clear stores
{
    switch (typeless) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 512.f / 1023.f;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 1.f;
    default: return 128.f / 255.f;
    }
}

void bury_locked(ComPtr<ID3D12Resource>& r)
{
    if (r) g_grave.push_back({g_frame, r});
    r.Reset();
}

bool committed(ID3D12Device* dev, const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
}

// The two native colour layers for this proxy's size and format (RENDER_TARGET state).
bool layers_locked(ID3D12Device* dev, const Proxy& p)
{
    const DXGI_FORMAT tl = TypelessOf(p.format);
    if (tl == DXGI_FORMAT_UNKNOWN) return false;
    if (g_l.device.Get() != dev) { // a new device: start over (old objects age out)
        bury_locked(g_l.n[0]); bury_locked(g_l.n[1]); bury_locked(g_l.d[0]); bury_locked(g_l.d[1]);
        g_l = Layers{};
        g_l.device = dev;
    }
    if (!g_l.rtv) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 * (kViews + 1), D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2 * kViews, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_l.rtv))) || FAILED(dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&g_l.dsv)))) {
            g_l.rtv.Reset(); g_l.dsv.Reset();
            return false;
        }
        g_l.rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_l.dsv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    if (g_l.n[0] && g_l.w == p.ow && g_l.h == p.oh && g_l.typeless == tl) return true;
    bury_locked(g_l.n[0]); bury_locked(g_l.n[1]);
    g_l.rtv_count = 0;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = p.ow;
    d.Height = p.oh;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = tl;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < 2; ++i)
        if (!committed(dev, d, D3D12_RESOURCE_STATE_RENDER_TARGET, g_l.n[i])) { g_l.n[0].Reset(); g_l.n[1].Reset(); return false; }
    g_l.n[0]->SetName(L"NeuralAlias native HUD (over black)");
    g_l.n[1]->SetName(L"NeuralAlias native HUD (over grey)");
    g_l.w = p.ow; g_l.h = p.oh; g_l.typeless = tl;
    // Slot 0: the plain typed view used for our clears (stores the clear values exactly).
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = clear_view_of(tl);
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (int i = 0; i < 2; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_l.rtv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * g_l.rtv_inc;
        dev->CreateRenderTargetView(g_l.n[i].Get(), &rv, h);
    }
    return true;
}
// The two layers' views in the format the game's pipeline writes; false = no room.
bool rtv_locked(DXGI_FORMAT f, D3D12_CPU_DESCRIPTOR_HANDLE out[2])
{
    uint32_t i = 0;
    while (i < g_l.rtv_count && g_l.rtv_format[i] != f) ++i;
    if (i == g_l.rtv_count) {
        if (g_l.rtv_count == kViews) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = f;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 2; ++k) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_l.rtv->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(2 * (i + 1) + k) * g_l.rtv_inc;
            g_l.device->CreateRenderTargetView(g_l.n[k].Get(), &rv, h);
        }
        g_l.rtv_format[i] = f;
        ++g_l.rtv_count;
    }
    for (int k = 0; k < 2; ++k) {
        out[k] = g_l.rtv->GetCPUDescriptorHandleForHeapStart();
        out[k].ptr += static_cast<SIZE_T>(2 * (i + 1) + k) * g_l.rtv_inc;
    }
    return true;
}
D3D12_CPU_DESCRIPTOR_HANDLE clear_rtv_locked(int k)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_l.rtv->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(k) * g_l.rtv_inc;
    return h;
}
// Two native depth targets shaped like the game's HUD depth (DEPTH_WRITE state), with views in
// the pipeline's depth format.
bool depth_locked(const D3D12_RESOURCE_DESC& game, DXGI_FORMAT view, uint32_t w, uint32_t h, D3D12_CPU_DESCRIPTOR_HANDLE out[2])
{
    if (!g_l.d[0] || g_l.dw != w || g_l.dh != h || g_l.d_format != game.Format) {
        bury_locked(g_l.d[0]); bury_locked(g_l.d[1]);
        g_l.dsv_count = 0;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = game.Format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        for (int i = 0; i < 2; ++i)
            if (!committed(g_l.device.Get(), d, D3D12_RESOURCE_STATE_DEPTH_WRITE, g_l.d[i])) { g_l.d[0].Reset(); g_l.d[1].Reset(); return false; }
        g_l.d[0]->SetName(L"NeuralAlias native HUD depth A");
        g_l.d[1]->SetName(L"NeuralAlias native HUD depth B");
        g_l.dw = w; g_l.dh = h; g_l.d_format = game.Format;
    }
    uint32_t i = 0;
    while (i < g_l.dsv_count && g_l.dsv_format[i] != view) ++i;
    if (i == g_l.dsv_count) {
        if (g_l.dsv_count == kViews) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = view;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 2; ++k) {
            D3D12_CPU_DESCRIPTOR_HANDLE hd = g_l.dsv->GetCPUDescriptorHandleForHeapStart();
            hd.ptr += static_cast<SIZE_T>(2 * i + k) * g_l.dsv_inc;
            g_l.device->CreateDepthStencilView(g_l.d[k].Get(), &dv, hd);
        }
        g_l.dsv_format[i] = view;
        ++g_l.dsv_count;
    }
    for (int k = 0; k < 2; ++k) {
        out[k] = g_l.dsv->GetCPUDescriptorHandleForHeapStart();
        out[k].ptr += static_cast<SIZE_T>(2 * i + k) * g_l.dsv_inc;
    }
    return true;
}
bool ds_desc_locked(na_object_id id, D3D12_RESOURCE_DESC* out)
{
    const auto it = g_ds.find(id);
    if (it != g_ds.end()) { *out = it->second; return true; }
    ID3D12Resource* r = na::observe::AcquireResource(id);
    if (!r) return false;
    *out = r->GetDesc();
    r->Release();
    if (g_ds.size() > 256) g_ds.clear();
    g_ds[id] = *out;
    return true;
}

// The resource's 2D size and format (0 = unknown).
bool dims(na_object_id id, uint32_t* w, uint32_t* h, DXGI_FORMAT* f)
{
    ComPtr<ID3D12Resource> r;
    r.Attach(na::observe::AcquireResource(id));
    if (!r) return false;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    *w = static_cast<uint32_t>(d.Width); *h = d.Height; *f = d.Format;
    return true;
}
bool track_reads(NativeReplay& r,ID3D12GraphicsCommandList* game,const std::vector<uint64_t>& reads,uint64_t replaced=0,uint64_t replaced_world=0){
    for(const auto id:reads){if(id==replaced||id==replaced_world)continue;
        const auto old=std::find_if(r.reads.begin(),r.reads.end(),[&](const auto& value){return value.id==id;});
        UINT subs=0;
        if(old!=r.reads.end()){for(const auto& value:r.reads)if(value.id==id)++subs;}
        else{ComPtr<ID3D12Resource> resource;resource.Attach(na::observe::AcquireResource(id));if(!resource){return false;}
            const auto d=resource->GetDesc();subs=1;
            if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER){D3D12_FEATURE_DATA_FORMAT_INFO format{d.Format,1};
                if(FAILED(r.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&format,sizeof format))){return false;}
                subs=UINT(d.MipLevels)*(d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1u:d.DepthOrArraySize)*format.PlaneCount;
                if(!subs||subs>4096){return false;}}
        }
        for(UINT sub=0;sub<subs;++sub){uint32_t resource_state=0;const auto lookup=na::observe::ListStateSub(game,id,sub,&resource_state);
            if(lookup!=na::observe::StateLookup::Known&&lookup!=na::observe::StateLookup::Absent){return false;}
            auto before=std::find_if(r.reads.begin(),r.reads.end(),[&](const auto& value){return value.id==id&&value.sub==sub;});
            if(before==r.reads.end())r.reads.push_back({id,sub,resource_state,lookup});
            else if(before->lookup!=lookup||before->state!=resource_state){return false;}
        }
    }
    return true;
}
void record_native_hud_locked(Proxy& p,ID3D12GraphicsCommandList* game,
                             const na::observe::DrawInfo& state,const na::observe::ShaderCall& call)
{
    if(!p.preserve)return; // this frame's original-size replay and final proof decide presentation
    if(!state.restorable||!state.shader_readonly||
       state.rt_count!=1||state.pso_rt_count!=1||state.samples!=1||
       (state.ds&&state.dsv_format!=DXGI_FORMAT_UNKNOWN)){
        if(p.replay)p.replay->complete=false;return;
    }
    if(!p.replay){
        ComPtr<ID3D12Resource> source;source.Attach(na::observe::AcquireResource(p.id));
        ComPtr<ID3D12Device> device;if(!source||FAILED(source->GetDevice(IID_PPV_ARGS(&device))))return;
        const auto retired=[](const auto& r){return r.use_count()==1&&std::all_of(r->uses.begin(),r->uses.end(),[](const auto& use){return use.Retired();});};
        const auto matches=[&](const auto& r){return r->device==device.Get()&&r->rw==p.rw&&r->rh==p.rh&&r->ow==p.ow&&r->oh==p.oh&&r->view==static_cast<DXGI_FORMAT>(state.rtv_format);};
        g_replay_pool.erase(std::remove_if(g_replay_pool.begin(),g_replay_pool.end(),[&](const auto& r){return retired(r)&&!matches(r);}),g_replay_pool.end());
        std::shared_ptr<NativeReplay> r;
        for(const auto& q:g_replay_pool)if(retired(q)&&matches(q)){r=q;break;}
        const bool reuse=!!r;
        if(reuse){
            if(!r->closed)r->list->Close();
            if(FAILED(r->alloc->Reset())||FAILED(r->list->Reset(r->alloc.Get(),nullptr)))return;
            r->picture->BeginCycle();r->uses.clear();r->sources.clear();r->reads.clear();r->queue.Reset();
            r->closed=r->submitted=r->attempted=r->restart=r->tail=r->tail_snapshot=false;r->tail_bindings[0].reset();r->tail_bindings[1].reset();r->complete=true;r->draws=0;*r->identity={};
            ++g_stats.native_reused;
        }else{
            if(g_replay_pool.size()>=8)return; // older paths retain the original; never overwrite an executable packet
            r=std::make_shared<NativeReplay>();r->device=device.Get();r->rw=p.rw;r->rh=p.rh;r->ow=p.ow;r->oh=p.oh;
        }
        r->identity->evaluation_succeeded=false;
        r->ui_only=p.scene==0;r->world_source=0;
        r->view=static_cast<DXGI_FORMAT>(state.rtv_format);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        auto desc=source->GetDesc();desc.Width=p.ow;desc.Height=p.oh;desc.Format=clear_view_of(TypelessOf(p.format));
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(!reuse&&(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r->native)))||
           FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&r->rtv)))||
           FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&r->alloc)))||
           FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,r->alloc.Get(),nullptr,IID_PPV_ARGS(&r->list)))))return;
        desc.Width=p.rw;desc.Height=p.rh;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if(!reuse&&FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r->check))))return;
        D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=r->view;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(r->native.Get(),&view,r->rtv->GetCPUDescriptorHandleForHeapStart());
        r->rtv_inc=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto check_view=r->rtv->GetCPUDescriptorHandleForHeapStart();check_view.ptr+=r->rtv_inc;
        device->CreateRenderTargetView(r->check.Get(),&view,check_view);
        if(!reuse){r->picture=picture_locked(device.Get(),source->GetDesc(),p.ow,p.oh);if(!r->picture)return;g_replay_pool.push_back(r);++g_stats.native_allocated;}
        auto use=na::nr::native::RetainRecording(game,r);
        if(!r->picture||!use||!r->picture->SnapshotScene(game,source.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET))return;
        r->Prefix();
        r->uses.push_back(use);r->sources.push_back({game,state.recording,na::observe::IdOf(game)});p.replay=std::move(r);
    }
    auto& r=*p.replay;
    if(r.restart&&!r.closed){
        r.list->Close();
        if(FAILED(r.alloc->Reset())||FAILED(r.list->Reset(r.alloc.Get(),nullptr))){r.complete=false;return;}
        ComPtr<ID3D12Resource> source;source.Attach(na::observe::AcquireResource(p.id));
        auto use=na::nr::native::RetainRecording(game,p.replay);
        if(!source||!use||!r.picture->SnapshotScene(game,source.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)){r.complete=false;return;}
        r.uses.push_back(use);r.sources.clear();r.sources.push_back({game,state.recording,na::observe::IdOf(game)});
        r.draws=0;r.complete=true;r.restart=false;r.reads.clear();r.Prefix();
    }
    if(!r.complete||r.closed)return;
    if(r.view!=static_cast<DXGI_FORMAT>(state.rtv_format)){r.complete=false;return;}
    if(std::none_of(r.sources.begin(),r.sources.end(),[&](const auto& s){return s.list==game&&s.recording==state.recording;})){
        auto use=na::nr::native::RetainRecording(game,p.replay);if(!use){r.complete=false;return;}
        r.uses.push_back(use);r.sources.push_back({game,state.recording,na::observe::IdOf(game)});
    }
    std::vector<uint64_t> reads;
    if(!na::observe::RestoreListState(game,r.list.Get(),true,&reads)){r.complete=false;return;}
    if(!track_reads(r,game,reads)){r.complete=false;return;}
    auto target=r.rtv->GetCPUDescriptorHandleForHeapStart();r.list->OMSetRenderTargets(1,&target,FALSE,nullptr);
    D3D12_VIEWPORT vp[16]{};D3D12_RECT sc[16]{};const float sx=float(p.ow)/p.rw,sy=float(p.oh)/p.rh;
    for(UINT i=0;i<state.viewport_count;++i){float v[6];ScaleViewport(state.viewports[i],sx,sy,v);vp[i]={v[0],v[1],v[2],v[3],v[4],v[5]};}
    for(UINT i=0;i<state.scissor_count;++i){int32_t v[4];ScaleScissor(state.scissors[i],sx,sy,p.ow,p.oh,v);sc[i]={v[0],v[1],v[2],v[3]};}
    r.list->RSSetViewports(state.viewport_count,vp);r.list->RSSetScissorRects(state.scissor_count,sc);
    const auto issue=[&]{if(call.kind==na::observe::ShaderKind::Draw)r.list->DrawInstanced(UINT(call.args[0]),UINT(call.args[1]),UINT(call.args[2]),UINT(call.args[3]));
        else r.list->DrawIndexedInstanced(UINT(call.args[0]),UINT(call.args[1]),UINT(call.args[2]),INT(call.args[3]),UINT(call.args[4]));};
    issue();
    target.ptr+=r.rtv_inc;r.list->OMSetRenderTargets(1,&target,FALSE,nullptr);
    for(UINT i=0;i<state.viewport_count;++i){const auto* v=state.viewports[i];vp[i]={v[0],v[1],v[2],v[3],v[4],v[5]};}
    for(UINT i=0;i<state.scissor_count;++i){const auto* v=state.scissors[i];sc[i]={v[0],v[1],v[2],v[3]};}
    r.list->RSSetViewports(state.viewport_count,vp);r.list->RSSetScissorRects(state.scissor_count,sc);issue();
    ++r.draws;
}
bool record_finishing_draw(NativeReplay& r,ID3D12GraphicsCommandList* game,uint64_t source,const na::observe::ShaderCall& call){
    na::observe::DrawInfo state;na::observe::NativeOperation op;na::observe::ShaderState bindings;
    if(!na::observe::ListDrawState(game,state)||!state.restorable||!state.shader_readonly||state.depth_enable!=0||
       state.rt_count!=1||state.pso_rt_count!=1||state.samples!=1||state.blend_mask!=0||
       !na::observe::ListNativeOperation(game,false,op)||!na::observe::ListShaderState(game,false,bindings))return false;
    bool found=false;for(const auto& d:op.descriptors)if(d.used&&d.view.resource==source){
        if(d.view.kind!=NA_DESC_SRV||!d.view.explicit_view||d.view.srv.ViewDimension!=D3D12_SRV_DIMENSION_TEXTURE2D||
           d.view.srv.Texture2D.MostDetailedMip||d.view.srv.Texture2D.MipLevels!=1)return false;found=true;
    }
    if(!found||op.colors.size()!=1)return false;
    if(r.ui_only){
        // The transparent UI image is not the world image. Find the actual
        // separately sampled HDR scene in this same compositor and snapshot
        // it at consumption, before the game can reuse it.
        const na::observe::OperationDescriptor* world=nullptr;
        for(const auto& d:op.descriptors)if(d.used&&d.view.kind==NA_DESC_SRV&&d.view.resource!=source&&d.resource){
            const auto desc=d.resource->GetDesc();
            const auto f=d.view.explicit_view?d.view.srv.Format:desc.Format;
            if(d.view.explicit_view&&(d.view.srv.ViewDimension!=D3D12_SRV_DIMENSION_TEXTURE2D||d.view.srv.Texture2D.MostDetailedMip||
               d.view.srv.Texture2D.PlaneSlice||d.view.srv.Texture2D.ResourceMinLODClamp||
               d.view.srv.Shader4ComponentMapping!=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING))continue;
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=r.rw||desc.Height!=r.rh||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||
               (f!=DXGI_FORMAT_R16G16B16A16_FLOAT&&f!=DXGI_FORMAT_R32G32B32A32_FLOAT&&f!=DXGI_FORMAT_R11G11B10_FLOAT))continue;
            if(world&&world->view.resource!=d.view.resource){r.complete=false;return false;}world=&d;
        }
        uint32_t world_state=0;
        if(!world||na::observe::ListStateSub(game,world->view.resource,0,&world_state)!=na::observe::StateLookup::Known){r.complete=false;return false;}
        const auto desc=world->resource->GetDesc();
        if(!r.world_picture||!r.world_picture->Matches(r.device,desc,r.ow,r.oh))r.world_picture=picture_locked(r.device,desc,r.ow,r.oh);
        else r.world_picture->BeginCycle();
        if(!r.world_picture||!r.world_picture->SnapshotScene(game,world->resource.Get(),static_cast<D3D12_RESOURCE_STATES>(world_state))){r.complete=false;return false;}
        r.world_source=world->view.resource;
        if(!r.world_native){
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;auto native_desc=desc;
            native_desc.Width=r.ow;native_desc.Height=r.oh;native_desc.MipLevels=1;native_desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            native_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if(FAILED(r.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&native_desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r.world_native)))){r.complete=false;return false;}
        }
    }
    auto target_view=op.colors[0].view.rtv;
    if(!op.colors[0].view.explicit_view){target_view.Format=static_cast<DXGI_FORMAT>(state.rtv_format);target_view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;}
    if(target_view.ViewDimension!=D3D12_RTV_DIMENSION_TEXTURE2D)return false;
    const auto format=static_cast<DXGI_FORMAT>(state.rtv_format);
    if(TypelessOf(format)!=TypelessOf(r.view))return false;
    // Allocate once per retired replay packet, not per frame or per draw.
    if(!r.tail_native||!r.tail_check||!r.tail_rtv){
        r.tail_native.Reset();r.tail_check.Reset();r.tail_rtv.Reset();
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;auto desc=r.native->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if(FAILED(r.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r.tail_native))))return false;
        desc.Width=r.rw;desc.Height=r.rh;
        if(FAILED(r.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r.tail_check))))return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(FAILED(r.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&r.tail_rtv))))return false;
    }
    std::shared_ptr<const na::scale::PreparedBindings> plans[2];std::string error;
    for(UINT pass=0;pass<2;++pass){
        na::scale::BindingSpec spec;spec.root=op.root;spec.descriptors=r.tail_arena;
        spec.native_extent={r.rw,r.rh};spec.render_extent=pass?spec.native_extent:na::scale::Extent{r.ow,r.oh};
        spec.viewports=op.viewports;spec.scissors=op.scissors;
        for(const auto& table:op.tables)spec.tables.push_back({table.param,table.first});
        for(const auto& d:op.descriptors){na::scale::ShaderView v;
            if(!d.used){v.srv.Format=DXGI_FORMAT_R8_UNORM;v.srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
                v.srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.srv.Texture2D.MipLevels=1;}
            else {const bool world=r.world_source&&d.view.resource==r.world_source;
                v.resource=d.view.resource==source?(pass?r.check.Get():r.native.Get()):world?(pass?r.world_picture->Scene():r.world_native.Get()):d.resource.Get();v.counter=d.counter;v.default_view=!d.view.explicit_view;
                if(d.view.kind==NA_DESC_SRV)v.srv=d.view.srv;
                else if(d.view.kind==NA_DESC_CBV){v.kind=na::scale::ShaderView::Kind::NativeCbv;v.cbv=d.view.cbv;}
                else return false;
                if(world){v.default_view=false;v.srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    v.srv.Format=pass?r.world_picture->Scene()->GetDesc().Format:DXGI_FORMAT_R16G16B16A16_FLOAT;
                    v.srv.Texture2D={0,1,0,0};}}
            spec.views.push_back(std::move(v));
        }
        if(!pass)for(const auto& c:op.constants)if(!c.dimensions.empty()){
            std::vector<uint8_t> bytes(c.bytes);
            if(na::observe::ReadBuffer(c.resource_id,c.offset,bytes.data(),c.bytes)!=c.bytes)return false;
            std::vector<na::scale::ConstantPatch> patches;uint64_t key=c.resource_id;
            na::observe::OperationKey::add(key,c.offset);na::observe::OperationKey::add(key,c.bytes);
            for(const auto& field:c.dimensions){
                if(field.optional)continue; // optional trailing pairs need the existing value test
                patches.push_back({field.offset,field.integer?na::scale::Scalar::UInt:na::scale::Scalar::Float,
                    static_cast<na::scale::Dimension>(field.dimension),spec.native_extent,spec.render_extent});
                na::observe::OperationKey::add(key,field.offset);na::observe::OperationKey::add(key,field.dimension);na::observe::OperationKey::add(key,field.integer);
            }
            if(patches.empty())continue;
            auto cached=r.tail_constants.find(key);
            if(cached==r.tail_constants.end()){
                auto cb=na::scale::GpuConstants::Create(r.device,c.resource.Get(),c.offset,bytes.data(),bytes.size(),patches,error);
                if(!cb)return false;if(r.tail_constants.size()>=64)r.tail_constants.clear();cached=r.tail_constants.emplace(key,std::move(cb)).first;
            }
            D3D12_HEAP_PROPERTIES hp{};if(FAILED(c.resource->GetHeapProperties(&hp,nullptr)))return false;
            uint32_t cb_state=D3D12_RESOURCE_STATE_GENERIC_READ;
            if(hp.Type!=D3D12_HEAP_TYPE_UPLOAD&&hp.Type!=static_cast<D3D12_HEAP_TYPE>(5)&&
               na::observe::ListStateSub(game,c.resource_id,0,&cb_state)!=na::observe::StateLookup::Known)return false;
            if(!cached->second->CanRecord(r.list.Get(),static_cast<D3D12_RESOURCE_STATES>(cb_state)))return false;
            const UINT index=UINT(spec.constants.size());spec.constants.push_back({cached->second,static_cast<D3D12_RESOURCE_STATES>(cb_state)});
            if(c.descriptor==UINT_MAX)spec.root_cbvs.push_back({c.param,index});
            else if(c.descriptor<spec.views.size()){spec.views[c.descriptor].kind=na::scale::ShaderView::Kind::PatchedCbv;spec.views[c.descriptor].constant=index;}
            else return false;
        }
        plans[pass]=na::scale::PreparedBindings::Create(r.device,std::move(spec),error);if(!plans[pass])return false;
    }
    std::vector<uint64_t> reads;if(!na::observe::RestoreListState(game,r.list.Get(),true,&reads)||!track_reads(r,game,reads,source,r.world_source))return false;
    if(r.world_source){D3D12_RESOURCE_BARRIER world_barriers[2]{};
        for(auto& barrier:world_barriers)barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        world_barriers[0].Transition={r.world_native.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        world_barriers[1].Transition={r.world_picture->Scene(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        r.list->ResourceBarrier(2,world_barriers);}
    D3D12_RESOURCE_BARRIER barriers[4]{};ID3D12Resource* resources[]={r.native.Get(),r.check.Get(),r.tail_native.Get(),r.tail_check.Get()};
    for(UINT i=0;i<4;++i){barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition={resources[i],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            i<2?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            i<2?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_RENDER_TARGET};}
    r.list->ResourceBarrier(4,barriers);
    for(UINT pass=0;pass<2;++pass){
        if(!na::observe::RestoreListState(game,r.list.Get(),true)){r.complete=false;return false;}
        const auto& plan=*plans[pass];const auto& spec=plan.Spec();
        for(const auto& c:spec.constants)c.buffer->RecordBefore(r.list.Get(),c.source_state);
        ID3D12DescriptorHeap* heaps[]={plan.ShaderHeap(),bindings.samplers};r.list->SetDescriptorHeaps(bindings.samplers?2:1,heaps);
        const UINT inc=r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto base=plan.ShaderHeap()->GetGPUDescriptorHandleForHeapStart();
        for(const auto& t:spec.tables)r.list->SetGraphicsRootDescriptorTable(t.param,{base.ptr+UINT64(plan.ShaderOffset()+t.first)*inc});
        for(UINT i=0;i<bindings.count;++i)if(bindings.tables[i].sampler)r.list->SetGraphicsRootDescriptorTable(bindings.tables[i].param,{bindings.tables[i].handle});
        for(const auto& cb:spec.root_cbvs)r.list->SetGraphicsRootConstantBufferView(cb.param,spec.constants[cb.constant].buffer->Address());
        auto target=r.tail_rtv->GetCPUDescriptorHandleForHeapStart();target.ptr+=SIZE_T(pass)*r.rtv_inc;
        auto view=target_view;r.device->CreateRenderTargetView(pass?r.tail_check.Get():r.tail_native.Get(),&view,target);
        r.list->OMSetRenderTargets(1,&target,FALSE,nullptr);r.list->RSSetViewports(UINT(plan.Viewports().size()),plan.Viewports().data());
        r.list->RSSetScissorRects(UINT(plan.Scissors().size()),plan.Scissors().data());
        if(call.kind==na::observe::ShaderKind::Draw)r.list->DrawInstanced(UINT(call.args[0]),UINT(call.args[1]),UINT(call.args[2]),UINT(call.args[3]));
        else r.list->DrawIndexedInstanced(UINT(call.args[0]),UINT(call.args[1]),UINT(call.args[2]),INT(call.args[3]),UINT(call.args[4]));
        for(const auto& c:spec.constants)c.buffer->RecordAfter(r.list.Get());
    }
    for(UINT i=0;i<4;++i){barriers[i].Transition.StateBefore=i<2?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[i].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;}
    r.list->ResourceBarrier(4,barriers);
    if(r.world_source){D3D12_RESOURCE_BARRIER world_barriers[2]{};
        for(auto& barrier:world_barriers)barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        world_barriers[0].Transition={r.world_native.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        world_barriers[1].Transition={r.world_picture->Scene(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        r.list->ResourceBarrier(2,world_barriers);}
    r.tail_bindings[0]=plans[0];r.tail_bindings[1]=plans[1];r.tail=true;return true;
}
void probe_canvas_locked(Proxy& output,ID3D12GraphicsCommandList* list)
{
    Canvas* best=nullptr;
    for(auto& c:g_canvases)if(c.id!=output.id&&c.rw==output.rw&&c.rh==output.rh&&
        TypelessOf(c.format)==TypelessOf(output.format)&&c.blended&&c.last_2d==c.last_write&&c.used+2>=g_frame&&
        (!best||c.probed<best->probed||(c.probed==best->probed&&c.last_2d>best->last_2d)))best=&c;
    if(!best)return;
    if(best->ow!=output.ow||best->oh!=output.oh){best->ow=output.ow;best->oh=output.oh;best->proofs=0;best->generation=++g_canvas_generation;}
    best->probed=++g_draw_order;
    // A producer-list immutable snapshot remains usable when the final draw
    // has no local state for the source. Submission leases establish ordering.
    if(best->snapshot&&best->snapshot->write==best->last_write){
        output.probe=std::move(best->snapshot);++g_stats.canvas_source_probes;return;
    }
    uint32_t state=0;
    if(na::observe::ListStateSub(list,best->id,0,&state)!=na::observe::StateLookup::Known){
        ++g_stats.canvas_no_state;std::snprintf(g_stats.upstream_why,sizeof g_stats.upstream_why,"canvas probe: no local texture state");return;
    }
    ComPtr<ID3D12Resource> source;source.Attach(na::observe::AcquireResource(best->id));
    ComPtr<ID3D12Device> device;if(!source||FAILED(source->GetDevice(IID_PPV_ARGS(&device))))return;
    auto picture=picture_locked(device.Get(),source->GetDesc(),output.ow,output.oh);
    if(!picture)return;
    auto capture=std::make_shared<ProbeCapture>();capture->picture=picture;capture->source_resource=source;
    auto lease=na::nr::native::RetainRecording(list,capture);
    if(!lease||!picture->SnapshotFull(list,source.Get(),static_cast<D3D12_RESOURCE_STATES>(state)))return;
    capture->use=std::move(lease);
    capture->source=best->id;capture->generation=best->generation;output.probe=std::move(capture);
    ++g_stats.canvas_probes;
}
// H1b: an opaque full-screen draw into a back buffer before its HUD: note the one picture-sized
// texture it reads (more than one = a composite of several inputs, not a hand-off: no source).
void note_source_locked(Proxy& p, ID3D12GraphicsCommandList* list,const na::observe::ShaderCall* call=nullptr)
{
    if (p.upstream) return;
    na::observe::NativeOperation op;
    uint64_t one = 0;
    const bool snapshot=na::observe::ListNativeOperation(list,false,op);
    bool identified=snapshot&&PictureSource(op,p.id,p.rw,p.rh,one)&&one;
    if(!identified&&snapshot&&op.reflection_complete){
        // A compositor may read BOTH the world and a separate UI canvas. The
        // UI texture can never equal the finished scene; follow its actual
        // dependency and redraw its glyphs before applying this compositor.
        Canvas* ui=nullptr;
        for(const auto& d:op.descriptors)if(d.used&&d.view.kind==NA_DESC_SRV&&d.view.resource!=p.id){
            auto c=std::find_if(g_canvases.begin(),g_canvases.end(),[&](const auto& x){return x.id==d.view.resource;});
            if(c!=g_canvases.end()&&c->blended&&c->rw==p.rw&&c->rh==p.rh&&
               (!ui||c->blended>ui->blended))ui=&*c;
        }
        if(ui&&observe_canvas_locked(*ui)){one=ui->id;identified=true;}
    }
    // This opaque full-target write replaces earlier output contents. A failed
    // earlier draw must not permanently poison a later valid handoff.
    p.source=identified?one:0;p.source_ambiguous=!identified;p.source_copy=false;p.output.reset();p.probe.reset();
    if(!identified){
        if(!snapshot)++g_stats.handoff_snapshot_missing;else++g_stats.handoff_no_source;
        // A learned canvas may be captured even when reflection cannot resolve
        // the final shader. The SAME-FRAME GPU proof still chooses original
        // complete colour if this is not the image actually presented.
        const Proxy* newest=nullptr;uint64_t order=0;
        for(const auto& u:g_proxy)if(u.id&&u.upstream&&u.pixel_proved&&(u.picture||u.hud||u.redirected||u.forwarded)&&
            u.rw==p.rw&&u.rh==p.rh&&u.ow==p.ow&&u.oh==p.oh){
            const auto c=std::find_if(g_canvases.begin(),g_canvases.end(),[&](const auto& canvas){return canvas.id==u.id;});
            const uint64_t written=c==g_canvases.end()?0:c->last_write;
            if(!newest||written>order){newest=&u;order=written;}
        }
        if(newest){p.source=newest->id;p.source_ambiguous=false;++g_stats.handoff_inferred;}
        if(!p.source){probe_canvas_locked(p,list);return;}
    }
    // Preserve the actual full upstream picture at the transfer occurrence.
    // Sampling the resource later at Present would permit another writer to
    // substitute a different frame. Original game draw is always forwarded.
    if (auto* u=find_locked(p.source);u&&u->preserve) {
        uint32_t state=0;const auto known=na::observe::ListStateSub(list,u->id,0,&state);
        ComPtr<ID3D12Resource> source;source.Attach(na::observe::AcquireResource(u->id));
        auto lease=u->picture?na::nr::native::RetainRecording(list,u->picture):na::nr::native::RecordingLease{};
        const auto canvas=std::find_if(g_canvases.begin(),g_canvases.end(),[&](const auto& c){return c.id==u->id;});
        const bool source_owned=u->full_snapshot&&u->full_snapshot->picture==u->picture&&canvas!=g_canvases.end()&&
            u->full_snapshot->write==canvas->last_write;
        const bool full=source_owned||(source&&known==na::observe::StateLookup::Known&&u->picture&&
            u->picture->SnapshotFull(list,source.Get(),static_cast<D3D12_RESOURCE_STATES>(state)));
        const bool native_candidate=u->replay&&u->replay->complete&&u->replay->draws&&call;
        if (source&&lease&&(full||native_candidate)&&g_layer_owner) {
            auto capture=std::make_shared<CapturedPicture>();capture->picture=u->picture;capture->owner=g_layer_owner;
            capture->uses=u->picture_uses;capture->uses.push_back(lease);if(source_owned)capture->uses.push_back(u->full_snapshot->use);capture->view=u->view;
            capture->final.Attach(na::observe::AcquireResource(p.id));capture->writer=list;
            capture->writer_id=na::observe::IdOf(list);capture->recording=na::observe::ListRecording(list);
            capture->order=++g_draw_order;capture->final_use=lease;
            capture->clean=full&&u->clean&&u->forwarded==0;capture->draws=u->redirected;capture->forwarded=u->forwarded;capture->needs_full=!full;
            if(auto replay=u->replay;replay&&replay->complete&&replay->draws&&!replay->closed&&!replay->restart){
                auto retained=na::nr::native::RetainRecording(list,replay);
                if(retained){
                    if(source_owned)replay->uses.push_back(u->full_snapshot->use);
                    const bool tail=call&&(call->args[0]==3||call->args[0]==6)&&call->args[1]==1&&
                        record_finishing_draw(*replay,list,u->id,*call);
                    const bool native_full=tail||source_owned||(known==na::observe::StateLookup::Known&&replay->picture->SnapshotFull(list,source.Get(),static_cast<D3D12_RESOURCE_STATES>(state)));
                    if(!tail){
                    D3D12_RESOURCE_BARRIER end{};end.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    end.Transition={replay->native.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
                    replay->list->ResourceBarrier(1,&end);
                    end.Transition.pResource=replay->check.Get();replay->list->ResourceBarrier(1,&end);
                    }
                    if(native_full&&replay->complete&&SUCCEEDED(replay->list->Close())){
                        replay->closed=true;replay->uses.push_back(retained);
                        const auto recording=na::observe::ListRecording(list);
                        if(std::none_of(replay->sources.begin(),replay->sources.end(),[&](const auto& s){return s.list==list&&s.recording==recording;}))
                            replay->sources.push_back({list,recording,na::observe::IdOf(list)});
                        capture->replay=replay;
                        ++g_stats.native_recorded;
                    }
                }
            }
            capture->owner->layers=std::move(g_l);g_layer_owner.reset();g_layers_cleared=false;
            if(full||capture->replay)p.output=std::move(capture);
            g_pending_outputs.erase(std::remove_if(g_pending_outputs.begin(),g_pending_outputs.end(),[](const auto& q){
                return q->published||q->final_use.Retired();
            }),g_pending_outputs.end());
            if(p.output)g_pending_outputs.push_back(p.output);
            // End the source occurrence at the handoff, not at a later CPU
            // Present. Render-ahead can already record the NEXT scene/HUD by
            // the time this back buffer is presented.
        }
        if(g_layer_owner){g_layer_owner->layers=std::move(g_l);g_layer_owner.reset();g_layers_cleared=false;}
        u->picture.reset();u->full_snapshot.reset();u->picture_uses.clear();u->hud=u->cleared=false;
        u->replay.reset();
        u->mirrored=0;u->view=DXGI_FORMAT_UNKNOWN;u->clean=true;
        u->scene=u->redirected=u->forwarded=u->depth_draws=u->blended=u->covered=u->world=u->masks=0;
    }
}
void note_forward_locked(Proxy& p, const char* why)
{
    ++p.forwarded;
    ++g_stats.forwarded;
    std::snprintf(g_stats.last_forward, sizeof g_stats.last_forward, "%s", why);
}

void trace_line_locked(const char* text)
{
    if (g_trace_dispatches) {
        char d[64];
        std::snprintf(d, sizeof d, "hud_trace {\"t\":\"dispatch\",\"n\":%u}\r\n", g_trace_dispatches);
        g_trace_dispatches = 0;
        if (g_trace_lines < kTraceLines) { g_trace_frame += d; ++g_trace_lines; } else ++g_trace_dropped;
    }
    if (g_trace_lines >= kTraceLines) { ++g_trace_dropped; return; }
    g_trace_frame += text;
    ++g_trace_lines;
}
void trace_draw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& call)
{
    if (call.kind == na::observe::ShaderKind::Dispatch) { std::lock_guard<std::mutex> lock(g_m); ++g_trace_dispatches; return; }
    na::observe::DrawInfo st;
    if (!na::observe::ListDrawState(l, st)) return;
    uint64_t scene_depth = 0, scene_color = 0;
    na::observe::GetLiveSelection(&scene_depth, &scene_color);
    if (st.ds && st.ds == scene_depth) { std::lock_guard<std::mutex> lock(g_m); ++g_trace_scene[st.rt]; return; }
    na_object_id srv[8];
    bool incomplete = false;
    const uint32_t n = na::observe::ListCurrentGraphicsSrvs(l, srv, 8, &incomplete);
    bool cam = false;
    {
        na::observe::LiveBinding cb[16];
        const uint32_t m = na::observe::ListCurrentGraphicsCbvs(l, cb, 16);
        for (uint32_t i = 0; i < m && !cam; ++i) cam = na::j2::CameraBinding(cb[i].resource, cb[i].offset, cb[i].param, cb[i].table);
    }
    uint32_t w = 0, h = 0; DXGI_FORMAT f{};
    if (st.rt) dims(st.rt, &w, &h, &f);
    char b[512];
    int k = std::snprintf(b, sizeof b,
        "hud_trace {\"t\":\"draw\",\"rt\":%llu,\"rts\":%u,\"w\":%u,\"h\":%u,\"fmt\":%u,\"view\":%u,\"ds\":%llu,\"depth_on\":%d,\"blend\":%d,\"n\":%llu,\"inst\":%llu,\"cam\":%d,\"srv_incomplete\":%d,\"srv\":[",
        (unsigned long long)st.rt, st.rt_count, w, h, unsigned(f), st.rtv_format, (unsigned long long)st.ds,
        st.depth_enable == NA_UNKNOWN_U32 ? -1 : int(st.depth_enable != 0), st.blend_mask == NA_UNKNOWN_U32 ? -1 : int(st.blend_mask & 1u),
        (unsigned long long)call.args[0], (unsigned long long)call.args[1], cam ? 1 : 0, incomplete ? 1 : 0);
    for (uint32_t i = 0; i < n && k > 0 && k < int(sizeof b) - 48; ++i) {
        uint32_t sw = 0, sh = 0; DXGI_FORMAT sf{};
        dims(srv[i], &sw, &sh, &sf);
        k += std::snprintf(b + k, sizeof b - k, "%s[%llu,%u,%u,%u]", i ? "," : "", (unsigned long long)srv[i], sw, sh, unsigned(sf));
    }
    if (k > 0 && k < int(sizeof b) - 6) std::snprintf(b + k, sizeof b - k, "]}\r\n");
    else return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_tracing.load(std::memory_order_relaxed)) trace_line_locked(b);
}

void draw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& c)
{
    if (c.kind == na::observe::ShaderKind::Draw)
        l->DrawInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), UINT(c.args[3]));
    else
        l->DrawIndexedInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), INT(c.args[3]), UINT(c.args[4]));
}

// A depth/stencil-only draw (no colour target) on the HUD's depth target while the HUD is being
// drawn natively: UI clipping masks (stencil) and depth pre-passes of UI 3D models. Our native
// depth mirrors must get them too, or the masked text and the model fail their tests in the
// layers (Anno: menu text visible only in some places, the main menu's character head and the
// loading tips missing, Cort 2026-11-03). The game's own draw still runs.
void mirror_depth_only(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& call, const na::observe::DrawInfo& st)
{
    if (!st.ds || !st.pipeline_known || st.pso_rt_count != 0 || st.dsv_format == DXGI_FORMAT_UNKNOWN ||
        !st.restorable || st.samples != 1 || !st.viewport_count || !st.scissor_count)
        return;
    std::lock_guard<std::mutex> lock(g_m);
    // The HUD's depth target: the one mirrored now, or the one the HUD used last frame (a mask can
    // come before the frame's first HUD draw).
    Proxy* p = nullptr;
    for (auto& q : g_proxy)
        if (q.id && q.ow && (q.mirrored == st.ds || (!q.mirrored && q.hud_ds == st.ds))) { p = &q; break; }
    if (!p || !g_on.load()) return;
    if(p->preserve&&(!st.shader_readonly||st.query_active)){p->clean=false;return;}
    // Before the HUD's first draw the target may still hold the picture's depth passes: only a few.
    if (!p->hud && p->masks >= kEarlyMasks) return;
    D3D12_RESOURCE_DESC dd{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv[2];
    ComPtr<ID3D12Device> dev;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev))) || !retain_layers_locked(l,*p) || !layers_locked(dev.Get(), *p)) return;
    if (!ds_desc_locked(st.ds, &dd) || dd.SampleDesc.Count != 1 ||
        !depth_locked(dd, static_cast<DXGI_FORMAT>(st.dsv_format), p->ow, p->oh, dsv))
        return;
    if (p->mirrored != st.ds) { // start the mirrors from the game's last clear of this target
        Clear c{};
        const auto it = g_clears.find(st.ds);
        if (it != g_clears.end()) c = it->second;
        else c.depth = (st.depth_func == D3D12_COMPARISON_FUNC_GREATER || st.depth_func == D3D12_COMPARISON_FUNC_GREATER_EQUAL) ? 0.f : 1.f;
        const D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
        l->ClearDepthStencilView(dsv[0], f, c.depth, c.stencil, 0, nullptr);
        l->ClearDepthStencilView(dsv[1], f, c.depth, c.stencil, 0, nullptr);
        p->mirrored = st.ds;
    }
    const float sx = float(p->ow) / float(p->rw), sy = float(p->oh) / float(p->rh);
    D3D12_VIEWPORT vp[16];
    D3D12_RECT sc[16];
    for (uint32_t i = 0; i < st.viewport_count; ++i) {
        float o[6];
        ScaleViewport(st.viewports[i], sx, sy, o);
        vp[i] = {o[0], o[1], o[2], o[3], o[4], o[5]};
    }
    for (uint32_t i = 0; i < st.scissor_count; ++i) {
        int32_t o[4];
        ScaleScissor(st.scissors[i], sx, sy, p->ow, p->oh, o);
        sc[i] = {o[0], o[1], o[2], o[3]};
    }
    l->RSSetViewports(st.viewport_count, vp);
    l->RSSetScissorRects(st.scissor_count, sc);
    for (int k = 0; k < 2; ++k) {
        l->OMSetRenderTargets(0, nullptr, FALSE, &dsv[k]);
        draw(l, call);
    }
    if (!na::observe::RestoreBindingState(l, false, false, 0, true, true, true)) ++g_stats.restore_failed;
    ++p->masks;
    ++g_stats.masks;
}

na::observe::ShaderScope redirect(ID3D12GraphicsCommandList* l, na::observe::ShaderCall& call)
{
    na::observe::DrawInfo st;
    if (!na::observe::ListDrawState(l, st) || st.recording != t_bind.recording) return {};
    // Depth/stencil only (the game's draw still runs): no colour target bound, or a pipeline
    // without one while the proxy is bound.
    if (st.ds && (st.rt_count == 0 ? st.ds == t_bind.mask_ds
                                   : t_bind.proxy && st.rt == t_bind.proxy && st.pipeline_known && st.pso_rt_count == 0)) {
        mirror_depth_only(l, call, st);
        return {};
    }
    if (!t_bind.proxy || st.rt != t_bind.proxy) return {};
    std::lock_guard<std::mutex> lock(g_m);
    Proxy* p = find_locked(st.rt);
    if (!p || !p->ow) return {};
    if (p->upstream && p->paused) return {}; // lineage not current: the original picture keeps its HUD
    const bool shaped = SceneShaped(st.pipeline_known && st.blend_mask != NA_UNKNOWN_U32, st.blend_mask, uint64_t(call.args[0]), uint64_t(call.args[1]));
    bool observed_output=false;
    if(!p->upstream)for(const auto& q:g_proxy)observed_output|=q.id&&q.upstream&&q.preserve&&q.rw==p->rw&&q.rh==p->rh&&q.ow==p->ow&&q.oh==p->oh;
    if(observed_output&&!shaped){note_forward_locked(*p,"late output UI preserved in the complete picture");return {};}
    if(p->preserve&&p->hud&&shaped&&CoversTarget(st.viewport_count?st.viewports[0]:nullptr,st.scissor_count?st.scissors[0]:nullptr,p->rw,p->rh)) {
        if(p->replay)p->replay->restart=true;
        p->clean=false;note_forward_locked(*p,"later full-picture processing preserved");return {};
    }
    // A draw bound to the camera block J2 jitters is drawn with the (jittered) camera: part of the
    // picture, whatever its order. In the native layer it would keep its jitter and shimmer
    // (Anno's foliage flicker, 2026-11-03: Cort saw it stop when those draws left the layer).
    {
        na::observe::LiveBinding cb[16];
        const uint32_t n = na::observe::ListCurrentGraphicsCbvs(l, cb, 16);
        for (uint32_t i = 0; i < n; ++i)
            if (na::j2::CameraBinding(cb[i].resource, cb[i].offset, cb[i].param, cb[i].table)) {
                ++p->world;
                if(p->replay)p->replay->restart=true;
                ++g_stats.world;
                if (p->hud) p->clean=false;
                return {};
            }
        // Depth-tested against the published scene depth: drawn into the 3D picture (a world
        // marker, a late effect), whatever its order. Combined evidence, never a single cue.
        uint64_t scene_depth = 0, scene_color = 0;
        na::observe::GetLiveSelection(&scene_depth, &scene_color);
        if (scene_depth && st.ds == scene_depth && st.pipeline_known && st.depth_enable != NA_UNKNOWN_U32 && st.depth_enable != 0u &&
            st.dsv_format != DXGI_FORMAT_UNKNOWN) {
            ++p->world;
            ++g_stats.world_depth;
            if(p->replay)p->replay->restart=true;
            if (p->hud) p->clean=false;
            return {};
        }
    }
    // Screen-position full-screen triangles are image processing, not a HUD
    // boundary merely because they lack the selected camera/depth binding.
    // Keep them in the scene and start the terminal HUD capture AFTER them.
    if(p->upstream&&p->preserve&&st.pixel_position&&call.args[0]==3&&call.args[1]==1&&
       CoversTarget(st.viewport_count?st.viewports[0]:nullptr,st.scissor_count?st.scissors[0]:nullptr,p->rw,p->rh)){
        if(p->replay)p->replay->restart=true;
        if(p->hud)p->clean=false;
        ++p->scene;++g_stats.scene_draws;return {};
    }
    if (!p->hud) {
        if (shaped) {
            ++p->scene;
            ++g_stats.scene_draws;
            if (!p->upstream && CoversTarget(st.viewport_count?st.viewports[0]:nullptr,
                st.scissor_count?st.scissors[0]:nullptr,p->rw,p->rh)) {
                note_source_locked(*p, l,&call);
                p->source_copy = false;
            }
            return {};
        }
        p->hud = true;
    } else if (g_cover.load(std::memory_order_relaxed) && call.args[0] <= 6 && call.args[1] == 1 &&
               CoversTarget(st.viewport_count ? st.viewports[0] : nullptr, st.scissor_count ? st.scissors[0] : nullptr, p->rw, p->rh)) {
        // A full-screen draw after the HUD started (Anno's pause menu: a blurred copy of the scene,
        // read by pixel position, so at native size it showed the render-size picture in the top-left
        // corner and edge streaks elsewhere, Cort 2026-11-03). Full-screen passes belong with the
        // picture: it goes to the proxy, where DLSS takes it. An opaque one hides everything drawn
        // before it, so the layers start again; a blended one leaves the earlier HUD on top.
        if (shaped && p->cleared) {
            const float grey = stored_grey(g_l.typeless);
            const float black[4] = {0.f, 0.f, 0.f, 1.f}, back[4] = {grey, grey, grey, 1.f};
            l->ClearRenderTargetView(clear_rtv_locked(0), black, 0, nullptr);
            l->ClearRenderTargetView(clear_rtv_locked(1), back, 0, nullptr);
        }
        ++p->covered;
        ++g_stats.covered;
        return {};
    }
    const char* why = nullptr;
    const DXGI_FORMAT view = static_cast<DXGI_FORMAT>(st.rtv_format);
    if(p->preserve&&p->cleared&&p->view!=view)p->clean=false;
    if (!st.restorable) why = "the list's targets cannot be handed back";
    else if (!st.pipeline_known) why = "unknown pipeline";
    else if (st.rt_count != 1 || st.pso_rt_count != 1) why = "draws to more than one target";
    else if (st.samples != 1) why = "multisampled";
    else if (!st.viewport_count || !st.scissor_count) why = "no viewport or scissor";
    else if (TypelessOf(view) != TypelessOf(p->format)) why = "pipeline format differs from the back buffer";
    if (why) { p->clean=false;note_forward_locked(*p, why); return {}; }
    record_native_hud_locked(*p,l,st,call);
    if(p->preserve) {
        if(!st.shader_readonly) {
            p->clean=false;
            const char* reason=(st.shader_rejection&na::observe::ShaderUav)?"HUD shader writes a UAV":
                (st.shader_rejection&na::observe::ShaderDirectHeap)?"HUD shader uses direct resource heap indexing":
                (st.shader_rejection&na::observe::ShaderStreamOutput)?"HUD pipeline writes stream output":"HUD executable metadata unavailable";
            note_forward_locked(*p,reason);return {};
        }
        if(!st.native_hud_affine)p->clean=false;
    }
    ComPtr<ID3D12Device> dev;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev))) || !retain_layers_locked(l,*p) || !layers_locked(dev.Get(), *p)) { p->clean=false;note_forward_locked(*p, "could not make the native layers"); return {}; }
    if (p->preserve && !p->picture) {
        ComPtr<ID3D12Resource> source;source.Attach(na::observe::AcquireResource(p->id));
        if (source) p->picture=picture_locked(dev.Get(),source->GetDesc(),p->ow,p->oh);
        auto lease=p->picture?na::nr::native::RetainRecording(l,p->picture):na::nr::native::RecordingLease{};
        if (!source||!lease||!p->picture->SnapshotScene(l,source.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)) {
            p->picture.reset();p->clean=false;note_forward_locked(*p,"could not snapshot pre-HUD scene");return {};
        }
        p->picture_uses.push_back(lease);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2], dsv[2];
    if (!rtv_locked(view, rtv)) { note_forward_locked(*p, "too many view formats"); return {}; }
    // Depth: the game's HUD depth is mirrored at native size by two targets (one per layer).
    bool depth = false;
    na_object_id ds = st.ds;
    if (ds && st.dsv_format == DXGI_FORMAT_UNKNOWN) ds = 0; // the pipeline does not use a depth target
    if (ds) {
        D3D12_RESOURCE_DESC dd{};
        if (!ds_desc_locked(ds, &dd)) { note_forward_locked(*p, "depth target unknown"); return {}; }
        if (dd.SampleDesc.Count != 1) { note_forward_locked(*p, "multisampled depth"); return {}; }
        if (!depth_locked(dd, static_cast<DXGI_FORMAT>(st.dsv_format), p->ow, p->oh, dsv)) { note_forward_locked(*p, "could not make the native depth"); return {}; }
        depth = true;
    }
    const float sx = float(p->ow) / float(p->rw), sy = float(p->oh) / float(p->rh);
    // Everything below is recorded into the game's list (host work, not observed).
    if (!p->cleared) {
        if (!g_layers_cleared) {
            const float grey = stored_grey(g_l.typeless);
            const float black[4] = {0.f, 0.f, 0.f, 1.f}, back[4] = {grey, grey, grey, 1.f};
            l->ClearRenderTargetView(clear_rtv_locked(0), black, 0, nullptr);
            l->ClearRenderTargetView(clear_rtv_locked(1), back, 0, nullptr);
            g_layers_cleared = true;
        }
        p->cleared = true;
        p->view = view;
    }
    if (depth && p->mirrored != ds) {
        Clear c{};
        const auto it = g_clears.find(ds);
        if (it != g_clears.end()) c = it->second;
        else c.depth = (st.depth_func == D3D12_COMPARISON_FUNC_GREATER || st.depth_func == D3D12_COMPARISON_FUNC_GREATER_EQUAL) ? 0.f : 1.f;
        const D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL;
        l->ClearDepthStencilView(dsv[0], f, c.depth, c.stencil, 0, nullptr);
        l->ClearDepthStencilView(dsv[1], f, c.depth, c.stencil, 0, nullptr);
        p->mirrored = ds;
    }
    if(p->preserve&&st.query_active){
        // Extra inline draws would change the game's query result. The delayed
        // native/check list has no BeginQuery/EndQuery and cannot change it.
        p->clean=false;note_forward_locked(*p,"query: native replay deferred, inline layers skipped");return {};
    }
    D3D12_VIEWPORT vp[16];
    D3D12_RECT sc[16];
    for (uint32_t i = 0; i < st.viewport_count; ++i) {
        float o[6];
        ScaleViewport(st.viewports[i], sx, sy, o);
        vp[i] = {o[0], o[1], o[2], o[3], o[4], o[5]};
    }
    for (uint32_t i = 0; i < st.scissor_count; ++i) {
        int32_t o[4];
        ScaleScissor(st.scissors[i], sx, sy, p->ow, p->oh, o);
        sc[i] = {o[0], o[1], o[2], o[3]};
    }
    for (int k = 0; k < 2; ++k) {
        l->OMSetRenderTargets(1, &rtv[k], FALSE, depth ? &dsv[k] : nullptr);
        if (k == 0) {
            l->RSSetViewports(st.viewport_count, vp);
            l->RSSetScissorRects(st.scissor_count, sc);
        }
        draw(l, call);
    }
    if (!na::observe::RestoreBindingState(l, false, false, 0, true, true, true)) ++g_stats.restore_failed;
    ++p->redirected;
    ++g_stats.redirected;
    if (depth) ++p->depth_draws;
    if (st.pipeline_known && st.blend_mask != NA_UNKNOWN_U32 && (st.blend_mask & 1u)) ++p->blended;
    // Observational upstream: replay gives native layers/coverage, but the
    // original complete game picture remains until the same-frame GPU choice.
    bool preserve=p->preserve;
    for (const auto& q:g_proxy) preserve|=q.id&&q.upstream&&q.preserve&&q.rw==p->rw&&q.rh==p->rh&&q.ow==p->ow&&q.oh==p->oh;
    return {preserve?na::observe::ShaderAction::Forward:na::observe::ShaderAction::Replaced, nullptr};
}

class DrawHook final : public na::observe::ShaderOperationHook {
public:
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* l, na::observe::ShaderCall& call) noexcept override
    {
        if (g_tracing.load(std::memory_order_relaxed)) trace_draw(l, call);
        if (hud_unblend::Enabled()) return hud_unblend::OnDraw(l, call); // the un-blend route owns the draws
        if(call.kind!=na::observe::ShaderKind::Dispatch&&t_bind.canvas&&t_bind.list==l&&g_on.load()){
            na::observe::DrawInfo info;
            if(na::observe::ListDrawState(l,info)&&info.recording==t_bind.recording&&info.rt==t_bind.canvas){
                std::lock_guard<std::mutex> lock(g_m);
                for(auto& c:g_canvases)if(c.id==info.rt){
                    c.last_write=++g_draw_order;c.used=g_frame;c.writer=l;c.writer_id=info.list;c.recording=info.recording;c.snapshot.reset();
                    if(info.pipeline_known&&info.rt_count==1&&info.pso_rt_count==1&&info.samples==1&&
                        info.blend_mask!=NA_UNKNOWN_U32&&(info.blend_mask&1u)&&info.depth_enable==0){
                        c.last_2d=c.last_write;++c.blended;
                    }
                    break;
                }
            }
        }
        if(call.kind!=na::observe::ShaderKind::Dispatch&&t_bind.proxy&&t_bind.list==l){
            std::lock_guard<std::mutex> lock(g_m);
            if(auto* target=find_locked(t_bind.proxy);target&&target->output)target->output.reset();
        }
        if (call.kind == na::observe::ShaderKind::Dispatch || (!t_bind.proxy && !t_bind.mask_ds) || t_bind.list != l || !g_on.load(std::memory_order_relaxed)) return {};
        return redirect(l, call);
    }
    void Closing(ID3D12GraphicsCommandList* l) noexcept override {
        if(!g_on.load())return;
        std::lock_guard<std::mutex> lock(g_m);
        Canvas* best=nullptr;const auto recording=na::observe::ListRecording(l),writer_id=na::observe::IdOf(l);
        // A driver may reuse a destroyed list's address AND its local Reset
        // count. That must not select an old canvas from a different object.
        for(auto& c:g_canvases)if(writer_id&&c.writer==l&&c.writer_id==writer_id&&c.recording==recording&&c.blended&&c.last_2d==c.last_write&&
            (!best||c.probed<best->probed||(c.probed==best->probed&&c.last_write>best->last_write)))best=&c;
        if(!best||!best->ow||!best->oh||best->probed>best->last_write)return;
        // Only an actually known source state is used. No guessed cross-list
        // state or GPU wait; an unknown state keeps the ordinary final-list path.
        uint32_t state=0;
        if(na::observe::ListStateSub(l,best->id,0,&state)!=na::observe::StateLookup::Known)return;
        if(best->snapshot&&best->snapshot->write==best->last_write)return;
        // Other immutable owners can retain a retired snapshot. They do not
        // consume a live recording slot, nor permit its Picture to be reused.
        g_probe_pool.erase(std::remove_if(g_probe_pool.begin(),g_probe_pool.end(),[](const auto& p){return p->use.Retired();}),g_probe_pool.end());
        if(g_probe_pool.size()>=8)return;
        ComPtr<ID3D12Resource> source;source.Attach(na::observe::AcquireResource(best->id));
        ComPtr<ID3D12Device> device;if(!source||FAILED(source->GetDevice(IID_PPV_ARGS(&device))))return;
        auto* learned=find_locked(best->id);
        auto picture=learned&&learned->preserve?learned->picture:picture_locked(device.Get(),source->GetDesc(),best->ow,best->oh);
        if(!picture)return;
        auto capture=std::make_shared<ProbeCapture>();capture->picture=picture;capture->source_resource=source;
        auto use=na::nr::native::RetainRecording(l,capture);
        if(!use||!picture->SnapshotFull(l,source.Get(),static_cast<D3D12_RESOURCE_STATES>(state)))return;
        capture->use=use;capture->source=best->id;capture->generation=best->generation;capture->write=best->last_write;
        capture->list=l;capture->recording=recording;
        if(learned&&learned->preserve){
            if(learned->replay&&!learned->replay->picture->SnapshotFull(l,source.Get(),static_cast<D3D12_RESOURCE_STATES>(state)))return;
            learned->full_snapshot=capture;
        }
        best->snapshot=capture;g_probe_pool.push_back(capture);++g_stats.canvas_source_snapshots;
    }
    void NativeBoundary(ID3D12GraphicsCommandList* l) noexcept override {
        std::lock_guard<std::mutex> lock(g_m);
        const auto recording=na::observe::ListRecording(l),writer_id=na::observe::IdOf(l);
        for(auto& c:g_canvases)if(writer_id&&c.writer==l&&c.writer_id==writer_id&&c.recording==recording){c.last_write=++g_draw_order;c.snapshot.reset();}
    }
    void After(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& call, na::observe::ShaderScope&) noexcept override {
        if(call.kind==na::observe::ShaderKind::Dispatch||!g_on.load()||t_bind.list!=l||!t_bind.proxy)return;
        na::observe::DrawInfo state;if(!na::observe::ListDrawState(l,state))return;
        std::lock_guard<std::mutex> lock(g_m);auto* output=find_locked(state.rt);
        if(!output||!output->output||!output->output->replay)return;
        auto& r=*output->output->replay;if(!r.tail||r.tail_snapshot)return;
        r.tail_snapshot=r.picture->SnapshotFull(l,output->output->final.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        if(r.tail_snapshot&&output->output->needs_full)
            r.tail_snapshot=output->output->picture->SnapshotFull(l,output->output->final.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        if(r.tail_snapshot)++g_stats.native_finishing_draws;
    }
};

// The game's clears: depth clear values are remembered (our mirrors start from them), and a
// clear of a mirrored depth or of a proxy whose HUD has started is applied to ours too.
class ClearHook final : public na::observe::TextureOperationHook {
public:
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* l, const na::observe::TextureCall& c) noexcept override
    {
        if ((c.kind == na::observe::TextureKind::Copy || c.kind == na::observe::TextureKind::CopyRegion) && c.destination && c.source) {
            // H1b: a whole-picture copy into a back buffer is a hand-off from its source.
            const na_object_id dst = na::observe::IdOf(c.destination), src = na::observe::IdOf(c.source);
            std::lock_guard<std::mutex> lock(g_m);
            if (g_tracing.load(std::memory_order_relaxed)) {
                uint32_t sw = 0, sh = 0, dw = 0, dh = 0; DXGI_FORMAT sf{}, df{};
                dims(src, &sw, &sh, &sf);
                dims(dst, &dw, &dh, &df);
                char t[256];
                std::snprintf(t, sizeof t, "hud_trace {\"t\":\"%s\",\"src\":[%llu,%u,%u,%u],\"dst\":[%llu,%u,%u,%u],\"whole\":%d}\r\n",
                              c.kind == na::observe::TextureKind::Copy ? "copy" : "copy_region", (unsigned long long)src, sw, sh, unsigned(sf),
                              (unsigned long long)dst, dw, dh, unsigned(df), (c.kind == na::observe::TextureKind::Copy || (!c.box && !c.x && !c.y)) ? 1 : 0);
                trace_line_locked(t);
            }
            Proxy* p = find_locked(dst);
            if(p&&p->output)p->output.reset();
            if (p && !p->upstream && src && (c.kind == na::observe::TextureKind::Copy || (!c.box && !c.x && !c.y))) {
                uint32_t w = 0, h = 0; DXGI_FORMAT f{};
                if (dims(src, &w, &h, &f) && w == p->rw && h == p->rh) {
                    if (p->source && p->source != src) { p->source = 0; p->source_ambiguous = true; }
                    else if (!p->source_ambiguous) { p->source = src; p->source_copy = true; }
                }
            }
            return {};
        }
        if (!c.target_id || (c.kind != na::observe::TextureKind::ClearDepth && c.kind != na::observe::TextureKind::ClearColor)) return {};
        if (hud_unblend::Enabled()) {
            if (c.kind == na::observe::TextureKind::ClearColor && !c.rect_count) hud_unblend::OnClear(l, c.target_id, c.color);
            return {};
        }
        std::lock_guard<std::mutex> lock(g_m);
        if (c.kind == na::observe::TextureKind::ClearDepth) {
            Clear& k = g_clears[c.target_id];
            if (c.flags & D3D12_CLEAR_FLAG_DEPTH) k.depth = c.depth;
            if (c.flags & D3D12_CLEAR_FLAG_STENCIL) k.stencil = c.stencil;
            if (g_clears.size() > 512) g_clears.clear();
            if (c.rect_count || !g_l.d[0] || !g_l.dsv_count) return {};
            for (const auto& p : g_proxy)
                if (p.id && p.mirrored == c.target_id) {
                    for (int i = 0; i < 2; ++i) {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = g_l.dsv->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += static_cast<SIZE_T>(i) * g_l.dsv_inc;
                        l->ClearDepthStencilView(h, c.flags, c.depth, c.stencil, 0, nullptr);
                    }
                    break;
                }
            return {};
        }
        Proxy* p = find_locked(c.target_id);
        if(p&&p->output)p->output.reset();
        if (!p || !p->hud || !p->cleared || !g_l.n[0] || c.rect_count) return {};
        // A clear after the HUD started replaces everything under it: both layers take the
        // colour, so the composite shows exactly that colour there.
        for (int i = 0; i < 2; ++i) l->ClearRenderTargetView(clear_rtv_locked(i), c.color, 0, nullptr);
        return {};
    }
    void After(ID3D12GraphicsCommandList*, const na::observe::TextureCall&, na::observe::ShaderScope&) noexcept override {}
};

void on_bind(ID3D12GraphicsCommandList* l, const na::observe::RasterScissors& s)
{
    na_object_id rt = 0, ds = 0;
    Bind b{l, s.recording, 0};
    if (hud_unblend::Enabled()) { hud_unblend::OnBind(l, s.recording); t_bind = b; return; }
    if (g_on.load(std::memory_order_relaxed) && na::observe::ListCurrentTargets(l, &rt, &ds) && (rt || ds)) {
        std::lock_guard<std::mutex> lock(g_m);
        if (rt) {
            if (find_locked(rt)) b.proxy = rt;
            auto c=std::find_if(g_canvases.begin(),g_canvases.end(),[&](const auto& q){return q.id==rt;});
            if(c==g_canvases.end()&&!b.proxy){
                auto shape=g_canvas_shapes.find(rt);
                if(shape==g_canvas_shapes.end()){
                    ComPtr<ID3D12Resource> resource;resource.Attach(na::observe::AcquireResource(rt));
                    if(resource){if(g_canvas_shapes.size()==512)g_canvas_shapes.clear();shape=g_canvas_shapes.emplace(rt,resource->GetDesc()).first;}
                }
                const auto desc=shape==g_canvas_shapes.end()?D3D12_RESOURCE_DESC{}:shape->second;
                const UINT w=UINT(desc.Width),h=desc.Height;const auto f=desc.Format;
                if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.MipLevels==1&&desc.DepthOrArraySize==1&&desc.SampleDesc.Count==1)
                for(const auto& q:g_proxy)if(q.id&&!q.upstream&&q.rw==w&&q.rh==h&&TypelessOf(q.format)!=DXGI_FORMAT_UNKNOWN&&TypelessOf(q.format)==TypelessOf(f)){
                    if(g_canvases.size()==32){
                        auto old=std::min_element(g_canvases.begin(),g_canvases.end(),[](const auto& a,const auto& z){return a.used<z.used;});
                        if(old->used+8>=g_frame)break;
                        g_canvases.erase(old);
                    }
                    Canvas canvas;canvas.id=rt;canvas.generation=++g_canvas_generation;canvas.rw=w;canvas.rh=h;
                    canvas.ow=q.ow;canvas.oh=q.oh;canvas.format=f;canvas.used=g_frame;g_canvases.push_back(canvas);
                    c=std::prev(g_canvases.end());++g_stats.canvas_candidates;break;
                }
            }
            if(c!=g_canvases.end())b.canvas=rt;
        } else {
            for (const auto& q : g_proxy) // depth only: the HUD's depth target (a UI mask)?
                if (q.id && (q.mirrored == ds || (!q.mirrored && q.hud_ds == ds))) { b.mask_ds = ds; break; }
        }
    }
    t_bind = b;
}

} // namespace

DXGI_FORMAT TypelessOf(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
float GreyFor(DXGI_FORMAT view)
{
    const float g = stored_grey(TypelessOf(view));
    if (!is_srgb(view)) return g;
    return g <= 0.04045f ? g / 12.92f : std::pow((g + 0.055f) / 1.055f, 2.4f);
}
bool SceneShaped(bool blend_known, uint32_t blend_mask, uint64_t count, uint64_t instances)
{
    return blend_known && (blend_mask & 1u) == 0 && count >= 3 && count <= 6 && instances == 1;
}
bool CoversTarget(const float* vp, const int32_t* sc, uint32_t w, uint32_t h)
{
    if (!vp || !sc || !w || !h) return false;
    const bool v = vp[0] <= 0.5f && vp[1] <= 0.5f && vp[0] + vp[2] >= float(w) - 0.5f && vp[1] + vp[3] >= float(h) - 0.5f;
    const bool s = sc[0] <= 0 && sc[1] <= 0 && sc[2] >= int32_t(w) && sc[3] >= int32_t(h);
    return v && s;
}
void ScaleViewport(const float in[6], float sx, float sy, float out[6])
{
    out[0] = in[0] * sx; out[1] = in[1] * sy; out[2] = in[2] * sx; out[3] = in[3] * sy; out[4] = in[4]; out[5] = in[5];
}
void ScaleScissor(const int32_t in[4], float sx, float sy, uint32_t ow, uint32_t oh, int32_t out[4])
{
    const auto clampx = [&](double v) { return int32_t((std::max)(0.0, (std::min)(double(ow), v))); };
    const auto clampy = [&](double v) { return int32_t((std::max)(0.0, (std::min)(double(oh), v))); };
    out[0] = clampx(std::floor(in[0] * double(sx) + 1e-4));
    out[1] = clampy(std::floor(in[1] * double(sy) + 1e-4));
    out[2] = clampx(std::ceil(in[2] * double(sx) - 1e-4));
    out[3] = clampy(std::ceil(in[3] * double(sy) - 1e-4));
}

void Install()
{
    if (g_installed.exchange(true)) return;
    na::observe::SetRasterStateTracking(true); // the game's targets/viewports/scissors can be handed back
    na::observe::SetOperationSnapshots(true); // exact used SRVs at candidate handoffs, not a table prefix
    na::observe::SetRasterStateListener(&on_bind);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Reconstruction, std::make_shared<DrawHook>());
    na::observe::SetTextureOperationHook(std::make_shared<ClearHook>());
}
bool Installed() { return g_installed.load(); }
void SetEnabled(bool on) { g_on.store(on); }
void SetCoverRule(bool on) { g_cover.store(on); }
bool Enabled() { return g_installed.load() && g_on.load(); }

bool PictureSource(const na::observe::NativeOperation& op, uint64_t destination,
                   uint32_t width, uint32_t height, uint64_t& source)
{
    source = 0;
    if (op.compute || !op.reflection_complete || !width || !height) return false;
    for (const auto& d : op.descriptors) {
        if (!d.used || d.view.kind != NA_DESC_SRV || !d.view.resource || d.view.resource == destination) continue;
        if (!d.resource) return false;
        const auto desc = d.resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1) continue;
        // Only a whole base-level 2D image gives this initial lineage edge.
        // Array/mip/subrect mappings need their actual view geometry, not a
        // resource-size guess. Leave the game's complete picture untouched.
        if (d.view.explicit_view && (d.view.srv.ViewDimension != D3D12_SRV_DIMENSION_TEXTURE2D ||
            d.view.srv.Texture2D.MostDetailedMip != 0 || d.view.srv.Texture2D.PlaneSlice != 0 ||
            d.view.srv.Texture2D.ResourceMinLODClamp != 0.f ||
            d.view.srv.Shader4ComponentMapping != D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)) continue;
        if (desc.Width != width || desc.Height != height) continue;
        if (source && source != d.view.resource) { source = 0; return false; }
        source = d.view.resource;
    }
    return true;
}

bool TakeFrame(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, Layer* out,ID3D12CommandQueue* queue)
{
    if (!g_installed.load() || !frame) return false;
    const na_object_id id = na::observe::IdOf(frame);
    std::lock_guard<std::mutex> lock(g_m);
    struct FinishLayers {~FinishLayers(){if(g_layer_owner&&std::none_of(std::begin(g_proxy),std::end(g_proxy),[](const auto& p){return p.preserve&&p.picture;})){
        g_layer_owner->layers=std::move(g_l);g_layer_owner.reset();g_layers_cleared=false;
    }}} finish_layers;
    g_stats.last_present_native=0;
    ++g_frame;
    if (g_tracing.load(std::memory_order_relaxed)) { // the traced frame is complete
        g_tracing.store(false);
        char t[256];
        int k = std::snprintf(t, sizeof t, "hud_trace {\"t\":\"end\",\"present\":%llu,\"lines\":%u,\"dropped\":%u,\"back_buffer\":%llu,\"scene_depth_draws\":{",
                              (unsigned long long)g_frame, g_trace_lines, g_trace_dropped, (unsigned long long)id);
        bool first = true;
        for (const auto& e : g_trace_scene) {
            if (k <= 0 || k >= int(sizeof t) - 40) break;
            k += std::snprintf(t + k, sizeof t - k, "%s\"%llu\":%u", first ? "" : ",", (unsigned long long)e.first, e.second);
            first = false;
        }
        if (k > 0 && k < int(sizeof t) - 6) std::snprintf(t + k, sizeof t - k, "}}\r\n");
        trace_line_locked(t);
        if (g_trace_out.size() < (1u << 20)) g_trace_out += g_trace_frame;
        g_trace_frame.clear();
        g_trace_next = g_frame + 1800;
    } else if (g_traces < kTraces && g_frame >= g_trace_next) { // trace the next frame
        ++g_traces;
        g_trace_lines = g_trace_dispatches = g_trace_dropped = 0;
        g_trace_scene.clear();
        g_trace_frame = "hud_trace {\"t\":\"begin\",\"present\":" + std::to_string(g_frame) + "}\r\n";
        g_tracing.store(true);
        g_stats.traces = g_traces;
    }
    g_grave.erase(std::remove_if(g_grave.begin(), g_grave.end(), [](const Grave& g) { return g.frame + kGraveFrames < g_frame; }), g_grave.end());
    if (!id) return false;
    if (hud_unblend::Enabled()) return hud_unblend::TakeFrame(frame, rw, rh, ow, oh, out);
    Proxy* p = find_locked(id);
    if (!p || p->rw != rw || p->rh != rh || p->ow != ow || p->oh != oh) {
        if (!p) { // learn it, replacing the proxy presented longest ago (never the upstream)
            for (auto& q : g_proxy) if (!q.upstream && (!p || q.used < p->used)) p = &q;
            if (!p) return false; // every slot holds an upstream: nothing to replace
        }
        *p = Proxy{};
        p->id = id; p->rw = rw; p->rh = rh; p->ow = ow; p->oh = oh;
        p->format = frame->GetDesc().Format;
        p->used = g_frame;
        return false;
    }
    p->used = g_frame;
    if(auto probe=std::move(p->probe);probe&&out&&g_on.load()&&queue&&probe->use.Ready(queue)){
        out->probe=probe->picture;out->probe_owner=probe;out->probe_source=probe->source;out->probe_generation=probe->generation;
    }
    auto executed=g_submitted_outputs.find(id);
    auto completed=executed==g_submitted_outputs.end()?std::shared_ptr<CapturedPicture>{}:std::move(executed->second);
    if(executed!=g_submitted_outputs.end())g_submitted_outputs.erase(executed);
    if(completed||p->output){
        auto capture=completed?std::move(completed):std::move(p->output);auto& layers=capture->owner->layers;
        const bool same_recording=p->output==capture||!p->output;
        if(p->output==capture)p->output.reset();
        const auto ready_use=[&](const auto& use){return use.Ready(queue);};
        const bool ready=g_on.load()&&queue&&layers.n[0]&&layers.n[1]&&layers.w==ow&&layers.h==oh&&
            std::all_of(capture->uses.begin(),capture->uses.end(),ready_use)&&std::all_of(capture->owner->uses.begin(),capture->owner->uses.end(),ready_use);
        if(ready&&out){
            g_stats.last_present_native=capture->clean?1u:0u;
            out->n0=layers.n[0].Get();out->n1=layers.n[1].Get();out->view=capture->view;
            out->grey=GreyFor(capture->view);out->srgb=is_srgb(capture->view);out->owner=capture->owner;
            out->picture=capture->picture;out->clean_allowed=capture->clean;
            ++g_stats.hud_frames;++g_stats.upstream_frames;
            if(capture->clean)++g_stats.draw_transfer_frames;else++g_stats.draw_protect_frames;
            if(capture->replay&&capture->replay->submitted&&capture->replay->queue.Get()==queue){
                g_stats.last_present_native=1;
                out->picture=capture->replay->picture;out->native_picture=capture->replay->tail?capture->replay->tail_native.Get():capture->replay->native.Get();
                out->native_check=capture->replay->tail?capture->replay->tail_check.Get():capture->replay->check.Get();out->native_owner=capture;out->owner=capture;
                if(capture->replay->world_source)out->reconstruction=capture->replay->world_picture;
                out->recording_identity=capture->replay->identity;
            }
        }
        ++g_stats.frames;g_stats.upstream_now=1;g_stats.last_source=1;
        g_stats.last_redirected=capture->draws;g_stats.last_forwarded=capture->forwarded;
        g_stats.proxy_format=p->format;g_stats.view_format=capture->view;g_stats.grey=GreyFor(capture->view);
        if(same_recording){p->source=0;p->source_ambiguous=p->source_copy=false;p->hud=p->cleared=false;p->scene=p->redirected=p->forwarded=0;}
        return ready;
    }
    // H1b: the upstream picture for this output (handled like the back buffer), and learning.
    // Follow this handoff's actual source. An older same-sized canvas must
    // neither hide a newly identified one nor prevent its promotion.
    Proxy* u = p->source&&!p->source_ambiguous?find_locked(p->source):nullptr;
    if(u&&(!u->upstream||u->ow!=ow||u->oh!=oh||u->rw!=rw||u->rh!=rh))u=nullptr;
    if(!u&&!p->source){
        uint64_t newest=0;
        for(auto& q:g_proxy)if(q.id&&q.upstream&&q.ow==ow&&q.oh==oh&&q.rw==rw&&q.rh==rh){
            const auto c=std::find_if(g_canvases.begin(),g_canvases.end(),[&](const auto& v){return v.id==q.id;});
            if(c!=g_canvases.end()&&c->used+2>=g_frame&&c->last_write>newest){u=&q;newest=c->last_write;}
        }
    }
    // Current lineage: this present's hand-off came from the upstream (actual output dependency).
    const bool lineage = u && p->source == u->id && !p->source_ambiguous;
    bool recovered = false;
    if (u) {
        u->used = g_frame;
        if (lineage) { u->lineage_miss = 0; u->paused = false; }
        else if(u->pixel_proved){
            // Missing CPU lineage (for example one query-protected frame) is
            // precisely why this canvas was learned from GPU pictures. Keep
            // observing its next draws; that frame still gets its own proof.
            u->paused=false;
        } else {
            // Its HUD draws of this frame (if any) are already in the layers: lay them on once so
            // nothing is lost, and keep the next frames' HUD in the picture until lineage returns.
            recovered = u->redirected > 0;
            if (recovered) ++g_stats.lineage_recovered;
            ++g_stats.lineage_miss;
            u->paused = true;
            if (++u->lineage_miss >= kDemoteFrames) { // the structure changed: learn again
                ++g_stats.upstream_dropped;
                *u = Proxy{};
                u = nullptr;
            }
        }
    } else if (g_on.load() && p->redirected == 0 && p->source && !p->source_ambiguous) {
        if (g_cand.id == p->source) ++g_cand.frames; else g_cand = {p->source, 1};
        if (g_cand.frames >= kPromoteFrames) {
            uint32_t w = 0, h = 0; DXGI_FORMAT f{};
            if (!dims(g_cand.id, &w, &h, &f)) std::snprintf(g_stats.upstream_why, sizeof g_stats.upstream_why, "candidate gone");
            else if (TypelessOf(f) == DXGI_FORMAT_UNKNOWN || TypelessOf(f) != TypelessOf(p->format))
                // A format change in the hand-off is a colour transformation, not a copy: laying the
                // upstream HUD over the final picture would be wrong (Astra 16bbca8 point 4).
                std::snprintf(g_stats.upstream_why, sizeof g_stats.upstream_why, "hand-off changes format %u->%u", unsigned(f), unsigned(p->format));
            else {
                Proxy* slot = nullptr;
                for (auto& q : g_proxy) if (q.id != p->id && !q.upstream && (!slot || q.used < slot->used)) slot = &q;
                if (!slot) std::snprintf(g_stats.upstream_why, sizeof g_stats.upstream_why, "no free slot");
                else {
                    *slot = Proxy{};
                    slot->id = g_cand.id; slot->upstream = true;
                    slot->preserve=!p->source_copy; // draws are observed, never blindly treated as copies
                    slot->rw = rw; slot->rh = rh; slot->ow = ow; slot->oh = oh; slot->format = f; slot->used = g_frame;
                    ++g_stats.upstream_learned;
                    g_stats.upstream_why[0] = 0;
                }
            }
            g_cand = {};
        }
    } else g_cand = {}; // consecutive observations only: any frame without a clean source restarts the count
    g_stats.upstream_now = u ? 1u : 0u;
    g_stats.last_source = p->source ? 1u : p->source_ambiguous ? 2u : 0u;
    p->source = 0; p->source_ambiguous = false; p->source_copy = false;
    const bool observational=u&&u->preserve;
    const auto picture=observational&&lineage?u->picture:std::shared_ptr<Picture>{};
    const bool ready=picture&&queue&&std::all_of(u->picture_uses.begin(),u->picture_uses.end(),[&](const auto& use){return use.Ready(queue);})&&
        g_layer_owner&&std::all_of(g_layer_owner->uses.begin(),g_layer_owner->uses.end(),[&](const auto& use){return use.Ready(queue);});
    const bool clean=ready&&u->clean&&u->forwarded==0&&p->redirected==0;
    bool hud = g_on.load() && p->cleared && p->redirected > 0 && g_l.n[0] && g_l.w == ow && g_l.h == oh;
    const bool up_hud = !hud && u && (lineage || recovered) && g_on.load() && u->cleared && u->redirected > 0 && g_l.n[0] && g_l.w == ow && g_l.h == oh;
    if (up_hud) {
        hud = true;
        ++g_stats.upstream_frames;
        p->view = u->view;
        p->redirected = u->redirected; p->forwarded += u->forwarded; p->world += u->world; p->masks += u->masks;
        p->depth_draws = u->depth_draws; p->blended = u->blended;
    }
    if (u&&!u->preserve) { // observed draw occurrences end at their handoff, including render-ahead
        u->hud = u->cleared = false;
        if (u->mirrored) u->hud_ds = u->mirrored;
        u->mirrored = 0;
        if (!up_hud) u->view = DXGI_FORMAT_UNKNOWN;
        u->scene = u->redirected = u->forwarded = 0;
        u->depth_draws = u->blended = u->covered = u->world = u->masks = 0;
    }
    if (hud && out) {
        if(!observational)g_stats.last_present_native=1;
        out->n0 = g_l.n[0].Get();
        out->n1 = g_l.n[1].Get();
        out->view = p->view;
        out->grey = GreyFor(p->view);
        out->srgb = is_srgb(p->view);
        out->owner=g_layer_owner;
        out->picture=ready?picture:std::shared_ptr<Picture>{};out->clean_allowed=clean;
        if (observational) {
            // If the exact source recording is not ready, the original full
            // picture still exists. Do not lay its already-present HUD twice.
            if (!ready) hud=false;
            else if(clean)++g_stats.draw_transfer_frames;else++g_stats.draw_protect_frames;
        }
    }
    ++g_stats.frames;
    if (hud) ++g_stats.hud_frames;
    g_stats.last_scene = p->scene; g_stats.last_redirected = p->redirected; g_stats.last_forwarded = p->forwarded;
    g_stats.last_depth = p->depth_draws; g_stats.last_blended = p->blended; g_stats.last_covered = p->covered; g_stats.last_world = p->world; g_stats.last_masks = p->masks;
    g_stats.proxy_format = uint32_t(p->format); g_stats.view_format = uint32_t(p->view);
    g_stats.grey = p->view != DXGI_FORMAT_UNKNOWN ? GreyFor(p->view) : 0.f;
    p->hud = p->cleared = false;
    if (p->mirrored) p->hud_ds = p->mirrored;
    p->mirrored = 0;
    p->view = DXGI_FORMAT_UNKNOWN;
    p->scene = p->redirected = p->forwarded = 0;
    p->depth_draws = p->blended = p->covered = p->world = p->masks = 0;
    if(u&&!u->preserve){u->picture.reset();u->picture_uses.clear();u->clean=true;}
    return hud;
}

std::string TakeTrace()
{
    std::lock_guard<std::mutex> lock(g_m);
    std::string out;
    out.swap(g_trace_out);
    return out;
}
Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g_m);
    return g_stats;
}
void ReportPictureResult(bool identity,bool clean,uint32_t tested,uint32_t matched,float error)
{
    std::lock_guard<std::mutex> lock(g_m);
    if(tested){g_stats.proof_tested=tested;g_stats.proof_matched=matched;g_stats.proof_maximum_error=error;}
    if(!identity)++g_stats.transfer_changed;
    else {++g_stats.transfer_verified;if(clean)++g_stats.clean_verified;else++g_stats.protection_verified;}
    g_stats.last_draw_method=!identity?3u:clean?1u:2u;
}
void SetPreHudRendererForTests(PreHudRenderer renderer){g_test_renderer.store(renderer);}
void AfterSubmission(ID3D12CommandQueue* queue,uint32_t count,ID3D12CommandList* const* lists)
{
    if(!queue||!count||!lists||!Enabled())return;
    std::vector<std::shared_ptr<CapturedPicture>> ready;
    {
        std::lock_guard<std::mutex> lock(g_m);
        // Publish only on actual queue submission. All earlier source leases
        // may be ready from separate Execute calls on this queue.
        std::stable_sort(g_pending_outputs.begin(),g_pending_outputs.end(),[&](const auto& a,const auto& b){
            const auto rank=[&](const auto& c){UINT rank=0;for(UINT i=0;i<count;++i)
                if(lists[i]==c->writer&&na::observe::IdOf(lists[i])==c->writer_id&&na::observe::ListRecording(lists[i])==c->recording)rank=i+1;return rank;};
            const auto ar=rank(a),br=rank(b);return ar!=br?ar<br:a->order<b->order;
        });
        for(const auto& capture:g_pending_outputs)if(capture&&!capture->published&&capture->final){
            if(!capture->final_use.Ready(queue)||!std::all_of(capture->uses.begin(),capture->uses.end(),[&](const auto& use){return use.Ready(queue);}))continue;
            const auto final_id=na::observe::IdOf(capture->final.Get());
            uint32_t final_state=UINT_MAX;
            for(UINT i=0;i<count;++i){uint32_t state=0;
                if(na::observe::ListStateSub(lists[i],final_id,0,&state)==na::observe::StateLookup::Known)final_state=state;
            }
            if(final_state!=D3D12_RESOURCE_STATE_PRESENT)continue;
            capture->published=true;g_submitted_outputs[final_id]=capture;
            if(!capture->replay)continue;
            auto& r=*capture->replay;if(!r.complete||!r.closed||r.attempted||(r.tail&&!r.tail_snapshot))continue;
            if(!std::all_of(r.uses.begin(),r.uses.end(),[&](const auto& use){return use.Ready(queue);}))continue;
            // The application fence remains behind this callback (the existing
            // producer queue lock also serialises Signal/Wait). Bindings must
            // additionally remain readable at this delayed replay point.
            bool readable=!r.restart;
            for(const auto& read:r.reads){auto lookup=read.lookup;uint32_t state=read.state;
                uint32_t carried=0;if(na::observe::KnownSubStateOnQueueNow(queue,read.id,read.sub,&carried)){
                    lookup=na::observe::StateLookup::Known;state=carried;
                }
                for(UINT i=0;i<count;++i){uint32_t next=0;const auto found=na::observe::ListStateSub(lists[i],read.id,read.sub,&next);
                    if(found!=na::observe::StateLookup::Absent){lookup=found;state=next;}}
                constexpr UINT writes=D3D12_RESOURCE_STATE_RENDER_TARGET|D3D12_RESOURCE_STATE_UNORDERED_ACCESS|D3D12_RESOURCE_STATE_DEPTH_WRITE|
                    D3D12_RESOURCE_STATE_COPY_DEST|D3D12_RESOURCE_STATE_RESOLVE_DEST|D3D12_RESOURCE_STATE_STREAM_OUT;
                if(lookup==na::observe::StateLookup::Unavailable||
                   (lookup==na::observe::StateLookup::Known&&((state&writes)||
                    (read.lookup==na::observe::StateLookup::Known&&(state&read.state)!=read.state)))){readable=false;break;}
            }
            if(!readable){r.attempted=true;++g_stats.native_resource_changed;continue;}
            r.attempted=true;ready.push_back(std::move(capture));
        }
        g_pending_outputs.erase(std::remove_if(g_pending_outputs.begin(),g_pending_outputs.end(),[](const auto& q){return q->published;}),g_pending_outputs.end());
    }
    // The original batch still owns its resources; no application fence can
    // be inserted between the original Execute and this callback's return.
    // Do not hold the HUD mutex while the existing DLSS renderer acquires its.
    for(const auto& capture:ready){
        const auto& r=capture->replay;const auto& bg=capture->owner->layers;
        Layer layer;layer.n0=bg.n[0].Get();layer.n1=bg.n[1].Get();layer.view=capture->view;
        layer.grey=GreyFor(capture->view);layer.srgb=is_srgb(capture->view);layer.owner=capture;
        layer.picture=r->picture;layer.clean_allowed=true;
        if(r->world_source){layer.reconstruction=r->world_picture;layer.ui_output=r->native.Get();layer.ui_view=r->view;}
        layer.recording_identity=r->identity;
        auto render=g_test_renderer.load();if(!render)render=&na::j2::PreHudUpscale;
        const bool submitted=render(queue,capture->final.Get(),r->rw,r->rh,r->world_source?r->world_native.Get():r->native.Get(),r->ow,r->oh,layer,r->list.Get());
        std::lock_guard<std::mutex> lock(g_m);r->submitted=submitted;if(submitted){r->queue=queue;++g_stats.native_submitted;}
    }
}
void ReportNativePictureResult(bool identity,uint32_t tested,uint32_t matched,float error){
    std::lock_guard<std::mutex> lock(g_m);
    if(tested){g_stats.proof_tested=tested;g_stats.proof_matched=matched;g_stats.proof_maximum_error=error;}
    const bool partial=!identity&&tested&&matched;
    if(identity)++g_stats.native_verified;else if(partial)++g_stats.native_partial;else++g_stats.native_rejected;
    g_stats.last_draw_method=identity?4u:partial?6u:5u;
}
void ReportCanvasProbe(uint64_t source,uint64_t generation,bool identity,uint32_t tested,uint32_t matched,float error)
{
    std::lock_guard<std::mutex> lock(g_m);
    if(tested){g_stats.proof_tested=tested;g_stats.proof_matched=matched;g_stats.proof_maximum_error=error;}
    auto c=std::find_if(g_canvases.begin(),g_canvases.end(),[&](const auto& q){return q.id==source&&q.generation==generation;});
    if(c==g_canvases.end())return;
    if(!identity){++g_stats.canvas_changed;c->proofs=0;return;}
    ++g_stats.canvas_verified;if(++c->proofs<2)return;
    if(auto* p=find_locked(source)){p->pixel_proved=true;return;}
    Proxy* slot=nullptr;
    for(auto& q:g_proxy)if(!q.id){slot=&q;break;}
    if(!slot)for(auto& q:g_proxy)if(!q.upstream&&q.used+8<g_frame&&(!slot||q.used<slot->used))slot=&q;
    if(!slot){ // Profile eviction is not GPU retirement: recording/Present owners still retain every packet.
        uint64_t oldest=UINT64_MAX;
        for(auto& q:g_proxy)if(q.upstream&&q.pixel_proved&&!q.picture&&!q.replay&&!q.hud){
            const auto previous=std::find_if(g_canvases.begin(),g_canvases.end(),[&](const auto& canvas){return canvas.id==q.id;});
            if(previous!=g_canvases.end()&&previous->used+2<g_frame&&previous->used<oldest){slot=&q;oldest=previous->used;}
        }
        if(slot)++g_stats.upstream_dropped;
    }
    if(!slot)return;
    *slot={};slot->id=source;slot->upstream=slot->preserve=slot->pixel_proved=true;
    slot->rw=c->rw;slot->rh=c->rh;slot->ow=c->ow;slot->oh=c->oh;slot->format=c->format;slot->used=g_frame;
    ++g_stats.upstream_learned;g_stats.upstream_why[0]=0;
}
std::string StatusLine()
{
    const Stats s = GetStats();
    char b[2048];
    std::snprintf(b, sizeof b,
                  "native_hud on=%u frames=%llu hud_frames=%llu scene_draws=%llu redirected=%llu forwarded=%llu restore_failed=%llu "
                  "covered=%llu world=%llu masks=%llu last_frame=scene:%u,hud:%u,forwarded:%u,depth:%u,blended:%u,covered:%u,world:%u,masks:%u proxy_fmt=%u view_fmt=%u grey=%.4f last_forward=\"%s\" "
                  "upstream{now=%u learned=%llu dropped=%llu hud_frames=%llu last_source=%u why=\"%s\" lineage_miss=%llu recovered=%llu} world_depth=%llu traces=%u "
                  "draw{candidates=%llu protect_candidates=%llu gpu_verified=%llu changed=%llu clean=%llu protected=%llu}",
                  Enabled() ? 1u : 0u, (unsigned long long)s.frames, (unsigned long long)s.hud_frames, (unsigned long long)s.scene_draws,
                  (unsigned long long)s.redirected, (unsigned long long)s.forwarded, (unsigned long long)s.restore_failed, (unsigned long long)s.covered, (unsigned long long)s.world, (unsigned long long)s.masks,
                  s.last_scene, s.last_redirected, s.last_forwarded, s.last_depth, s.last_blended, s.last_covered, s.last_world, s.last_masks, s.proxy_format, s.view_format,
                  double(s.grey), s.last_forward, s.upstream_now, (unsigned long long)s.upstream_learned, (unsigned long long)s.upstream_dropped,
                  (unsigned long long)s.upstream_frames, s.last_source, s.upstream_why, (unsigned long long)s.lineage_miss,
                  (unsigned long long)s.lineage_recovered, (unsigned long long)s.world_depth, s.traces,
                  (unsigned long long)s.draw_transfer_frames,(unsigned long long)s.draw_protect_frames,(unsigned long long)s.transfer_verified,
                  (unsigned long long)s.transfer_changed,(unsigned long long)s.clean_verified,(unsigned long long)s.protection_verified);
    std::string result=b;
    std::snprintf(b,sizeof b," canvas{candidates=%llu probes=%llu verified=%llu changed=%llu no_state=%llu} handoff{snapshot_missing=%llu no_source=%llu inferred=%llu}",
        (unsigned long long)s.canvas_candidates,(unsigned long long)s.canvas_probes,(unsigned long long)s.canvas_verified,
        (unsigned long long)s.canvas_changed,(unsigned long long)s.canvas_no_state,(unsigned long long)s.handoff_snapshot_missing,
        (unsigned long long)s.handoff_no_source,(unsigned long long)s.handoff_inferred);
    result+=b;
    std::snprintf(b,sizeof b," native{recorded=%llu submitted=%llu verified=%llu rejected=%llu allocated=%llu reused=%llu resource_changed=%llu}",
        (unsigned long long)s.native_recorded,(unsigned long long)s.native_submitted,
        (unsigned long long)s.native_verified,(unsigned long long)s.native_rejected,
        (unsigned long long)s.native_allocated,(unsigned long long)s.native_reused,(unsigned long long)s.native_resource_changed);
    result+=b;
    std::snprintf(b,sizeof b," agreement{matched=%llu tested=%llu max_error=%.6f native_partial=%llu}",
        (unsigned long long)s.proof_matched,(unsigned long long)s.proof_tested,double(s.proof_maximum_error),(unsigned long long)s.native_partial);
    result+=b;
    std::snprintf(b,sizeof b," source_probe{snapshots=%llu consumed=%llu finishing=%llu}",(unsigned long long)s.canvas_source_snapshots,(unsigned long long)s.canvas_source_probes,(unsigned long long)s.native_finishing_draws);result+=b;
    const auto shaders=na::observe::GetShaderInspectionStats();
    std::snprintf(b,sizeof b," shader_inspection{unique=%llu hits=%llu disassembled=%llu dxil=%llu us=%llu bytes=%llu evictions=%llu}",
        (unsigned long long)shaders.misses,(unsigned long long)shaders.hits,(unsigned long long)shaders.disassemblies,
        (unsigned long long)shaders.dxil,(unsigned long long)shaders.microseconds,(unsigned long long)shaders.bytes,(unsigned long long)shaders.evictions);
    result+=b;return result;
}

} // namespace na::j2::native_hud
