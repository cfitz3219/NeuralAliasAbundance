// P4-4 Stage M: a read-only map of ONE frame's GPU passes, from the observation stream the
// diagnostic adapter already receives (no new hooks, no producers changes).
//
// For the slider pilot (Sniper with its render scale below 100 %) it answers: which passes draw
// at the reduced size, which pass stretches the reduced picture to full size (the game's own
// upscale, which DLSS will replace in Stage P), what that pass reads and writes, and what runs
// after it (tonemap, HUD). The same map gives Astra the native Anno operation/dependency map.
//
// A "pass" is a run of draws (or dispatches) with the same targets and viewport in one
// recording, or one copy / resolve / clear. Reads and writes come from the resources bound at
// each draw: render/depth targets, root SRV/UAV views, and descriptors at each bound table's
// start (the first kTableSpan slots: table range sizes are not in the stream, so table reads
// are an approximation and say so). Bounded everywhere; overflow is reported, never hidden.
// Pure C++17 on host_observe.h types (CPU-tested).
#pragma once
#include "host_dx12/host_observe.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace na::discovery {

class PassMap {
public:
    static constexpr uint32_t kTableSpan = 16;       // descriptors read from each bound table's start
    static constexpr size_t kMaxDescriptors = 1u << 20;
    static constexpr size_t kMaxRecordings = 1024, kMaxPassesPerRecording = 2048, kMaxIds = 24;

    enum class Kind { Raster, Compute, Copy, Resolve, Clear };
    struct Pass {
        Kind kind = Kind::Raster;
        uint32_t ops = 0;                         // draws / dispatches merged into this pass
        uint32_t vp_w = 0, vp_h = 0;              // first viewport (raster)
        uint64_t pipeline = 0;                    // first pipeline
        std::vector<uint64_t> targets;            // RTVs then DSV (raster) / copy destination
        std::vector<uint64_t> reads, writes;      // bounded, in first-seen order
        bool approx_reads = false, truncated = false;
    };

    // Records the frame that starts `skip_presents` Presents after this call.
    void Arm(uint32_t skip_presents)
    {
        armed_ = true;
        skip_ = skip_presents;
        capturing_ = done_ = false;
        frame_.clear();
        recordings_.clear();
    }
    bool Armed() const { return armed_; }
    bool Done() const { return done_; }

    void OnResource(const na_ev_resource_created& e)
    {
        if (res_.size() < 65536 || res_.count(e.resource)) res_[e.resource] = {static_cast<uint32_t>(e.width), e.height, e.format};
    }
    void OnRetire(uint64_t id) { res_.erase(id); }
    void OnSwapchain(uint32_t w, uint32_t h) { out_w_ = w; out_h_ = h; }
    void OnDescriptor(uint64_t heap, uint32_t slot, uint32_t kind, uint64_t resource)
    {
        const uint64_t k = key(heap, slot);
        auto it = desc_.find(k);
        if (it != desc_.end()) it->second = {kind, resource};
        else if (desc_.size() < kMaxDescriptors) desc_.emplace(k, Desc{kind, resource});
        else desc_overflow_ = true;
    }

    // CopyDescriptors: games commonly stage descriptors in CPU heaps and copy them into the
    // shader-visible heap. Copies the known contents (bounded range).
    void OnDescriptorCopy(uint64_t src_heap, uint32_t src_slot, uint64_t dst_heap, uint32_t dst_slot, uint32_t count)
    {
        for (uint32_t i = 0; i < count && i < 65536; ++i) {
            auto it = desc_.find(key(src_heap, src_slot + i));
            if (it != desc_.end()) OnDescriptor(dst_heap, dst_slot + i, it->second.kind, it->second.resource);
            else desc_.erase(key(dst_heap, dst_slot + i)); // unknown source: the destination is unknown too
        }
    }

