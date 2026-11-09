// DX11 frame model: finds, from what a DX11 game does each frame, the scene
// depth, the scene colour drawn with it, the camera's constant buffer, and the
// point to run DLSS (just before the game first READS the finished scene
// colour, which is the tonemap or post pass, so before the HUD and before the
// HDR/SDR conversion).
//
// Game-agnostic. It sees only resource ids and counts, never a game's name, and
// uses the same rules as the DX12 host's discovery (universal_heuristics.hpp):
//  - depth: ReShade's generic_depth ranking (most vertices drawn with it), with
//    its aspect filter against the swap chain (|aspect delta| <= 0.1 and
//    0.5 <= screen/buffer <= 1.85 per axis), skipping depth buffers with no
//    draws or <= 3 vertices, and "frames" with a single depth and <= 8 draws
//    (loading screens, menus);
//  - colour: the render target drawn most with that depth bound, same size;
//  - camera block: the vertex-shader constant buffer bound during the most
//    scene draws, leaving out buffers re-uploaded for more than a quarter of
//    those draws (object constants change every draw or two);
//  - DLSS point: learned per frame as "the n-th read of the scene colour",
//    the first read after its last scene draw. The next frame fires right
//    before that read. A scene draw after we fired is a contradiction (the
//    point was too early) and is counted; the point is re-learned every frame.
//
// Deferred contexts (multithreaded engines such as Dunia): each command list's
// events are recorded when it is built and replayed into the model when the
// immediate context executes it. The model can fire before an immediate read,
// or before a command list whose first scene-colour read is the target and
// which draws no scene before that read. If the read sits inside a list after
// scene draws in that same list, there is no place to insert: counted as
// `unplaceable` (with a reason), never forced.
//
// Scene-in-back-buffer games (older DX9 titles; some DX11 ones) draw the scene
// straight into the back buffer and the HUD on top, so the scene colour is
// never read. For them the model learns a second point: the first draw into
// the scene colour that is NOT a scene draw (depth test off or another depth),
// after the last scene draw: "the n-th such draw". The next frame fires right
// before it (ShouldFireBeforeDraw). It is used only when no read point exists.
// With neither, a host may fire at Present (the HUD is then inside the picture).
//
// Selection changes need 3 frames in a row (hysteresis), so one odd frame (a
// loading screen, a menu) does not flip the choice.
//
// Vulkan (src/host_vk) uses the same model. Its command buffers are recorded
// before they are submitted, so it cannot fire at the read itself: each read
// carries a `tag` (where the read sits inside its command buffer), the model
// learns the tag of the first read after the last scene draw, and the Vulkan
// layer records DLSS at the read with that tag in the next frame. A tag seen
// more than once in a frame is not learned (it would not name one place).
//
// Portable C++17 (Linux-tested); the DX11 hooks and the Vulkan layer fill it.
// (2026-11-09: moved from the separate DX11 host of PR #90 into the native DX11 input
// provider of the one program, src/host_dx12/native11; see input_provider.hpp.)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace na::native11 {

using Id = uint64_t; // a resource identity (the game's ID3D11Resource pointer); 0 = none

constexpr int kCbSlots = 14; // D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT (per stage)
// Constant-buffer bindings kept per draw: vertex-shader slots 0..13, then pixel-shader slots at
// kPsCbBase + 0..13. Engines that rebuild positions from depth keep the camera in pixel constants
// (WWII's scene pose sits there; the DX12 bridge found it on the pixel stage of the scene list).
constexpr int kPsCbBase = kCbSlots;
constexpr int kCbBindSlots = 2 * kCbSlots;

struct ResourceInfo {
    uint32_t width = 0, height = 0, format = 0, samples = 1;
    bool depth = false; // bound as a depth-stencil
};

enum class Kind : uint8_t {
    Draw,       // a draw with the context's current targets
    ClearDepth, // ClearDepthStencilView(resource)
    Read,       // the resource is bound for reading (SRV) or is a copy/resolve source
    CbUpdate,   // a constant buffer's contents changed (Map/Unmap, UpdateSubresource)
};

struct Event {
    Kind kind = Kind::Draw;
    Id resource = 0;              // ClearDepth / Read / CbUpdate
    Id rt0 = 0, depth = 0;        // Draw: render target slot 0 and depth-stencil
    bool depth_test = false;      // Draw: depth test enabled in the bound state
    uint64_t vertices = 0;        // Draw: vertices (indices) x instances; 0 for indirect
    bool indirect = false;
    Id cb[kCbBindSlots] = {};     // Draw: constant buffers (VS 0..13, PS kPsCbBase + 0..13)
    uint16_t cb_first[kCbBindSlots] = {}, cb_count[kCbBindSlots] = {}; // in 16-byte constants
    uint64_t tag = 0;             // Read: the host's key for where the read sits (Vulkan); 0 = none
};

