// C13: discovery heuristics (docs/ARGON-DROPIN-HEURISTICS.md), checked against
// their claimed sources (2026-10-25):
//  - Depth: ReShade's generic_depth add-on (crosire/reshade,
//    examples/09-depth/generic_depth_addon.cpp, BSD-3), reproduced exactly in
//    its default modes: prefer_vertices ranking, similar_aspect_ratio filter
//    (|aspect delta| <= 0.1 and 0.5 <= screen/buffer ratio <= 1.85 per axis),
//    skip buffers with no draws (or <= 3 vertices and no indirect draws) or
//    first used in the previous frame, skip "frames" with one depth-stencil and
//    <= 8 draws, best clear segment by vertices.
//  - Pre-HUD boundary: NOT from Special K or Luma (both mark HUD shaders by
//    hand / per-game hashes). Our own hint: the first alpha-blended,
//    depth-disabled draw after 3D draws in a recording. Unproven; ranking only.
//  - Per-frame camera block: our own hint (UEVR uses Unreal's reflection,
//    3Dmigoto relies on manual shader hunting): the root CBV range bound while
//    the most draws are issued is the per-frame view block, since object
//    constants change every draw or two.
// Unlike SubmissionEvidence, nothing here rejects a frame or recording outright:
// unknown pipelines, dispatches, copies or truncated ops weaken a candidate's
// reasons, they never zero the ranking. Rankings are acquisition priorities and
// diagnostics, never proof; qualification stays with the classifier checks.
#pragma once
#include "capture_decoder.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace na::discovery {

struct ExecutionIdentity {
    uint64_t queue{}, ticket{}, list{}, generation{};
    uint64_t first_write{}, last_write{}; // op indices within that recording
    uint64_t scene_end{}, hud_begin{};    // recording boundaries (0 = not observed)
};

struct DepthCandidate {
    uint64_t resource{};
    uint32_t width{}, height{}, format{};
    uint64_t draws{}, vertices{}, clears{}, mid_frame_clears{}, best_segment_draws{};
    bool aspect_match{}, exact_match{}, swapchain_known{};
    double score{};
    ExecutionIdentity last;
    std::vector<std::string> reasons;
};

struct ColorCandidate {
    uint64_t resource{};
    uint32_t width{}, height{}, format{};
    uint64_t draws{}, vertices{}, hud_draws{};
    bool aspect_match{}, written_before_hud{}, receives_hud{};
    double score{};
    ExecutionIdentity last;
    std::vector<std::string> reasons;
};

struct CameraBlock {
    uint64_t resource{}, offset{};
    uint64_t draws{};      // draws issued while bound, in the frame
    uint64_t recordings{}; // recordings that bound it, in the frame
    double share{};        // draws / all draws in the frame
};

class UniversalHeuristics {
public:
    struct Metrics { uint64_t frames{}, executions{}, unmatched_lists{}, draws{}, hud_boundaries{}, unknown_pipelines{}; };