    // `ev` is the whole event (payload_offset is from its start).
    void OnOp(const na_ev_list_op& e, const uint8_t* ev)
    {
        if (!armed_ || done_) return;
        Rec& r = rec(e.list, e.recording_generation);
        if (e.hdr.flags & (NA_EVF_METHOD_UNOBSERVED | NA_EVF_TRUNCATED)) r.incomplete = true;
        const auto view = [&](uint64_t k) {
            na_view_ref v{};
            std::memcpy(&v, ev + e.payload_offset + k * (e.payload_stride ? e.payload_stride : sizeof v), sizeof v);
            return v;
        };
        switch (e.op) {
        case NA_OP_SET_PIPELINE: r.pipeline = e.a[0]; break;
        case NA_OP_SET_ROOT_SIGNATURE: (e.a[1] ? r.ctab : r.gtab).clear(); (e.a[1] ? r.cview : r.gview).clear(); break;
        case NA_OP_SET_ROOT_TABLE: (e.a[4] ? r.ctab : r.gtab)[static_cast<uint32_t>(e.a[3])] = {e.a[0], static_cast<uint32_t>(e.a[1])}; break;
        case NA_OP_SET_ROOT_VIEW:
            (e.a[4] ? r.cview : r.gview)[static_cast<uint32_t>(e.a[2])] = {static_cast<uint32_t>(e.a[3]), e.a[0]};
            break;
        case NA_OP_OM_SET_RENDER_TARGETS: {
            r.targets.clear();
            if (e.payload_bytes && !(e.hdr.flags & NA_EVF_TRUNCATED))
                for (uint64_t k = 0; k < e.a[0] + e.a[1] && k < 9; ++k) r.targets.push_back(view(k).resource);
            break;
        }
        case NA_OP_RS_VIEWPORT: {
            float wh[2];
            std::memcpy(wh, &e.a[2], sizeof wh);
            r.vp_w = static_cast<uint32_t>(wh[0]);
            r.vp_h = static_cast<uint32_t>(wh[1]);
            break;
        }
        case NA_OP_DRAW: case NA_OP_DRAW_INDEXED: case NA_OP_DISPATCH: case NA_OP_EXECUTE_INDIRECT: {
            const bool compute = e.op == NA_OP_DISPATCH ||
                                 (e.op == NA_OP_EXECUTE_INDIRECT && r.targets.empty()); // indirect: guess by targets
            Pass* p = r.passes.empty() ? nullptr : &r.passes.back();
            const Kind k = compute ? Kind::Compute : Kind::Raster;
            const bool same = p && p->kind == k && (compute ? p->pipeline == r.pipeline
                                                            : p->targets == r.targets && p->vp_w == r.vp_w && p->vp_h == r.vp_h);
            if (!same) {
                if (!push(r)) return;
                p = &r.passes.back();
                p->kind = k;
                p->pipeline = r.pipeline;
                if (!compute) { p->targets = r.targets; p->vp_w = r.vp_w; p->vp_h = r.vp_h; }
            }
            ++p->ops;
            if (!compute) for (uint64_t t : r.targets) add(p->writes, t, p);
            for (const auto& [param, v] : compute ? r.cview : r.gview) {
                (void)param;
                if (v.kind == NA_DESC_SRV) add(p->reads, v.resource, p);
                else if (v.kind == NA_DESC_UAV) add(p->writes, v.resource, p);
            }
            for (const auto& [param, t] : compute ? r.ctab : r.gtab) {
                (void)param;
                for (uint32_t s = 0; s < kTableSpan; ++s) {
                    auto it = desc_.find(key(t.heap, t.slot + s));
                    if (it == desc_.end() || !it->second.resource || it->second.resource == NA_UNKNOWN_U64) continue;
                    if (it->second.kind == NA_DESC_SRV) { add(p->reads, it->second.resource, p); p->approx_reads = true; }
                    else if (it->second.kind == NA_DESC_UAV) add(p->writes, it->second.resource, p);
                }
            }
            break;
        }
        case NA_OP_COPY_RESOURCE: case NA_OP_COPY_TEXTURE_REGION: case NA_OP_RESOLVE: {
            if (!push(r)) return;
            Pass& p = r.passes.back();
            p.kind = e.op == NA_OP_RESOLVE ? Kind::Resolve : Kind::Copy;
            p.ops = 1;
            const uint64_t dst = e.a[0], src = e.op == NA_OP_COPY_RESOURCE ? e.a[1] : e.a[2];
            p.targets = {dst};
            add(p.writes, dst, &p);
            add(p.reads, src, &p);
            break;
        }
        case NA_OP_CLEAR_RTV: case NA_OP_CLEAR_DSV: {
            if (!e.payload_bytes || !push(r)) return;
            Pass& p = r.passes.back();
            p.kind = Kind::Clear;
            p.ops = 1;
            const uint64_t t = view(0).resource;
            p.targets = {t};
            add(p.writes, t, &p);
            break;
        }
        default: break;
        }
    }