struct Selection {
    Id depth = 0, color = 0, camera_cb = 0;
    int camera_slot = -1;
    ResourceInfo depth_info, color_info;
    bool valid() const { return depth && color; }
};

// Per-list summary used to decide whether we can fire before ExecuteCommandList.
struct ListSummary {
    uint32_t color_reads = 0;            // reads of the selected colour in this list
    uint32_t scene_draws_before_first_read = 0;
    uint32_t color_writes_before_first_read = 0; // any draw into the selected colour (scene or not)
    uint32_t scene_draws = 0;
};

class FrameModel {
public:
    struct Stats {
        uint64_t frames = 0, selected_frames = 0, fired = 0, missed = 0, unplaceable = 0, contradictions = 0;
        uint64_t selection_changes = 0, loading_frames = 0;
        uint32_t last_scene_draws = 0, last_draws = 0, learned_read = 0, last_reads = 0;
        uint32_t camera_scene_draws = 0; // scene draws with the camera cb bound (last frame)
        uint64_t tag_not_unique = 0;     // frames whose point's tag named more than one read
        uint32_t learned_overlay = 0;    // the HUD-rule point (n-th non-scene draw into the scene colour); 0 = none
        double camera_share = 0;
        const char* last_miss = "";
    };

    void SetSwapChainSize(uint32_t w, uint32_t h) { swap_w_ = w; swap_h_ = h; }
    // Opt in to the HUD rule (a host that calls ShouldFireBeforeDraw). Off by default.
    void EnableOverlayRule(bool on) { overlay_rule_ = on; }
    void SetInfo(Id id, const ResourceInfo& info) { info_[id] = info; }
    void Forget(Id id)
    {
        info_.erase(id);
        if (sel_.depth == id || sel_.color == id) { sel_ = Selection{}; learned_read_ = 0; learned_tag_ = 0; learned_overlay_ = 0; }
        if (sel_.camera_cb == id) { sel_.camera_cb = 0; sel_.camera_slot = -1; }
    }
    const Selection& Current() const { return sel_; }
    const Stats& GetStats() const { return stats_; }
    uint32_t SceneDrawsThisFrame() const { return scene_draws_; }
    bool FiredThisFrame() const { return fired_; }
    // The tag of the read learned as the DLSS point (Vulkan); 0 = none or not unique.
    uint64_t LearnedTag() const { return learned_tag_; }

    // ---- online decisions (before the event/list is forwarded to the game's context) ----
    // An immediate-context read of `resource` is about to happen.
    bool ShouldFireBeforeRead(Id resource) const
    {
        if (fired_ || !sel_.valid() || resource != sel_.color || !learned_read_ || !scene_draws_) return false;
        return color_reads_ + 1 == learned_read_;
    }
    // The immediate context is about to execute a command list with this summary.
    bool ShouldFireBeforeList(const ListSummary& l)
    {
        if (fired_ || !sel_.valid() || !learned_read_ || !l.color_reads) return false;
        if (color_reads_ >= learned_read_ || color_reads_ + l.color_reads < learned_read_) return false;
        if (color_reads_ + 1 != learned_read_ || l.scene_draws_before_first_read || l.color_writes_before_first_read) {
            // The target read is inside this list, after scene work or other writes to the scene
            // colour in the same list: inputs copied before the list would be stale (Astra's rule).
            ++stats_.unplaceable;
            unplaceable_this_frame_ = true;
            stats_.last_miss = "The scene colour is read inside a command list that also draws the scene; no place to insert.";
            return false;
        }
        return scene_draws_ > 0;
    }
    // A draw (Event e) is about to happen: the HUD-rule point (scene drawn into the
    // back buffer, no read of it). Only when no read point was learned.
    bool ShouldFireBeforeDraw(const Event& e) const
    {
        if (fired_ || !sel_.valid() || learned_read_ || !learned_overlay_ || !scene_draws_) return false;
        if (e.kind != Kind::Draw || e.rt0 != sel_.color || IsSceneDraw(e)) return false;
        return overlay_draws_ + 1 == learned_overlay_;
    }
    // Which point the next frame will fire at: 1 = before a read, 2 = before the
    // first HUD draw (HUD rule), 0 = none learned.
    int LearnedPointKind() const { return learned_read_ ? 1 : learned_overlay_ ? 2 : 0; }
    void MarkFired() { fired_ = true; ++stats_.fired; fired_at_scene_draws_ = scene_draws_; }