    void Consume(const DecodedBatch& b, const na_observe_status& status) {
        if (session_ != status.session || epoch_ != status.epoch) {
            *this = UniversalHeuristics{};
            session_ = status.session;
            epoch_ = status.epoch;
        }
        if (!b.valid || b.header.session != session_ || b.header.epoch != epoch_) return;
        for (const auto& item : b.events) {
            const auto* p = b.bytes.data() + item.offset;
            switch (item.header.kind) {
            case NA_EV_RESOURCE_CREATED: {
                const auto e = Load<na_ev_resource_created>(p);
                if (resources_.size() < kMaxResources || resources_.count(e.resource)) resources_[e.resource] = e;
                break;
            }
            case NA_EV_PIPELINE_CREATED: {
                const auto e = Load<na_ev_pipeline_created>(p);
                if (pipelines_.size() < kMaxPipelines || pipelines_.count(e.pipeline))
                    pipelines_[e.pipeline] = {e.depth_enable, e.depth_write, e.blend_enable_mask};
                break;
            }
            case NA_EV_SWAPCHAIN_CREATED:
            case NA_EV_SWAPCHAIN_RESIZED: {
                uint32_t w = 0, h = 0;
                if (item.header.kind == NA_EV_SWAPCHAIN_CREATED) {
                    const auto e = Load<na_ev_swapchain_created>(p);
                    w = e.width;
                    h = e.height;
                } else {
                    const auto e = Load<na_ev_swapchain_resized>(p);
                    w = e.width;
                    h = e.height;
                }
                if (w && h && w != NA_UNKNOWN_U32 && h != NA_UNKNOWN_U32) {
                    swap_w_ = w;
                    swap_h_ = h;
                }
                break;
            }
            case NA_EV_OBJECT_RETIRED: {
                const auto id = Load<na_ev_object_retired>(p).object;
                resources_.erase(id);
                pipelines_.erase(id);
                frame_.depth.erase(id);
                frame_.color.erase(id);
                last_.depth.erase(id);
                last_.color.erase(id);
                break;
            }
            case NA_EV_LIST_RESET: {
                const auto e = Load<na_ev_list_reset_close>(p);
                Recording& r = recordings_[e.list];
                r = Recording{};
                r.generation = e.recording_generation;
                Trim();
                break;
            }
            case NA_EV_LIST_OP: Op(Load<na_ev_list_op>(p), p); break;
            case NA_EV_QUEUE_EXECUTE: Execute(Load<na_ev_queue_execute>(p), p); break;
            case NA_EV_PRESENT: EndFrame(); break;
            default: break;
            }
        }
    }

    // ReShade generic_depth (default modes): candidates are depth-stencils used
    // in the scored frame that pass the filters; the best has the highest
    // prefer_vertices rank. Filtered buffers are still listed (score 0) with
    // the reason, for diagnostics.
    std::vector<DepthCandidate> RankDepth(size_t n) const {
        const Frame& f = Scored();
        std::vector<DepthCandidate> out;
        for (const auto& kv : f.depth) {
            DepthCandidate c{};
            const auto& t = kv.second;
            c.resource = kv.first;
            Describe(kv.first, c.width, c.height, c.format);
            c.draws = t.draws;
            c.vertices = t.vertices;
            c.clears = t.clears;
            c.mid_frame_clears = t.mid_clears;
            c.best_segment_draws = std::max(t.best_segment, t.segment);
            c.last = t.last;
            c.swapchain_known = swap_w_ && swap_h_;
            c.aspect_match = SimilarAspect(c.width, c.height);
            c.exact_match = c.swapchain_known && c.width == swap_w_ && c.height == swap_h_;
            bool eligible = true;
            if (t.draws == 0 || (t.vertices <= 3 && t.indirect == 0)) {
                eligible = false;
                c.reasons.emplace_back("No draws (or <= 3 vertices and no indirect draws) this frame");
            }
            const auto first = first_used_.find(kv.first);
            if (have_last_ && first != first_used_.end() && frame_index_ <= first->second + 1) {
                eligible = false;
                c.reasons.emplace_back("First used in the previous frame (generic_depth waits one frame)");
            }
            if (!c.swapchain_known) c.reasons.emplace_back("Swapchain size unknown; aspect filter not applied");
            else if (!c.aspect_match) {
                eligible = false;
                c.reasons.emplace_back("Fails generic_depth similar-aspect filter (shadow/cube/half-res buffer)");
            } else c.reasons.emplace_back(c.exact_match ? "Swapchain-sized depth-stencil" : "Passes generic_depth similar-aspect filter");
            if (c.mid_frame_clears) c.reasons.emplace_back("Cleared after geometry; best segment kept by vertices");
            if (!t.depth_known) c.reasons.emplace_back("Some draws used unobserved pipelines; depth write assumed from bound DSV");
            // prefer_vertices: vertices unless indirect draws are >= a third of draws.
            const bool by_vertices = t.indirect < t.draws / 3;
            c.score = !eligible ? 0.0 : (by_vertices ? static_cast<double>(t.vertices) : static_cast<double>(t.draws)) + 1.0;
            out.push_back(std::move(c));
        }
        std::stable_sort(out.begin(), out.end(), [](const DepthCandidate& a, const DepthCandidate& b) { return a.score > b.score; });
        if (out.size() > n) out.resize(n);
        return out;
    }

