#pragma once
#include <cstdint>

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
class ColourDomain {
    Source source_{};
    Convention convention_ = Convention::Unknown, candidate_ = Convention::Unknown;
    uint32_t streak_ = 0;
    uint64_t epoch_ = 0, last_frame_ = 0;
public:
    void Bind(Source source) {
        if (source == source_) return;
        source_ = source; convention_ = candidate_ = Convention::Unknown;
        streak_ = 0; last_frame_ = 0; ++epoch_;
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
        convention_ = vote; ++epoch_; return true;
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
