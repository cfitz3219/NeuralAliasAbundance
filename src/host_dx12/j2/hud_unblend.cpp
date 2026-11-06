#include "host_dx12/j2/hud_unblend.hpp"
#include "host_dx12/gpu/mem_alloc.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/hud_picture.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/texture_operation.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/scale/scoped_bindings.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>

namespace na::j2::hud_unblend {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kSets = 3;  // frames in flight whose layers may still be read by Present
constexpr uint32_t kViews = 4; // render-target view formats per set

std::atomic<bool> g_configured{false}, g_on{true};
std::mutex g_m;
Stats g_stats;
uint64_t g_frame = 0;
struct Dims { uint32_t rw = 0, rh = 0, ow = 0, oh = 0; DXGI_FORMAT proxy = DXGI_FORMAT_UNKNOWN; } g_dims;

struct Set {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> n[2], l[2]; // native black/grey, render-size black/grey (RENDER_TARGET between uses)
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12DescriptorHeap> rtv; // [view i * 4 + target]: targets l0, l1, n0, n1; then 4 clear views (non-sRGB)
    DXGI_FORMAT views[kViews] = {};
    uint32_t nviews = 0;
    UINT inc = 0;
    // The frame being recorded into it:
    uint64_t frame = ~0ull;
    na_object_id target = 0; // exact destination identity, not the most recently recorded candidate
    bool cleared = false;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    uint32_t draws = 0, covered = 0;
    std::vector<na::nr::native::RecordingLease> uses;
    std::shared_ptr<native_hud::Picture> picture;
    bool scene_captured = false, full_captured = false, clean = true;
    ID3D12GraphicsCommandList* writer = nullptr;
    uint64_t writer_id = 0, recording = 0;
    uint64_t scene_source = 0;
    bool source_srgb = false;
    bool closed = false;
    uint64_t submitted = 0;
    std::shared_ptr<na::scale::ShaderDescriptorArena> descriptors = std::make_shared<na::scale::ShaderDescriptorArena>();
    std::unordered_map<uint64_t, std::array<std::shared_ptr<const na::scale::PreparedBindings>,4>> bindings;
};
struct FrameSet {
    std::unordered_map<na_object_id, std::shared_ptr<Set>> active, transferred;
    std::unordered_set<na_object_id> scene;
};
struct Packet {
    std::shared_ptr<Set> owner;
    ID3D12CommandList* list=nullptr;
    uint64_t list_id=0,recording=0,target=0,order=0;
    na::nr::native::RecordingLease use;
};
std::vector<std::shared_ptr<Packet>> g_packets;
std::unordered_map<uint64_t,std::shared_ptr<Set>> g_submitted;
struct Writes {uint64_t recording=0;std::unordered_set<uint64_t> targets;};
std::unordered_map<uint64_t,Writes> g_writes;
uint64_t g_order=0;
uint64_t g_submission=0;
std::unordered_map<uint64_t,uint64_t> g_presented;
struct Association {uint64_t output=0,source=0,seen=0;uint32_t confirmations=0,matched=0,tested=0;};
struct Probe {uint64_t output=0,source=0,frame=0;};
std::vector<Association> g_associations;
std::unordered_map<uint64_t,Probe> g_probes;
uint64_t g_probe_serial=0,g_probe_cursor=0;
FrameSet g_set[kSets];
std::vector<std::shared_ptr<Set>> g_pool;
constexpr size_t kMaxTargets = 8, kMaxPool = 48;

struct Bind { ID3D12GraphicsCommandList* list = nullptr; uint64_t recording = 0; na_object_id rt = 0; bool candidate = false; };
thread_local Bind t_bind;
std::unordered_map<na_object_id, D3D12_RESOURCE_DESC> g_shapes; // render targets seen (bounded)

// Self-check window.
uint64_t g_w_frames = 0, g_w_ui = 0, g_w_opaque = 0, g_w_id = 0, g_w_dec = 0, g_w_enc = 0;
std::atomic<uint32_t> g_verdict{0};

bool is_srgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
// Same numbers as native_hud: what the clear stores, exactly representable, written through a
// non-sRGB view so the stored value is the number itself (GreyFor reports how `view` reads it).
float stored_grey(DXGI_FORMAT typeless)
{
    switch (typeless) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 512.f / 1023.f;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 1.f;
    default: return 128.f / 255.f;
    }
}
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

