#include "host_dx12/j2/live_dlaa.hpp"

#include "core/motion_arbiter_gpu.hpp"
#include "core/temporal_jitter.hpp"
#include "host_dx12/depth_format.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/observe/producers.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace na::j2 {
namespace {

constexpr uint32_t kRing = 6;            // frames in flight before a slot is reused (never waited on)
constexpr uint32_t kBlockBytes = 512;    // root CBVs have no size: read this much (the producer's own bound)
constexpr size_t kMaxBlocks = 256;       // unique constant blocks examined per frame (until the camera slot is learned)
constexpr uint32_t kMaxBlockRead = 1024; // bytes read from one constant buffer
constexpr uint64_t kViewId = 0x4A32;     // DLSS view for this source ("J2")

struct Slot {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 fence_value = 0;
};

struct State {
    std::mutex m;
    std::atomic<bool> enabled{false};
    std::atomic<bool> configured{false};
    std::atomic<bool> paused{false}; // the user's on/off switch in the overlay
    bool resume_reset = false;       // history is stale after a pause
    bool latched = false;            // stopped for this session after a failure (reason kept)
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    Slot slots[kRing];
    uint32_t slot = 0;
    // Our textures (render size == output size: DLAA).
    ComPtr<ID3D12Resource> depth32, motion, provenance, output;
    ComPtr<ID3D12Resource> color_in; // plain copy of the scene colour's first slice / mip (DLSS input)
    uint32_t w = 0, h = 0;
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    na::motion::gpu::Arbiter arbiter;
    bool arbiter_ok = false;
    na::camera::CameraTracker tracker;
    uint64_t frame = 1;             // game Present interval, not Execute attempt
    uint64_t submitted_frame = 0;   // last interval that actually submitted DLAA
    int64_t camera_offset = -1; // learned offset of the ViewProjection inside its block
    int64_t camera_param = -1;  // learned root parameter that binds the camera block
    int64_t camera_inv_delta = 64; // learned byte distance from the ViewProjection to its inverse
    bool camera_table = false;  // ... through a descriptor table
    // J1: this frame's injected camera jitter (see jitter_batch).
    struct Jitter {
        bool valid = false;
        uint64_t res = 0, off = 0;           // the camera block it was found in
        double px[2] = {0, 0};               // sample, render pixels (DLSS: colour sampled at centre - jitter)
        uint8_t vp[2][64], inv[2][64];       // original matrices as stored (row / column layout)
        uint8_t jvp[2][64], jinv[2][64];     // jittered replacements
        uint32_t phase = 0;
        int layout = 0;                      // 0 row-major, 1 column-major (as the game stores it)
    } jit;
    bool jitter_on = true;
    Stats stats;
};
// Never destroyed: at process exit the observation hooks' own state may already
// be gone, and releasing D3D12 objects then would call into them.
State& g = *new State;

void reason(const char* r) { std::snprintf(g.stats.reason, sizeof(g.stats.reason), "%s", r); }

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, UINT before, UINT after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
    b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(after);
    return b;
}
D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    return b;
}
// Appends a transition unless the states are equal.
void add(std::vector<D3D12_RESOURCE_BARRIER>& v, ID3D12Resource* r, UINT before, UINT after)
{
    if (before != after) v.push_back(transition(r, before, after));
}
// One subresource only (the scene's mip / slice; the rest of the texture is never touched).
void addsub(std::vector<D3D12_RESOURCE_BARRIER>& v, ID3D12Resource* r, UINT sub, UINT before, UINT after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b = transition(r, before, after);
    b.Transition.Subresource = sub;
    v.push_back(b);
}
void flush(ID3D12GraphicsCommandList* l, std::vector<D3D12_RESOURCE_BARRIER>& v)
{
    if (!v.empty()) l->ResourceBarrier(static_cast<UINT>(v.size()), v.data());
    v.clear();
}

bool texture(UINT w, UINT h, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    out.Reset();
    return SUCCEEDED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
}
// A typed format for our own copies of a (possibly typeless) game texture.
DXGI_FORMAT typed(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}
bool uav_store(DXGI_FORMAT fmt)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = fmt;
    return SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
           (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}
bool is_float_color(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT ||
           f == DXGI_FORMAT_R32G32B32_FLOAT;
}

bool ensure_device(ID3D12CommandQueue* q)
{
    if (g.device && g.queue.Get() == q) return true;
    if (g.device) { reason("Live DLAA follows one queue; another queue submitted the scene."); return false; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    g.device = dev;
    g.queue = q;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    const D3D12_COMMAND_LIST_TYPE type = q->GetDesc().Type;
    for (auto& s : g.slots) {
        if (FAILED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&s.alloc))) ||
            FAILED(dev->CreateCommandList(0, type, s.alloc.Get(), nullptr, IID_PPV_ARGS(&s.list))))
            return false;
        s.list->Close();
    }
    g.arbiter_ok = g.arbiter.Init(dev.Get(), kRing);
    return true;
}