    // Summary of a recorded list against the CURRENT selection (for ShouldFireBeforeList).
    ListSummary Summarize(const std::vector<Event>& events) const
    {
        ListSummary s;
        for (const Event& e : events) {
            if (e.kind == Kind::Read && sel_.color && e.resource == sel_.color) ++s.color_reads;
            if (e.kind == Kind::Draw && sel_.color && e.rt0 == sel_.color && !s.color_reads) ++s.color_writes_before_first_read;
            if (e.kind == Kind::Draw && IsSceneDraw(e)) {
                ++s.scene_draws;
                if (!s.color_reads) ++s.scene_draws_before_first_read;
            }
        }
        return s;
    }

    // ---- feeding (in GPU order: immediate events live, list events at execute) ----
    // Returns, for CbUpdate, the buffer's new version (the engine files its bytes under it).
    uint32_t Feed(const Event& e)
    {
        ++events_;
        switch (e.kind) {
        case Kind::Draw: Draw(e); break;
        case Kind::ClearDepth: {
            Cand& d = depth_[e.resource];
            ++d.clears;
            break;
        }
        case Kind::Read:
            if (sel_.color && e.resource == sel_.color) {
                ++color_reads_;
                if (e.tag) ++read_tags_[e.tag];
                if (last_scene_draw_event_ && !first_read_after_scene_pending_) {
                    first_read_after_scene_pending_ = color_reads_;
                    pending_tag_ = e.tag;
                }
            }
            break;
        case Kind::CbUpdate: return ++cb_version_[e.resource];
        }
        return 0;
    }
    uint32_t CbVersion(Id cb) const
    {
        const auto it = cb_version_.find(cb);
        return it == cb_version_.end() ? 0 : it->second;
    }
    // The camera buffer version used by most scene draws so far this frame (0 = none yet).
    uint32_t CameraVersionThisFrame() const
    {
        uint32_t best = 0, best_n = 0;
        for (const auto& kv : camera_versions_)
            if (kv.second > best_n) { best_n = kv.second; best = kv.first; }
        return best;
    }
    // A previous candidate is useful only if this scene actually bound it.
    uint32_t CameraCandidateVersion(Id cb, int slot, uint32_t& first, uint32_t& count) const
    {
        const auto it = cb_scene_.find({cb, slot});
        if (it == cb_scene_.end()) return 0;
        first = it->second.first;
        count = it->second.count;
        return it->second.last_version;
    }
    // Camera-buffer candidates of the last frame, most scene draws first (for camera search).
    std::vector<std::pair<Id, int>> CameraCandidates(size_t max) const
    {
        std::vector<std::pair<Id, int>> r;
        for (const auto& c : last_cb_order_) {
            if (r.size() >= max) break;
            r.push_back(c);
        }
        return r;
    }
    // The engine found the camera's matrices in this buffer (or, with 0, gave up on it).
    void SetCamera(Id cb, int slot) { sel_.camera_cb = cb; sel_.camera_slot = slot; }