    // Scene colour (pre-HUD): render targets written by 3D draws before an
    // observed HUD boundary rank first; the target that receives the HUD is
    // the composite (it holds scene + HUD) and is ranked below them.
    std::vector<ColorCandidate> RankSceneColor(size_t n) const {
        const Frame& f = Scored();
        std::vector<ColorCandidate> out;
        for (const auto& kv : f.color) {
            ColorCandidate c{};
            const auto& t = kv.second;
            c.resource = kv.first;
            Describe(kv.first, c.width, c.height, c.format);
            c.draws = t.draws;
            c.vertices = t.vertices;
            c.hud_draws = t.hud_draws;
            c.last = t.last;
            c.aspect_match = AspectMatch(c.width, c.height);
            c.written_before_hud = t.before_hud;
            c.receives_hud = t.hud_draws != 0;
            double s = static_cast<double>(c.vertices) + 1.0;
            if (swap_w_ && !c.aspect_match) {
                s *= 0.05;
                c.reasons.emplace_back("Aspect ratio differs from swapchain");
            }
            if (c.written_before_hud) {
                s *= 4.0;
                c.reasons.emplace_back("Last 3D write precedes an observed HUD boundary");
            }
            if (c.receives_hud) {
                s *= 0.25;
                c.reasons.emplace_back("Receives HUD draws (composite, not pre-HUD scene)");
            }
            if (!c.written_before_hud && !c.receives_hud) c.reasons.emplace_back("No HUD boundary observed for this target");
            c.score = s;
            out.push_back(std::move(c));
        }
        std::stable_sort(out.begin(), out.end(), [](const ColorCandidate& a, const ColorCandidate& b) { return a.score > b.score; });
        if (out.size() > n) out.resize(n);
        return out;
    }

    // Per-frame camera block: most draws while bound (object constants change
    // per draw). Requires at least 8 draws.
    std::vector<CameraBlock> RankCameraBlocks(size_t n) const {
        const Frame& f = Scored();
        std::vector<CameraBlock> out;
        for (const auto& kv : f.cbv) {
            if (kv.second.draws < 8) continue;
            CameraBlock c{kv.first.first, kv.first.second, kv.second.draws, kv.second.recordings,
                          f.draws ? static_cast<double>(kv.second.draws) / static_cast<double>(f.draws) : 0.0};
            out.push_back(c);
        }
        std::stable_sort(out.begin(), out.end(), [](const CameraBlock& a, const CameraBlock& b) { return a.draws > b.draws; });
        if (out.size() > n) out.resize(n);
        return out;
    }
    // Rank (1-based) of a binding among the top camera blocks, 0 if not ranked.
    size_t CameraRank(uint64_t resource, uint64_t offset, size_t top = 8) const {
        const auto ranked = RankCameraBlocks(top);
        for (size_t i = 0; i < ranked.size(); ++i)
            if (ranked[i].resource == resource && ranked[i].offset == offset) return i + 1;
        return 0;
    }