// The resource state at the end of the scene list: its last whole-resource
// transition there, else in an earlier list of the same batch.
bool state_after(ID3D12CommandList* const* lists, UINT last, na_object_id id, uint32_t* st)
{
    for (int i = static_cast<int>(last); i >= 0; --i)
        if (na::observe::ListLastState(lists[i], id, st)) return true;
    // Not transitioned in this batch up to the scene: the state it carried in.
    return na::observe::KnownStateInCurrentSubmission(id, st);
}

// ---- J1: camera jitter ---------------------------------------------------
// DLAA needs a different sub-pixel sample every frame. Anno (like most games
// without temporal AA) renders with a fixed camera, so J1 shifts it: before a
// batch is forwarded, the camera ViewProjection and its inverse in the bound
// constant buffers are replaced by jittered copies (core/temporal_jitter.hpp,
// Halton 2,3 over 8 phases, DLSS's recommendation for DLAA). The same
// replacement goes into every bound block that holds an identical copy, so a
// depth pre-pass in an earlier batch and the scene agree. J2 then reports the
// known sample to the camera tracker (which removes it exactly) and to DLSS.
// Runs only while DLAA is producing frames, so the shake is never left
// unresolved on screen.
constexpr uint32_t kJitterPhases = 8;

void store_matrix(const na::camera::Matrix& m, na::camera::Layout layout, uint8_t out[64])
{
    for (size_t r = 0; r < 4; ++r)
        for (size_t c = 0; c < 4; ++c) {
            const float v = static_cast<float>(m[r * 4 + c]);
            std::memcpy(out + (layout == na::camera::Layout::RowMajor ? r * 4 + c : c * 4 + r) * 4, &v, 4);
        }
}

// A camera block's bytes hold a VP + inverse at the learned offset: start this frame's jitter.
// Last main camera the jitter started from (a block outside the learned camera
// slot may start a frame only if it shows that same camera, not a light's).
struct { bool valid = false; na::camera::VpGeometry geo{}; } g_jit_cam;

bool same_camera(const na::camera::Matrix& m)
{
    na::camera::VpGeometry v{};
    if (!g_jit_cam.valid || !na::camera::DecomposeViewProjection(m, v)) return false;
    const auto& p = g_jit_cam.geo;
    double dot = 0, d2 = 0, len2 = 0;
    for (int j = 0; j < 3; ++j) {
        dot += v.forward[j] * p.forward[j];
        d2 += (v.position[j] - p.position[j]) * (v.position[j] - p.position[j]);
        len2 += p.position[j] * p.position[j];
    }
    return dot > 0.98 && std::abs(v.sy - p.sy) <= 0.05 * std::abs(p.sy) && d2 <= 0.01 * len2 + 100.0;
}

bool begin_jitter(uint64_t res, uint64_t off, const std::vector<uint8_t>& bytes, bool slot)
{
    const size_t n = bytes.size();
    if (g.camera_offset < 0 || static_cast<uint64_t>(g.camera_offset) + 64 > n || !g.w || !g.h) return false;
    for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
        const auto m = na::camera::LoadMatrix(bytes, static_cast<size_t>(g.camera_offset), layout);
        na::camera::Candidate c{};
        if (!na::camera::ViewProjection(m, c)) continue;
        if (!slot && !same_camera(m)) continue;
        // The inverse at its learned position. A block from the camera slot must
        // carry it; another block (an earlier pass with a smaller constant buffer)
        // may start the frame from the ViewProjection alone, the inverse being
        // computed; the game's own inverse copy is adopted when the slot block shows it.
        const int64_t io = g.camera_offset + g.camera_inv_delta;
        const bool inv_here = io >= 0 && static_cast<uint64_t>(io) + 64 <= n;
        na::camera::Matrix inv{};
        if (inv_here) {
            inv = na::camera::LoadMatrix(bytes, static_cast<size_t>(io), layout);
            if (!na::camera::IsInversePair(m, inv)) {
                if (slot) continue;
                if (!na::camera::Invert(m, inv)) continue;
            }
        } else {
            if (slot || !na::camera::Invert(m, inv)) continue;
        }
        g_jit_cam.valid = na::camera::DecomposeViewProjection(m, g_jit_cam.geo);
        const neural_alias::JitterSample js = neural_alias::temporal_jitter(++g.jit.phase, kJitterPhases);
        std::array<double, 16> src{};
        for (int i = 0; i < 16; ++i) src[i] = m[i];
        const auto jm = neural_alias::jitter_projection(src, js, g.w, g.h);
        na::camera::Matrix jvp{}, jinv{};
        for (int i = 0; i < 16; ++i) jvp[i] = jm[i];
        if (!na::camera::Invert(jvp, jinv)) return false;
        auto& j = g.jit;
        j.res = res;
        j.off = off;
        j.px[0] = js.x_pixels;
        j.px[1] = js.y_pixels;
        j.layout = layout == na::camera::Layout::RowMajor ? 0 : 1;
        for (int l = 0; l < 2; ++l) {
            const auto L = l ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
            store_matrix(m, L, j.vp[l]);
            store_matrix(inv, L, j.inv[l]);
            store_matrix(jvp, L, j.jvp[l]);
            store_matrix(jinv, L, j.jinv[l]);
        }
        // Keep the bytes exactly as the game stored them for matching (float round trip).
        std::memcpy(j.vp[j.layout], bytes.data() + g.camera_offset, 64);
        if (inv_here && na::camera::IsInversePair(m, na::camera::LoadMatrix(bytes, static_cast<size_t>(io), layout)))
            std::memcpy(j.inv[j.layout], bytes.data() + io, 64);
        j.valid = true;
        return true;
    }
    return false;
}