    void EndFrame()
    {
        ++stats_.frames;
        if (fired_ && scene_draws_ > fired_at_scene_draws_) {
            ++stats_.contradictions;
            stats_.last_miss = "The scene was still being drawn after DLSS ran (the point was too early); re-learning.";
        }
        if (!fired_ && sel_.valid() && (learned_read_ || learned_overlay_)) {
            ++stats_.missed;
            if (!unplaceable_this_frame_)
                stats_.last_miss = color_reads_ < learned_read_ ? "The scene colour was read fewer times than last frame; no point found."
                                                                : "The point was not reached.";
        }
        stats_.last_scene_draws = scene_draws_;
        stats_.last_draws = draws_;
        stats_.last_reads = color_reads_;
        if (sel_.valid()) ++stats_.selected_frames;

        // Learn the DLSS point for the next frame: the first read after the last scene draw.
        learned_read_ = sel_.valid() && first_read_after_scene_pending_ ? first_read_after_scene_pending_ : 0;
        stats_.learned_read = learned_read_;
        // HUD rule: only when the scene colour had no read after its last scene draw.
        learned_overlay_ = overlay_rule_ && sel_.valid() && !learned_read_ && first_overlay_after_scene_pending_ ? first_overlay_after_scene_pending_ : 0;
        stats_.learned_overlay = learned_overlay_;
        learned_tag_ = 0;
        if (learned_read_ && pending_tag_) {
            const auto it = read_tags_.find(pending_tag_);
            if (it != read_tags_.end() && it->second == 1) learned_tag_ = pending_tag_;
            else {
                ++stats_.tag_not_unique;
                stats_.last_miss = "The scene colour's first read after the scene has the same position key as another read; no single place to insert.";
            }
        }

        Choose();

        // Camera ranking for the next frame.
        last_cb_order_.clear();
        std::vector<std::pair<std::pair<Id, int>, CbUse>> cbs(cb_scene_.begin(), cb_scene_.end());
        std::sort(cbs.begin(), cbs.end(), [](const auto& a, const auto& b) {
            return a.second.draws != b.second.draws ? a.second.draws > b.second.draws : a.first.first < b.first.first;
        });
        // Steady blocks first. Blocks re-uploaded between most scene draws (per-object constants, or a
        // pass block the engine rewrites per shader variant with the camera repeated in it) come after
        // them, so they are still searched when the steady ones hold no camera. One entry per buffer
        // (a buffer bound on both stages is searched once, at its most-used slot).
        std::vector<std::pair<Id, int>> reuploaded;
        std::unordered_set<Id> seen;
        for (const auto& c : cbs) {
            if (!seen.insert(c.first.first).second) continue;
            const uint32_t allowed = std::max<uint32_t>(4, c.second.draws / 4);
            if (c.second.versions > allowed) reuploaded.push_back(c.first);
            else last_cb_order_.push_back(c.first);
        }
        last_cb_order_.insert(last_cb_order_.end(), reuploaded.begin(), reuploaded.end());
        stats_.camera_scene_draws = 0;
        for (const auto& c : cbs)
            if (sel_.camera_cb && c.first.first == sel_.camera_cb) stats_.camera_scene_draws += c.second.draws;
        stats_.camera_share = scene_draws_ ? double(stats_.camera_scene_draws) / scene_draws_ : 0;

        // Reset per-frame counters.
        depth_.clear();
        color_.clear();
        cb_scene_.clear();
        camera_versions_.clear();
        draws_ = scene_draws_ = color_reads_ = 0;
        events_ = last_scene_draw_event_ = 0;
        first_read_after_scene_pending_ = 0;
        pending_tag_ = 0;
        read_tags_.clear();
        overlay_draws_ = 0;
        first_overlay_after_scene_pending_ = 0;
        unplaceable_this_frame_ = false;
        fired_ = false;
        fired_at_scene_draws_ = 0;
    }

    bool IsSceneDraw(const Event& e) const { return sel_.valid() && e.rt0 == sel_.color && e.depth == sel_.depth; }

private:
    struct Cand { uint64_t draws = 0, vertices = 0, clears = 0; bool indirect = false; };
    struct PairCount { uint64_t draws = 0; };

    void Draw(const Event& e)
    {
        ++draws_;
        if (e.depth) {
            Cand& d = depth_[e.depth];
            ++d.draws;
            d.vertices += e.vertices;
            d.indirect = d.indirect || e.indirect;
            if (e.rt0) ++color_[{e.depth, e.rt0}].draws;
        }
        if (IsSceneDraw(e)) {
            ++scene_draws_;
            last_scene_draw_event_ = events_;
            first_read_after_scene_pending_ = 0; // a read before this draw was mid-scene
            pending_tag_ = 0;
            first_overlay_after_scene_pending_ = 0; // so was a non-scene draw into the colour
            CountCbs(e);
            if (sel_.camera_cb && sel_.camera_slot >= 0 && e.cb[sel_.camera_slot] == sel_.camera_cb)
                ++camera_versions_[CbVersion(sel_.camera_cb)];
        } else if (sel_.valid() && e.rt0 == sel_.color) {
            ++overlay_draws_;
            if (last_scene_draw_event_ && !first_overlay_after_scene_pending_) first_overlay_after_scene_pending_ = overlay_draws_;
        } else if (!sel_.valid() && e.depth && e.rt0) {
            // Before a selection exists, rank camera candidates by draws with any depth.
            CountCbs(e);
        }
    }

    void CountCbs(const Event& e)
    {
        for (int i = 0; i < kCbBindSlots; ++i) {
            if (!e.cb[i]) continue;
            CbUse& u = cb_scene_[{e.cb[i], i}];
            ++u.draws;
            u.first = e.cb_first[i];
            u.count = e.cb_count[i];
            const uint32_t v = CbVersion(e.cb[i]);
            if (!u.versions || v != u.last_version) { ++u.versions; u.last_version = v; }
        }
    }