    uint32_t SwapchainWidth() const { return swap_w_; }
    uint32_t SwapchainHeight() const { return swap_h_; }
    const Metrics& Stats() const { return metrics_; }

private:
    struct Pipe { uint32_t depth_enable{NA_UNKNOWN_U32}, depth_write{NA_UNKNOWN_U32}, blend{NA_UNKNOWN_U32}; };
    struct Span {
        uint64_t first{}, last{}, draws{}, indirect{}, vertices{}, clears{}, mid_clears{}, segment{}, best_segment{}, hud_draws{};
        uint64_t segment_vertices{}, best_segment_vertices{};
        bool depth{}, color{}, depth_known{true};
    };
    struct Recording {
        uint64_t generation{}, pipeline{}, last_op{};
        uint64_t scene_end{}, hud_begin{};
        std::vector<na_view_ref> targets;
        std::map<uint64_t, std::pair<uint64_t, uint64_t>> root_cbv[2]; // [graphics/compute]: param -> {resource, offset}
        std::map<uint64_t, Span> spans;
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> cbv_draws;
        uint64_t draws{};
    };
    struct DepthStat {
        uint64_t draws{}, indirect{}, vertices{}, clears{}, mid_clears{}, segment{}, best_segment{};
        bool depth_known{true};
        ExecutionIdentity last;
    };
    struct ColorStat {
        uint64_t draws{}, vertices{}, hud_draws{};
        bool before_hud{};
        ExecutionIdentity last;
    };
    struct CbvStat { uint64_t draws{}, recordings{}; };
    struct Frame {
        std::map<uint64_t, DepthStat> depth;
        std::map<uint64_t, ColorStat> color;
        std::map<std::pair<uint64_t, uint64_t>, CbvStat> cbv;
        uint64_t draws{};
    };

    static constexpr size_t kMaxResources = 32768, kMaxPipelines = 65536, kMaxRecordings = 4096, kMaxSpans = 512, kMaxCbvs = 4096;
    template <class T> static T Load(const unsigned char* p) { T e{}; std::memcpy(&e, p, sizeof(e)); return e; }

    uint64_t session_{}, epoch_{};
    uint32_t swap_w_{}, swap_h_{};
    std::map<uint64_t, na_ev_resource_created> resources_;
    std::map<uint64_t, Pipe> pipelines_;
    std::map<uint64_t, Recording> recordings_; // by list: the recording in progress or last closed
    Frame frame_, last_;
    bool have_last_{};
    uint64_t frame_index_{};
    std::map<uint64_t, uint64_t> first_used_; // depth-stencil -> frame index first used (generic_depth)
    Metrics metrics_;

    const Frame& Scored() const { return have_last_ ? last_ : frame_; }
    void Describe(uint64_t id, uint32_t& w, uint32_t& h, uint32_t& f) const {
        const auto it = resources_.find(id);
        if (it == resources_.end()) return;
        w = it->second.width > UINT32_MAX ? 0 : static_cast<uint32_t>(it->second.width);
        h = it->second.height;
        f = it->second.format;
    }
    bool AspectMatch(uint32_t w, uint32_t h) const { return SimilarAspect(w, h); }
    // generic_depth check_aspect_ratio, similar_aspect_ratio mode.
    bool SimilarAspect(uint32_t w, uint32_t h) const {
        if (!swap_w_ || !swap_h_ || !w || !h) return false;
        const double delta = static_cast<double>(swap_w_) / swap_h_ - static_cast<double>(w) / h;
        const double w_ratio = static_cast<double>(swap_w_) / w, h_ratio = static_cast<double>(swap_h_) / h;
        return std::abs(delta) <= 0.1 && w_ratio <= 1.85 && w_ratio >= 0.5 && h_ratio <= 1.85 && h_ratio >= 0.5;
    }
    void Trim() {
        while (recordings_.size() > kMaxRecordings) recordings_.erase(recordings_.begin());
    }
    Span* SpanFor(Recording& r, uint64_t resource) {
        if (!resource || resource == NA_UNKNOWN_U64) return nullptr;
        if (r.spans.size() >= kMaxSpans && !r.spans.count(resource)) return nullptr;
        return &r.spans[resource];
    }