// Replaces this frame's VP / inverse in the bound constant buffers of this batch.
// The frame's sample starts at the first block holding a VP + inverse at the
// learned offset (blocks of the learned camera slot are tried first); then every
// exact copy of that VP / inverse in ANY bound block of the batch is replaced,
// because an earlier pass (a depth pre-pass, often with its own root signature)
// must see the same jittered camera as the scene, or depth tests disagree.
void jitter_batch(ID3D12CommandList* const* lists, UINT count)
{
    // Only while DLAA is producing frames: the previous game frame submitted one
    // (a batch without the scene must not count as "not running").
    const bool running = g.submitted_frame && g.frame == g.submitted_frame + 1;
    if (!g.jitter_on || g.camera_param < 0 || g.camera_offset < 0 || !running || !g.w) return;
    struct JB { uint64_t res, off; uint32_t size; bool slot; };
    std::map<std::pair<uint64_t, uint64_t>, JB> blocks;
    for (UINT i = 0; i < count && blocks.size() < 256; ++i) {
        if (!lists[i]) continue;
        for (const auto& b : na::observe::ListRootConstantBuffers(lists[i])) {
            const uint32_t sz = b.table && b.size ? b.size : kBlockBytes;
            JB& k = blocks[{b.resource, b.offset}];
            k.res = b.resource;
            k.off = b.offset;
            k.size = sz < kMaxBlockRead ? sz : kMaxBlockRead;
            k.slot = k.slot || (b.param == static_cast<uint32_t>(g.camera_param) && b.table == g.camera_table);
            if (blocks.size() >= 256) break;
        }
    }
    std::vector<const JB*> order;
    for (const auto& kv : blocks) order.push_back(&kv.second);
    std::stable_sort(order.begin(), order.end(), [](const JB* a, const JB* b) { return a->slot && !b->slot; });
    std::vector<uint8_t> bytes(kMaxBlockRead);
    for (const JB* k : order) {
        const uint32_t n = na::observe::ReadBuffer(k->res, k->off, bytes.data(), k->size);
        if (n < 64) continue;
        std::vector<uint8_t> view(bytes.begin(), bytes.begin() + n);
        const auto holds = [&](const uint8_t (&m)[2][64]) {
            for (size_t o = 0; o + 64 <= n; o += 16)
                if (!std::memcmp(view.data() + o, m[0], 64) || !std::memcmp(view.data() + o, m[1], 64)) return true;
            return false;
        };
        if (!g.jit.valid || !holds(g.jit.vp)) {
            if (g.jit.valid && holds(g.jit.jvp)) continue; // already jittered by an earlier batch of this frame
            if (g.jit.valid) continue;                     // a different matrix (another camera): leave it
            if (!begin_jitter(k->res, k->off, view, k->slot)) continue;
        }
        if (k->slot) { // adopt the game's exact inverse bytes (they may differ from a computed inverse)
            const int64_t vo = g.camera_offset, io = vo + g.camera_inv_delta;
            if (io >= 0 && static_cast<uint64_t>(io) + 64 <= n && static_cast<uint64_t>(vo) + 64 <= n &&
                !std::memcmp(view.data() + vo, g.jit.vp[g.jit.layout], 64) && std::memcmp(view.data() + io, g.jit.inv[g.jit.layout], 64)) {
                const auto L = g.jit.layout ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor;
                if (na::camera::IsInversePair(na::camera::LoadMatrix(view, static_cast<size_t>(vo), L), na::camera::LoadMatrix(view, static_cast<size_t>(io), L)))
                    std::memcpy(g.jit.inv[g.jit.layout], view.data() + io, 64);
            }
        }
        for (size_t o = 0; o + 64 <= n; o += 16)
            for (int l = 0; l < 2; ++l) {
                const uint8_t* rep = nullptr;
                if (!std::memcmp(view.data() + o, g.jit.vp[l], 64)) rep = g.jit.jvp[l];
                else if (!std::memcmp(view.data() + o, g.jit.inv[l], 64)) rep = g.jit.jinv[l];
                if (rep && na::observe::WriteBuffer(k->res, k->off + o, rep, 64) == 64) ++g.stats.jitter_writes;
                if (rep) break;
            }
    }
}

