#pragma once
#include "host_dx12/j2/insertion_trace.hpp"
#include <algorithm>
#include <tuple>

namespace na::j2 {
// First real operation after a colour handoff. Descriptor membership is not
// required: GPU-indexed tables and indirect draws still have a PSO and root.
// Only completed, submitted frame sequences teach which entries were terminal.
class PostEntryProfile {
    using Key=std::tuple<uint64_t,uint64_t,uint64_t,uint32_t,uint32_t,uint32_t,bool>;
    struct Item { Key key; unsigned ends=0; bool middle=false; uint64_t frame=0; };
    std::vector<Item> items_;
    static Key key(const InsertionEvent& e) {
        const bool compute=e.op==Op::ComputeEntry || e.op==Op::IndirectComputeEntry;
        return {e.pipeline,e.root,e.destination,e.dst_w,e.dst_h,e.dst_fmt,compute};
    }
public:
    enum class Verdict { Unknown, Learning, Middle, Accepted };
    void Clear(){items_.clear();}
    size_t Size()const{return items_.size();}
    Verdict Judge(const InsertionEvent& e)const {
        if(!IsEntry(e.op) || !e.pipeline || !e.root)return Verdict::Unknown;
        const auto k=key(e);
        for(const auto& i:items_)if(i.key==k)return i.middle?Verdict::Middle:i.ends>=3?Verdict::Accepted:Verdict::Learning;
        return Verdict::Unknown;
    }
    bool Accepts(const InsertionEvent& e)const {return Judge(e)==Verdict::Accepted;}
    bool Ready()const {for(const auto& i:items_)if(i.ends>=3 && !i.middle)return true;return false;}
    void Observe(const std::vector<ColorEvent>& events,bool complete,uint64_t frame) {
        if(!complete)return;
        size_t last=events.size();
        for(size_t n=0;n<events.size();++n)if(events[n].write)last=n;
        if(last==events.size())return;
        std::vector<Key> seen;
        for(size_t n=0;n<events.size();++n) {
            const auto& e=events[n];
            if(!IsEntry(e.op) || !e.raw.pipeline || !e.raw.root)continue;
            const auto k=key(e.raw);
            auto it=std::find_if(items_.begin(),items_.end(),[&](const Item& i){return i.key==k;});
            if(it==items_.end()) {
                if(items_.size()==512) {
                    auto old=std::min_element(items_.begin(),items_.end(),[](const Item&a,const Item&b){return a.frame<b.frame;});
                    items_.erase(old);
                }
                items_.push_back({k,0,false,frame});it=items_.end()-1;
            }
            it->frame=frame;
            if(n<last)it->middle=true;
            else if(std::find(seen.begin(),seen.end(),k)==seen.end()) {it->ends=(std::min)(3u,it->ends+1);seen.push_back(k);}
        }
    }
};
}
