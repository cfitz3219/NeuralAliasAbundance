#pragma once
// J2 placement: role classes and the learned per-game consumer profile.
// Design: docs/PLACEMENT-DESIGN-RESPONSE.md (Opus's revision after Astra's response).
//
//  * A consumer of the selected scene colour is recognised by its ROLE CLASS: operation
//    kind, structural root-signature hash, destination size class relative to the scene,
//    destination format, scene-colour descriptor count and other view-sized inputs. Shader
//    variants of one pass share a class; exact PSO identity is not part of it.
//  * Frames are labelled at Present from the submission-ordered writes, consumers and
//    RT->read barriers of the colour (LabelFrame). Scene construction CONTINUES after a
//    point when a draw into the colour follows (with or without a depth attachment) before
//    the image has been copied onward; clears/copies into the colour, and anything after a
//    copy onward, are reuse of the texture (Astra's copy-then-clear case).
//  * The profile accepts a class after kAcceptHits post-entry observations with no
//    temporary one. A class seen in both roles is ambiguous and is accepted only together
//    with the producer (pipeline of the preceding scene write) that distinguishes it.
//  * Barriers are learned as QUALIFIED SCENE-ENDING PATTERNS: the drawing pipeline of the
//    last write before the barrier (a pipeline, not a game object: many objects can share
//    one), and, when that pipeline also appears mid-scene, the class of the latest consumer
//    earlier in the same recording as extra context. A pattern that stays ambiguous is
//    never handed to the boundary matcher.
// Pure CPU, bounded, no D3D12.
#include <cstdint>
#include <vector>

#include "host_dx12/j2/insertion_trace.hpp"

namespace na::j2 {

struct ClassKey {
    Op op = Op::Unknown;
    uint64_t root = 0;        // structural root-layout hash (0 for copies)
    uint8_t w_class = 4, h_class = 4; // destination size vs scene: 0 same, 1 half, 2 quarter, 3 other, 4 unknown
    uint32_t fmt = 0;         // destination format (0 unknown)
    uint8_t color_srvs = 0;   // 0, 1, 2 (= 2 or more)
    uint8_t view_srvs = 0;    // 0, 1, 2 (= 2 or more)
    bool operator==(const ClassKey& o) const
    {
        return op == o.op && root == o.root && w_class == o.w_class && h_class == o.h_class && fmt == o.fmt &&
               color_srvs == o.color_srvs && view_srvs == o.view_srvs;
    }
    bool operator!=(const ClassKey& o) const { return !(*this == o); }
};

inline uint8_t SizeClass(uint32_t dst, uint32_t scene)
{
    if (!dst || !scene) return 4;
    if (dst == scene) return 0;
    if (dst * 2 >= scene - 1 && dst * 2 <= scene + 1) return 1;
    if (dst * 4 >= scene - 3 && dst * 4 <= scene + 3) return 2;
    return 3;
}
inline ClassKey MakeClass(const InsertionEvent& e, uint32_t scene_w, uint32_t scene_h)
{
    ClassKey k;
    k.op = ClassifyOp(e);
    k.root = k.op == Op::CopyRead ? 0 : e.root;
    k.w_class = SizeClass(e.dst_w, scene_w);
    k.h_class = SizeClass(e.dst_h, scene_h);
    k.fmt = e.dst_fmt;
    k.color_srvs = e.color_srvs > 2 ? 2 : e.color_srvs;
    k.view_srvs = e.view_srvs > 2 ? 2 : e.view_srvs;
    return k;
}

// One frame's writes and consumers of the colour in SUBMISSION order (all lists).
enum class Role : uint8_t { Temporary = 0, PostEntry = 1, Later = 2 }; // Later: after the post entry (bloom, HUD...)
struct Labelled { ClassKey cls; uint64_t producer = 0; uint64_t context = 0; Role role = Role::Temporary; };

inline uint64_t ClassHash(const ClassKey& k)
{
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : {uint64_t(k.op), k.root, uint64_t(k.w_class), uint64_t(k.h_class), uint64_t(k.fmt), uint64_t(k.color_srvs), uint64_t(k.view_srvs)})
        h = (h ^ v) * 1099511628211ull;
    return h ? h : 1;
}
// Qualification key of a barrier pattern: producer pipeline + context (never 0, never the bare producer).
inline uint64_t Qualified(uint64_t producer, uint64_t context)
{
    const uint64_t q = (producer * 0x9E3779B97F4A7C15ull) ^ (context | 1) ^ 0xA5A5A5A5A5A5A5A5ull;
    return q && q != producer ? q : 3;
}