// This frame's camera: a constant buffer holding a perspective ViewProjection
// together with its inverse, bound in this batch up to the scene list. Root CBVs
// and CBVs reached through descriptor tables both count (Anno binds its camera
// through a table). Lists that use the scene depth are searched first, then the
// rest; within that, the binding slot that held the camera last time leads.
struct CamDiag { size_t lists = 0, binds = 0, blocks = 0, read = 0, vp = 0, cands = 0; } g_cd;

struct Block {
    uint64_t resource = 0, offset = 0;
    uint32_t size = 0, param = 0, uses = 0, list_rank = 0;
    bool table = false;
};

bool check_learned(const std::vector<uint8_t>& bytes, na::camera::Matrix* vp, bool* reversed)
{
    const size_t n = bytes.size();
    if (g.camera_offset < 0 || static_cast<uint64_t>(g.camera_offset) + 64 > n) return false;
    for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
        const auto m = na::camera::LoadMatrix(bytes, static_cast<size_t>(g.camera_offset), layout);
        na::camera::Candidate c{};
        if (!na::camera::ViewProjection(m, c)) continue;
        for (int64_t d : {g.camera_inv_delta}) { // the learned inverse position (Anno: not adjacent)
            const int64_t off = g.camera_offset + d;
            if (off < 0 || static_cast<uint64_t>(off) + 64 > n) continue;
            if (na::camera::IsInversePair(m, na::camera::LoadMatrix(bytes, static_cast<size_t>(off), layout))) {
                *vp = m;
                *reversed = c.reversed;
                return true;
            }
        }
    }
    return false;
}

bool g_injected = false; // the camera found this frame carries J1's jitter (g.jit.px)