bool committed(ID3D12Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT typeless, ComPtr<ID3D12Resource>& out, const wchar_t* name)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = typeless;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // D3D12 Memory Allocator (placed): safe here because every layer's first GPU operation in
    // a frame is clear_background_locked's full ClearRenderTargetView (see OnDraw/TakeFrame:
    // layers are only read in frames that cleared them).
    if (FAILED(na::gpu::mem::CreateResource(dev, D3D12_HEAP_TYPE_DEFAULT, d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, out.ReleaseAndGetAddressOf()))) return false;
    out->SetName(name);
    return true;
}
bool set_ready_locked(ID3D12Device* dev, Set& s, const Dims& d)
{
    const DXGI_FORMAT typeless = native_hud::TypelessOf(d.proxy);
    if (typeless == DXGI_FORMAT_UNKNOWN) return false;
    if (s.device.Get() == dev && s.rw == d.rw && s.rh == d.rh && s.ow == d.ow && s.oh == d.oh && s.typeless == typeless && s.n[0]) return true;
    s = Set{};
    s.device = dev;
    if (!committed(dev, d.ow, d.oh, typeless, s.n[0], L"NeuralAlias HUD native black") ||
        !committed(dev, d.ow, d.oh, typeless, s.n[1], L"NeuralAlias HUD native grey") ||
        !committed(dev, d.rw, d.rh, typeless, s.l[0], L"NeuralAlias HUD small black") ||
        !committed(dev, d.rw, d.rh, typeless, s.l[1], L"NeuralAlias HUD small grey")) { s = Set{}; return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kViews * 4 + 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtv)))) { s = Set{}; return false; }
    s.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    {
        ID3D12Resource* const t[4] = {s.l[0].Get(), s.l[1].Get(), s.n[0].Get(), s.n[1].Get()};
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = clear_view_of(typeless);
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 4; ++k) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtv->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(kViews * 4 + k) * s.inc;
            dev->CreateRenderTargetView(t[k], &rd, h);
        }
    }
    s.rw = d.rw; s.rh = d.rh; s.ow = d.ow; s.oh = d.oh; s.typeless = typeless;
    return true;
}
std::shared_ptr<Set> acquire_set_locked(ID3D12Device* dev, const Dims& d)
{
    for (const auto& p : g_pool) {
        if (p.use_count() != 1 || !std::all_of(p->uses.begin(), p->uses.end(), [](const auto& u) { return u.Retired(); })) continue;
        if (!set_ready_locked(dev, *p, d)) return {};
        p->uses.clear(); p->frame = g_frame; p->cleared = false; p->draws = p->covered = 0; p->target = 0;
        p->view = DXGI_FORMAT_UNKNOWN;
        p->scene_captured = p->full_captured = false; p->clean = true;
        p->writer = nullptr; p->writer_id = p->recording = 0;
        p->scene_source = 0; p->source_srgb = false; p->closed=false;p->submitted=0; p->bindings.clear();
        if (p->picture) p->picture->BeginCycle();
        return p;
    }
    if (g_pool.size() >= kMaxPool) return {};
    auto p = std::make_shared<Set>();
    if (!set_ready_locked(dev, *p, d)) return {};
    p->frame = g_frame; g_pool.push_back(p); return p;
}
bool retain_locked(ID3D12GraphicsCommandList* l, const std::shared_ptr<Set>& p)
{
    auto use = na::nr::native::RetainRecording(l, p);
    if (!use) return false;
    if (std::none_of(p->uses.begin(), p->uses.end(), [&](const auto& u) { return u.SameRecording(use); })) p->uses.push_back(use);
    return true;
}
void packet_locked(ID3D12GraphicsCommandList* l,uint64_t target,std::shared_ptr<Set> owner={})
{
    const auto id=na::observe::IdOf(l),recording=na::observe::ListRecording(l);
    if(!id||!recording)return;
    auto p=std::make_shared<Packet>();p->owner=std::move(owner);p->list=l;p->list_id=id;p->recording=recording;p->target=target;p->order=++g_order;
    p->use=na::nr::native::RetainRecording(l,p);if(!p->use)return;
    g_packets.push_back(std::move(p));
    if(g_packets.back()->owner)g_writes[id].targets.erase(target); // next write must supersede this frozen handoff
}
void mutation_locked(ID3D12GraphicsCommandList* l,uint64_t target,uint64_t id,uint64_t recording)
{
    auto& w=g_writes[id];if(w.recording!=recording){w.recording=recording;w.targets.clear();}
    if(w.targets.insert(target).second)packet_locked(l,target);
}
void copy_layers(ID3D12GraphicsCommandList* l, Set& dst, const Set& src)
{
    ID3D12Resource* to[] = {dst.l[0].Get(), dst.l[1].Get(), dst.n[0].Get(), dst.n[1].Get()};
    ID3D12Resource* from[] = {src.l[0].Get(), src.l[1].Get(), src.n[0].Get(), src.n[1].Get()};
    D3D12_RESOURCE_BARRIER b[8]{};
    for (UINT i = 0; i < 8; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition = {i < 4 ? from[i] : to[i - 4], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_RENDER_TARGET, i < 4 ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST};
    }
    l->ResourceBarrier(8, b);
    for (UINT i = 0; i < 4; ++i) l->CopyResource(to[i], from[i]);
    for (auto& v : b) std::swap(v.Transition.StateBefore, v.Transition.StateAfter);
    l->ResourceBarrier(8, b);
    dst.draws = src.draws; dst.covered = src.covered; dst.view = src.view; dst.cleared = true;
    dst.clean = src.clean;
    dst.scene_source = src.scene_source; dst.source_srgb = src.source_srgb;
}
bool picture_locked(Set& s, const D3D12_RESOURCE_DESC& desc)
{
    if (!s.picture || !s.picture->Matches(s.device.Get(), desc, s.ow, s.oh))
        s.picture = native_hud::Picture::Create(s.device.Get(), desc, s.ow, s.oh);
    return !!s.picture;
}
// RTVs of the four targets through `view` (made on first use; at most kViews formats per set).
bool rtvs_locked(Set& s, DXGI_FORMAT view, D3D12_CPU_DESCRIPTOR_HANDLE out[4])
{
    uint32_t i = 0;
    for (; i < s.nviews; ++i) if (s.views[i] == view) break;
    if (i == s.nviews) {
        if (s.nviews == kViews) return false;
        ID3D12Resource* const t[4] = {s.l[0].Get(), s.l[1].Get(), s.n[0].Get(), s.n[1].Get()};
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = view;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        for (int k = 0; k < 4; ++k) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtv->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(i * 4 + k) * s.inc;
            s.device->CreateRenderTargetView(t[k], &rd, h);
        }
        s.views[i] = view;
        ++s.nviews;
    }
    for (int k = 0; k < 4; ++k) {
        out[k] = s.rtv->GetCPUDescriptorHandleForHeapStart();
        out[k].ptr += SIZE_T(i * 4 + k) * s.inc;
    }
    return true;
}
// Background: black / grey through the non-sRGB clear views (stores the numbers themselves).
void clear_background_locked(ID3D12GraphicsCommandList* l, Set& s, float grey_value = -1.f)
{
    const float grey = grey_value >= 0 ? grey_value : stored_grey(s.typeless);
    const float black[4] = {0.f, 0.f, 0.f, 1.f}, back[4] = {grey, grey, grey, 1.f};
    for (int k = 0; k < 4; ++k) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(kViews * 4 + k) * s.inc;
        l->ClearRenderTargetView(h, (k & 1) ? back : black, 0, nullptr);
    }
}
void forward_locked(const char* why) { ++g_stats.forwarded; std::snprintf(g_stats.last_forward, sizeof g_stats.last_forward, "%s", why); }
void draw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& c)
{
    if (c.kind == na::observe::ShaderKind::Draw) l->DrawInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), UINT(c.args[3]));
    else l->DrawIndexedInstanced(UINT(c.args[0]), UINT(c.args[1]), UINT(c.args[2]), INT(c.args[3]), UINT(c.args[4]));
}
// Reuse the Layer-2 private-descriptor/constant machinery. Only a draw with an
// actual reflected picture input needs rebasing; ordinary glyph draws keep
// the cheap native binding path. The four immutable plans outlive execution.
const std::array<std::shared_ptr<const na::scale::PreparedBindings>,4>*
world_bindings(ID3D12GraphicsCommandList* l, Set& dst, Set& world,
               uint64_t source, const na::observe::NativeOperation& op)
{
    uint64_t key=op.pipeline_id;
    na::observe::OperationKey::add(key,op.root.Get());
    na::observe::OperationKey::add(key,source);
    for(const auto& t:op.tables){na::observe::OperationKey::add(key,t.param);na::observe::OperationKey::add(key,t.first);na::observe::OperationKey::add(key,t.count);}
    for(const auto& d:op.descriptors){
        na::observe::OperationKey::add(key,d.used);
        if(!d.used)continue;
        na::observe::OperationKey::add(key,d.view.kind);na::observe::OperationKey::add(key,d.view.resource);
        na::observe::OperationKey::add(key,d.version);
        if(d.view.kind==NA_DESC_SRV)na::observe::OperationKey::add(key,d.view.srv);
        else if(d.view.kind==NA_DESC_CBV)na::observe::OperationKey::add(key,d.view.cbv);
    }
    for(const auto& c:op.constants){na::observe::OperationKey::add(key,c.resource_id);na::observe::OperationKey::add(key,c.offset);na::observe::OperationKey::add(key,c.bytes);}
    for(const auto* r:{world.l[0].Get(),world.l[1].Get(),world.n[0].Get(),world.n[1].Get()})na::observe::OperationKey::add(key,r);
    auto old=dst.bindings.find(key);if(old!=dst.bindings.end())return &old->second;
    std::array<std::shared_ptr<const na::scale::PreparedBindings>,4> plans;
    std::string error;
    for(UINT pass=0;pass<4;++pass){
        na::scale::BindingSpec spec;spec.root=op.root;spec.descriptors=dst.descriptors;
        spec.native_extent={dst.rw,dst.rh};spec.render_extent=pass<2?spec.native_extent:na::scale::Extent{dst.ow,dst.oh};
        spec.viewports=op.viewports;spec.scissors=op.scissors;
        for(const auto& t:op.tables)spec.tables.push_back({t.param,t.first});
        for(const auto& d:op.descriptors){
            na::scale::ShaderView v;
            if(!d.used){v.srv.Format=DXGI_FORMAT_R8_UNORM;v.srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
                v.srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.srv.Texture2D.MipLevels=1;}
            else{
                v.resource=d.resource;v.counter=d.counter;v.default_view=!d.view.explicit_view;
                if(d.view.kind==NA_DESC_SRV){v.srv=d.view.srv;
                    if(d.view.resource==source){v.resource=pass<2?world.l[pass].Get():world.n[pass-2].Get();
                        v.default_view=false;v.srv.Format=d.view.explicit_view?d.view.srv.Format:d.resource->GetDesc().Format;v.srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
                        v.srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.srv.Texture2D={0,1,0,0};}}
                else if(d.view.kind==NA_DESC_CBV){v.kind=na::scale::ShaderView::Kind::NativeCbv;v.cbv=d.view.cbv;}
                else return nullptr;
            }
            spec.views.push_back(std::move(v));
        }
        // Existing named dimension reflection patches only actual size fields,
        // through a GPU copy; never scan/modify the game's constant buffer.
        if(pass>=2)for(const auto& c:op.constants)if(!c.dimensions.empty()){
            std::vector<uint8_t> bytes(c.bytes);
            if(na::observe::ReadBuffer(c.resource_id,c.offset,bytes.data(),c.bytes)!=c.bytes)return nullptr;
            std::vector<na::scale::ConstantPatch> patches;
            for(const auto& f:c.dimensions)if(!f.optional)patches.push_back({f.offset,f.integer?na::scale::Scalar::UInt:na::scale::Scalar::Float,
                static_cast<na::scale::Dimension>(f.dimension),spec.native_extent,spec.render_extent});
            if(patches.empty())continue;
            auto cb=na::scale::GpuConstants::Create(dst.device.Get(),c.resource.Get(),c.offset,bytes.data(),bytes.size(),patches,error);
            if(!cb)return nullptr;
            D3D12_HEAP_PROPERTIES hp{};if(FAILED(c.resource->GetHeapProperties(&hp,nullptr)))return nullptr;
            uint32_t state=D3D12_RESOURCE_STATE_GENERIC_READ;
            if(hp.Type!=D3D12_HEAP_TYPE_UPLOAD&&hp.Type!=static_cast<D3D12_HEAP_TYPE>(5)&&
               na::observe::ListStateSub(l,c.resource_id,0,&state)!=na::observe::StateLookup::Known)return nullptr;
            if(!cb->CanRecord(l,static_cast<D3D12_RESOURCE_STATES>(state)))return nullptr;
            const UINT index=UINT(spec.constants.size());spec.constants.push_back({cb,static_cast<D3D12_RESOURCE_STATES>(state)});
            if(c.descriptor==UINT_MAX)spec.root_cbvs.push_back({c.param,index});
            else if(c.descriptor<spec.views.size()){spec.views[c.descriptor].kind=na::scale::ShaderView::Kind::PatchedCbv;spec.views[c.descriptor].constant=index;}
            else return nullptr;
        }
        plans[pass]=na::scale::PreparedBindings::Create(dst.device.Get(),std::move(spec),error);
        if(!plans[pass])return nullptr;
    }
    return &dst.bindings.emplace(key,std::move(plans)).first->second;
}
void bind_world(ID3D12GraphicsCommandList* l, const na::scale::PreparedBindings& plan,const na::observe::ShaderState& original)
{
    const auto& spec=plan.Spec();
    for(const auto& c:spec.constants)c.buffer->RecordBefore(l,c.source_state);
    ID3D12DescriptorHeap* heaps[]={plan.ShaderHeap(),original.samplers};l->SetDescriptorHeaps(original.samplers?2:1,heaps);
    ComPtr<ID3D12Device> dev;l->GetDevice(IID_PPV_ARGS(&dev));
    const UINT inc=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto base=plan.ShaderHeap()->GetGPUDescriptorHandleForHeapStart();
    for(const auto& table:spec.tables)l->SetGraphicsRootDescriptorTable(table.param,{base.ptr+UINT64(plan.ShaderOffset()+table.first)*inc});
    for(UINT i=0;i<original.count;++i)if(original.tables[i].sampler)l->SetGraphicsRootDescriptorTable(original.tables[i].param,{original.tables[i].handle});
    for(const auto& c:spec.root_cbvs)l->SetGraphicsRootConstantBufferView(c.param,spec.constants[c.constant].buffer->Address());
}
} // namespace