inline void LabelFrame(const std::vector<ColorEvent>& seq, uint32_t scene_w, uint32_t scene_h, std::vector<Labelled>& out)
{
    out.clear();
    bool post_found = false;
    bool post_barrier_found = false;
    // A barrier (RT -> read transition) is not a consumer: it never counts as "the colour
    // consumed again", and never as copying the image onward.
    const auto consumes = [](const ColorEvent& e) { return !e.write && e.op != Op::Barrier; };
    // Scene construction continues after `from`: a draw into the colour (depth attachment
    // or not) before the image is copied onward. Clears/copies into the colour are reuse.
    const auto continued = [&](size_t from) {
        for (size_t j = from; j < seq.size(); ++j) {
            if (!seq[j].write && seq[j].op == Op::CopyRead) return false; // copied onward: later writes are reuse
            if (seq[j].write && seq[j].op == Op::DrawWrite) return true;
        }
        return false;
    };
    uint64_t list_context = 0; // class of the latest consumer earlier in the current recording
    for (size_t i = 0; i < seq.size(); ++i) {
        const ColorEvent& c = seq[i];
        if (c.list_start) list_context = 0;
        if (c.write) continue;
        if (c.op == Op::Barrier) {
            const bool temporary = continued(i + 1);
            Labelled l;
            l.cls.op = Op::Barrier;
            l.producer = c.producer;
            l.context = list_context;
            l.role = temporary ? Role::Temporary : (post_barrier_found ? Role::Later : Role::PostEntry);
            if (!temporary) post_barrier_found = true;
            out.push_back(l);
            continue;
        }
        // The next write after this consumer, and whether the colour is consumed again after it.
        size_t w = i + 1;
        while (w < seq.size() && !seq[w].write) ++w;
        bool temporary = false;
        if (w < seq.size()) {
            bool consumed_after = false;
            for (size_t j = w + 1; j < seq.size() && !consumed_after; ++j) consumed_after = consumes(seq[j]);
            if (c.op == Op::CopyRead) {
                // A copy carries the image onward: only depth-paired scene content after it
                // (a mid-scene snapshot) or a later read of the original makes it temporary.
                bool scene_after = false;
                for (size_t j = w; j < seq.size() && !scene_after; ++j) scene_after = seq[j].write && seq[j].with_depth;
                temporary = consumed_after || scene_after;
            } else {
                temporary = consumed_after || continued(i + 1);
            }
        }
        Labelled l;
        l.cls = MakeClass(c.raw, scene_w, scene_h);
        l.producer = c.producer;
        l.role = temporary ? Role::Temporary : (post_found ? Role::Later : Role::PostEntry);
        if (!temporary) post_found = true;
        list_context = ClassHash(l.cls);
        out.push_back(l);
    }
}

class ConsumerProfile {
public:
    static constexpr uint32_t kAcceptHits = 3;
    static constexpr size_t kMaxEntries = 256; // SER needed more than 64 (classes + barrier/producer pairs)
    struct Entry {
        ClassKey cls;
        uint64_t producer = 0; // 0: the class itself; else the (class, producer) qualification
        uint32_t post = 0, temp = 0;
        uint64_t last_frame = 0;
    };
    enum class Verdict : uint8_t { Unknown = 0, Accepted, AcceptedWithProducer, AcceptedQualified, Rejected, Ambiguous };

