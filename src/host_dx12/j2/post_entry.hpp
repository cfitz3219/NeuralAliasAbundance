#pragma once
#include "host_dx12/j2/insertion_trace.hpp"
#include <algorithm>
#include <tuple>

namespace na::j2 {
// First real operation after a colour handoff. Descriptor membership is not
// required: GPU-indexed tables and indirect draws still have a PSO and root.
// Only completed, submitted frame sequences teach which entries were terminal.
//
// Two tiers (2026-11-06, SE5 40 s bursts): the consumer key alone (PSO, root, target, extent,
// format, compute) is marked Middle for good once it is seen mid-scene. SE5 merges its last two
// command lists every ~40 s; the terminal entry is then a consumer that, in the split shape, also
// runs mid-scene, so every frame of the burst was refused. The contextual tier adds where the
// entry sits in its recording: the pipeline of the colour's last write before it in that
// recording and its ordinal since that write (InsertionEvent::context/producer). A contextual key
// is Middle only if THAT occurrence was ever mid-scene, so a terminal occurrence in another shape
// is learned separately. Never an early placement: a context seen mid-scene stays refused, and the
// consumer-only tier still accepts whatever it accepted before (final geometry may vary).
class PostEntryProfile {
    using Key=std::tuple<uint64_t,uint64_t,uint64_t,uint32_t,uint32_t,uint32_t,bool,uint64_t,uint32_t>;
    struct Item { Key key; unsigned ends=0; bool middle=false; uint64_t frame=0; };
    std::vector<Item> base_, ctx_;
    static Key key(const InsertionEvent& e,bool contextual) {
        const bool compute=e.op==Op::ComputeEntry || e.op==Op::IndirectComputeEntry;
        return {e.pipeline,e.root,e.destination,e.dst_w,e.dst_h,e.dst_fmt,compute,contextual?e.producer:0,contextual?e.context|0x100u:0u};
    }
public:
    enum class Verdict { Unknown, Learning, Middle, Accepted };
    void Clear(){base_.clear();ctx_.clear();}
    size_t Size()const{return base_.size();}
    size_t ContextSize()const{return ctx_.size();}
    static Verdict judge(const std::vector<Item>& items,const Key& k){
        for(const auto& i:items)if(i.key==k)return i.middle?Verdict::Middle:i.ends>=3?Verdict::Accepted:Verdict::Learning;
        return Verdict::Unknown;
    }
    Verdict Judge(const InsertionEvent& e)const {
        if(!IsEntry(e.op) || !e.pipeline || !e.root)return Verdict::Unknown;
        const Verdict base=judge(base_,key(e,false));
        if(base==Verdict::Accepted)return base;
        return judge(ctx_,key(e,true))==Verdict::Accepted?Verdict::Accepted:base;
    }
    // Accepted only through the contextual tier (statistics).
    bool AcceptedByContext(const InsertionEvent& e)const {
        return judge(base_,key(e,false))!=Verdict::Accepted && judge(ctx_,key(e,true))==Verdict::Accepted;
    }
    bool Accepts(const InsertionEvent& e)const {return Judge(e)==Verdict::Accepted;}
    bool Ready()const {
        for(const auto* v:{&base_,&ctx_})for(const auto& i:*v)if(i.ends>=3 && !i.middle)return true;
        return false;
    }
    void Observe(const std::vector<ColorEvent>& events,bool complete,uint64_t frame) {
        if(!complete)return;
        size_t last=events.size();
        for(size_t n=0;n<events.size();++n)if(events[n].write)last=n;
        if(last==events.size())return;
        std::vector<Key> seen;
        // Each entry's context recomputed from the submission-ordered sequence, exactly as
        // InsertionTrace::SetEntryContext saw it live: per recording (list_start), since its last write.
        bool written=false;uint32_t ordinal=0;uint64_t producer=0;
        for(size_t n=0;n<events.size();++n) {
            const auto& e=events[n];
            if(e.list_start){written=false;ordinal=0;producer=0;}
            if(e.write){written=true;ordinal=0;producer=e.producer?e.producer:e.raw.pipeline;continue;}
            if(!IsEntry(e.op))continue;
            ++ordinal;
            if(!e.raw.pipeline || !e.raw.root)continue;
            auto raw=e.raw;raw.context=EntryContext(written,ordinal);raw.producer=written?producer:0;
            for(int tier=0;tier<2;++tier){
                auto& items=tier?ctx_:base_;
                const auto k=key(raw,tier!=0);
                auto it=std::find_if(items.begin(),items.end(),[&](const Item& i){return i.key==k;});
                if(it==items.end()) {
                    if(items.size()==512) {
                        auto old=std::min_element(items.begin(),items.end(),[](const Item&a,const Item&b){return a.frame<b.frame;});
                        items.erase(old);
                    }
                    items.push_back({k,0,false,frame});it=items.end()-1;
                }
                it->frame=frame;
                if(n<last)it->middle=true;
                else if(std::find(seen.begin(),seen.end(),k)==seen.end()) {it->ends=(std::min)(3u,it->ends+1);seen.push_back(k);}
            }
        }
    }
};
}
