#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <map>

namespace na::j2 {
// The game rotates upload buffers independently of Present. Remember OUR exact
// writes by resource identity and byte offset, not just the last frame's matrix.
// A game rewrite wins: bytes that no longer equal our write are never undone.
class JitterWrites {
    struct Patch { std::array<uint8_t, 64> original{}, written{}; };
    using Key = std::pair<uint64_t, uint64_t>;
    std::map<Key, Patch> patches_;
public:
    static constexpr size_t kMaxPatches = 65536;
    void Clear() { patches_.clear(); }
    uint32_t Normalize(uint64_t resource, uint64_t offset, uint8_t* bytes, size_t size) const {
        uint32_t count = 0;
        for (auto i = patches_.lower_bound({resource, offset}); i != patches_.end() && i->first.first == resource; ++i) {
            const uint64_t relative = i->first.second - offset;
            if (relative > size || size - relative < 64) break;
            if (!std::memcmp(bytes + relative, i->second.written.data(), 64)) {
                std::memcpy(bytes + relative, i->second.original.data(), 64);
                ++count;
            }
        }
        return count;
    }
    template<class Write>
    bool Apply(uint64_t resource, uint64_t offset, const uint8_t* original, const uint8_t* written, Write write) {
        const Key key{resource, offset};
        auto i = patches_.find(key);
        // Never evict a live patch then mistake its injected bytes for native data.
        if (i == patches_.end() && patches_.size() == kMaxPatches) return false;
        if (!write()) return false;
        auto& p = patches_[key];
        std::memcpy(p.original.data(), original, 64);
        std::memcpy(p.written.data(), written, 64);
        return true;
    }
};
}