void SetConfigured(bool on) { g_configured.store(on); }
bool Configured() { return g_configured.load(); }
void SetEnabled(bool on) { g_on.store(on); }
bool Enabled() { return g_configured.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed); }

void OnBind(ID3D12GraphicsCommandList* l, uint64_t recording)
{
    Bind b{l, recording, 0, false};
    na_object_id rt = 0, ds = 0;
    if (na::observe::ListCurrentTargets(l, &rt, &ds) && rt) {
        std::lock_guard<std::mutex> lock(g_m);
        b.rt = rt;
        if (g_dims.ow) {
            auto it = g_shapes.find(rt);
            if (it == g_shapes.end()) {
                ComPtr<ID3D12Resource> r;
                r.Attach(na::observe::AcquireResource(rt));
                if (r) {
                    if (g_shapes.size() >= 512) g_shapes.clear();
                    it = g_shapes.emplace(rt, r->GetDesc()).first;
                }
            }
            if (it != g_shapes.end()) {
                const auto& d = it->second;
                b.candidate = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.MipLevels == 1 && d.DepthOrArraySize == 1 &&
                              d.SampleDesc.Count == 1 && d.Width == g_dims.rw && d.Height == g_dims.rh &&
                              native_hud::TypelessOf(d.Format) != DXGI_FORMAT_UNKNOWN &&
                              native_hud::TypelessOf(d.Format) == native_hud::TypelessOf(g_dims.proxy);
            }
        }
    }
    t_bind = b;
}