    void OnExecute(const na_list_ref* refs, uint32_t n)
    {
        if (!capturing_ || done_) return;
        for (uint32_t i = 0; i < n; ++i) {
            auto it = recordings_.find(refs[i].list);
            ++executed_lists_;
            if (it == recordings_.end() || it->second.gen != refs[i].recording_generation) { ++unrecorded_lists_; continue; }
            if (it->second.incomplete) ++incomplete_lists_;
            for (const Pass& p : it->second.passes) frame_.push_back({refs[i].list, p});
        }
    }
    void OnPresent()
    {
        if (!armed_ || done_) return;
        if (capturing_) { done_ = true; capturing_ = false; return; }
        if (skip_) { --skip_; return; }
        capturing_ = true;
    }

    // The map, one line per pass, then the summary. Sizes as WxH, resources as R<id>.
    std::string Report() const
    {
        std::string s;
        char line[512];
        const uint32_t ow = out_w_ ? out_w_ : widest(), oh = out_h_ ? out_h_ : tallest();
        std::snprintf(line, sizeof line, "stage_m: output %ux%u, %zu passes, lists executed %u (unrecorded %u, incomplete %u)%s%s\n", ow, oh,
                      frame_.size(), executed_lists_, unrecorded_lists_, incomplete_lists_, desc_overflow_ ? ", descriptor map full" : "",
                      truncated_ ? ", passes truncated" : "");
        s += line;
        std::set<uint64_t> reduced_written; // resources written by a pass at a reduced size
        int stretch = -1;
        for (size_t i = 0; i < frame_.size(); ++i) {
            const Pass& p = frame_[i].pass;
            const auto [tw, th] = target_size(p);
            const bool reduced = ow && tw && th && (tw * 100 < ow * 97 || (p.vp_w && p.vp_w * 100 < ow * 97));
            const bool full = ow && tw >= ow && th >= oh && (!p.vp_w || p.vp_w >= ow * 97 / 100);
            bool reads_reduced = false;
            for (uint64_t r : p.reads) reads_reduced = reads_reduced || reduced_written.count(r);
            const bool cand = full && reads_reduced && stretch < 0 && (p.kind == Kind::Raster || p.kind == Kind::Compute || p.kind == Kind::Copy);
            if (cand) stretch = static_cast<int>(i);
            if (reduced) for (uint64_t w : p.writes) reduced_written.insert(w);
            std::snprintf(line, sizeof line, "#%zu list %llu %s x%u%s target %s viewport %ux%u%s%s\n", i, (unsigned long long)frame_[i].list,
                          kind_name(p.kind), p.ops, p.pipeline ? (" pso " + std::to_string(p.pipeline)).c_str() : "",
                          ids(p.targets).c_str(), p.vp_w, p.vp_h, reduced ? " REDUCED" : full ? " FULL" : "",
                          cand ? " <- STRETCH CANDIDATE" : "");
            s += line;
            s += "    reads " + ids(p.reads) + (p.approx_reads ? " (table heads, approx)" : "") + "\n";
            s += "    writes " + ids(p.writes) + (p.truncated ? " (truncated)" : "") + "\n";
        }
        if (stretch >= 0) {
            const Pass& p = frame_[static_cast<size_t>(stretch)].pass;
            std::snprintf(line, sizeof line, "stage_m: stretch candidate #%d (%s) writes %s from reduced %s; %zu passes after it\n", stretch,
                          kind_name(p.kind), ids(p.writes).c_str(), ids(reduced_reads(p, reduced_written)).c_str(),
                          frame_.size() - static_cast<size_t>(stretch) - 1);
        } else {
            std::snprintf(line, sizeof line, "stage_m: no stretch candidate (no full-size pass read a reduced-size result)%s\n",
                          reduced_written.empty() ? "; no reduced-size pass either: is the render scale below 100 %?" : "");
        }
        s += line;
        return s;
    }