    bool AspectOk(const ResourceInfo& r) const
    {
        if (!swap_w_ || !swap_h_ || !r.width || !r.height) return true; // unknown screen: no filter
        const double a = double(r.width) / r.height, s = double(swap_w_) / swap_h_;
        const double rx = double(swap_w_) / r.width, ry = double(swap_h_) / r.height;
        return std::fabs(a - s) <= 0.1 && rx >= 0.5 && rx <= 1.85 && ry >= 0.5 && ry <= 1.85;
    }

    void Choose()
    {
        // Loading screens and menus: one depth with <= 8 draws -> keep the old choice.
        uint64_t total_depth_draws = 0;
        for (const auto& kv : depth_) total_depth_draws += kv.second.draws;
        if (depth_.size() <= 1 && total_depth_draws <= 8) {
            ++stats_.loading_frames;
            return;
        }
        Id best_depth = 0;
        uint64_t best_v = 0;
        for (const auto& kv : depth_) {
            const Cand& c = kv.second;
            if (!c.draws || (c.vertices <= 3 && !c.indirect)) continue;
            const auto it = info_.find(kv.first);
            if (it == info_.end() || !AspectOk(it->second)) continue;
            const uint64_t v = c.vertices ? c.vertices : c.draws; // indirect-only: rank by draws
            if (v > best_v) { best_v = v; best_depth = kv.first; }
        }
        Id best_color = 0;
        uint64_t best_cd = 0;
        if (best_depth) {
            const ResourceInfo& di = info_[best_depth];
            for (const auto& kv : color_) {
                if (kv.first.first != best_depth) continue;
                const auto it = info_.find(kv.first.second);
                if (it == info_.end() || it->second.width != di.width || it->second.height != di.height) continue;
                if (kv.second.draws > best_cd) { best_cd = kv.second.draws; best_color = kv.first.second; }
            }
        }
        if (!best_color) return; // nothing usable this frame: keep the old choice
        if (best_depth == sel_.depth && best_color == sel_.color) { pending_frames_ = 0; return; }
        if (best_depth == pending_.first && best_color == pending_.second) ++pending_frames_;
        else { pending_ = {best_depth, best_color}; pending_frames_ = 1; }
        if (pending_frames_ >= 3 || !sel_.valid()) {
            if (sel_.valid()) ++stats_.selection_changes;
            sel_.depth = best_depth;
            sel_.color = best_color;
            sel_.depth_info = info_[best_depth];
            sel_.color_info = info_[best_color];
            sel_.camera_cb = 0;
            sel_.camera_slot = -1;
            learned_read_ = 0; // learn the point with the new selection first
            learned_tag_ = 0;
            learned_overlay_ = 0;
            pending_frames_ = 0;
        }
    }

    struct PairHash {
        size_t operator()(const std::pair<Id, Id>& p) const { return std::hash<Id>()(p.first) ^ static_cast<size_t>(std::hash<Id>()(p.second) * 1099511628211ull); }
    };
    struct CbHash {
        size_t operator()(const std::pair<Id, int>& p) const { return std::hash<Id>()(p.first) ^ (size_t(p.second) * 2654435761u); }
    };

    uint32_t swap_w_ = 0, swap_h_ = 0;
    std::unordered_map<Id, ResourceInfo> info_;
    std::unordered_map<Id, Cand> depth_;
    std::unordered_map<std::pair<Id, Id>, PairCount, PairHash> color_;
    struct CbUse { uint32_t draws = 0, versions = 0, last_version = 0, first = 0, count = 0; };
    std::unordered_map<std::pair<Id, int>, CbUse, CbHash> cb_scene_;
    std::unordered_map<Id, uint32_t> cb_version_;
    std::map<uint32_t, uint32_t> camera_versions_;
    std::vector<std::pair<Id, int>> last_cb_order_;
    Selection sel_;
    std::pair<Id, Id> pending_{0, 0};
    uint32_t pending_frames_ = 0;
    uint32_t draws_ = 0, scene_draws_ = 0, color_reads_ = 0, learned_read_ = 0;
    uint64_t events_ = 0, last_scene_draw_event_ = 0;
    bool unplaceable_this_frame_ = false;
    uint32_t first_read_after_scene_pending_ = 0;
    uint64_t pending_tag_ = 0, learned_tag_ = 0;
    bool overlay_rule_ = false;
    uint32_t overlay_draws_ = 0, first_overlay_after_scene_pending_ = 0, learned_overlay_ = 0;
    std::unordered_map<uint64_t, uint32_t> read_tags_;
    bool fired_ = false;
    uint32_t fired_at_scene_draws_ = 0;
    Stats stats_;
};

} // namespace na::native11
