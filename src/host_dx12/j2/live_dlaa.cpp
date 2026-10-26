#include "host_dx12/j2/live_dlaa.hpp"

#include "core/motion_arbiter_gpu.hpp"
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
constexpr size_t kMaxBlocks = 384;       // unique constant blocks examined per frame
constexpr uint64_t kViewId = 0x4A32;     // DLSS view for this source ("J2")

struct Slot {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT64 fence_value = 0;
};

struct State {
    std::mutex m;
    std::atomic<bool> enabled{false};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_next = 1;
    Slot slots[kRing];
    uint32_t slot = 0;
    // Our textures (render size == output size: DLAA).
    ComPtr<ID3D12Resource> depth32, motion, provenance, output;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
    na::motion::gpu::Arbiter arbiter;
    bool arbiter_ok = false;
    na::camera::CameraTracker tracker;
    uint64_t frame = 0;
    int64_t camera_offset = -1; // learned offset of the ViewProjection inside its block
    Stats stats;
} g;

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
    return false;
}

// This frame's camera: the most-bound constant block in the scene list that
// holds a perspective ViewProjection together with its inverse.
bool find_camera(ID3D12CommandList* scene, na::camera::Matrix* vp, bool* reversed)
{
    const auto binds = na::observe::ListRootConstantBuffers(scene);
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> count;
    for (const auto& b : binds) ++count[{b.resource, b.offset}];
    std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> order;
    order.reserve(count.size());
    for (const auto& kv : count) order.push_back({kv.second, kv.first});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (order.size() > kMaxBlocks) order.resize(kMaxBlocks);
    std::vector<uint8_t> bytes(kBlockBytes);
    for (const auto& o : order) {
        const uint32_t n = na::observe::ReadBuffer(o.second.first, o.second.second, bytes.data(), kBlockBytes);
        if (n < 128) continue;
        na::camera::Snapshot s{};
        s.complete = s.lifetime_valid = true;
        s.sync = na::camera::Sync::AppSyncUnknown;
        s.bytes.assign(bytes.begin(), bytes.begin() + n);
        if (g.camera_offset >= 0 && static_cast<uint64_t>(g.camera_offset) + 128 <= n) {
            // Learned layout: check just that offset (both memory layouts).
            for (auto layout : {na::camera::Layout::RowMajor, na::camera::Layout::ColumnMajor}) {
                const auto m = na::camera::LoadMatrix(s.bytes, static_cast<size_t>(g.camera_offset), layout);
                na::camera::Candidate c{};
                if (!na::camera::ViewProjection(m, c)) continue;
                bool pair = false;
                for (int64_t d : {64, -64}) {
                    const int64_t off = g.camera_offset + d;
                    if (off < 0 || static_cast<uint64_t>(off) + 64 > n) continue;
                    pair = pair || na::camera::IsInversePair(m, na::camera::LoadMatrix(s.bytes, static_cast<size_t>(off), layout));
                }
                if (pair) { *vp = m; *reversed = c.reversed; return true; }
            }
            continue;
        }
        const auto scan = na::camera::FindMatrices(s);
        for (const auto& c : scan.candidates) {
            if (c.kind != na::camera::Kind::ViewProjection || !c.has_inverse_pair) continue;
            g.camera_offset = static_cast<int64_t>(c.byte_offset);
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
bool record(ID3D12CommandList* const* lists, UINT scene, na_object_id depth_id, na_object_id color_id,
            ID3D12GraphicsCommandList** out_list)
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
    const uint32_t w = static_cast<uint32_t>(cd.Width), h = cd.Height;
    if (dd.Width != cd.Width || dd.Height != cd.Height || !na::depth::PlaneZeroCopiesToR32F(dd.Format)) {
        reason("Scene depth does not match the colour size or is not 32-bit depth.");
        return false;
    }
    na::camera::Matrix vp{};
    bool reversed = false;
    if (!find_camera(lists[scene], &vp, &reversed)) {
        ++g.stats.no_camera;
        g.camera_offset = -1; // relearn
        reason("Waiting: no camera matrix with its inverse in the scene pass.");
        return false;
    }
    const double zero[2] = {0, 0}; // no jitter injected yet (J1)
    const na::camera::CameraFrame cam = g.tracker.Update(vp, ++g.frame, w, h, zero);
    if (!cam.valid) { ++g.stats.no_camera; reason("The camera matrix could not be inverted."); return false; }
    if (cam.camera_cut) ++g.stats.cuts;

    // Ring slot (never wait) and size/format changes (only once the GPU is done with the old set).
    const UINT64 done = g.fence->GetCompletedValue();
    Slot& s = g.slots[g.slot];
    if (done == UINT64_MAX) { reason("Device removed."); return false; }
    if (s.fence_value && done < s.fence_value) { ++g.stats.busy; reason("Skipped: the previous use of this frame slot is still running."); return false; }
    const bool resize = !g.output || g.w != w || g.h != h || g.color_format != cd.Format;
    if (resize) {
        if (done + 1 < g.fence_next) { ++g.stats.busy; reason("Waiting for the previous picture size to finish."); return false; }
        if (!uav_store(cd.Format) || !uav_store(DXGI_FORMAT_R32G32_FLOAT)) { reason("This scene colour format cannot be written by DLSS here."); return false; }
        if (!texture(w, h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, g.depth32) ||
            !texture(w, h, DXGI_FORMAT_R32G32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.motion) ||
            !texture(w, h, DXGI_FORMAT_R8_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.provenance) ||
            !texture(w, h, cd.Format, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g.output)) {
            g.output.Reset();
            reason("Could not allocate the DLAA working textures.");
            return false;
        }
        g.w = w; g.h = h; g.color_format = cd.Format;
        g.tracker.Reset();
    }
    if (FAILED(s.alloc->Reset()) || FAILED(s.list->Reset(s.alloc.Get(), nullptr))) { reason("Could not reset the DLAA command list."); return false; }
    ID3D12GraphicsCommandList* l = s.list.Get();
    std::vector<D3D12_RESOURCE_BARRIER> b;
    constexpr UINT NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, SRC = D3D12_RESOURCE_STATE_COPY_SOURCE,
                   DST = D3D12_RESOURCE_STATE_COPY_DEST, UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const UINT color_read = (sc & NPSR) ? sc : NPSR; // already readable by compute: keep the game's state
    // 1. depth plane 0 -> R32F
    add(b, depth.Get(), sd, SRC);
    add(b, g.depth32.Get(), NPSR, DST);
    add(b, color.Get(), sc, color_read);
    flush(l, b);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = depth.Get();
    src.SubresourceIndex = 0;
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
        in.inv_view_proj_current = to_mat4(cam.inv_view_proj_current_row);
        in.view_proj_previous = to_mat4(cam.view_proj_previous_row);
        na::motion::gpu::Outputs o{};
        o.xy = g.motion.Get();
        o.provenance = g.provenance.Get();
        o.width = w;
        o.height = h;
        have_motion = g.arbiter.Dispatch(l, in, o, g.slot);
    }
    if (!have_motion) {
        l->Close();
        reason("Motion vectors could not be built.");
        return false;
    }
    b.push_back(uav_barrier(g.motion.Get()));
    add(b, g.motion.Get(), UAV, NPSR);
    flush(l, b);
    // 4. DLSS at native resolution
    na::dlss::EvalDesc e{};
    e.color = color.Get();
    e.depth = g.depth32.Get();
    e.motion = g.motion.Get();
    e.output = g.output.Get();
    e.render_w = e.max_render_w = e.out_w = w;
    e.render_h = e.max_render_h = e.out_h = h;
    e.quality = na::dlss::Quality::DLAA;
    e.view_id = kViewId;
    e.reset = cam.camera_cut || resize;
    e.hdr = is_float_color(cd.Format);
    e.depth_inverted = reversed;
    e.auto_exposure = true;
    const bool evaluated = na::dlss::Evaluate(l, e);
    if (!evaluated) ++g.stats.dlss_failed;
    // 5. result -> scene colour, everything back to the game's states
    if (evaluated) {
        b.push_back(uav_barrier(g.output.Get()));
        add(b, g.output.Get(), UAV, SRC);
        add(b, color.Get(), color_read, DST);
        flush(l, b);
        l->CopyResource(color.Get(), g.output.Get());
        add(b, g.output.Get(), SRC, UAV);
        add(b, color.Get(), DST, sc);
    } else {
        add(b, color.Get(), color_read, sc);
    }
    add(b, g.motion.Get(), NPSR, UAV);
    flush(l, b);
    if (FAILED(l->Close())) { reason("Could not close the DLAA command list."); return false; }
    if (!evaluated) {
        reason("DLSS refused the frame (see DLSS details).");
        return false; // nothing useful to submit
    }
    g.stats.width = w;
    g.stats.height = h;
    g.stats.color_format = static_cast<uint32_t>(cd.Format);
    *out_list = l;
    return true;
}

} // namespace