bool find_camera(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na::camera::Matrix* vp, bool* reversed)
{
    g_cd = {};
    g_injected = false;
    // Lists to search: the scene list, then earlier lists using the depth, then the others.
    std::vector<std::pair<uint32_t, UINT>> order_lists;
    for (int i = static_cast<int>(scene); i >= 0; --i) {
        if (!lists[i]) continue;
        const uint32_t rank = static_cast<UINT>(i) == scene ? 0u : (na::observe::ListReferences(lists[i], depth_id) ? 1u : 2u);
        order_lists.push_back({rank, static_cast<UINT>(i)});
    }
    std::stable_sort(order_lists.begin(), order_lists.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::map<std::pair<uint64_t, uint64_t>, Block> blocks;
    for (const auto& ol : order_lists) {
        const auto binds = na::observe::ListRootConstantBuffers(lists[ol.second]);
        if (binds.empty()) continue;
        ++g_cd.lists;
        g_cd.binds += binds.size();
        for (const auto& b : binds) {
            Block& k = blocks[{b.resource, b.offset}];
            if (!k.uses) {
                k.resource = b.resource;
                k.offset = b.offset;
                k.size = b.table && b.size ? b.size : kBlockBytes;
                k.param = b.param;
                k.table = b.table;
                k.list_rank = ol.first;
            }
            ++k.uses;
        }
    }
    std::vector<const Block*> order;
    order.reserve(blocks.size());
    for (const auto& kv : blocks) order.push_back(&kv.second);
    const auto learned = [](const Block* b) { return g.camera_param >= 0 && b->param == static_cast<uint32_t>(g.camera_param) && b->table == g.camera_table; };
    std::stable_sort(order.begin(), order.end(), [&](const Block* a, const Block* b) {
        if (learned(a) != learned(b)) return learned(a);
        if (a->list_rank != b->list_rank) return a->list_rank < b->list_rank;
        return a->uses > b->uses;
    });
    if (order.size() > kMaxBlocks) order.resize(kMaxBlocks);
    g_cd.blocks = order.size();
    std::vector<uint8_t> bytes(kMaxBlockRead);
    for (const Block* o : order) {
        const uint32_t want = o->size < kMaxBlockRead ? o->size : kMaxBlockRead;
        const uint32_t n = na::observe::ReadBuffer(o->resource, o->offset, bytes.data(), want);
        if (n < 128) continue;
        ++g_cd.read;
        // An in-process read at submission time, inside the submitting call:
        // its scope is known (FindMatrices rejects snapshots without one).
        na::camera::Snapshot s{};
        s.session = s.epoch = 1;
        s.submission = s.content_version = g.frame;
        s.resource = o->resource;
        s.base_offset = 0; // offsets are relative to the block
        s.complete = s.lifetime_valid = true;
        s.sync = na::camera::Sync::AppSyncUnknown;
        s.bytes.assign(bytes.begin(), bytes.begin() + n);
        if (learned(o) && check_learned(s.bytes, vp, reversed)) {
            ++g.stats.camera_learned_hits;
            const size_t lo = static_cast<size_t>(g.camera_offset);
            g_injected = g.jit.valid && lo + 64 <= s.bytes.size() &&
                         (!std::memcmp(s.bytes.data() + lo, g.jit.jvp[0], 64) || !std::memcmp(s.bytes.data() + lo, g.jit.jvp[1], 64));
            return true;
        }
        const auto scan = na::camera::FindMatrices(s);
        g_cd.cands += scan.candidates.size();
        for (const auto& c : scan.candidates) {
            if (c.kind == na::camera::Kind::ViewProjection) ++g_cd.vp;
            if (c.kind != na::camera::Kind::ViewProjection || !c.has_inverse_pair) continue;
            g.camera_offset = static_cast<int64_t>(c.byte_offset);
            g.camera_inv_delta = static_cast<int64_t>(c.inverse_byte_offset) - static_cast<int64_t>(c.byte_offset);
            g.stats.camera_inv_delta = g.camera_inv_delta;
            ++g.stats.camera_scans;
            g.camera_param = static_cast<int64_t>(o->param);
            g.camera_table = o->table;
            g.stats.camera_offset = c.byte_offset;
            *vp = c.matrix;
            *reversed = c.reversed;
            return true;
        }
    }
    return false;
}

na::motion::Mat4 to_mat4(const na::camera::Matrix& m)
{
    na::motion::Mat4 r{};
    for (int i = 0; i < 16; ++i) r.m[i] = static_cast<float>(m[i]);
    return r;
}

// Records the DLAA list for this frame. Returns false (nothing recorded worth
// submitting) when a precondition fails.
struct PendingHistory {
    na::camera::CameraTracker tracker;
    bool cut = false;
};

bool record(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na_object_id color_id,
            ID3D12GraphicsCommandList** out_list, PendingHistory& pending)
{
    uint32_t sc = 0, sd = 0;
    if (!state_after(lists, scene, color_id, &sc) || !state_after(lists, scene, depth_id, &sd)) {
        ++g.stats.no_state;
        reason("Waiting: the scene colour/depth state at the end of the scene pass is not established.");
        return false;
    }
    ComPtr<ID3D12Resource> color, depth;
    color.Attach(na::observe::AcquireResource(color_id));
    depth.Attach(na::observe::AcquireResource(depth_id));
    if (!color || !depth) { reason("Scene resources are not available."); return false; }
    const D3D12_RESOURCE_DESC cd = color->GetDesc(), dd = depth->GetDesc();
    g.stats.color_mips = cd.MipLevels ? cd.MipLevels : 1;
    g.stats.color_slices = cd.DepthOrArraySize;
    g.stats.color_samples = cd.SampleDesc.Count;
    g.stats.depth_samples = dd.SampleDesc.Count;
    g.stats.depth_slices = dd.DepthOrArraySize;
    // Multisampling is not supported (DLSS takes resolved pictures).
    if (cd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1) {
        char why[160];
        std::snprintf(why, sizeof why, "The game renders with multisampling (colour x%u, depth x%u). Set the game's anti-aliasing to Off for DLAA.",
                      cd.SampleDesc.Count, dd.SampleDesc.Count);
        reason(why);
        return false;
    }
    if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        reason("The scene colour or depth is not a 2D texture.");
        return false;
    }
    // The subresources (mip + slice) the scene pass rendered into, from its views:
    // arrays and mip chains are fine, J2 works on exactly that one through its own copies.
    uint32_t csub = 0, dsub = 0;
    if (!na::observe::ListSceneSubresource(lists[scene], color_id, depth_id, &csub, &dsub) || csub == 0xFFFFFFFFu || dsub == 0xFFFFFFFFu) {
        reason("The scene is drawn through a view NeuralAlias cannot place (several array slices at once, or an unusual view).");
        return false;
    }
    g.stats.color_sub = csub;
    const uint32_t cmip = csub % g.stats.color_mips;
    const uint32_t dmip = dsub % (dd.MipLevels ? dd.MipLevels : 1u);
    const uint32_t w = (std::max)(1u, static_cast<uint32_t>(cd.Width >> cmip)), h = (std::max)(1u, cd.Height >> cmip);
    if ((std::max)(1u, static_cast<uint32_t>(dd.Width >> dmip)) != w || (std::max)(1u, dd.Height >> dmip) != h ||
        !na::depth::PlaneZeroCopiesToR32F(dd.Format)) {
        reason("Scene depth does not match the colour size or is not 32-bit depth.");
        return false;
    }
    const DXGI_FORMAT cf = typed(cd.Format); // our copies' format

    // Camera first (nothing is allocated for a frame that cannot run); the tracker
    // update comes after the size check, because a resize resets the tracker.
    na::camera::Matrix vp{};
    bool reversed = false;
    if (!find_camera(lists, scene, depth_id, &vp, &reversed)) {
        ++g.stats.no_camera;
        g.camera_offset = g.camera_param = -1; // relearn
        char why[160];
        std::snprintf(why, sizeof why, "Waiting for the camera (lists %zu, binds %zu, blocks %zu, read %zu, matrices %zu, view-proj %zu).",
                      g_cd.lists, g_cd.binds, g_cd.blocks, g_cd.read, g_cd.cands, g_cd.vp);
        reason(why);
        return false;
    }
    g_jit_cam.valid = na::camera::DecomposeViewProjection(vp, g_jit_cam.geo); // J1: the main camera, for earlier batches next frame
    // Ring slot (never wait) and size/format changes (only once the GPU is done with the old set).
    const UINT64 done = g.fence->GetCompletedValue();
    Slot& s = g.slots[g.slot];
    if (done == UINT64_MAX) { reason("Device removed."); return false; }
    if (s.fence_value && done < s.fence_value) { ++g.stats.busy; reason("Skipped: the previous use of this frame slot is still running."); return false; }
    const bool resize = !g.output || !g.color_in || g.w != w || g.h != h || g.color_format != cf;
    if (resize) {
        if (done + 1 < g.fence_next) { ++g.stats.busy; reason("Waiting for the previous picture size to finish."); return false; }
        if (!uav_store(cf) || !uav_store(DXGI_FORMAT_R32G32_FLOAT)) { reason("This scene colour format cannot be written by DLSS here."); return false; }
        if (!texture(w, h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.depth32) ||
            !texture(w, h, DXGI_FORMAT_R32G32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.motion) ||
            !texture(w, h, DXGI_FORMAT_R8_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.provenance) ||
            !texture(w, h, cf, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.output) ||
            !texture(w, h, cf, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.color_in)) {
            g.output.Reset();
            reason("Could not allocate the DLAA working textures.");
            return false;
        }
        g.w = w; g.h = h; g.color_format = cf;
        g.resume_reset = true; // remains pending if this recording is not submitted
    }
    const double zero[2] = {0, 0};
    const double* injected = g_injected ? g.jit.px : zero; // J1: the exact sample we injected (removed from the matrix)
    pending.tracker = g.tracker;
    // A skipped game frame breaks the sequence of input pictures seen by DLSS.
    if (g.resume_reset || (g.submitted_frame && g.frame != g.submitted_frame + 1)) pending.tracker.Reset();
    const na::camera::CameraFrame cam = pending.tracker.Update(vp, g.frame, w, h, injected);
    if (!cam.valid) { ++g.stats.no_camera; reason("The camera matrix could not be inverted."); return false; }
    pending.cut = cam.camera_cut;
    if (FAILED(s.alloc->Reset()) || FAILED(s.list->Reset(s.alloc.Get(), nullptr))) { reason("Could not reset the DLAA command list."); return false; }
    ID3D12GraphicsCommandList* l = s.list.Get();
    std::vector<D3D12_RESOURCE_BARRIER> b;
    constexpr UINT NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, SRC = D3D12_RESOURCE_STATE_COPY_SOURCE,
                   DST = D3D12_RESOURCE_STATE_COPY_DEST, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // 1. depth plane 0 -> R32F; scene colour slice 0 / mip 0 -> our plain copy
    add(b, depth.Get(), sd, SRC);
    add(b, g.depth32.Get(), NPSR, DST);
    addsub(b, color.Get(), csub, sc, SRC);
    add(b, g.color_in.Get(), NPSR, DST);
    flush(l, b);
    {
        D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
        cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.pResource = color.Get(); // the subresource the scene was drawn into
        cs.SubresourceIndex = csub;
        cdst.pResource = g.color_in.Get();
        l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, nullptr);
    }
    add(b, g.color_in.Get(), DST, NPSR);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = depth.Get();
    src.SubresourceIndex = dsub; // plane 0 of the slice / mip the scene used
    dst.pResource = g.depth32.Get();
    dst.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    add(b, g.depth32.Get(), DST, NPSR);
    add(b, depth.Get(), SRC, sd);
    flush(l, b);
    // 2/3. camera motion vectors
    bool have_motion = false;
    if (g.arbiter_ok) {
        na::motion::gpu::Inputs in{};
        in.depth = g.depth32.Get();
        in.tier2_valid_flag = true;
        in.depth_clear = reversed ? 0.0f : 1.0f;
        // One composed current->previous clip matrix (double, then float); the
        // arbiter's second matrix is identity. Never chain two float matrices
        // through world space (false motion on a still camera -> DLAA blur).
        in.inv_view_proj_current = to_mat4(na::camera::ComposeReprojectionRow(cam.inv_view_proj_current_row, cam.view_proj_previous_row));
        {
            na::camera::Matrix id{};
            id[0] = id[5] = id[10] = id[15] = 1.0;
            in.view_proj_previous = to_mat4(id);
        }
        // J1: this frame's raster sample sits at pixel centre - jitter in the unjittered
        // projection; the arbiter removes it before reprojecting (Astra, 378663e finding).
        in.jitter_current_px[0] = static_cast<float>(cam.jitter_px_x);
        in.jitter_current_px[1] = static_cast<float>(cam.jitter_px_y);
        na::motion::gpu::Outputs o{};
        o.xy = g.motion.Get();
        o.provenance = g.provenance.Get();
        o.width = w;
        o.height = h;
        have_motion = g.arbiter.Dispatch(l, in, o, g.slot);
    }
    if (!have_motion) {
        l->Close();
        reason("Stopped: motion vectors could not be built.");
        g.latched = true;
        return false;
    }
    b.push_back(uav_barrier(g.motion.Get()));
    add(b, g.motion.Get(), UAV, NPSR);
    flush(l, b);
    // 4. DLSS at native resolution
    na::dlss::EvalDesc e{};
    e.color = g.color_in.Get();
    e.depth = g.depth32.Get();
    e.motion = g.motion.Get();
    e.output = g.output.Get();
    e.render_w = e.max_render_w = e.out_w = w;
    e.render_h = e.max_render_h = e.out_h = h;
    e.quality = na::dlss::Quality::DLAA;
    e.view_id = kViewId;
    e.jitter_x = static_cast<float>(cam.jitter_px_x); // DLSS: colour sampled at pixel centre - jitter
    e.jitter_y = static_cast<float>(cam.jitter_px_y);
    e.reset = cam.camera_cut || resize || g.resume_reset;
    e.hdr = is_float_color(cf);
    e.depth_inverted = reversed;
    g.stats.depth_reversed = reversed;
    e.auto_exposure = true;
    const bool evaluated = na::dlss::Evaluate(l, e);
    if (!evaluated) ++g.stats.dlss_failed;
    // 5. result -> scene colour, everything back to the game's states
    if (evaluated) {
        b.push_back(uav_barrier(g.output.Get()));
        add(b, g.output.Get(), UAV, SRC);
        addsub(b, color.Get(), csub, SRC, DST);
        flush(l, b);
        D3D12_TEXTURE_COPY_LOCATION cs{}, cdst{};
        cs.Type = cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cs.pResource = g.output.Get();
        cdst.pResource = color.Get(); // back into the scene's subresource
        cdst.SubresourceIndex = csub;
        l->CopyTextureRegion(&cdst, 0, 0, 0, &cs, nullptr);
        add(b, g.output.Get(), SRC, UAV);
        addsub(b, color.Get(), csub, DST, sc);
    } else {
        addsub(b, color.Get(), csub, SRC, sc);
    }
    add(b, g.motion.Get(), NPSR, UAV);
    flush(l, b);
    const bool closed = SUCCEEDED(l->Close());
    if (!closed) {
        // A rejected list repeats every frame and may cost the device: stop for this
        // session and say exactly what was being processed. Never retried.
        char why[160];
        std::snprintf(why, sizeof why, "Stopped: the DLAA list was rejected (colour fmt %u mips %u slices %u state 0x%X, depth fmt %u state 0x%X). %s",
                      static_cast<unsigned>(cd.Format), static_cast<unsigned>(cd.MipLevels), static_cast<unsigned>(cd.DepthOrArraySize), sc, static_cast<unsigned>(dd.Format), sd,
                      na::dlss::GetStats().reason);
        reason(why);
        g.latched = true;
        return false;
    }
    if (!evaluated) {
        char why[160];
        std::snprintf(why, sizeof why, "DLSS refused the frame: %s", na::dlss::GetStats().reason[0] ? na::dlss::GetStats().reason : "no reason given.");
        reason(why);
        return false; // nothing useful to submit; retried next frame
    }
    g.stats.width = w;
    g.stats.height = h;
    g.stats.color_format = static_cast<uint32_t>(cd.Format);
    *out_list = l;
    return true;
}

} // namespace