na::observe::ShaderScope OnDraw(ID3D12GraphicsCommandList* l, const na::observe::ShaderCall& call)
{
    if (call.kind == na::observe::ShaderKind::Dispatch || t_bind.list != l || !t_bind.candidate) return {};
    na::observe::DrawInfo st;
    if (!na::observe::ListDrawState(l, st) || st.recording != t_bind.recording || st.rt != t_bind.rt) return {};
    const uint64_t verts = uint64_t(call.args[0]), inst = uint64_t(call.args[1]);
    const bool blend_known = st.pipeline_known && st.blend_mask != NA_UNKNOWN_U32;
    // Blending switched on but the destination replaced (DestBlend ZERO) is an opaque draw.
    // Cort's p23896 run (2026-11-05): every replayed pixel came out independent of the
    // background (ui_px == opaque, 1.3M px a frame) and differed from the final picture, which
    // is what a full-screen post pass with blending "on" but ONE/ZERO factors looks like.
    const bool blended = blend_known && (st.blend_mask & 1u) != 0 && st.blend_class != 3;
    std::lock_guard<std::mutex> lock(g_m);
    const Dims d = g_dims;
    if (!d.ow) return {};
    FrameSet& sets = g_set[g_frame % kSets];
    if(g_writes.size()>512)g_writes.clear();
    mutation_locked(l,st.rt,st.list,st.recording);
    const float* vp0 = st.viewport_count ? st.viewports[0] : nullptr;
    const int32_t* sc0 = st.scissor_count ? st.scissors[0] : nullptr;
    const bool full_pass=verts<=6&&inst==1&&native_hud::CoversTarget(vp0,sc0,d.rw,d.rh);
    na::observe::NativeOperation op;
    bool op_known=false;
    uint64_t sampled_source=0;
    const na::observe::OperationDescriptor* sampled=nullptr;
    std::shared_ptr<Set> sampled_layers;
    // A reflected source with already recorded HUD is an actual dependency,
    // even when its final compositor is opaque. No target-order promotion.
    // Viewport/scissor bounds are not geometry coverage. A six-vertex opaque
    // glyph may occupy a tiny rectangle with both set to the full target. It
    // must not erase earlier HUD or become the scene snapshot. Let the actual
    // raster writes update the layers; only an observed full Clear invalidates
    // the whole target. Camera/depth still identifies scene geometry below.
    if(full_pass&&blended)++g_stats.cover_blend;
    if(full_pass&&blend_known&&(st.blend_mask&1u)&&st.blend_class==3)++g_stats.opaque_blend;
    {   // Drawn with the scene camera, or depth-tested on the scene depth: part of the picture.
        na::observe::LiveBinding cb[16];
        const uint32_t n = na::observe::ListCurrentGraphicsCbvs(l, cb, 16);
        for (uint32_t i = 0; i < n; ++i)
            if (na::j2::CameraBinding(cb[i].resource, cb[i].offset, cb[i].param, cb[i].table)) {
                sets.scene.insert(st.rt);
                const auto old=sets.active.find(st.rt);if(old!=sets.active.end()&&old->second->closed)sets.active.erase(old);
                sets.transferred.erase(st.rt);++g_stats.world;return {};
            }
        uint64_t scene_depth = 0, scene_color = 0;
        na::observe::GetLiveSelection(&scene_depth, &scene_color);
        if (scene_depth && st.ds == scene_depth && st.pipeline_known && st.depth_enable != NA_UNKNOWN_U32 && st.depth_enable != 0u &&
            st.dsv_format != DXGI_FORMAT_UNKNOWN) {
            sets.scene.insert(st.rt);
            const auto old=sets.active.find(st.rt);if(old!=sets.active.end()&&old->second->closed)sets.active.erase(old);
            sets.transferred.erase(st.rt);++g_stats.world;return {};
        }
    }
    const DXGI_FORMAT view = static_cast<DXGI_FORMAT>(st.rtv_format);
    const char* why = nullptr;
    // Target identity is independent of recording order. Each candidate owns its own
    // layers; only an observed transfer can associate it with the presented resource.
    if (!st.restorable) why = "targets cannot be handed back";
    else if (!st.pipeline_known) why = "unknown pipeline";
    else if (st.rt_count != 1 || st.pso_rt_count != 1) why = "more than one target";
    else if (st.samples != 1) why = "multisampled";
    else if (!st.viewport_count || !st.scissor_count) why = "no viewport or scissor";
    else if (st.ds && st.dsv_format != DXGI_FORMAT_UNKNOWN) why = "uses a depth/stencil target";
    else if (native_hud::TypelessOf(view) != native_hud::TypelessOf(d.proxy)) why = "view format differs from the picture";
    else if (!blend_known) why = "blend state unknown";
    else if (st.blend_class == 4) { ++g_stats.irreversible; why = "blend reads the destination's alpha or uses min/max"; }
    else if (!st.shader_readonly || st.query_active) why = "draw has side effects";
    if (why) { forward_locked(why); return {}; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(l->GetDevice(IID_PPV_ARGS(&dev)))) { forward_locked("no device"); return {}; }
    // Expensive descriptor snapshots happen only AFTER camera/depth rejects
    // scene geometry. Normal world draws keep their original inexpensive path.
    op_known=na::observe::ListNativeOperation(l,false,op)&&op.reflection_complete;
    if(op_known&&native_hud::PictureSource(op,st.rt,d.rw,d.rh,sampled_source))
        for(const auto& input:op.descriptors)if(input.used&&input.view.resource==sampled_source){sampled=&input;break;}
    const auto source_active=sets.active.find(sampled_source),source_transfer=sets.transferred.find(sampled_source);
    if(source_active!=sets.active.end())sampled_layers=source_active->second;
    else if(source_transfer!=sets.transferred.end())sampled_layers=source_transfer->second;
    const auto previous=sets.active.find(st.rt);
    if(full_pass&&!blended&&sampled_source&&sets.scene.count(sampled_source)&&
       (!sampled_layers||!sampled_layers->draws)&&(previous==sets.active.end()||!previous->second->draws)){
        // A terminal HUD has not begun on either image, and the sampled input
        // descends from actual camera/depth scene work. Keep normal scene
        // postprocessing native; do not four-times replay the entire PP chain.
        sets.scene.insert(st.rt);++g_stats.scene;return {};
    }
    if(full_pass&&!blended&&sampled_layers&&sampled_layers->draws&&sampled_layers->scene_captured){
        // A full-image draw can be an identity handoff even with different
        // source/target sRGB views. Preserve the source's coefficient domain;
        // do NOT replay an already combined scene as a new opaque HUD layer.
        // Snapshot the source at this exact read. The current GPU comparison
        // against the final destination decides whether it really was copied,
        // including coverage/discard, colour transformations and late writes.
        uint32_t source_state=0;
        auto frozen=acquire_set_locked(dev.Get(),d);
        if(frozen)frozen->uses=sampled_layers->uses;
        if(frozen&&retain_locked(l,sampled_layers)&&retain_locked(l,frozen)&&
           na::observe::ListStateSub(l,sampled_source,0,&source_state)==na::observe::StateLookup::Known){
            copy_layers(l,*frozen,*sampled_layers);
            if(picture_locked(*frozen,sampled_layers->picture->Scene()->GetDesc())){
                frozen->scene_captured=frozen->picture->SnapshotScene(l,sampled_layers->picture->Scene(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                frozen->full_captured=frozen->picture->SnapshotFull(l,sampled->resource.Get(),static_cast<D3D12_RESOURCE_STATES>(source_state));
                frozen->target=st.rt;
                frozen->closed=true;packet_locked(l,st.rt,frozen);
                sets.active.erase(st.rt);sets.transferred[st.rt]=std::move(frozen);
                ++g_stats.draw_handoffs;return {};
            }
        }
        forward_locked("picture handoff snapshot unavailable");return {};
    }
    std::shared_ptr<Set> owner;
    if (previous != sets.active.end()&&!previous->second->closed) owner = previous->second;
    else {
        if (sets.active.size() >= kMaxTargets) { forward_locked("candidate pool busy"); return {}; }
        owner = acquire_set_locked(dev.Get(), d);
        if (!owner) { forward_locked("could not make the layers"); return {}; }
        const auto transferred=sets.transferred.find(st.rt);
        auto inherited=previous!=sets.active.end()?previous->second:transferred!=sets.transferred.end()?transferred->second:std::shared_ptr<Set>{};
        if(inherited){
            const auto& base = *inherited;
            copy_layers(l, *owner, base);
            owner->uses=base.uses;
            if (base.picture && base.scene_captured && picture_locked(*owner, base.picture->Scene()->GetDesc()))
                owner->scene_captured = owner->picture->SnapshotScene(l, base.picture->Scene(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        sets.active[st.rt]=owner;
    }
    if (!retain_locked(l, owner)) { forward_locked("could not retain recording"); return {}; }
    Set& s = *owner;
    s.target = t_bind.rt;
    s.writer = l; s.writer_id = st.list; s.recording = st.recording;
    s.full_captured = false;
    s.clean &= st.blend_class >= 1 && st.blend_class <= 3;
    std::shared_ptr<Set> world;
    const std::array<std::shared_ptr<const na::scale::PreparedBindings>,4>* plans=nullptr;
    na::observe::ShaderState original_bindings;
    if(sampled&&sampled->resource&&native_hud::TypelessOf(sampled->resource->GetDesc().Format)==s.typeless){
        // A UI batch may also copy the world image (even with output alpha1).
        // Substitute its actual source in the four private replays. Otherwise
        // L0=L1 contains the world and cannot be called a separated HUD.
        const auto source_view=sampled->view.explicit_view?sampled->view.srv.Format:sampled->resource->GetDesc().Format;
        if(native_hud::TypelessOf(source_view)!=DXGI_FORMAT_UNKNOWN&&source_view!=s.typeless){
            world=sampled_layers;
            if(!world){
                world=acquire_set_locked(dev.Get(),d);
                if(world&&sets.active.size()<kMaxTargets){world->target=sampled_source;sets.active.emplace(sampled_source,world);}
                else world.reset();
            }
            if(world&&!world->cleared){
                float grey=native_hud::GreyFor(view);
                if(is_srgb(source_view))grey=grey<=.0031308f?12.92f*grey:1.055f*std::pow(grey,1.f/2.4f)-.055f;
                clear_background_locked(l,*world,grey);world->cleared=true;world->view=source_view;
            }
            uint32_t source_state=0;
            if(world&&!world->scene_captured&&picture_locked(*world,sampled->resource->GetDesc())&&
               na::observe::ListStateSub(l,sampled_source,0,&source_state)==na::observe::StateLookup::Known){
                world->scene_captured=world->picture->SnapshotScene(l,sampled->resource.Get(),static_cast<D3D12_RESOURCE_STATES>(source_state));
                world->scene_source=sampled_source;world->source_srgb=is_srgb(source_view);
            }
            if(world&&world->scene_captured&&retain_locked(l,world)&&na::observe::ListShaderState(l,false,original_bindings)){
                plans=world_bindings(l,s,*world,sampled_source,op);
                if(plans&&(!s.scene_captured||!s.draws)){
                    if(picture_locked(s,sampled->resource->GetDesc()))s.scene_captured=s.picture->SnapshotScene(l,world->picture->Scene(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    s.scene_source=world->scene_source;s.source_srgb=world->source_srgb;
                }else if(plans&&s.scene_source!=world->scene_source)s.clean=false;
            }
        }
    }
    if (!op_known) s.clean = false;
    else for (const auto& input : op.descriptors) if (input.used && input.view.kind == NA_DESC_SRV && input.resource && input.view.resource != st.rt) {
        const auto desc = input.resource->GetDesc();
        // A batch can carry the world through a sampled image as well as draw
        // text. Such a source needs substitution in the native replay; an
        // empty UI canvas is not a reconstruction input for that world.
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == d.rw && desc.Height == d.rh &&
            native_hud::TypelessOf(desc.Format) != DXGI_FORMAT_UNKNOWN&&(!plans||input.view.resource!=sampled_source)) s.clean = false;
    }
    if (!s.scene_captured) {
        ComPtr<ID3D12Resource> original; original.Attach(na::observe::AcquireResource(t_bind.rt));
        if (original && picture_locked(s, original->GetDesc())) {
            s.scene_captured = s.picture->SnapshotScene(l, original.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            s.scene_source=st.rt;s.source_srgb=is_srgb(view);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    if (s.frame != g_frame) { s.frame = g_frame; s.cleared = false; s.draws = 0; s.covered = 0; s.view = DXGI_FORMAT_UNKNOWN; s.target = 0; }
    if (!s.cleared) {
        s.target = t_bind.rt;
        if (!rtvs_locked(s, view, rtv)) { forward_locked("too many view formats"); return {}; }
        clear_background_locked(l, s);
        s.cleared = true;
        s.view = view;
    } else {
        if (view != s.view) ++g_stats.view_changed; // the grey background follows the first view
        if (!rtvs_locked(s, view, rtv)) { forward_locked("too many view formats"); return {}; }
    }
    // Everything below is host work recorded into the game's list; the game's own draw follows.
    const float sx = float(d.ow) / float(d.rw), sy = float(d.oh) / float(d.rh);
    D3D12_VIEWPORT vp[16];
    D3D12_RECT sc[16];
    for (uint32_t i = 0; i < st.viewport_count; ++i) {
        float o[6];
        native_hud::ScaleViewport(st.viewports[i], sx, sy, o);
        vp[i] = {o[0], o[1], o[2], o[3], o[4], o[5]};
    }
    for (uint32_t i = 0; i < st.scissor_count; ++i) {
        int32_t o[4];
        native_hud::ScaleScissor(st.scissors[i], sx, sy, d.ow, d.oh, o);
        sc[i] = {o[0], o[1], o[2], o[3]};
    }
    uint64_t restore_cbvs=0;
    if(plans)for(const auto& plan:*plans)for(const auto& cb:plan->Spec().root_cbvs)restore_cbvs|=uint64_t(1)<<cb.param;
    for(UINT pass=0;pass<4;++pass){
        ID3D12Resource* source=plans?(pass<2?world->l[pass].Get():world->n[pass-2].Get()):nullptr;
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        if(plans){b.Transition={source,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
            l->ResourceBarrier(1,&b);bind_world(l,*(*plans)[pass],original_bindings);}
        if(pass>=2){l->RSSetViewports(st.viewport_count,vp);l->RSSetScissorRects(st.scissor_count,sc);}
        l->OMSetRenderTargets(1,&rtv[pass],FALSE,nullptr);draw(l,call);
        if(plans){
            for(const auto& c:(*plans)[pass]->Spec().constants)c.buffer->RecordAfter(l);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
        }
    }
    if (!na::observe::RestoreBindingState(l, false, plans!=nullptr, restore_cbvs, true, true, true)) ++g_stats.restore_failed;
    if(plans)++g_stats.world_substitutions;
    ++s.draws;
    ++g_stats.replayed;
    if (blended) ++g_stats.blended;
    return {};
}

void OnClear(ID3D12GraphicsCommandList* l, uint64_t target, const float color[4])
{
    if (!target) return;
    (void)l; (void)color;
    std::lock_guard<std::mutex> lock(g_m);
    auto& sets = g_set[g_frame % kSets];
    mutation_locked(l,target,na::observe::IdOf(l),na::observe::ListRecording(l));
    sets.scene.erase(target);
    const auto erased = sets.active.erase(target) + sets.transferred.erase(target);
    if (erased) ++g_stats.cleared_after;
}

void OnTexture(ID3D12GraphicsCommandList* l, const na::observe::TextureCall& c)
{
    // Runtime wrappers may implement a CopyResource through an internal
    // CopyTextureRegion on another list interface. Only the observed game's
    // recording owns this handoff; the implementation detail is not another
    // submitted game recording and must not replace its frozen layers.
    if (!na::observe::ListRecording(l)) return;
    if (!c.source || !c.destination ||
        (c.kind != na::observe::TextureKind::Copy && c.kind != na::observe::TextureKind::CopyRegion)) return;
    const auto dst_id = na::observe::IdOf(c.destination), src_id = na::observe::IdOf(c.source);
    if (!dst_id || !src_id || dst_id == src_id) return;
    const auto sd = c.source->GetDesc(), dd = c.destination->GetDesc();
    const bool whole = sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && dd.Dimension == sd.Dimension &&
        sd.Width == dd.Width && sd.Height == dd.Height && sd.DepthOrArraySize == 1 && dd.DepthOrArraySize == 1 &&
        sd.MipLevels == 1 && dd.MipLevels == 1 && sd.SampleDesc.Count == 1 && dd.SampleDesc.Count == 1 &&
        native_hud::TypelessOf(sd.Format) == native_hud::TypelessOf(dd.Format) &&
        (c.kind == na::observe::TextureKind::Copy || (!c.x && !c.y && !c.z &&
          c.source_location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && c.source_location.SubresourceIndex == 0 &&
          c.destination_location.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && c.destination_location.SubresourceIndex == 0 &&
          (!c.box || (!c.box->left && !c.box->top && !c.box->front && c.box->right == sd.Width &&
                      c.box->bottom == sd.Height && c.box->back == 1))));
    std::lock_guard<std::mutex> lock(g_m);
    auto& sets = g_set[g_frame % kSets];
    mutation_locked(l,dst_id,na::observe::IdOf(l),na::observe::ListRecording(l));
    ++g_stats.copies;
    std::shared_ptr<Set> source;
    const auto active = sets.active.find(src_id), transferred = sets.transferred.find(src_id);
    if (active != sets.active.end() && active->second->draws) source = active->second;
    else if (transferred != sets.transferred.end()) source = transferred->second;
    sets.active.erase(dst_id); sets.transferred.erase(dst_id);
    if (!whole) ++g_stats.unsupported_copies;
    if (!whole || !source || sets.transferred.size() >= kMaxTargets) return;
    // Freeze at the actual handoff, before a later source draw can replace the
    // layers. Keep both snapshots alive through the submitted copy recording.
    auto frozen = acquire_set_locked(source->device.Get(), g_dims);
    if(frozen)frozen->uses=source->uses;
    if (!frozen || !retain_locked(l, source) || !retain_locked(l, frozen)) return;
    copy_layers(l, *frozen, *source);
    if (source->picture && source->scene_captured && picture_locked(*frozen, source->picture->Scene()->GetDesc())) {
        frozen->scene_captured = frozen->picture->SnapshotScene(l, source->picture->Scene(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        uint32_t source_state = 0;
        if (na::observe::ListStateSub(l, src_id, 0, &source_state) == na::observe::StateLookup::Known)
            frozen->full_captured = frozen->picture->SnapshotFull(l, c.source, static_cast<D3D12_RESOURCE_STATES>(source_state));
    }
    frozen->target = dst_id;
    frozen->closed=true;packet_locked(l,dst_id,frozen);
    sets.transferred.emplace(dst_id, std::move(frozen));
    ++g_stats.copied_layers;
}

void OnClosing(ID3D12GraphicsCommandList* l)
{
    const auto id = na::observe::IdOf(l), recording = na::observe::ListRecording(l);
    if (!id || !recording) return;
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto& item : g_set[g_frame % kSets].active) {
        auto& s = *item.second;
        if (s.writer_id != id || s.recording != recording || s.closed || !s.draws || !s.picture || !s.scene_captured) continue;
        uint32_t state = 0;
        if (na::observe::ListStateSub(l, s.target, 0, &state) != na::observe::StateLookup::Known) continue;
        ComPtr<ID3D12Resource> original; original.Attach(na::observe::AcquireResource(s.target));
        if (original) s.full_captured = s.picture->SnapshotFull(l, original.Get(), static_cast<D3D12_RESOURCE_STATES>(state));
        s.closed=true;
        if(s.full_captured)packet_locked(l,s.target,item.second);
    }
}

void AfterSubmission(ID3D12CommandQueue* queue,uint32_t count,ID3D12CommandList* const* lists)
{
    if(!queue||!count||!lists)return;
    std::lock_guard<std::mutex> lock(g_m);
    const auto submission=++g_submission;
    const auto rank=[&](const auto& p){for(UINT i=0;i<count;++i)if(lists[i]==p->list&&na::observe::IdOf(lists[i])==p->list_id&&
        na::observe::ListRecording(lists[i])==p->recording)return i+1;return 0u;};
    std::vector<std::shared_ptr<Packet>> executing;
    for(const auto& p:g_packets)if(rank(p)&&p->use.Ready(queue))executing.push_back(p);
    std::stable_sort(executing.begin(),executing.end(),[&](const auto& a,const auto& b){const auto ar=rank(a),br=rank(b);return ar!=br?ar<br:a->order<b->order;});
    for(const auto& p:executing){
        if(!p->owner)g_submitted.erase(p->target);
        else if(std::all_of(p->owner->uses.begin(),p->owner->uses.end(),[&](const auto& u){return u.Ready(queue);})){p->owner->submitted=submission;g_submitted[p->target]=p->owner;}
    }
    g_packets.erase(std::remove_if(g_packets.begin(),g_packets.end(),[](const auto& p){return p->use.Retired();}),g_packets.end());
}

bool TakeFrame(ID3D12Resource* frame, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, native_hud::Layer* out,
               ID3D12CommandQueue* queue)
{
    if (!frame || !rw || !rh || !ow || !oh) return false;
    std::lock_guard<std::mutex> lock(g_m);
    const DXGI_FORMAT pf = frame->GetDesc().Format;
    if (g_dims.rw != rw || g_dims.rh != rh || g_dims.ow != ow || g_dims.oh != oh || g_dims.proxy != pf) {
        g_dims = Dims{rw, rh, ow, oh, pf};
        g_shapes.clear();
        for (auto& old : g_set) { old.active.clear(); old.transferred.clear();old.scene.clear(); }
        g_submitted.clear();
        g_presented.clear();
        g_associations.clear();g_probes.clear();
    }
    const auto id = na::observe::IdOf(frame);
    std::shared_ptr<Set> owner;
    const auto executed=g_submitted.find(id);
    if(executed!=g_submitted.end())owner=executed->second;
    const auto ready=[&](const auto& p){return p&&p->cleared&&p->draws&&p->picture&&p->scene_captured&&p->full_captured&&queue&&
        !p->uses.empty()&&std::all_of(p->uses.begin(),p->uses.end(),[&](const auto& use){return use.Ready(queue);});};
    // Reuse the existing asynchronous GPU picture probe when final shader
    // lineage is hidden. A learned source is still compared with THIS final
    // picture, pixel by pixel, before affecting reconstruction/composition.
    if(!owner){
        const Association* best=nullptr;
        for(const auto& a:g_associations)if(a.output==id&&a.confirmations>=2&&a.seen+120>=g_frame){
            const auto found=g_submitted.find(a.source);
            if(found==g_submitted.end()||!ready(found->second))continue;
            if(!best||uint64_t(a.matched)*best->tested>uint64_t(best->matched)*a.tested)best=&a;
        }
        if(best){owner=g_submitted.find(best->source)->second;++g_stats.learned_frames;}
    }
    if(out){
        std::vector<std::shared_ptr<Set>> probes;
        const auto since=g_presented[id];
        for(const auto& entry:g_submitted)if(entry.first!=id&&entry.second!=owner&&entry.second->submitted>since&&ready(entry.second))probes.push_back(entry.second);
        std::sort(probes.begin(),probes.end(),[](const auto& a,const auto& b){return a->target<b->target;});
        if(!probes.empty()){
            auto p=probes[g_probe_cursor++%probes.size()];
            const auto serial=++g_probe_serial;g_probes[serial]={id,p->target,g_frame};
            out->probe=p->picture;out->probe_owner=p;out->probe_source=p->target;out->probe_generation=serial;++g_stats.probes;
        }
        if(owner){const auto serial=++g_probe_serial;g_probes[serial]={id,owner->target,g_frame};
            out->association_source=owner->target;out->association_generation=serial;}
    }
    for(auto it=g_probes.begin();it!=g_probes.end();)if(it->second.frame+120<g_frame)it=g_probes.erase(it);else++it;
    g_presented[id]=g_submission;
    g_stats.requested_target = id;
    g_stats.matched_target = owner ? owner->target : 0;
    g_stats.pending_recordings = 0;
    if (owner) for (const auto& use : owner->uses) if (!use.Ready(queue)) ++g_stats.pending_recordings;
    const bool hud = owner && owner->cleared && owner->draws > 0 && owner->rw == rw && owner->rh == rh &&
        owner->ow == ow && owner->oh == oh && queue && !owner->uses.empty() &&
        std::all_of(owner->uses.begin(), owner->uses.end(), [&](const auto& u) { return u.Ready(queue); });
    ++g_stats.frames;
    g_stats.last_draws = hud ? owner->draws : 0;
    g_stats.last_covered = hud ? owner->covered : 0;
    g_stats.last_view = hud ? uint32_t(owner->view) : 0u;
    const bool current_proof = hud && owner->picture && owner->scene_captured && owner->full_captured;
    g_stats.current_picture = current_proof;
    const bool compose = current_proof || g_verdict.load(std::memory_order_relaxed) != 2u;
    if (hud) {
        ++g_stats.hud_frames;
        if (compose) ++g_stats.composed; else ++g_stats.held;
        if (out) {
            Set& s = *owner;
            out->n0 = s.n[0].Get(); out->n1 = s.n[1].Get();
            out->l0 = s.l[0].Get(); out->l1 = s.l[1].Get();
            out->view = s.view;
            out->grey = native_hud::GreyFor(s.view);
            out->srgb = is_srgb(s.view);
            out->unblend = true;
            out->compose = compose;
            out->owner = owner;
            if (current_proof) {
                s.picture->SetValidationLayers(s.l[0].Get(), s.l[1].Get(),s.source_srgb);
                out->picture = s.picture; out->clean_allowed = s.clean;
            }
        }
    }
    // The set of the frame that is kSets presents back is free again (its Present's GPU work
    // was submitted before anything recorded from now on).
    ++g_frame;
    auto& next = g_set[g_frame % kSets];
    next.active.clear(); next.transferred.clear();next.scene.clear();
    return hud;
}

bool ReportProbe(uint64_t source,uint64_t generation,bool identity,uint32_t tested,uint32_t matched)
{
    std::lock_guard<std::mutex> lock(g_m);
    const auto pending=g_probes.find(generation);
    if(pending==g_probes.end()||pending->second.source!=source)return false;
    const auto key=pending->second;g_probes.erase(pending);
    auto a=std::find_if(g_associations.begin(),g_associations.end(),[&](const auto& v){return v.output==key.output&&v.source==source;});
    if(a==g_associations.end()){
        if(g_associations.size()==64)g_associations.erase(std::min_element(g_associations.begin(),g_associations.end(),[](const auto& x,const auto& y){return x.seen<y.seen;}));
        g_associations.push_back({key.output,source});a=std::prev(g_associations.end());
    }
    if(key.frame<a->seen)return true; // older GPU receipt cannot overwrite newer evidence
    a->seen=key.frame;a->matched=matched;a->tested=tested;
    if(identity||(tested&&uint64_t(matched)*1000>=uint64_t(tested)*980)){a->confirmations=std::min(a->confirmations+1,3u);++g_stats.proved;}
    else {a->confirmations=0;++g_stats.probe_mismatch;}
    return true;
}

void ReportCheck(uint32_t ui_px, uint32_t opaque, uint32_t match_id, uint32_t match_dec, uint32_t match_enc)
{
    std::lock_guard<std::mutex> lock(g_m);
    ++g_stats.check_frames;
    g_w_ui += ui_px; g_w_opaque += opaque; g_w_id += match_id; g_w_dec += match_dec; g_w_enc += match_enc;
    g_stats.ui_px = g_w_ui; g_stats.opaque_px = g_w_opaque; g_stats.match_id = g_w_id; g_stats.match_dec = g_w_dec; g_stats.match_enc = g_w_enc;
    if (++g_w_frames < 30) return;
    if (g_w_opaque >= 500) { // enough fully covered UI pixels to decide
        const auto permille = [&](uint64_t n) { return uint32_t(n * 1000u / g_w_opaque); };
        g_stats.rate_id = permille(g_w_id); g_stats.rate_dec = permille(g_w_dec); g_stats.rate_enc = permille(g_w_enc);
        const uint32_t v = g_stats.rate_id >= 850 ? 1u : 2u;
        g_stats.verdict = v;
        g_verdict.store(v, std::memory_order_relaxed);
    }
    g_w_frames = g_w_ui = g_w_opaque = g_w_id = g_w_dec = g_w_enc = 0;
}

Stats GetStats() { std::lock_guard<std::mutex> lock(g_m); return g_stats; }
std::string StatusLine()
{
    const Stats s = GetStats();
    char b[1024];
    std::snprintf(b, sizeof b,
        "hud_unblend on=%u frames=%llu hud_frames=%llu composed=%llu held=%llu replayed=%llu blended=%llu forwarded=%llu world=%llu scene=%llu covered=%llu "
        "cover_blend=%llu opaque_blend=%llu irreversible=%llu other_target=%llu retargeted=%llu cleared_after=%llu view_changed=%llu restore_failed=%llu last{draws=%u covered=%u view=%u} last_forward=\"%s\" "
        "check{frames=%llu window{ui_px=%llu opaque=%llu id=%llu dec=%llu enc=%llu} permille{id=%u dec=%u enc=%u} diagnostic_verdict=%s}",
        Enabled() ? 1u : 0u, (unsigned long long)s.frames, (unsigned long long)s.hud_frames, (unsigned long long)s.composed, (unsigned long long)s.held,
        (unsigned long long)s.replayed, (unsigned long long)s.blended, (unsigned long long)s.forwarded, (unsigned long long)s.world, (unsigned long long)s.scene,
        (unsigned long long)s.covered, (unsigned long long)s.cover_blend, (unsigned long long)s.opaque_blend, (unsigned long long)s.irreversible, (unsigned long long)s.other_target, (unsigned long long)s.retargeted, (unsigned long long)s.cleared_after, (unsigned long long)s.view_changed,
        (unsigned long long)s.restore_failed, s.last_draws, s.last_covered, s.last_view, s.last_forward,
        (unsigned long long)s.check_frames, (unsigned long long)s.ui_px, (unsigned long long)s.opaque_px, (unsigned long long)s.match_id,
        (unsigned long long)s.match_dec, (unsigned long long)s.match_enc, s.rate_id, s.rate_dec, s.rate_enc,
        s.verdict == 1 ? "identity" : s.verdict == 2 ? "mismatch_holding" : "undecided");
    char routing[512];
    std::snprintf(routing, sizeof routing, " routing{copies=%llu linked=%llu unsupported=%llu requested=%llu matched=%llu pending=%u method=%s draw_handoffs=%llu world_substitution=%llu} discovery{probes=%llu proved=%llu mismatch=%llu learned_frames=%llu}",
                  (unsigned long long)s.copies, (unsigned long long)s.copied_layers, (unsigned long long)s.unsupported_copies,
                  (unsigned long long)s.requested_target, (unsigned long long)s.matched_target, s.pending_recordings,
                  s.current_picture ? "current_picture" : s.last_draws ? "legacy_unblend" : "none",
                  (unsigned long long)s.draw_handoffs,(unsigned long long)s.world_substitutions,
                  (unsigned long long)s.probes,(unsigned long long)s.proved,(unsigned long long)s.probe_mismatch,(unsigned long long)s.learned_frames);
    return std::string(b) + routing;
}
} // namespace na::j2::hud_unblend