void SetEnabled(bool on) { g.enabled.store(on); }
bool Enabled() { return g.enabled.load(); }

void Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists, ExecuteFn forward)
{
    if (!g.enabled.load(std::memory_order_relaxed) || !queue || !lists || !count) {
        forward(queue, count, lists);
        return;
    }
    std::unique_lock<std::mutex> lock(g.m);
    ++g.stats.submissions;
    uint64_t depth_id = 0, color_id = 0;
    na::observe::GetLiveSelection(&depth_id, &color_id);
    if (!depth_id || !color_id) {
        ++g.stats.no_selection;
        reason("Waiting for discovery to choose the scene depth and colour.");
        lock.unlock();
        forward(queue, count, lists);
        return;
    }
    int scene = -1;
    for (UINT i = 0; i < count; ++i)
        if (lists[i] && na::observe::ListReferences(lists[i], color_id) && na::observe::ListReferences(lists[i], depth_id)) scene = static_cast<int>(i);
    ID3D12GraphicsCommandList* ours = nullptr;
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
        else if (record(lists, static_cast<UINT>(scene), depth_id, color_id, &ours)) {
            ++g.stats.frames;
            reason("Live DLAA is running.");
        }
    }
    g.stats.active = ours != nullptr;
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
    g.depth32.Reset(); g.motion.Reset(); g.provenance.Reset(); g.output.Reset();
    for (auto& s : g.slots) { s.list.Reset(); s.alloc.Reset(); s.fence_value = 0; }
    g.fence.Reset(); g.queue.Reset(); g.device.Reset();
    g.arbiter_ok = false;
    g.w = g.h = 0;
}

} // namespace na::j2