void SetEnabled(bool on) { g.enabled.store(on); g.configured.store(on); }
bool Enabled() { return g.enabled.load(); }
bool Configured() { return g.configured.load(); }
void SetPaused(bool paused)
{
    if (!paused && g.paused.load()) { std::lock_guard<std::mutex> lock(g.m); g.resume_reset = true; }
    g.paused.store(paused);
}
bool Paused() { return g.paused.load(); }
void SetJitter(bool on)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.jitter_on != on) g.resume_reset = true; // the input sequence changes character
    g.jitter_on = on;
    if (!on) g.jit.valid = false;
}
bool Jitter()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.jitter_on;
}

void OnPresent(ID3D12CommandQueue* queue)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (queue && g.queue.Get() == queue) {
        ++g.frame;
        g.jit.valid = false; // J1: the next camera block starts a new jitter sample
    }
}

void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward)
{
    if (!g.enabled.load(std::memory_order_relaxed) || g.paused.load(std::memory_order_relaxed) || !queue || !lists || !count) {
        forward(queue, count, lists);
        return;
    }
    const na::observe::HostWorkScope host; // our objects and our list are never observed as the game's
    std::unique_lock<std::mutex> lock(g.m);
    if (g.latched) {
        g.stats.active = false;
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    ++g.stats.submissions;
    if (g.submitted_frame == g.frame) {
        ++g.stats.duplicate_submissions;
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    uint64_t depth_id = 0, color_id = 0;
    na::observe::GetLiveSelection(&depth_id, &color_id);
    jitter_batch(lists, count); // J1, before anything in this batch is forwarded
    if (!depth_id || !color_id) {
        ++g.stats.no_selection;
        reason("Waiting for discovery to choose the scene depth and colour.");
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    int scene = -1;
    for (UINT i = 0; i < count; ++i)
        if (lists[i] && na::observe::ListRendersTo(lists[i], color_id, depth_id)) scene = static_cast<int>(i); // the last list that drew the scene (not a later one that merely transitions it)
    ID3D12GraphicsCommandList* ours = nullptr;
    PendingHistory pending;
    if (scene < 0) {
        ++g.stats.no_scene_list;
    } else if (!ensure_device(queue)) {
        // reason set
    } else {
        const char* dr = nullptr;
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetTempPathW(MAX_PATH, path);
        if (n && n < MAX_PATH - 20) {
            wcscat_s(path, L"NeuralAlias-ngx");
            CreateDirectoryW(path, nullptr);
            if (!na::dlss::EnsureOn(g.device.Get(), path)) dr = "DLSS did not start on this device (see DLSS details).";
        } else {
            dr = "No folder for DLSS data.";
        }
        if (dr) reason(dr);
        else if (record(lists, static_cast<UINT>(scene), depth_id, color_id, &ours, pending)) {
            g.stats.later_passes = na::observe::ListTargetsAfter(lists[scene], color_id, depth_id);
        }
    }
    g.stats.active = ours != nullptr;
    if (scene >= 0) g.jit.valid = false; // the scene closes this frame's jitter: the next camera block starts a new sample
    if (!ours) {
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    std::vector<ID3D12CommandList*> batch(lists, lists + count);
    batch.insert(batch.begin() + scene + 1, ours);
    const UINT n2 = static_cast<UINT>(batch.size());
    Slot& s = g.slots[g.slot];
    g.slot = (g.slot + 1) % kRing;
    na::dlss::BeforeExecute(queue, n2, batch.data());
    forward(queue, n2, batch.data());
    // The void D3D12 forward has now submitted our list. Failed recording/NGX
    // attempts above must never replace the previous submitted camera.
    g.tracker = pending.tracker;
    g.submitted_frame = g.frame;
    g.resume_reset = false;
    ++g.stats.frames;
    if (pending.cut) ++g.stats.cuts;
    if (g.stats.later_passes) {
        char why[160];
        std::snprintf(why, sizeof why, "Live DLAA is running (inserted after the scene list; it draws %u other targets after the scene).", g.stats.later_passes);
        reason(why);
    } else {
        reason("Live DLAA is running.");
    }
    na::dlss::OnExecuted(queue, n2, batch.data());
    s.fence_value = g.fence_next++;
    queue->Signal(g.fence.Get(), s.fence_value);
}

Stats GetStats()
{
    std::lock_guard<std::mutex> lock(g.m);
    return g.stats;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g.m);
    g.depth32.Reset(); g.motion.Reset(); g.provenance.Reset(); g.output.Reset(); g.color_in.Reset();
    for (auto& s : g.slots) { s.list.Reset(); s.alloc.Reset(); s.fence_value = 0; }
    g.fence.Reset(); g.queue.Reset(); g.device.Reset();
    g.arbiter = na::motion::gpu::Arbiter{};
    g.arbiter_ok = false;
    g.tracker.Reset();
    g.frame = 1;
    g.submitted_frame = 0;
    g.resume_reset = false;
    g.w = g.h = 0;
}

} // namespace na::j2