    std::vector<Pass> Passes() const
    {
        std::vector<Pass> v;
        for (const auto& f : frame_) v.push_back(f.pass);
        return v;
    }

private:
    struct Res { uint32_t w = 0, h = 0, format = 0; };
    struct Desc { uint32_t kind = 0; uint64_t resource = 0; };
    struct Table { uint64_t heap = 0; uint32_t slot = 0; };
    struct View { uint32_t kind = 0; uint64_t resource = 0; };
    struct Rec {
        uint64_t gen = 0;
        uint64_t pipeline = 0;
        std::vector<uint64_t> targets;
        uint32_t vp_w = 0, vp_h = 0;
        std::map<uint32_t, Table> gtab, ctab;
        std::map<uint32_t, View> gview, cview;
        std::vector<Pass> passes;
        bool incomplete = false;
    };
    struct FramePass { uint64_t list; Pass pass; };

    static uint64_t key(uint64_t heap, uint32_t slot) { return (heap << 24) ^ slot; }
    Rec& rec(uint64_t list, uint64_t gen)
    {
        // A new recording of a list replaces its previous one (bounded by lists, not by time).
        auto it = recordings_.find(list);
        if (it != recordings_.end()) {
            if (it->second.gen != gen) { it->second = Rec{}; it->second.gen = gen; }
            return it->second;
        }
        if (recordings_.size() >= kMaxRecordings) { truncated_ = true; recordings_.erase(recordings_.begin()); }
        Rec& r = recordings_[list];
        r.gen = gen;
        return r;
    }
    bool push(Rec& r)
    {
        if (r.passes.size() >= kMaxPassesPerRecording) { truncated_ = true; return false; }
        r.passes.emplace_back();
        return true;
    }
    static void add(std::vector<uint64_t>& v, uint64_t id, Pass* p)
    {
        if (!id || id == NA_UNKNOWN_U64 || std::find(v.begin(), v.end(), id) != v.end()) return;
        if (v.size() >= kMaxIds) { p->truncated = true; return; }
        v.push_back(id);
    }
    std::pair<uint32_t, uint32_t> target_size(const Pass& p) const
    {
        for (uint64_t t : p.targets) {
            auto it = res_.find(t);
            if (it != res_.end()) return {it->second.w, it->second.h};
        }
        if (p.kind == Kind::Compute)
            for (uint64_t t : p.writes) {
                auto it = res_.find(t);
                if (it != res_.end() && it->second.h > 1) return {it->second.w, it->second.h};
            }
        return {0, 0};
    }
    std::vector<uint64_t> reduced_reads(const Pass& p, const std::set<uint64_t>& reduced) const
    {
        std::vector<uint64_t> v;
        for (uint64_t r : p.reads) if (reduced.count(r)) v.push_back(r);
        return v;
    }
    uint32_t widest() const
    {
        uint32_t w = 0;
        for (const auto& f : frame_) w = (std::max)(w, target_size(f.pass).first);
        return w;
    }
    uint32_t tallest() const
    {
        uint32_t h = 0;
        for (const auto& f : frame_) h = (std::max)(h, target_size(f.pass).second);
        return h;
    }
    std::string ids(const std::vector<uint64_t>& v) const
    {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i) {
            char b[96];
            auto it = res_.find(v[i]);
            if (it != res_.end()) std::snprintf(b, sizeof b, "%sR%llu %ux%u f%u", i ? ", " : "", (unsigned long long)v[i], it->second.w, it->second.h, it->second.format);
            else std::snprintf(b, sizeof b, "%sR%llu", i ? ", " : "", (unsigned long long)v[i]);
            s += b;
        }
        return s + "]";
    }
    static const char* kind_name(Kind k)
    {
        switch (k) {
        case Kind::Raster: return "draw";
        case Kind::Compute: return "dispatch";
        case Kind::Copy: return "copy";
        case Kind::Resolve: return "resolve";
        case Kind::Clear: return "clear";
        }
        return "?";
    }

    bool armed_ = false, capturing_ = false, done_ = false, desc_overflow_ = false, truncated_ = false;
    uint32_t skip_ = 0, out_w_ = 0, out_h_ = 0, executed_lists_ = 0, unrecorded_lists_ = 0, incomplete_lists_ = 0;
    std::unordered_map<uint64_t, Res> res_;
    std::unordered_map<uint64_t, Desc> desc_;
    std::unordered_map<uint64_t, Rec> recordings_;
    std::vector<FramePass> frame_;
};

} // namespace na::discovery
