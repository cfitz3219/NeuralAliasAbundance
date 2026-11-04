#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>

namespace na::jitter {
// Raster writes and sampled colour are different domains. A completed image
// observation qualifies a SOURCE convention, never this frame's raster write.
enum class Convention : uint32_t { Unknown, Raster, Unjittered, Opposite };
struct Source {
    uint64_t generation = 0;
    uint32_t width = 0, height = 0, format = 0;
    friend bool operator==(Source a, Source b) {
        return a.generation == b.generation && a.width == b.width && a.height == b.height && a.format == b.format;
    }
};
struct Sample { float x = 0, y = 0; };
struct Evidence {
    uint64_t frame = 0, tested = 0, unshifted = 0, raster = 0, opposite = 0;
};
// Remembered verdicts (Cort 2026-11-04: re-learning after every scene change is not
// acceptable). A qualified convention belongs to the game's pipeline stage, not to one
// selection generation or render size: a new source of the same stage starts from the last
// verdict for its format (or the stage's saved verdict, any format) and keeps observing, so
// contrary evidence still overturns it after two decisive windows. Stage 0 = no memory.
struct Memory {
    struct Entry { uint32_t stage = 0, format = 0; uint32_t convention = 0; };
    std::mutex m;
    Entry e[16]{};
    // Off until the host's settings are loaded (tests and standalone tools stay independent).
    std::atomic<bool> enabled{false};
    void Put(uint32_t stage, uint32_t format, uint32_t c) {
        if (!enabled.load()) return;
        std::lock_guard<std::mutex> lk(m);
        Entry* slot = nullptr;
        for (auto& x : e) if (x.stage == stage && x.format == format) { slot = &x; break; }
        if (!slot) for (auto& x : e) if (!x.stage) { slot = &x; break; }
        if (!slot) slot = &e[15];
        *slot = {stage, format, c};
    }
    uint32_t Get(uint32_t stage, uint32_t format) {
        if (!enabled.load()) return 0;
        std::lock_guard<std::mutex> lk(m);
        for (auto& x : e) if (x.stage == stage && x.format == format) return x.convention;
        for (auto& x : e) if (x.stage == stage && x.format == 0) return x.convention; // saved per game
        return 0;
    }
};
// Stages with memory: the early flow colour and the final DLSS input colour.
constexpr uint32_t kStageFlow = 1, kStageFinal = 2;
inline Memory& Remembered() { static Memory m; return m; }
// Called when a stage's verdict changes (e.g. to save it per game); stage, convention.
inline void (*&OnRemember())(uint32_t, uint32_t) { static void (*f)(uint32_t, uint32_t) = nullptr; return f; }

class ColourDomain {
    uint32_t stage_ = 0;
    Source source_{};
    Convention convention_ = Convention::Unknown, candidate_ = Convention::Unknown;
    uint32_t streak_ = 0;
    uint64_t epoch_ = 0, last_frame_ = 0;
public:
    ColourDomain() = default;
    explicit ColourDomain(uint32_t stage) : stage_(stage) {}
    void Bind(Source source) {
        if (source == source_) return;
        source_ = source; convention_ = candidate_ = Convention::Unknown;
        streak_ = 0; last_frame_ = 0; ++epoch_;
        if (stage_) {
            const uint32_t c = Remembered().Get(stage_, source.format);
            if (c >= static_cast<uint32_t>(Convention::Raster) && c <= static_cast<uint32_t>(Convention::Opposite)) {
                convention_ = candidate_ = static_cast<Convention>(c);
                streak_ = 2;
            }
        }
    }
    // Counts are from a completed multi-frame GPU window. Ignore evidence
    // belonging to an old source or an already consumed/out-of-order window.
    bool Observe(Source source, Evidence e) {
        if (!(source == source_) || !e.frame || e.frame <= last_frame_) return false;
        last_frame_ = e.frame;
        Convention vote = Convention::Unknown;
        if (e.tested >= 20000) {
            const uint64_t n[] = {e.unshifted, e.raster, e.opposite};
            const Convention c[] = {Convention::Unjittered, Convention::Raster, Convention::Opposite};
            for (uint32_t i = 0; i < 3; ++i) {
                const uint64_t other = n[(i+1)%3] + n[(i+2)%3];
                if (n[i] <= e.tested && n[i] >= e.tested - e.tested/3 && n[i]/4 > other)
                    vote = c[i];
            }
        }
        if (vote == Convention::Unknown) { candidate_ = vote; streak_ = 0; return false; }
        if (vote != candidate_) { candidate_ = vote; streak_ = 1; }
        else if (streak_ < 2) ++streak_;
        if (streak_ < 2 || convention_ == vote) return false;
        convention_ = vote; ++epoch_;
        if (stage_) {
            Remembered().Put(stage_, source_.format, static_cast<uint32_t>(vote));
            if (auto f = OnRemember()) f(stage_, static_cast<uint32_t>(vote));
        }
        return true;
    }
    Sample Resolve(Sample raster) const {
        if (convention_ == Convention::Unjittered) return {};
        if (convention_ == Convention::Opposite) return {-raster.x, -raster.y};
        return raster; // unqualified preserves the current path during acquisition
    }
    Convention Get() const { return convention_; }
    Source Key() const { return source_; }
    uint64_t Epoch() const { return epoch_; }
};
inline const char* Name(Convention c) {
    switch (c) {
    case Convention::Raster: return "raster";
    case Convention::Unjittered: return "unjittered";
    case Convention::Opposite: return "opposite";
    default: return "learning";
    }
}
}
