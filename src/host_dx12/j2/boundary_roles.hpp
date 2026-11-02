#pragma once
#include "host_dx12/j2/insertion_trace.hpp"
#include <algorithm>
#include <map>
#include <tuple>

namespace na::j2 {
// Completed, submission-ordered frames distinguish repeated barriers from the
// SAME writer and identical end barriers in DIFFERENT scene segments. No NGX
// speculation/predication and no requirement for an observed SRV consumer.
class BoundaryRoles {
    using Key = std::tuple<uint64_t,uint64_t,uint32_t>; // list, writer, occurrence
    struct Role { uint64_t middle=0,end=0,deferred=0,retry=0; unsigned middles=0,ends=0; };
    struct Span { uint64_t list; size_t begin,end; };
    std::map<Key,Role> roles_;
    std::vector<Span> spans_;
    uint64_t color_=0,depth_=0;
    bool ordered_=true;
    static constexpr uint64_t age=120;
public:
    enum Verdict { Unknown, Middle, End };
    void Select(uint64_t c,uint64_t d) { if(c!=color_ || d!=depth_) { *this={};color_=c;depth_=d; } }
    void OtherQueueWroteScene() { ordered_=false; }
    void Submitted(uint64_t list,size_t begin,size_t end) { if(list && end>begin) spans_.push_back({list,begin,end}); }
    Verdict Judge(uint64_t list,uint64_t writer,uint32_t occurrence,uint64_t frame) {
        if(!ordered_ || !occurrence) return Unknown;
        auto it=roles_.find({list,writer,occurrence});if(it==roles_.end())return Unknown;
        auto& r=it->second;
        const bool middle=r.middles>=2 && frame-r.middle<=age;
        const bool end=r.ends>=3 && frame-r.end<=age;
        if(middle && (!r.end || frame-r.end>age) && frame>r.retry) {r.deferred=frame;return Middle;}
        if(end && (!r.middle || frame-r.middle>age))return End;
        return Unknown; // a pooled list/writer seen in both jobs is ambiguous
    }
    bool Close(const std::vector<ColorEvent>& events,bool complete,bool ran,uint64_t frame) {
        bool recovered=false;
        for(auto it=roles_.begin();it!=roles_.end();) {
            auto& r=it->second;
            if(!ran && r.deferred==frame) {r.retry=frame+age;recovered=true;}
            if(frame-(std::max)(r.middle,r.end)>age)it=roles_.erase(it);else ++it;
        }
        if(complete && ordered_ && !spans_.empty() && spans_.back().end<=events.size()) {
            size_t last_write=events.size();
            // A later colour writer need not bind the main depth (sky or a
            // separate scene segment). It still invalidates an earlier end.
            for(size_t i=0;i<events.size();++i)if(events[i].write)last_write=i;
            bool end_available=false;
            for(const auto& s:spans_)for(size_t i=s.begin;i<s.end;++i)if(i>last_write && events[i].op==Op::Barrier)end_available=true;
            for(const auto& s:spans_) {
                std::map<uint64_t,uint32_t> occurrences;
                bool wrote=false;
                for(size_t i=s.begin;i<s.end;++i) {
                    const auto& e=events[i];if(e.write)wrote=true;
                    if(!wrote || e.op!=Op::Barrier)continue;
                    const auto key=Key{s.list,e.producer,++occurrences[e.producer]};
                    auto it=roles_.find(key);
                    if(it==roles_.end()) {if(roles_.size()>=4096)continue;it=roles_.emplace(key,Role{}).first;}
                    auto& r=it->second;
                    if(i>last_write) {if(frame-r.end>age)r.ends=0;r.end=frame;r.ends=(std::min)(3u,r.ends+1);}
                    else if(end_available) {if(frame-r.middle>age)r.middles=0;r.middle=frame;r.middles=(std::min)(2u,r.middles+1);}
                }
            }
        }
        spans_.clear();ordered_=true;return recovered;
    }
};
}
