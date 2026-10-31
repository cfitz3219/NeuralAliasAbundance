// Worker-side H2 reduction. Acquisition hypotheses only, never camera/HUD proof.
#pragma once
#include "capture_decoder.hpp"
#include "resource_formats.hpp"
#include <deque>
#include <map>
#include <set>

namespace na::discovery {
struct ExecutedResource {
    classifier::Window window;
    classifier::Observation observation;
    uint32_t format{};
};
class SubmissionEvidence {
public:
    struct Metrics {uint64_t executions{},accepted{},rejected{},evicted{},sequence_gaps{},unknown_pipeline{},unknown_view{};};
private:
    Metrics metrics_;
    using Key=std::pair<uint64_t,uint32_t>;
    struct Write {
        uint64_t first{},last{};
        uint32_t width{},height{},format{};
        bool depth{},color{},clear_known{},invalid{};
        double clear{};
    };
    struct Recording {
        uint64_t generation{},op{},pipeline{};
        bool began{},closed{},bad{};
        uint32_t width{},height{};
        std::vector<na_view_ref> targets;
        std::map<Key,Write> writes;
        std::set<uint64_t> invalidated;
    };
    struct Execution { uint64_t queue{},ticket{};std::vector<ExecutedResource> resources; };
    struct Pending { na_ev_queue_execute event{};std::vector<unsigned char> bytes; };
    uint64_t session_{},epoch_{},loss_{},foundation_loss_{},version_{};
    std::map<uint64_t,na_ev_resource_created> resources_;
    std::map<uint64_t,na_ev_pipeline_created> pipelines_;
    std::map<uint32_t,uint64_t> thread_sequences_;
    std::map<std::pair<uint64_t,uint64_t>,Recording> recordings_;
    std::deque<std::pair<uint64_t,uint64_t>> record_order_;
    std::deque<Pending> pending_;
    std::deque<Execution> history_;
    std::map<Key,ExecutedResource> latest_;
    static constexpr size_t max_records=4096,max_writes=256,max_history=256,max_resources=32768;
    template<class T> static T Load(const unsigned char* p) { T e{};std::memcpy(&e,p,sizeof(e));return e; }
    void LoseRecordings() { recordings_.clear();record_order_.clear();pending_.clear();history_.clear();latest_.clear(); }
    Recording& Record(uint64_t list,uint64_t generation) {
        const auto key=std::make_pair(list,generation);
        if(!recordings_.count(key)) {
            if(recordings_.size()==max_records) {recordings_.erase(record_order_.front());record_order_.pop_front();}
            record_order_.push_back(key);
        }
        return recordings_[key];
    }
    void WriteView(Recording& r,const na_view_ref& v,bool depth,bool color,bool clear,double value) {
        if(!classifier::Known(v.resource) || v.first_subresource==NA_UNKNOWN_U32) { ++metrics_.unknown_view;r.bad=true;return; }
        if(r.writes.size()==max_writes && !r.writes.count({v.resource,v.first_subresource})) {r.bad=true;return;}
        auto& w=r.writes[{v.resource,v.first_subresource}];
        if(!w.first) w.first=r.op;
        w.last=r.op;w.depth|=depth;w.color|=color;
        if(clear) {w.clear_known=true;w.clear=value;}
        else {
            // Mixed extents within a content version need subrect evidence.
            if(w.width && (w.width!=r.width || w.height!=r.height)) w.invalid=true;
            w.width=r.width;w.height=r.height;
        }
        w.format=v.format;
    }
    void Operation(const na_ev_list_op& e,const unsigned char* p) {
        if(!classifier::Known(e.list) || e.recording_generation==NA_UNKNOWN_U64) return;
        auto& r=Record(e.list,e.recording_generation);
        if(r.generation!=e.recording_generation) {r={};r.generation=e.recording_generation;}
        if(!r.began || r.closed || e.op_index!=r.op+1 ||
            (e.hdr.flags&(NA_EVF_METHOD_UNOBSERVED|NA_EVF_TRUNCATED|NA_EVF_ORDER_AMBIGUOUS))) r.bad=true;
        r.op=e.op_index;
        if(r.bad) return;
        switch(e.op) {
        case NA_OP_SET_PIPELINE:r.pipeline=e.a[0];break;
        case NA_OP_RS_VIEWPORT: {
            r.width=r.height=0;
            if(e.a[0]!=1) break;
            const auto x=Load<float>(p+e.payload_offset),y=Load<float>(p+e.payload_offset+4);
            const auto w=Load<float>(p+e.payload_offset+8),h=Load<float>(p+e.payload_offset+12);
            if(x==0 && y==0 && std::isfinite(w) && std::isfinite(h) && w>=1 && h>=1 &&
                w<=16384 && h<=16384 && std::floor(w)==w && std::floor(h)==h) {
                r.width=static_cast<uint32_t>(w);r.height=static_cast<uint32_t>(h);
            }
            break;
        }
        case NA_OP_OM_SET_RENDER_TARGETS:
            r.targets.clear();
            for(uint64_t i=0;i<e.a[0]+e.a[1];++i) r.targets.push_back(Load<na_view_ref>(p+e.payload_offset+i*e.payload_stride));
            break;
        case NA_OP_CLEAR_DSV:case NA_OP_CLEAR_RTV: {
            auto v=Load<na_view_ref>(p+e.payload_offset);
            if(e.op==NA_OP_CLEAR_DSV && !(e.a[0]&1)) break;
            const uint32_t bits=static_cast<uint32_t>(e.a[1]);float value{};std::memcpy(&value,&bits,4);
            WriteView(r,v,false,false,e.op==NA_OP_CLEAR_DSV,value);break;
        }
        case NA_OP_DRAW:case NA_OP_DRAW_INDEXED: {
            if(!e.a[0] || !e.a[1]) break;
            const auto ps=pipelines_.find(r.pipeline);
            // Initial-PSO omission cannot be guessed from the bound DSV.
            if(ps==pipelines_.end()) {++metrics_.unknown_pipeline;r.bad=true;break;}
            for(const auto& v:r.targets) {
                if(v.descriptor_kind==NA_DESC_RTV) WriteView(r,v,false,true,false,0);
                else if(v.descriptor_kind==NA_DESC_DSV && ps->second.depth_enable==1 && ps->second.depth_write==1)
                    WriteView(r,v,true,false,false,0);
            }
            break;
        }
        case NA_OP_COPY_RESOURCE:case NA_OP_COPY_BUFFER_REGION:case NA_OP_COPY_TEXTURE_REGION:case NA_OP_RESOLVE:
            // Copy lineage is not a draw-produced version; never silently keep
            // an earlier render result after its storage has been overwritten.
            if(r.invalidated.size()==max_writes) r.bad=true;
            else r.invalidated.insert(e.a[0]);
            break;
        case NA_OP_RESOURCE_BARRIER:
            for(uint64_t i=0;i<e.a[0];++i) {
                const auto b=Load<na_barrier>(p+e.payload_offset+i*e.payload_stride);
                if(b.barrier_type==1) { // aliasing revokes both incarnations
                    if(!b.resource || !b.resource_after) {r.bad=true;break;}
                    if(r.invalidated.size()+2>max_writes) {r.bad=true;break;}
                    r.invalidated.insert(b.resource);r.invalidated.insert(b.resource_after);
                }
            }
            break;
        case NA_OP_DISPATCH:case NA_OP_EXECUTE_INDIRECT:case NA_OP_EXECUTE_BUNDLE:
            r.bad=true;break; // UAV/indirect effects need their own reducer.
        default:
            if(e.op<NA_OP_SET_PIPELINE || e.op>NA_OP_RS_SCISSOR) r.bad=true;
            break;
        }
    }
    void Execute(const na_ev_queue_execute& e,const unsigned char* p) {
        ++metrics_.executions;
        Execution out{e.queue,e.queue_ticket,{}};
        bool clean=classifier::Known(e.queue) && classifier::Known(e.queue_ticket) && !e.contains_bundles &&
            !(e.hdr.flags&(NA_EVF_ORDER_AMBIGUOUS|NA_EVF_TRUNCATED|NA_EVF_METHOD_UNOBSERVED));
        std::map<Key,ExecutedResource> current;
        for(uint32_t i=0;i<e.list_count;++i) {
            const auto ref=Load<na_list_ref>(p+e.refs_offset+size_t(i)*e.ref_stride);
            const auto found=recordings_.find({ref.list,ref.recording_generation});
            if(found==recordings_.end() || found->second.generation!=ref.recording_generation ||
                !found->second.began || !found->second.closed || found->second.bad) {clean=false;break;}
            const auto& r=found->second;
            for(auto id:r.invalidated) {
                for(auto it=current.begin();it!=current.end();) it=it->first.first==id ? current.erase(it) : std::next(it);
            }
            for(const auto& entry:r.writes) {
                current.erase(entry.first);
                const auto resource=resources_.find(entry.first.first);
                if(resource==resources_.end() || r.invalidated.count(entry.first.first)) continue;
                const auto& w=entry.second;const auto& info=resource->second;
                if(w.invalid || !w.width || !w.height || (!w.depth && !w.color) || info.width>UINT32_MAX) continue;
                ExecutedResource value{};
                auto& window=value.window;auto& o=value.observation;
                window.scope={session_,epoch_,e.queue_ticket,e.queue,ref.list,ref.recording_generation};
                window.width=w.width;window.height=w.height;window.scene_end=r.op;
                // End of observed recording is an acquisition boundary. HUD
                // status and semantic scene boundary remain explicitly unknown.
                window.coverage_complete=window.order_unambiguous=window.loss_free=true;
                o.scope=window.scope;o.resource=entry.first.first;o.subresource=entry.first.second;
                o.content_version=++version_;o.width=static_cast<uint32_t>(info.width);o.height=info.height;
                value.format=(w.format && w.format!=NA_UNKNOWN_U32) ? w.format : info.format;o.format=ResourceFormat(value.format);
                o.alive=o.external_work=true;o.depth_write=w.depth;o.color_write=w.color;
                o.first_write=w.first;o.last_write=w.last;o.clear_known=w.clear_known;o.clear_value=w.clear;
                current[entry.first]=value;
            }
            if(current.size()>max_writes) {clean=false;break;}
        }
        if(clean) {
            metrics_.accepted+=current.size();
            for(auto& pair:current) {out.resources.push_back(pair.second);latest_[pair.first]=pair.second;}
            while(latest_.size()>max_resources) latest_.erase(latest_.begin());
        } else {
            ++metrics_.rejected;
            // Unknown writes may affect any retained candidate. Keep historical
            // completed identities only, and reacquire fresh candidates.
            latest_.clear();
        }
        history_.push_back(std::move(out));if(history_.size()>max_history) history_.pop_front();
    }
    void Drain() {
        for(auto it=pending_.begin();it!=pending_.end();) {
            bool ready=true;
            for(uint32_t i=0;i<it->event.list_count;++i) {
                const auto ref=Load<na_list_ref>(it->bytes.data()+it->event.refs_offset+size_t(i)*it->event.ref_stride);
                const auto found=recordings_.find({ref.list,ref.recording_generation});
                if(found==recordings_.end() || (!found->second.closed && !found->second.bad)) {ready=false;break;}
            }
            if(ready) {Execute(it->event,it->bytes.data());it=pending_.erase(it);}
            else ++it;
        }
    }
public:
    void Consume(const DecodedBatch& b,const na_observe_status& status) {
        if(session_!=status.session || epoch_!=status.epoch) {
            *this=SubmissionEvidence{};session_=status.session;epoch_=status.epoch;
            loss_=status.detail_dropped;foundation_loss_=status.foundational_dropped;
        }
        if(!b.valid || b.unknown_foundational || loss_!=status.detail_dropped || foundation_loss_!=status.foundational_dropped) {
            LoseRecordings();loss_=status.detail_dropped;foundation_loss_=status.foundational_dropped;
            if(!b.valid) return;
        }
        if(b.header.session!=session_ || b.header.epoch!=epoch_) return;
        for(const auto& item:b.events) {
            const auto* p=b.bytes.data()+item.offset;
            if(item.header.thread_seq && item.header.producer_thread) {
                if(thread_sequences_.size()==1024 && !thread_sequences_.count(item.header.producer_thread)) {
                    LoseRecordings();return;
                }
                auto& prior=thread_sequences_[item.header.producer_thread];
                if(prior && item.header.thread_seq!=prior+1) {++metrics_.sequence_gaps;LoseRecordings();}
                prior=item.header.thread_seq;
            }
            switch(item.header.kind) {
            case NA_EV_RESOURCE_CREATED: {
                const auto e=Load<na_ev_resource_created>(p);
                if(resources_.size()<max_resources || resources_.count(e.resource)) resources_[e.resource]=e;
                break;
            }
            case NA_EV_PIPELINE_CREATED: {
                const auto e=Load<na_ev_pipeline_created>(p);
                if(pipelines_.size()<max_records || pipelines_.count(e.pipeline)) pipelines_[e.pipeline]=e;
                break;
            }
            case NA_EV_OBJECT_RETIRED: {
                const auto id=Load<na_ev_object_retired>(p).object;resources_.erase(id);pipelines_.erase(id);
                for(auto& r:recordings_) if(r.first.first==id) r.second.bad=true;
                for(auto it=latest_.begin();it!=latest_.end();) it=it->first.first==id ? latest_.erase(it) : std::next(it);
                for(auto& h:history_) h.resources.erase(std::remove_if(h.resources.begin(),h.resources.end(),
                    [id](const ExecutedResource& v){return v.observation.resource==id;}),h.resources.end());
                break;
            }
            case NA_EV_LIST_RESET:case NA_EV_LIST_CLOSE: {
                const auto e=Load<na_ev_list_reset_close>(p);
                auto& r=Record(e.list,e.recording_generation);
                const bool ok=e.hresult>=0 && !(e.list_flags&NA_LISTF_RESET_FAILED) &&
                    !(e.hdr.flags&(NA_EVF_TRUNCATED|NA_EVF_METHOD_UNOBSERVED|NA_EVF_ORDER_AMBIGUOUS));
                if(item.header.kind==NA_EV_LIST_RESET) {r={};r.generation=e.recording_generation;r.began=ok;r.bad=!ok;}
                else {r.closed=ok;r.bad|=!ok || r.generation!=e.recording_generation;}
                break;
            }
            case NA_EV_LIST_OP:Operation(Load<na_ev_list_op>(p),p);break;
            case NA_EV_QUEUE_EXECUTE: {
                if(pending_.size()==max_history) {++metrics_.evicted;pending_.pop_front();latest_.clear();}
                pending_.push_back({Load<na_ev_queue_execute>(p),{p,p+item.header.size_bytes}});break;
            }
            case NA_EV_ORDER_AMBIGUITY_RANGE:case NA_EV_LOSS_MARK:LoseRecordings();break;
            default:break;
            }
        }
        Drain();
    }
    const std::map<Key,ExecutedResource>& Latest() const {return latest_;}
    const Metrics& Stats() const {return metrics_;}
    const ExecutedResource* Match(uint64_t queue,uint64_t ticket,uint64_t resource,uint32_t subresource) const {
        for(auto it=history_.rbegin();it!=history_.rend();++it) if(it->queue==queue && it->ticket==ticket) {
            for(const auto& v:it->resources) if(v.observation.resource==resource && v.observation.subresource==subresource) return &v;
            return nullptr;
        }
        return nullptr;
    }
};
} // namespace na::discovery