    void Op(const na_ev_list_op& e, const unsigned char* p) {
        if (!e.list || e.list == NA_UNKNOWN_U64) return;
        Recording& r = recordings_[e.list];
        if (r.generation != e.recording_generation) {
            r = Recording{};
            r.generation = e.recording_generation;
            Trim();
        }
        r.last_op = e.op_index;
        switch (e.op) {
        case NA_OP_SET_PIPELINE: r.pipeline = e.a[0]; break;
        case NA_OP_SET_ROOT_SIGNATURE: {
            r.root_cbv[e.a[1] ? 1 : 0].clear(); // a new signature unbinds all
            break;
        }
        case NA_OP_SET_ROOT_VIEW:
            if (e.a[3] == NA_DESC_CBV && e.a[2] < 64) r.root_cbv[e.a[4] ? 1 : 0][e.a[2]] = {e.a[0], e.a[1]};
            break;
        case NA_OP_OM_SET_RENDER_TARGETS: {
            r.targets.clear();
            const uint64_t count = std::min<uint64_t>(e.a[0] + e.a[1], 9);
            for (uint64_t k = 0; k < count; ++k) {
                if (e.payload_offset + (k + 1) * sizeof(na_view_ref) > e.hdr.size_bytes || !e.payload_stride) break;
                r.targets.push_back(Load<na_view_ref>(p + e.payload_offset + k * e.payload_stride));
            }
            break;
        }
        case NA_OP_CLEAR_DSV: {
            if (e.payload_bytes < sizeof(na_view_ref)) break;
            const auto v = Load<na_view_ref>(p + e.payload_offset);
            if (!(e.a[0] & 1)) break; // stencil-only clear
            if (Span* s = SpanFor(r, v.resource)) {
                s->depth = true;
                if (!s->segment) break; // generic_depth: a clear with no prior draw work is ignored
                ++s->clears;
                ++s->mid_clears;
                if (s->segment_vertices >= s->best_segment_vertices) { // generic_depth: best by vertices
                    s->best_segment_vertices = s->segment_vertices;
                    s->best_segment = s->segment;
                }
                s->segment = s->segment_vertices = 0;
            }
            break;
        }
        case NA_OP_DRAW:
        case NA_OP_DRAW_INDEXED: {
            const uint64_t verts = e.a[0] * (e.a[1] ? e.a[1] : 1);
            ++r.draws;
            const auto ps = pipelines_.find(r.pipeline);
            const bool known = ps != pipelines_.end();
            if (!known) ++metrics_.unknown_pipelines;
            bool has_dsv = false;
            for (const auto& v : r.targets)
                if (v.descriptor_kind == NA_DESC_DSV && v.resource && v.resource != NA_UNKNOWN_U64) has_dsv = true;
            const bool depth_tested = has_dsv && (!known || ps->second.depth_enable != 0);
            // Special K / Luma: alpha-blended, depth-disabled, no depth target.
            const bool hud = known && !depth_tested && ps->second.depth_enable == 0 && ps->second.blend != 0 &&
                             ps->second.blend != NA_UNKNOWN_U32;
            if (hud) {
                if (r.scene_end && !r.hud_begin) {
                    r.hud_begin = e.op_index;
                    ++metrics_.hud_boundaries;
                }
            } else {
                r.scene_end = e.op_index;
            }
            for (const auto& v : r.targets) {
                Span* s = SpanFor(r, v.resource);
                if (!s) continue;
                if (!s->first) s->first = e.op_index;
                if (v.descriptor_kind == NA_DESC_DSV) {
                    if (known && (ps->second.depth_enable == 0 || ps->second.depth_write == 0)) continue; // tested but not written
                    s->depth = true;
                    s->depth_known = s->depth_known && known;
                    ++s->draws;
                    ++s->segment;
                    s->vertices += verts;
                    s->segment_vertices += verts;
                    s->last = e.op_index;
                } else if (v.descriptor_kind == NA_DESC_RTV) {
                    s->color = true;
                    if (hud) {
                        ++s->hud_draws;
                    } else {
                        ++s->draws;
                        s->vertices += verts;
                        s->last = e.op_index;
                    }
                }
            }
            for (const auto& bound : r.root_cbv[0]) {
                const auto& key = bound.second;
                if (!key.first || key.first == NA_UNKNOWN_U64 || key.second == NA_UNKNOWN_U64) continue;
                if (r.cbv_draws.size() < kMaxCbvs || r.cbv_draws.count(key)) ++r.cbv_draws[key];
            }
            break;
        }
        case NA_OP_EXECUTE_INDIRECT:
            // generic_depth counts indirect draws separately (vertex count unknown).
            for (const auto& v : r.targets) {
                if (v.descriptor_kind != NA_DESC_DSV) continue;
                if (Span* s = SpanFor(r, v.resource)) {
                    if (!s->first) s->first = e.op_index;
                    s->depth = true;
                    ++s->draws;
                    ++s->indirect;
                    ++s->segment;
                    s->last = e.op_index;
                }
            }
            break;
        default: break;
        }
    }

