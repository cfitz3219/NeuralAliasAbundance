#pragma once
#include "host_dx12/j2/insertion_trace.hpp"
#include <algorithm>
#include <map>

namespace na::j2 {
// Learn from the order in which the game's recordings actually ran. A list
// object can change jobs, and the same drawing pipeline can serve both roles.
// Only defer a barrier seen repeatedly in earlier recordings of complete frames.
class SceneListRoles {
    struct Role {
        uint64_t middle = 0, terminal = 0, deferred = 0, retry_after = 0;
        unsigned observations = 0;
    };
    struct Span { uint64_t list; size_t begin, end; bool has_boundary; };
    std::map<std::pair<uint64_t,uint64_t>,Role> roles_;
    std::vector<Span> scene_;
    uint64_t color_ = 0, depth_ = 0;
    bool ordered_ = true;
    static constexpr uint64_t age = 120;
public:
    void Select(uint64_t color, uint64_t depth) {
        if (color == color_ && depth == depth_) return;
        color_ = color;
        depth_ = depth;
        roles_.clear();
        scene_.clear();
        ordered_ = true;
    }
    void OtherQueueWroteScene() { ordered_ = false; }
    void Submitted(uint64_t list, size_t begin, size_t end, bool has_boundary) {
        if (list && end > begin) scene_.push_back({list, begin, end, has_boundary});
    }
    bool Defer(uint64_t color, uint64_t depth, uint64_t list, uint64_t writer, uint64_t frame) {
        if (!ordered_ || color != color_ || depth != depth_) return false;
        auto it = roles_.find({list,writer});
        if (it == roles_.end()) return false;
        auto& role = it->second;
        if (role.observations < 2 || frame - role.middle > age ||
            (role.terminal && frame - role.terminal <= age) || frame <= role.retry_after) return false;
        role.deferred = frame;
        return true;
    }
    bool Close(const std::vector<ColorEvent>& events, bool complete, bool ran, uint64_t frame) {
        bool recovered = false;
        if (!ran || frame % age == 0) {
            for (auto it = roles_.begin(); it != roles_.end();) {
                auto& r = it->second;
                if (!ran && r.deferred && r.deferred == frame) {
                    r.retry_after = frame + age;
                    recovered = true;
                }
                if (frame - (std::max)(r.middle,r.terminal) > age) it = roles_.erase(it);
                else ++it;
            }
        }
        if (complete && ordered_ && !scene_.empty() && scene_.back().end <= events.size()) {
            for (const auto& s : scene_) {
                const bool terminal = &s == &scene_.back();
                if (!terminal && !scene_.back().has_boundary) continue;
                bool written = false;
                for (size_t i = s.begin; i < s.end; ++i) {
                    const auto& e = events[i];
                    if (e.write) written = true;
                    if (!written || e.op != Op::Barrier) continue;
                    const auto key = std::make_pair(s.list, e.producer);
                    auto it = roles_.find(key);
                    if (it == roles_.end()) {
                        if (roles_.size() >= 512) continue;
                        it = roles_.emplace(key, Role{}).first;
                    }
                    auto& role = it->second;
                    if (terminal) role.terminal = frame;
                    else if (role.middle != frame) {
                        if (frame - role.middle > age) role.observations = 0;
                        role.observations = (std::min)(2u,role.observations + 1);
                        role.middle = frame;
                    }
                }
            }
        }
        scene_.clear();
        ordered_ = true;
        return recovered;
    }
};
}