    void Clear() { e_.clear(); }
    void Observe(const Labelled& l, uint64_t frame)
    {
        if (l.role == Role::Later) return; // downstream passes teach nothing about the boundary
        for (uint64_t producer : {uint64_t(0), l.producer}) {
            Entry& x = find_or_add(l.cls, producer, frame);
            if (l.role == Role::PostEntry) ++x.post; else ++x.temp;
            x.last_frame = frame;
            if (!l.producer) break;
        }
        if (l.cls.op == Op::Barrier && l.producer) { // the qualified pattern (pipeline + context)
            Entry& q = find_or_add(l.cls, Qualified(l.producer, l.context), frame);
            if (l.role == Role::PostEntry) ++q.post; else ++q.temp;
            q.last_frame = frame;
        }
    }
    Verdict Judge(const ClassKey& cls, uint64_t producer, uint64_t context = 0) const
    {
        if (cls.op == Op::Barrier) {
            // All barriers share one class: only the pattern (producer pipeline, then the
            // context when that pipeline also appears mid-scene) can identify completion.
            const Entry* pair = producer ? find(cls, producer) : nullptr;
            if (!pair) return Verdict::Unknown;
            if (pair->temp == 0) return pair->post >= kAcceptHits ? Verdict::AcceptedWithProducer : Verdict::Unknown;
            if (pair->post == 0) return Verdict::Rejected;
            const Entry* q = find(cls, Qualified(producer, context));
            if (q && q->temp == 0 && q->post >= kAcceptHits) return Verdict::AcceptedQualified;
            if (q && q->post == 0 && q->temp > 0) return Verdict::Rejected;
            return Verdict::Ambiguous; // seen both mid-scene and at completion; context does not separate them
        }
        const Entry* plain = find(cls, 0);
        if (!plain) return Verdict::Unknown;
        if (plain->temp == 0) return plain->post >= kAcceptHits ? Verdict::Accepted : Verdict::Unknown;
        if (plain->post == 0) return Verdict::Rejected;
        const Entry* pair = producer ? find(cls, producer) : nullptr;
        if (pair && pair->temp == 0 && pair->post >= kAcceptHits) return Verdict::AcceptedWithProducer;
        return Verdict::Ambiguous;
    }
    // A class that was accepted and then followed by more scene content: demote it now
    // (a later scene write is a fact, not a vote).
    // The specific pattern that failed loses its acceptance (pair and qualified entry).
    void Contradict(const ClassKey& cls, uint64_t producer, uint64_t frame, uint64_t context = 0)
    {
        find_or_add(cls, 0, frame).temp += kAcceptHits;
        if (producer) find_or_add(cls, producer, frame).temp += kAcceptHits;
        if (cls.op == Op::Barrier && producer) find_or_add(cls, Qualified(producer, context), frame).temp += kAcceptHits;
    }
    uint32_t AcceptedCount() const
    {
        uint32_t n = 0;
        for (const auto& x : e_) n += x.cls.op == Op::Barrier ? (x.producer && x.temp == 0 && x.post >= kAcceptHits)
                                                              : (!x.producer && x.temp == 0 && x.post >= kAcceptHits);
        return n;
    }
    uint32_t RejectedCount() const
    {
        uint32_t n = 0;
        for (const auto& x : e_) n += !x.producer && x.post == 0 && x.temp > 0;
        return n;
    }
    uint32_t AmbiguousCount() const
    {
        uint32_t n = 0;
        for (const auto& x : e_) n += !x.producer && x.post > 0 && x.temp > 0;
        return n;
    }
    size_t Size() const { return e_.size(); }

private:
    const Entry* find(const ClassKey& cls, uint64_t producer) const
    {
        for (const auto& x : e_) if (x.producer == producer && x.cls == cls) return &x;
        return nullptr;
    }
    Entry& find_or_add(const ClassKey& cls, uint64_t producer, uint64_t frame)
    {
        for (auto& x : e_) if (x.producer == producer && x.cls == cls) return x;
        if (e_.size() >= kMaxEntries) {
            size_t lru = 0;
            for (size_t i = 1; i < e_.size(); ++i) if (e_[i].last_frame < e_[lru].last_frame) lru = i;
            e_.erase(e_.begin() + static_cast<std::ptrdiff_t>(lru));
        }
        Entry n;
        n.cls = cls;
        n.producer = producer;
        n.last_frame = frame;
        e_.push_back(n);
        return e_.back();
    }
    std::vector<Entry> e_;
};

} // namespace na::j2