    void Execute(const na_ev_queue_execute& e, const unsigned char* p) {
        ++metrics_.executions;
        for (uint32_t i = 0; i < e.list_count; ++i) {
            const auto ref = Load<na_list_ref>(p + e.refs_offset + static_cast<size_t>(i) * e.ref_stride);
            const auto it = recordings_.find(ref.list);
            if (it == recordings_.end() || it->second.generation != ref.recording_generation) {
                ++metrics_.unmatched_lists;
                continue;
            }
            const Recording& r = it->second;
            frame_.draws += r.draws;
            metrics_.draws += r.draws;
            for (const auto& kv : r.spans) {
                const Span& s = kv.second;
                ExecutionIdentity id{e.queue, e.queue_ticket, ref.list, ref.recording_generation, s.first, s.last, r.scene_end, r.hud_begin};
                if (s.depth) {
                    DepthStat& d = frame_.depth[kv.first];
                    d.draws += s.draws;
                    d.indirect += s.indirect;
                    d.vertices += s.vertices;
                    d.clears += s.clears;
                    d.mid_clears += s.mid_clears;
                    d.best_segment = std::max({d.best_segment, s.best_segment, s.segment});
                    d.depth_known = d.depth_known && s.depth_known;
                    if (s.draws) d.last = id;
                }
                if (s.color) {
                    ColorStat& c = frame_.color[kv.first];
                    c.draws += s.draws;
                    c.vertices += s.vertices;
                    c.hud_draws += s.hud_draws;
                    if (r.hud_begin && s.last && s.last <= r.scene_end && s.last < r.hud_begin) c.before_hud = true;
                    if (s.draws) c.last = id;
                }
            }
            for (const auto& kv : r.cbv_draws) {
                if (frame_.cbv.size() >= kMaxCbvs && !frame_.cbv.count(kv.first)) continue;
                CbvStat& c = frame_.cbv[kv.first];
                c.draws += kv.second;
                ++c.recordings;
            }
        }
    }

    void EndFrame() {
        ++metrics_.frames;
        // generic_depth: a "frame" with a single depth-stencil and <= 8 draws
        // (loading screens, overlays) does not replace the last real frame.
        const bool trivial = frame_.depth.size() == 1 && frame_.depth.begin()->second.draws <= 8;
        if ((frame_.draws || !frame_.depth.empty()) && !trivial) {
            ++frame_index_;
            for (const auto& kv : frame_.depth)
                if (!first_used_.count(kv.first) && first_used_.size() < kMaxResources) first_used_[kv.first] = frame_index_;
            last_ = std::move(frame_);
            have_last_ = true;
        }
        frame_ = Frame{};
    }
};

} // namespace na::discovery
