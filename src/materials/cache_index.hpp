// The runtime's view of the material cache: which game textures have a prepared map, asked in the
// two steps the runtime can afford. Owner: Fable. Portable C++17; no hooks, no files.
//
//  1. When the game creates a texture: WantedViews(shape). Almost every texture has no entry of its
//     shape, and is then never hashed. For one that has, the answer lists the typed view formats
//     to compute the "texture-subresources-v1" digest for (the digest covers the view format).
//  2. When a digest is known: Find(...), the map prepared from exactly that source.
//
// Qualified(): which maps the v1 runtime may put in a game texture's place at all. A replacement is
// sampled by the game's own shader through the game's own sampler, so it has to mean the same
// thing channel for channel and have the same extent and mip count. Anything else is refused with
// the reason, not adapted.
#pragma once

#include "materials/manifest_reader.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace na::materials {

struct TextureShape {
    uint32_t resource_format = 0, width = 0, height = 0, mip_count = 0;
    bool operator==(const TextureShape& o) const
    {
        return resource_format == o.resource_format && width == o.width && height == o.height && mip_count == o.mip_count;
    }
};

// Null when `map` may replace a game texture described by its own source; otherwise why not.
inline const char* Qualified(const Map& map)
{
    const Source& s = map.source;
    if (s.identity_domain != "texture-subresources-v1") return "prepared from a file, not from a game texture";
    if (!s.complete_observed_range) return "the source's levels were not all observed";
    if (s.first_mip != 0 || s.first_array_slice != 0 || s.array_size != 1) return "the source is a partial view or an array";
    if (!s.resource_format || !s.view_format) return "the source's formats are not recorded";
    // Same extent and mip count: a shader may address texels or read the size, and the first
    // generator preserves dimensions. Larger maps wait for a per-shader check.
    if (map.width != s.width || map.height != s.height || map.mips.size() != s.mip_count)
        return "prepared at a different size or mip count than the game's texture";
    switch (map.role) {
    case Role::BaseColor: // BC7 sRGB in place of an sRGB colour view: same decode, all four channels
        switch (s.view_format) {
        case 29: case 72: case 75: case 78: case 91: case 93: case 99: return nullptr;
        default: return "the game reads this colour texture without sRGB decoding";
        }
    case Role::Normal: // BC5 holds X and Y only; right only where the game's texture is BC5 too
        return s.view_format == 83 ? nullptr : "the game's normal texture is not two-channel BC5";
    case Role::Roughness: // BC4 holds one channel
        return s.view_format == 80 ? nullptr : "the game's roughness texture is not one-channel BC4";
    default: return "packed maps are not loadable in v1";
    }
}

class CacheIndex {
public:
    struct Hit {
        size_t record = 0, map = 0;
    };
    struct Refusal {
        std::string entry_key, map_id, why;
    };

    // Adds a read record. Its qualified maps become findable; the others are listed in refusals().
    // False (nothing added) when the entry is already present.
    bool Add(Record record)
    {
        for (const Record& r : records_)
            if (r.entry_key == record.entry_key) return false;
        const size_t ri = records_.size();
        for (size_t mi = 0; mi < record.maps.size(); ++mi) {
            const Map& m = record.maps[mi];
            if (const char* why = Qualified(m)) {
                refusals_.push_back({record.entry_key, m.id, why});
                continue;
            }
            const Source& s = m.source;
            // Two entries prepared from the same source: the first added stays (entries are added
            // in a stated order by the loader), the later one is listed.
            if (FindKey(s.content_sha256, s.resource_format, s.view_format, s.width, s.height, s.mip_count)) {
                refusals_.push_back({record.entry_key, m.id, "another entry already covers this game texture"});
                continue;
            }
            keys_.push_back({s.content_sha256, {s.resource_format, s.width, s.height, s.mip_count}, s.view_format, {ri, mi}});
        }
        records_.push_back(std::move(record));
        return true;
    }

    // The view formats worth a digest for a texture of this shape; empty for nearly all textures.
    std::vector<uint32_t> WantedViews(const TextureShape& shape) const
    {
        std::vector<uint32_t> out;
        for (const Key& k : keys_)
            if (k.shape == shape && std::find(out.begin(), out.end(), k.view_format) == out.end()) out.push_back(k.view_format);
        return out;
    }

    bool Find(std::string_view content_sha256, const TextureShape& shape, uint32_t view_format, Hit& out) const
    {
        const Key* k = FindKey(content_sha256, shape.resource_format, view_format, shape.width, shape.height, shape.mip_count);
        if (!k) return false;
        out = k->hit;
        return true;
    }

    // The maps of `record` that are findable: a record is shown as a unit, so the runtime waits
    // for all of these before any of them.
    size_t QualifiedMaps(size_t record) const
    {
        size_t n = 0;
        for (const Key& k : keys_)
            if (k.hit.record == record) ++n;
        return n;
    }

    const std::vector<Record>& records() const { return records_; }
    const std::vector<Refusal>& refusals() const { return refusals_; }
    size_t findable() const { return keys_.size(); }

private:
    struct Key {
        std::string digest;
        TextureShape shape;
        uint32_t view_format;
        Hit hit;
    };
    const Key* FindKey(std::string_view digest, uint32_t resource_format, uint32_t view_format, uint32_t w, uint32_t h, uint32_t mips) const
    {
        const TextureShape shape{resource_format, w, h, mips};
        for (const Key& k : keys_)
            if (k.view_format == view_format && k.shape == shape && k.digest == digest) return &k;
        return nullptr;
    }
    std::vector<Record> records_;
    std::vector<Key> keys_;
    std::vector<Refusal> refusals_;
};

} // namespace na::materials
