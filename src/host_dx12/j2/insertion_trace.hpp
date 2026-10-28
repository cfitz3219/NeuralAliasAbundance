#pragma once
#include <cstdint>
#include <vector>
#include <map>

namespace na::j2 {
// CPU-only recording evidence; never retains game objects or waits for the GPU.
// A read binding is an insertion candidate, not proof of an AA/tonemap shader.
// Operation kinds (P4-0D consumer anchor). Op::Unknown events are classified from
// the legacy fields: write -> draw write, pipeline UINT64_MAX -> barrier, pipeline 0
// -> copy read, otherwise a graphics read.
enum class Op : uint8_t {
    Unknown = 0, DrawWrite, ClearWrite, CopyWrite,  // writes
    Barrier,                                        // RT -> read transition (not a consumer)
    GraphicsRead, ComputeRead, CopyRead,            // actual consumers
    DerivedRead                                     // read of a texture copied from the colour this frame (lineage only)
};
struct InsertionEvent {
    uint64_t resource = 0, destination = 0, pipeline = 0;
    bool write = false;
    Op op = Op::Unknown;
    uint64_t root = 0;      // structural hash of the consumer's root-signature layout (0 = unknown / copy)
    // Role-class inputs of a consumer (placement design, docs/PLACEMENT-DESIGN-RESPONSE.md):
    uint32_t dst_w = 0, dst_h = 0, dst_fmt = 0; // destination texture (0 = unknown / buffer)
    uint8_t color_srvs = 0;  // descriptors of the scene colour in the bound tables
    uint8_t view_srvs = 0;   // other view-sized textures read alongside (history etc.)
    bool with_depth = false; // write: the selected scene depth was bound (scene content, not reuse)
};
inline Op ClassifyOp(const InsertionEvent& e)
{
    if (e.op != Op::Unknown) return e.op;
    if (e.write) return Op::DrawWrite;
    if (e.pipeline == UINT64_MAX) return Op::Barrier;
    return e.pipeline ? Op::GraphicsRead : Op::CopyRead;
}
inline bool IsConsumer(Op op) { return op == Op::GraphicsRead || op == Op::ComputeRead || op == Op::CopyRead; }

// P4-0D (Argon's PostEntryPSO, with Astra's corrections): the first ACTUAL consumer of
// the selected colour after its final write in a recording, learned from completed,
// submitted recordings and matched in a later one. A predictor, not an invariant.
struct ConsumerSignature {
    enum Mode : uint8_t {
        None = 0,
        UniqueConsumer = 1,   // its shader PSO never read the colour earlier in the recording
        ProducerConsumer = 2, // shared PSO, but the (final producer, consumer) pair never occurred earlier
        CopyDestination = 3,  // copy/resolve consumer: no PSO; its destination never received an earlier copy
        Legacy = 4            // ambiguous: fall back to the existing boundary matcher
    };
    Mode mode = None;
    Op op = Op::Unknown;
    uint64_t consumer_pso = 0, consumer_root = 0, destination = 0, producer_pso = 0;
    const char* why = "no consumer of the colour after its final write";
};
// The latest event of a recording, as seen at a live trigger.
struct LiveConsumer {
    bool valid = false;        // the latest event is an actual consumer of the colour
    bool after_write = false;  // a write of the colour precedes it in this recording
    Op op = Op::Unknown;
    uint64_t pso = 0, root = 0, destination = 0, producer_pso = 0;
    uint32_t since_write = 0;  // consumers since the last write, this one included
    InsertionEvent raw;        // the event itself (role-class inputs)
    bool barrier = false;      // the latest event is a RT -> read transition (producer_pso/after_write filled)
};
inline const char* ModeName(ConsumerSignature::Mode m)
{
    switch (m) {
    case ConsumerSignature::UniqueConsumer: return "unique-consumer";
    case ConsumerSignature::ProducerConsumer: return "producer+consumer";
    case ConsumerSignature::CopyDestination: return "copy-destination";
    case ConsumerSignature::Legacy: return "legacy-boundary";
    default: return "none";
    }
}
// nullptr = matches; else the name of the failing predicate. Legacy/None never match here.
inline const char* MatchConsumer(const ConsumerSignature& sig, const LiveConsumer& live)
{
    if (sig.mode == ConsumerSignature::None || sig.mode == ConsumerSignature::Legacy) return "no consumer signature";
    if (!live.valid) return "not a consumer";
    if (!live.after_write) return "no scene write before it";
    if (live.op != sig.op) return "different consumer operation";
    if (sig.mode == ConsumerSignature::CopyDestination)
        return live.destination == sig.destination ? nullptr : "different copy destination";
    if (live.pso != sig.consumer_pso) return "different consumer pipeline";
    if (sig.consumer_root && live.root && live.root != sig.consumer_root) return "different consumer root signature";
    if (sig.mode == ConsumerSignature::ProducerConsumer && live.producer_pso != sig.producer_pso) return "different final producer";
    return nullptr;
}
struct InsertionDecision {
    uint32_t candidates = 0, final_candidate = 0, writes = 0;
    uint32_t all_reads = 0;
    uint32_t transitions = 0, final_transition = 0;
    uint64_t destination = 0, pipeline = 0;
    uint64_t boundary_writer = 0;
    uint32_t writer_occurrence = 0;
    uint32_t derived_reads = 0; // reads of textures copied from the colour this frame
};
// A write or an actual consumer of the selected colour, in recording order (frame labelling).
struct ColorEvent {
    bool write = false;
    Op op = Op::Unknown;
    bool with_depth = false;   // write with the scene depth bound
    InsertionEvent raw;        // the consumer's class inputs (consumers only)
    uint64_t producer = 0;     // consumer: pipeline of the preceding write in this recording
};
class InsertionTrace {
public:
    void Reset() { events_.clear(); complete_ = true; }
    void Add(InsertionEvent event) {
        if (events_.size() == 8192) { complete_ = false; return; }
        events_.push_back(event);
    }
    InsertionDecision Analyze(uint64_t color) const {
        InsertionDecision result;
        if (!complete_ || !color) return result;
        uint64_t writer = 0;
        std::map<uint64_t, uint32_t> writer_reads;
        for (const auto& event : events_) {
            if (event.resource != color) continue;
            if (event.op == Op::DerivedRead) { ++result.derived_reads; continue; } // lineage only
            if (!event.write) ++result.all_reads;
            if (!event.write && event.pipeline == UINT64_MAX) ++result.transitions;
            if (event.write) {
                writer = event.pipeline;
                ++result.writes;
                result.final_candidate = 0; // later geometry invalidates earlier reads
                result.final_transition = 0;
                result.destination = result.pipeline = 0;
                result.boundary_writer = 0;
                result.writer_occurrence = 0;
            } else if (result.writes && event.destination != color) {
                const uint32_t occurrence = event.pipeline == UINT64_MAX ? ++writer_reads[writer] : 0;
                ++result.candidates;
                // First downstream read after the LAST scene write, not last read:
                // later reads can already be HUD/compositing on processed content.
                if (!result.final_candidate) {
                    result.final_candidate = result.candidates;
                    result.final_transition = event.pipeline == UINT64_MAX ? result.transitions : 0;
                    result.destination = event.destination;
                    result.pipeline = event.pipeline;
                    result.boundary_writer = writer;
                    result.writer_occurrence = occurrence;
                }
            }
        }
        return result;
    }
    ConsumerSignature AnalyzeConsumer(uint64_t color) const {
        ConsumerSignature sig;
        if (!complete_ || !color) { sig.why = complete_ ? "no colour" : "trace truncated"; return sig; }
        int last_write = -1;
        for (int i = 0; i < static_cast<int>(events_.size()); ++i)
            if (events_[i].resource == color && events_[i].write) last_write = i;
        if (last_write < 0) { sig.why = "the colour was not written in this recording"; return sig; }
        const InsertionEvent* first = nullptr;
        for (size_t i = static_cast<size_t>(last_write) + 1; i < events_.size() && !first; ++i) {
            const auto& e = events_[i];
            if (e.resource == color && !e.write && IsConsumer(ClassifyOp(e)) && e.destination != color) first = &e;
        }
        if (!first) return sig; // only barriers / nothing after the final write
        sig.op = ClassifyOp(*first);
        sig.consumer_pso = sig.op == Op::CopyRead ? 0 : first->pipeline;
        sig.consumer_root = first->root;
        sig.destination = first->destination;
        sig.producer_pso = events_[static_cast<size_t>(last_write)].pipeline;
        // Earlier consumers (before the final write), each with the producer that preceded it.
        bool same_consumer = false, same_pair = false, same_copy = false;
        uint64_t writer = 0;
        for (int i = 0; i < last_write; ++i) {
            const auto& e = events_[static_cast<size_t>(i)];
            if (e.resource != color) continue;
            if (e.write) { writer = e.pipeline; continue; }
            const Op op = ClassifyOp(e);
            if (!IsConsumer(op) || e.destination == color || op != sig.op) continue;
            if (op == Op::CopyRead) { same_copy |= e.destination == sig.destination; continue; }
            if (e.pipeline == sig.consumer_pso) {
                same_consumer = true;
                same_pair |= writer == sig.producer_pso;
            }
        }
        if (sig.op == Op::CopyRead) {
            sig.mode = same_copy ? ConsumerSignature::Legacy : ConsumerSignature::CopyDestination;
            sig.why = same_copy ? "copy consumer repeats an earlier copy to the same destination" : "copy consumer";
        } else if (!sig.consumer_pso || sig.consumer_pso == UINT64_MAX) {
            sig.mode = ConsumerSignature::Legacy;
            sig.why = "consumer pipeline unknown";
        } else if (!same_consumer) {
            sig.mode = ConsumerSignature::UniqueConsumer;
            sig.why = "consumer pipeline never read the colour earlier";
        } else if (sig.producer_pso && !same_pair) {
            sig.mode = ConsumerSignature::ProducerConsumer;
            sig.why = "consumer pipeline also read mid-scene; final producer distinguishes it";
        } else {
            sig.mode = ConsumerSignature::Legacy;
            sig.why = "consumer pipeline and producer both repeat mid-scene (ambiguous)";
        }
        return sig;
    }
    LiveConsumer Live(uint64_t color) const {
        LiveConsumer live;
        if (!complete_ || !color || events_.empty()) return live;
        const auto& e = events_.back();
        if (e.resource != color || e.write) return live;
        live.op = ClassifyOp(e);
        if (live.op == Op::Barrier) {
            live.barrier = true;
            live.raw = e;
            for (size_t i = events_.size(); i-- > 0;) {
                const auto& p = events_[i];
                if (p.resource == color && p.write) { live.after_write = true; live.producer_pso = p.pipeline; break; }
            }
            return live;
        }
        if (!IsConsumer(live.op) || e.destination == color) return live;
        live.valid = true;
        live.raw = e;
        live.pso = live.op == Op::CopyRead ? 0 : e.pipeline;
        live.root = e.root;
        live.destination = e.destination;
        for (size_t i = events_.size(); i-- > 0;) {
            const auto& p = events_[i];
            if (p.resource != color) continue;
            if (p.write) { live.after_write = true; live.producer_pso = p.pipeline; break; }
            if (IsConsumer(ClassifyOp(p)) && p.destination != color) ++live.since_write;
        }
        return live;
    }
    // Writes and actual consumers of the colour (barriers and lineage reads omitted).
    // Returns false when the trace is truncated (the caller must treat the frame as unknown).
    bool ColorEvents(uint64_t color, std::vector<ColorEvent>& out) const {
        if (!complete_ || !color) return complete_;
        uint64_t producer = 0;
        for (const auto& e : events_) {
            if (e.resource != color) continue;
            const Op op = ClassifyOp(e);
            if (e.write) {
                producer = e.pipeline;
                ColorEvent c;
                c.write = true; c.op = op; c.with_depth = e.with_depth;
                out.push_back(c);
            } else if ((IsConsumer(op) && e.destination != color) || op == Op::Barrier) {
                ColorEvent c;
                c.op = op; c.raw = e; c.producer = producer;
                out.push_back(c);
            }
        }
        return true;
    }
private:
    std::vector<InsertionEvent> events_;
    bool complete_ = true;
};
} // namespace na::j2
