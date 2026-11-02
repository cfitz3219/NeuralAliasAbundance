#include "auto_scene.hpp"
#include "native_bindings.hpp"
#include "scoped_transfers.hpp"
#include "scene_frame.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/scale/settings_read_hook.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <cmath>
#include <tuple>
#include <deque>
#include <unordered_set>
#include <chrono>

namespace na::scale {
namespace {
using Microsoft::WRL::ComPtr;
using na::observe::NativeOperation;
using na::observe::TextureCall;
using na::observe::TextureKind;
using Clock=std::chrono::steady_clock;
struct Costs{std::atomic<uint64_t> wait_ns{0},hold_ns{0},max_hold_ns{0},worker_ns{0};};
class ControlLock{
    Costs& cost_;Clock::time_point start_=Clock::now();std::unique_lock<std::shared_mutex> lock_;Clock::time_point acquired_;
public:
    ControlLock(std::shared_mutex& mutex,Costs& cost):cost_(cost),lock_(mutex),acquired_(Clock::now()){
        cost_.wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(acquired_-start_).count(),std::memory_order_relaxed);}
    ~ControlLock(){const uint64_t ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-acquired_).count();
        cost_.hold_ns.fetch_add(ns,std::memory_order_relaxed);auto old=cost_.max_hold_ns.load(std::memory_order_relaxed);
        while(old<ns&&!cost_.max_hold_ns.compare_exchange_weak(old,ns,std::memory_order_relaxed)){} }
};
struct WorkerCost{Costs& cost;Clock::time_point start=Clock::now();~WorkerCost(){cost.worker_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(),std::memory_order_relaxed);}};

uint64_t signature(const NativeOperation& op,const std::vector<uint64_t>& members){
    na::observe::OperationKey key;using K=na::observe::OperationKey;
    K::add(key.state,op.pipeline_id);K::add(key.state,op.compute);const auto root=reinterpret_cast<uintptr_t>(op.root.Get());K::add(key.state,root);
    bool rebase=false;const auto member=[&](uint64_t id){return std::find(members.begin(),members.end(),id)!=members.end();};
    for(const auto& d:op.descriptors){key.descriptor(d.view,d.used);rebase|=d.used&&(member(d.view.resource)||member(d.view.counter));}
    for(const auto& c:op.constants){for(const auto& f:c.dimensions)key.constant(c.param,c.descriptor,c.resource_id,c.offset,f);rebase|=c.descriptor!=UINT_MAX;}
    for(const auto& d:op.colors){K::add(key.state,d.view.resource);K::add(key.state,d.view.rtv);}
    if(!op.compute){K::add(key.state,op.depth.view.resource);K::add(key.state,op.depth.view.dsv);}
    for(const auto& v:op.viewports)K::add(key.state,v);for(const auto& s:op.scissors)K::add(key.state,s);
    return key.finish(rebase);
}
uint64_t preparation_evidence(const NativeOperation& op){
    uint64_t h=1469598103934665603ull;using K=na::observe::OperationKey;
    for(const auto& c:op.constants){uint32_t state=0;const bool known=na::observe::KnownSubStateNow(c.resource_id,0,&state);K::add(h,known);K::add(h,state);
        for(const auto& f:c.dimensions){uint32_t value=0;const auto read=na::observe::ReadBuffer(c.resource_id,c.offset+f.offset,&value,4);K::add(h,read);K::add(h,value);}}
    return h;
}
struct TransferKey {
    TextureKind kind{};uint64_t source=0,destination=0;DXGI_FORMAT format{};
    bool operator<(const TransferKey& b)const{return std::tie(kind,source,destination,format)<std::tie(b.kind,b.source,b.destination,b.format);}
};
TransferKey transfer_key(const TextureCall& c){return {c.kind,c.source?na::observe::IdOf(c.source):0,
    c.destination?na::observe::IdOf(c.destination):c.target_id,c.format};}
struct MemberSeed {uint64_t id=0;ComPtr<ID3D12Resource> resource;Role role{};};
struct Built {
    std::shared_ptr<const SceneGroup> group;
    std::shared_ptr<SceneFrame> prototype;
    std::unordered_map<uint64_t,std::shared_ptr<const PreparedBindings>> bindings;
    std::map<TransferKey,std::shared_ptr<const PreparedTransfer>> transfers;
    std::unordered_map<uint64_t,std::string> failed;
    std::unordered_map<uint64_t,uint64_t> failed_evidence;
    std::vector<uint64_t> failed_keys;
    std::unordered_map<uint64_t,std::shared_ptr<std::atomic<uint64_t>>> last_seen;
    std::map<TransferKey,std::string> failed_transfers;
    uint64_t model=0,group_revision=0;int mode=0;bool optimal=false,optimal_available=false;
};
struct Run {
    std::shared_ptr<Built> built;std::shared_ptr<SceneFrame> frame;
    uint64_t epoch=0,color=0,depth=0;std::atomic<bool> aborted{false},seeded{false},redirected{false};
};
struct Bound {std::shared_ptr<Run> run;na::nr::native::RecordingLease lease;
    std::unordered_set<const GpuConstants*> constants;std::vector<const GpuConstants*> constant_tail;
    struct PrivateState{ID3D12Resource* resource=nullptr;D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON;};
    std::map<std::pair<uint64_t,UINT>,PrivateState> private_states;
    bool batch_open=false;
};
class Automatic;
struct Acquisition{Automatic* owner=nullptr;ID3D12GraphicsCommandList* list=nullptr;Bound* bound=nullptr;uint64_t list_id=0,recording=0;};
thread_local Acquisition acquisition;
struct Submission {std::shared_ptr<Run> run;ComPtr<ID3D12CommandQueue> queue;std::vector<ID3D12CommandList*> lists;};
thread_local std::shared_ptr<Submission> submission,reporting;
struct SelectionContext {
    uint64_t color=0,depth=0,model=0;
    std::map<uint64_t,MemberSeed> members;
    std::vector<uint64_t> key_members;
    std::map<uint64_t,std::pair<uint64_t,ComPtr<ID3D12Resource>>> copied_from;
    std::unordered_map<uint64_t,std::shared_ptr<const NativeOperation>> observed;
    std::unordered_map<uint64_t,std::shared_ptr<std::atomic<uint64_t>>> last_seen;
    size_t failure_probe=0;
    std::deque<uint64_t> expiry_queue;
    std::deque<uint64_t> pending;
    std::unordered_set<uint64_t> queued;
    std::unordered_set<uint64_t> native_ops;
    uint64_t group_revision=1,worker_revision=0,failed_group_revision=0,last_novel_epoch=0;
    int worker_mode=-1,failed_group_mode=-1;
    bool worker_optimal=false,failed_group_optimal=false;
    std::map<TransferKey,TransferSpec> observed_transfers;
    std::shared_ptr<Built> built,prepared;std::shared_ptr<Run> current;
    std::map<uint64_t,uint64_t> list_queues;
    ComPtr<ID3D12CommandQueue> execution_queue;
    bool unsupported=false;
};
class Automatic final:public SelectionContext,public BindingProvider,public na::observe::TextureOperationHook,public std::enable_shared_from_this<Automatic>{
public:
    std::shared_mutex mutex;bool enabled=false;uint64_t epoch=1,next_generation=1;
    Costs cost;
    std::atomic<uint64_t> operations{0},redirected{0},fast_hits{0},snapshots{0},redirected_frames{0},constant_copies{0},constant_reuses{0};
    std::atomic<uint32_t> open_batches{0};
    std::atomic<bool> current_recording{false};
    std::atomic<bool> work_pending{false};
    std::deque<SelectionContext> selections,retired_selections;
    uint64_t candidate_color=0,candidate_depth=0,candidate_since=0;
    std::map<std::pair<uint64_t,uint64_t>,Bound> bound;
    std::deque<Bound> retired_recordings;
    std::deque<std::shared_ptr<Run>> retired_runs;
    std::deque<std::shared_ptr<Built>> retired_profiles;
    AutomaticSceneStats stats;
    void note(const char* text){std::snprintf(stats.reason,sizeof stats.reason,"%s",text);}
    void acquired(ID3D12GraphicsCommandList* list,Bound& binding,uint64_t id,uint64_t recording){acquisition={this,list,&binding,id,recording};}
    void acquired(ID3D12GraphicsCommandList* list){const auto id=na::observe::IdOf(list),recording=na::observe::ListRecording(list);
        const auto found=bound.find({id,recording});if(found!=bound.end())acquired(list,found->second,id,recording);}
    bool RecordingOwnsPlans()const noexcept override{return true;}
    void OperationScope(ID3D12GraphicsCommandList* list,na::observe::ShaderScope& scope)noexcept override{
        if(acquisition.owner!=this||acquisition.list!=list||!acquisition.bound)return;
        scope.context=acquisition.bound->run;scope.opaque=acquisition.bound;scope.list_id=acquisition.list_id;scope.recording=acquisition.recording;
    }
    bool RecordConstants(ID3D12GraphicsCommandList* list,const std::vector<ConstantUse>& constants,const na::observe::ShaderScope& scope)noexcept override{
        if(!scope.opaque)return false;
        auto& recording=*static_cast<Bound*>(scope.opaque);
        for(const auto& use:constants){const auto* buffer=use.buffer.get();
            if(buffer->ReusableWithinRecording()){
                if(recording.constants.count(buffer)){++constant_reuses;continue;}
                if(!buffer->RecordBefore(list,use.source_state))return false;
                recording.constants.insert(buffer);recording.constant_tail.push_back(buffer);++constant_copies;
            }else{if(!buffer->RecordBefore(list,use.source_state))return false;++constant_copies;}}
        return true;
    }
    void RestoreConstants(ID3D12GraphicsCommandList* list,const std::vector<ConstantUse>& constants)noexcept override{
        for(auto use=constants.rbegin();use!=constants.rend();++use)if(!use->buffer->ReusableWithinRecording())use->buffer->RecordAfter(list);
    }
    bool RecordResources(ID3D12GraphicsCommandList* list,const std::vector<ResourceUse>& uses,const na::observe::ShaderScope& scope)noexcept override{
        if(!scope.opaque)return false;auto& recording=*static_cast<Bound*>(scope.opaque);
        D3D12_RESOURCE_BARRIER barriers[32]{};UINT count=0;
        const auto add=[&](D3D12_RESOURCE_BARRIER value){barriers[count++]=value;if(count==32){list->ResourceBarrier(count,barriers);count=0;}};
        for(const auto& use:uses){const auto* member=recording.run->built->group->Find(use.member);if(!member)return false;
            const auto format=member->reduced_desc.Format;
            const bool stencil=format==DXGI_FORMAT_R32G8X24_TYPELESS||format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT||format==DXGI_FORMAT_R24G8_TYPELESS||format==DXGI_FORMAT_D24_UNORM_S8_UINT;
            const UINT first=use.subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES?0:use.subresource;
            const UINT end=use.subresource==D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES?(stencil?2u:1u):first+1;
            bool unordered=false;
            for(UINT sub=first;sub<end;++sub){auto& state=recording.private_states[{use.member,sub}];state.resource=member->reduced.Get();
                if(state.state!=use.state){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition={state.resource,sub,state.state,use.state};add(barrier);state.state=use.state;}
                else unordered|=(use.state&D3D12_RESOURCE_STATE_UNORDERED_ACCESS)!=0;
            }
            if(unordered){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;barrier.UAV.pResource=member->reduced.Get();add(barrier);}
        }
        if(count)list->ResourceBarrier(count,barriers);
        if(!recording.batch_open){recording.batch_open=true;++open_batches;}return true;
    }
    void flush_resources(ID3D12GraphicsCommandList* list,Bound& recording){
        if(!recording.batch_open)return;const na::observe::HostWorkScope host;
        D3D12_RESOURCE_BARRIER barriers[32]{};UINT count=0;
        for(auto& entry:recording.private_states){auto& state=entry.second;if(state.state==D3D12_RESOURCE_STATE_COMMON)continue;
            auto& barrier=barriers[count++];barrier={};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={state.resource,entry.first.second,state.state,D3D12_RESOURCE_STATE_COMMON};state.state=D3D12_RESOURCE_STATE_COMMON;
            if(count==32){list->ResourceBarrier(count,barriers);count=0;}}
        if(count)list->ResourceBarrier(count,barriers);recording.batch_open=false;--open_batches;
    }
    void requested_native(ID3D12GraphicsCommandList* list,bool force=false){
        if(!force&&!current_recording.load())return;ControlLock lock(mutex,cost);
        const auto found=bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});
        if(found!=bound.end()){checkpoint(list,found->second.run);found->second.run->aborted=true;}
    }
    bool selected(uint64_t c,uint64_t d){return c&&d&&color==c&&depth==d;}
    bool select(uint64_t c,uint64_t d){
        if(selected(c,d)){candidate_color=candidate_depth=candidate_since=0;return true;}
        if(candidate_color!=c||candidate_depth!=d){candidate_color=c;candidate_depth=d;candidate_since=epoch;return false;}
        if(epoch-candidate_since<2)return false;
        SelectionContext previous=std::move(static_cast<SelectionContext&>(*this));
        const auto cached=std::find_if(selections.begin(),selections.end(),[&](const auto& s){return s.color==c&&s.depth==d;});
        if(cached!=selections.end()){static_cast<SelectionContext&>(*this)=std::move(*cached);selections.erase(cached);}
        else{static_cast<SelectionContext&>(*this)=SelectionContext{};color=c;depth=d;}
        if(current)retired_runs.push_back(std::move(current));if(previous.color&&previous.depth)selections.push_back(std::move(previous));
        // Current + one archived scene. Release discarded COM references on
        // the control worker, never while a recording thread holds this lock.
        while(selections.size()>1){retired_selections.push_back(std::move(selections.front()));selections.pop_front();}
        candidate_color=candidate_depth=candidate_since=0;
        const auto latest=prepared?prepared:built;
        work_pending.store(!latest||latest->model!=model||latest->group_revision!=group_revision||!pending.empty()||
            latest->mode!=static_cast<int>(na::settings::Get().j2_scale_mode.load()));
        if(latest&&latest->group){stats.generation=latest->group->Generation();stats.render_w=latest->group->Render().width;stats.render_h=latest->group->Render().height;
            stats.out_w=latest->group->Output().width;stats.out_h=latest->group->Output().height;stats.members=UINT(members.size());stats.plans=UINT(latest->bindings.size());
            stats.failed_plans=latest->failed.size()+latest->failed_transfers.size();note("Using the scene's previously prepared rendering path.");}
        else note("Identifying the scene's rendering operations.");
        return true;
    }
    bool member(uint64_t id,ID3D12Resource* resource,Role role){
        if(!id||!resource||members.count(id))return false;
        const auto d=resource->GetDesc();if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1)return false;
        MemberSeed value;value.id=id;value.resource=resource;value.role=role;members.emplace(id,std::move(value));key_members.push_back(id);++model;++group_revision;work_pending.store(true);return true;
    }
    void dependency(uint64_t id){
        std::vector<uint64_t> chain;
        while(!members.count(id)&&chain.size()<32){
            if(std::find(chain.begin(),chain.end(),id)!=chain.end())return;
            const auto copy=copied_from.find(id);if(copy==copied_from.end())return;chain.push_back(id);id=copy->second.first;
        }
        if(!members.count(id))return;const auto role=members.at(id).role==Role::Depth?Role::Depth:Role::Intermediate;
        for(auto it=chain.rbegin();it!=chain.rend();++it)member(*it,copied_from.at(*it).second.Get(),role);
    }
    bool bind(ID3D12GraphicsCommandList* list,const std::shared_ptr<Run>& run){
        const auto key=std::make_pair(na::observe::IdOf(list),na::observe::ListRecording(list));if(!key.first||!key.second)return false;
        auto found=bound.find(key);if(found!=bound.end())return found->second.run==run;
        for(auto i=bound.begin();i!=bound.end();)if(i->second.lease.Retired()){
            retired_recordings.push_back(std::move(i->second));i=bound.erase(i);
        }else++i;
        if(bound.size()>=1024)return false;
        auto lease=na::nr::native::RetainRecording(list,run);if(!lease)return false;
        bound.emplace(key,Bound{run,std::move(lease)});return true;
    }
    std::shared_ptr<Run> run_for(ID3D12GraphicsCommandList* list){
        const auto key=std::make_pair(na::observe::IdOf(list),na::observe::ListRecording(list));
        const auto found=bound.find(key);if(found!=bound.end())return found->second.run;
        if(unsupported||work_pending.load()||!built||built->model!=model||epoch-last_novel_epoch<3||!built->failed.empty()||!built->failed_transfers.empty()||built->bindings.empty()||!na::j2::Enabled()||na::j2::Paused())return {};
        const auto q=list_queues.find(key.first);
        if(!execution_queue||q==list_queues.end()||q->second!=na::observe::IdOf(execution_queue.Get()))return {};
        if(!current||current->epoch!=epoch){
            if(built->mode!=static_cast<int>(na::settings::Get().j2_scale_mode.load()))return {};
            current=std::make_shared<Run>();current->built=built;current->frame=built->prototype->NewFrame();current->epoch=epoch;current->color=color;current->depth=depth;++stats.frames;
            current_recording=true;
            char text[192];std::snprintf(text,sizeof text,"Scene rendering at %u x %u; DLSS status is shown below.",built->group->Render().width,built->group->Render().height);note(text);
        }
        if(!bind(list,current))return {};return current;
    }
    std::vector<FrameMemberState> states(ID3D12GraphicsCommandList* list,const std::shared_ptr<Run>& run,bool seed=false){
        std::vector<FrameMemberState> result;
        for(const auto& m:run->built->group->Members()){
            const bool stencil=m.role==Role::Depth&&m.native_desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS;
            const UINT first=seed&&stencil?1:0,count=stencil&&!seed?2:first+1;
            for(UINT sub=first;sub<count;++sub){uint32_t state=0;
                // An absent local barrier is inherited from earlier GPU lists,
                // not from whichever CPU thread updated global evidence last.
                // The frame also has the original typed use of this recording.
                if(na::observe::ListStateSub(list,m.id,sub,&state)==na::observe::StateLookup::Known){
                    FrameMemberState value{m.id,static_cast<D3D12_RESOURCE_STATES>(state)};value.native_planes=1u<<sub;result.push_back(value);}}
        }
        return result;
    }
    bool checkpoint(ID3D12GraphicsCommandList* list,const std::shared_ptr<Run>& run){
        if(!run)return true;
        const auto found=bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});if(found!=bound.end())flush_resources(list,found->second);
        if(!run->frame->HasPending(list))return true;
        if(!run->frame->Checkpoint(list,states(list,run)))return false;
        ++stats.checkpoints;return true;
    }
    void abort(ID3D12GraphicsCommandList* list,const std::shared_ptr<Run>& run,const char* why){
        if(run&&!run->aborted){checkpoint(list,run);run->aborted=true;}stats.active=false;++stats.misses;note(why);
    }
    std::shared_ptr<const PreparedBindings> Acquire(ID3D12GraphicsCommandList* list,const na::observe::ShaderCall& call)noexcept override{
        acquisition={};
        if(na::settings::Get().j2_scale_mode.load()==0){requested_native(list);return {};}
        uint64_t d=0,c=0;na::observe::GetLiveSelection(&d,&c);if(!c||!d){requested_native(list);return {};}
        const bool compute=call.kind==na::observe::ShaderKind::Dispatch;
        na::observe::NativeOperationKey stamp;bool known=false;
        {
            std::shared_lock<std::shared_mutex> lock(mutex);if(!enabled||!selected(c,d)){lock.unlock();requested_native(list);return {};}
            known=na::observe::ListNativeOperationKey(list,compute,key_members.data(),key_members.size(),stamp);
            if(!known&&(!stamp.list||(!stamp.recording&&!na::observe::ListRecording(list))))return {}; // no observed executable recording
            if(known){const auto binding=bound.find({stamp.list,stamp.recording});
                if(binding!=bound.end()){const auto& run=binding->second.run;
                    if(run->epoch==epoch&&!run->aborted&&run->seeded){const auto plan=run->built->bindings.find(stamp.key);
                        if(plan!=run->built->bindings.end()){const auto seen=run->built->last_seen.find(stamp.key);if(seen!=run->built->last_seen.end())seen->second->store(epoch,std::memory_order_relaxed);
                            acquired(list,binding->second,stamp.list,stamp.recording);++operations;++fast_hits;return plan->second;}}
                }
                const bool learning=unsupported||work_pending.load()||!built||built->model!=model||epoch-last_novel_epoch<3||
                    !built->failed.empty()||!built->failed_transfers.empty();
                if(observed.count(stamp.key)&&(learning||(binding!=bound.end()&&binding->second.run->aborted))){
                    const auto seen=last_seen.find(stamp.key);if(seen!=last_seen.end())seen->second->store(epoch,std::memory_order_relaxed);++operations;
                    const bool has_run=binding!=bound.end();lock.unlock();if(has_run)requested_native(list,true);return {};}
            }
        }
        if(known){
            ControlLock lock(mutex,cost);if(!selected(c,d))return {};
            auto run=run_for(list);
            if(native_ops.count(stamp.key)){++operations;if(run&&!checkpoint(list,run))abort(list,run,"A native consumer could not restore its current scene.");return {};}
            const auto latest=prepared?prepared:built;
            if(latest&&latest->group_revision==group_revision&&latest->failed.count(stamp.key)){++operations;return {};}
            if(run&&!run->aborted){const auto plan=run->built->bindings.find(stamp.key);
                if(plan!=run->built->bindings.end()){
                    if(!run->seeded){run->frame->UpdateStates(states(list,run,true),true);
                        run->frame->ObserveNativeUses(list,plan->second->Uses());
                        if(!run->frame->SeedHistories(list)){abort(list,run,"The scene's initial contents are not available yet.");return {};}run->seeded=true;}
                    const auto seen=run->built->last_seen.find(stamp.key);if(seen!=run->built->last_seen.end())seen->second->store(epoch,std::memory_order_relaxed);
                    acquired(list);++operations;++fast_hits;return plan->second;
                }
            }
        }
        ++snapshots;
        NativeOperation op;const bool have=na::observe::ListNativeOperation(list,call.kind==na::observe::ShaderKind::Dispatch,op);
        ControlLock lock(mutex,cost);if(!enabled||!selected(c,d))return {};++operations;
        auto run=run_for(list);
        if(!have){if(run)abort(list,run,"An operation's native bindings are not yet available.");return {};}
        bool scene=!op.compute&&op.depth.view.resource==d;
        if(!scene&&!op.compute)for(const auto& rt:op.colors)scene|=rt.view.resource==c;
        bool reads_depth=false,buffer_outputs=true,has_buffer_output=false;
        for(const auto& v:op.descriptors)if(v.used){reads_depth|=v.view.kind==NA_DESC_SRV&&v.view.resource==d;
            if(v.view.kind==NA_DESC_UAV&&v.resource){const bool buffer=v.resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;buffer_outputs&=buffer;has_buffer_output|=buffer;}}
        scene|=op.compute&&reads_depth&&buffer_outputs&&has_buffer_output;
        if(!scene){if(known&&native_ops.size()<4096)native_ops.insert(stamp.key);
            if(run&&!checkpoint(list,run))abort(list,run,"A native consumer could not restore its current scene.");return {};}
        ComPtr<ID3D12Resource> native_color,native_depth;
        if(members.count(c))native_color=members.at(c).resource;else native_color.Attach(na::observe::AcquireResource(c));
        if(members.count(d))native_depth=members.at(d).resource;else native_depth.Attach(na::observe::AcquireResource(d));
        if(!native_color||!native_depth)return {};
        const auto native_size=native_color->GetDesc();member(c,native_color.Get(),Role::Color);member(d,native_depth.Get(),Role::Depth);
        // A copied full-size picture is a scene dependency only when scene
        // work actually binds it. Presentation copies are native consumers.
        for(const auto& view:op.descriptors)if(view.used&&view.view.kind==NA_DESC_SRV)dependency(view.view.resource);
        for(const auto& rt:op.colors)if(rt.resource){const auto size=rt.resource->GetDesc();
            if(size.Width==native_size.Width&&size.Height==native_size.Height)member(rt.view.resource,rt.resource.Get(),rt.view.resource==c?Role::Color:Role::Intermediate);}
        const uint64_t key=signature(op,key_members);
        const auto seen=last_seen.find(key);if(seen!=last_seen.end())seen->second->store(epoch,std::memory_order_relaxed);
        else last_seen.emplace(key,std::make_shared<std::atomic<uint64_t>>(epoch));
        if(!observed.count(key)&&observed.size()<16384){observed.emplace(key,std::make_shared<const NativeOperation>(std::move(op)));expiry_queue.push_back(key);++model;
            pending.push_back(key);queued.insert(key);work_pending.store(true);last_novel_epoch=epoch;}
        if(!run||run->aborted)return {};
        const auto learned=observed.find(key);if(learned==observed.end()){abort(list,run,"The bounded scene plan cache is full.");return {};}
        for(const auto& descriptor:learned->second->descriptors)if(descriptor.used&&members.count(descriptor.view.resource)&&!run->built->group->Find(descriptor.view.resource)){
            abort(list,run,"Preparing a newly observed scene dependency.");return {};
        }
        const auto plan=run->built->bindings.find(key);
        if(plan==run->built->bindings.end()){++stats.unknown_operations;abort(list,run,"Preparing a newly observed scene operation.");return {};}
        if(!run->seeded){run->frame->UpdateStates(states(list,run,true),true);
            run->frame->ObserveNativeUses(list,plan->second->Uses());
            if(!run->frame->SeedHistories(list)){abort(list,run,"The scene's initial contents are not available yet.");return {};}run->seeded=true;}
        acquired(list);return plan->second;
    }
    void Refused(ID3D12GraphicsCommandList* list,const na::observe::ShaderCall&,const char* why)noexcept override{
        ControlLock lock(mutex,cost);const auto found=bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});
        ++stats.binding_failures;unsupported=true;
        abort(list,found==bound.end()?nullptr:found->second.run,why);
    }
    void AppliedScope(ID3D12GraphicsCommandList*,const na::observe::ShaderCall&,const std::shared_ptr<const PreparedBindings>& plan,const na::observe::ShaderScope& scope)noexcept override{
        const auto run=std::static_pointer_cast<Run>(scope.context);
        if(!run)return; // an already-recorded draw must be marked even if another list just aborted
        run->frame->Applied(scope.list_id,scope.recording,plan->Uses());++redirected;run->redirected=true;
    }
    void Closing(ID3D12GraphicsCommandList* list)noexcept override{
        ControlLock lock(mutex,cost);const auto found=bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});
        if(found!=bound.end())flush_resources(list,found->second);
        if(found!=bound.end()){for(const auto* buffer:found->second.constant_tail)buffer->RecordAfter(list);found->second.constant_tail.clear();found->second.constants.clear();}
        if(found!=bound.end()&&!checkpoint(list,found->second.run))abort(list,found->second.run,"A scene list could not publish its full-size dependencies.");
    }
    void NativeBoundary(ID3D12GraphicsCommandList* list)noexcept override{
        ControlLock lock(mutex,cost);const auto found=bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});
        if(found!=bound.end()){unsupported=true;++stats.native_boundaries;abort(list,found->second.run,"The scene uses an indirect, bundle or render-pass operation; using its current native picture.");}
    }
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList* list,const TextureCall& call)noexcept override{
        if(na::settings::Get().j2_scale_mode.load()==0)return {};
        uint64_t d=0,c=0;na::observe::GetLiveSelection(&d,&c);
        const auto key=transfer_key(call);na::observe::OperationDescriptor descriptor;
        if(call.target.ptr)na::observe::NativeDescriptor(call.target,descriptor);
        ControlLock lock(mutex,cost);if(!enabled||!selected(c,d))return {};
        if((call.kind==TextureKind::Copy||call.kind==TextureKind::CopyRegion||call.kind==TextureKind::Resolve)&&(members.count(key.source)||copied_from.count(key.source))&&call.destination){
            const auto a=call.source->GetDesc(),b=call.destination->GetDesc();
            if(a.Width==b.Width&&a.Height==b.Height&&(copied_from.size()<256||copied_from.count(key.destination)))copied_from[key.destination]={key.source,call.destination};
        }
        if(members.count(key.destination)&&(key.source==0||members.count(key.source))&&!observed_transfers.count(key)){
            TransferSpec value;value.kind=key.kind;value.source=key.source;value.destination=key.destination;value.resolve_format=key.format;
            value.color=descriptor.view.rtv;value.depth=descriptor.view.dsv;
            if(!descriptor.view.explicit_view&&descriptor.resource){value.color.Format=value.depth.Format=descriptor.resource->GetDesc().Format;
                value.color.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;value.depth.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;}
            observed_transfers.emplace(key,value);++model;work_pending.store(true);last_novel_epoch=epoch;
        }
        auto run=run_for(list);if(!run||run->aborted)return {};
        if(!run->built->group->Find(key.destination)&&!run->built->group->Find(key.source))return {};
        // Publish pending scene writes before the native copy reads them. The
        // native operation still runs, keeping full-size clear/copy semantics.
        if(!checkpoint(list,run)&&run->seeded){abort(list,run,"A scene transfer could not publish its input.");return {};}
        const auto transfer=run->built->transfers.find(key);
        if(transfer==run->built->transfers.end()){
            if(run->built->group->Find(key.destination))abort(list,run,"Preparing a newly observed scene transfer.");return {};
        }
        const char* why=nullptr;
        if(!transfer->second->Record(list,call,&why)){++stats.transfer_failures;unsupported=true;abort(list,run,why?why:"The scene transfer changed.");return {};}
        if(call.kind==TextureKind::ClearDepth&&(call.flags&D3D12_CLEAR_FLAG_STENCIL))run->frame->StencilCleared(key.destination);
        return {na::observe::ShaderAction::Forward,run};
    }
    void After(ID3D12GraphicsCommandList* list,const TextureCall& call,na::observe::ShaderScope& scope)noexcept override{
        const auto run=std::static_pointer_cast<Run>(scope.state);if(!run)return;
        ControlLock lock(mutex,cost);const auto key=transfer_key(call);
        run->frame->Written(key.destination,list);run->frame->NativeCurrent(key.destination,list);
    }
};
std::shared_ptr<Automatic>& automatic(){static auto* value=new std::shared_ptr<Automatic>();return *value;}
na::j2::PhysicalLookup lookup(void*,uint64_t color,uint64_t depth,ID3D12GraphicsCommandList* list,std::shared_ptr<const na::j2::PhysicalInput>* out){
    auto a=automatic();if(!a||!out)return na::j2::PhysicalLookup::None;out->reset();std::shared_ptr<Run> run;
    {ControlLock lock(a->mutex,a->cost);
        if(list){const auto found=a->bound.find({na::observe::IdOf(list),na::observe::ListRecording(list)});if(found!=a->bound.end()){
            a->flush_resources(list,found->second);run=found->second.run;}}
        else if(submission)run=submission->run;
        if(!run||run->aborted||run->color!=color||run->depth!=depth)return na::j2::PhysicalLookup::None;
        if(submission)reporting=submission;else{reporting=std::make_shared<Submission>();reporting->run=run;}
    }
    if(list){std::vector<FrameMemberState> current;
        for(const auto& member:run->built->group->Members()){
            uint32_t state=0;const auto evidence=na::observe::ListStateSub(list,member.id,0,&state);
            if(evidence==na::observe::StateLookup::Known||(evidence==na::observe::StateLookup::Absent&&na::observe::KnownSubStateInCurrentSubmission(member.id,0,&state)))
                current.push_back({member.id,static_cast<D3D12_RESOURCE_STATES>(state)});
        }
        run->frame->UpdateStates(current);
    }
    return run->frame->Input(out);
}
void outcome(void*,uint64_t generation,na::j2::PhysicalOutcome result,const char*){
    auto report=std::move(reporting);if(report&&report->run&&report->run->built->group->Generation()==generation)report->run->frame->Outcome(result);
}
}
void ConfigureAutomaticScene(bool enabled){
    if(!enabled)return;
    auto a=std::make_shared<Automatic>();a->enabled=true;a->stats.enabled=true;a->note("Waiting for the game's scene selection.");automatic()=a;
    na::observe::SetOperationSnapshots(true);na::observe::SetRasterStateTracking(true);
    na::observe::SetShaderOperationHook(na::observe::ShaderLane::Bindings,std::make_shared<BindingHook>(a));
    na::observe::SetTextureOperationHook(a);na::j2::SetPhysicalInputProvider({&lookup,&outcome,nullptr,true});
}
void PollAutomaticScene(){
    auto a=automatic();if(!a)return;
    WorkerCost measured{a->cost};
    const bool optimal_available=na::dlss::GetCapabilities().optimal_settings;
    std::map<uint64_t,MemberSeed> members;std::unordered_map<uint64_t,std::shared_ptr<const NativeOperation>> observed;
    std::vector<std::shared_ptr<const NativeOperation>> retired_requests;
    std::vector<uint64_t> expired;
    std::unordered_map<uint64_t,std::shared_ptr<std::atomic<uint64_t>>> usage;
    struct Probe{uint64_t key,evidence;std::shared_ptr<const NativeOperation> request;};std::vector<Probe> probes;
    std::deque<SelectionContext> retired_selections;
    std::deque<Bound> retired_recordings;
    std::deque<std::shared_ptr<Run>> retired_runs;
    std::deque<std::shared_ptr<Built>> retired_profiles;
    std::map<TransferKey,TransferSpec> transfers;std::shared_ptr<Built> previous;uint64_t model=0,color=0,depth=0,generation=0,group_revision=0;int mode=0;
    {ControlLock lock(a->mutex,a->cost);mode=static_cast<int>(na::settings::Get().j2_scale_mode.load());
        retired_selections.swap(a->retired_selections);
        retired_recordings.swap(a->retired_recordings);retired_runs.swap(a->retired_runs);retired_profiles.swap(a->retired_profiles);
        // Material/streaming resources belong to executable recordings, not an
        // immortal discovery cache. Evicted plans remain alive through leases.
        const size_t sweep=std::min(size_t(16),a->expiry_queue.size());
        for(size_t i=0;i<sweep;++i){const auto key=a->expiry_queue.front();a->expiry_queue.pop_front();const auto seen=a->last_seen.find(key);
            if(seen==a->last_seen.end())continue;
            if(a->epoch-seen->second->load(std::memory_order_relaxed)>240){const auto old=a->observed.find(key);
                if(old!=a->observed.end()){retired_requests.push_back(std::move(old->second));a->observed.erase(old);}expired.push_back(key);a->last_seen.erase(seen);
            }else a->expiry_queue.push_back(key);}
        if(!a->enabled||!a->color||!a->depth||a->members.size()<2||mode<=0){if(mode<=0){
            if(a->prepared)retired_profiles.push_back(std::move(a->prepared));a->failed_group_revision=0;a->worker_revision=0;}a->work_pending.store(false);return;}
        const auto latest=a->prepared?a->prepared:a->built;
        if(a->failed_group_revision==a->group_revision&&a->failed_group_mode==mode&&a->failed_group_optimal==optimal_available){a->work_pending.store(false);return;}
        const bool current_basis=latest&&latest->mode==mode&&latest->group_revision==a->group_revision&&latest->optimal_available==optimal_available;
        if(!latest||a->worker_revision!=a->group_revision||a->worker_mode!=mode||a->worker_optimal!=optimal_available){
            a->pending.clear();a->queued.clear();
            for(const auto& op:a->observed){a->pending.push_back(op.first);a->queued.insert(op.first);}
            a->worker_revision=a->group_revision;a->worker_mode=mode;a->worker_optimal=optimal_available;
        }
        // Only shared request pointers cross the control lock; descriptor
        // vectors/resources are immutable and prepared outside it.
        constexpr size_t budget=16;
        while(!a->pending.empty()&&observed.size()<budget){const auto key=a->pending.front();a->pending.pop_front();a->queued.erase(key);
            const auto request=a->observed.find(key);if(request==a->observed.end())continue;
            if(current_basis&&(latest->bindings.count(key)||latest->failed.count(key)))continue;
            observed.insert(*request);
            const auto seen=a->last_seen.find(key);if(seen!=a->last_seen.end())usage.insert(*seen);
        }
        for(const auto& transfer:a->observed_transfers){
            if(current_basis&&(latest->transfers.count(transfer.first)||latest->failed_transfers.count(transfer.first)))continue;
            if(transfers.size()+observed.size()>=budget)break;transfers.insert(transfer);
        }
        if(current_basis&&!latest->failed_keys.empty()&&observed.size()+transfers.size()<budget){
            const auto start=a->failure_probe%latest->failed_keys.size();
            const auto probe_budget=budget-observed.size()-transfers.size();
            for(size_t i=0;i<std::min(probe_budget,latest->failed_keys.size());++i){const auto key=latest->failed_keys[(start+i)%latest->failed_keys.size()];
                const auto request=a->observed.find(key),seen=a->observed.end();const auto fingerprint=latest->failed_evidence.find(key);
                if(request!=seen&&fingerprint!=latest->failed_evidence.end())probes.push_back({key,fingerprint->second,request->second});}
            a->failure_probe+=probe_budget;
        }
        if(current_basis&&observed.empty()&&transfers.empty()&&probes.empty()&&expired.empty()){a->work_pending.store(false);return;}
        members=a->members;previous=latest;model=a->model;color=a->color;depth=a->depth;group_revision=a->group_revision;
    }
    std::vector<uint64_t> retry;
    for(const auto& probe:probes)if(preparation_evidence(*probe.request)!=probe.evidence){observed.emplace(probe.key,probe.request);retry.push_back(probe.key);}
    if(previous&&previous->group_revision==group_revision&&previous->mode==mode&&previous->optimal_available==optimal_available&&observed.empty()&&transfers.empty()&&expired.empty()){
        ControlLock lock(a->mutex,a->cost);if(a->color==color&&a->depth==depth&&a->model==model)a->work_pending.store(!a->pending.empty());return;}
    const na::observe::HostWorkScope host;
    ComPtr<ID3D12Device> device;if(FAILED(members.at(color).resource->GetDevice(IID_PPV_ARGS(&device))))return;
    const auto native=members.at(color).resource->GetDesc();const Extent output{UINT(native.Width),native.Height};
    const double fraction=FractionForMode(mode);Extent render{UINT(std::max(1.0,std::round(output.width*fraction))),UINT(std::max(1.0,std::round(output.height*fraction)))};
    const na::dlss::Quality qualities[]={na::dlss::Quality::DLAA,na::dlss::Quality::MaxQuality,na::dlss::Quality::Balanced,na::dlss::Quality::MaxPerformance,na::dlss::Quality::UltraPerformance};
    na::dlss::OptimalSettings optimal;
    const bool optimal_used=mode>=1&&mode<=4&&na::dlss::IsBoundTo(device.Get())&&na::dlss::QueryOptimal(output.width,output.height,qualities[mode],&optimal)&&optimal.valid;
    if(optimal_used)
        render={optimal.render_w,optimal.render_h};
    auto next=std::make_shared<Built>();next->mode=mode;next->model=model;next->group_revision=group_revision;next->optimal=optimal_used;next->optimal_available=optimal_available;std::string error;
    bool same=previous&&previous->mode==mode&&previous->group->Render()==render&&previous->group->Output()==output&&previous->group->Members().size()==members.size();
    if(same)for(const auto& m:members)same&=previous->group->Find(m.first)!=nullptr;
    if(same){next->group=previous->group;next->prototype=previous->prototype;
        next->bindings=previous->bindings;next->failed=previous->failed;next->failed_evidence=previous->failed_evidence;next->last_seen=previous->last_seen;
        next->transfers=previous->transfers;next->failed_transfers=previous->failed_transfers;}
    else{
        {ControlLock lock(a->mutex,a->cost);generation=a->next_generation++;}
        std::vector<MemberRequest> requests;for(const auto& value:members)requests.push_back({value.first,value.second.resource.Get(),value.second.role,render});
        next->group=SceneGroup::Create(device.Get(),generation,output,render,requests,error);
        if(next->group){FrameSpec frame;frame.group=next->group;frame.color=color;frame.depth=depth;
            for(const auto& m:members){const auto state=m.second.role==Role::Depth?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
                const bool transfer_only=(m.second.resource->GetDesc().Flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))==0;
                frame.members.push_back({m.first,state,false,state,transfer_only});}
            next->prototype=SceneFrame::Create(device.Get(),std::move(frame),error);}
    }
    if(next->prototype){
        for(const auto key:expired){next->bindings.erase(key);next->failed.erase(key);next->failed_evidence.erase(key);next->last_seen.erase(key);}
        for(const auto key:retry){next->failed.erase(key);next->failed_evidence.erase(key);}
        for(const auto& value:usage)next->last_seen[value.first]=value.second;
        for(const auto& op:observed)if(!next->bindings.count(op.first)&&!next->failed.count(op.first)){
            const auto fingerprint=preparation_evidence(*op.second);
            BindingSpec spec;if(!NativeBindingSpec(*op.second,next->group,spec,error)){next->failed.emplace(op.first,error);next->failed_evidence.emplace(op.first,fingerprint);continue;}
            auto plan=PreparedBindings::Create(device.Get(),std::move(spec),error);if(plan)next->bindings.emplace(op.first,std::move(plan));else{next->failed.emplace(op.first,error);next->failed_evidence.emplace(op.first,fingerprint);}
        }
        for(auto transfer:transfers)if(!next->transfers.count(transfer.first)&&!next->failed_transfers.count(transfer.first)){
            transfer.second.group=next->group;auto plan=PreparedTransfer::Create(device.Get(),std::move(transfer.second),error);if(plan)next->transfers.emplace(transfer.first,std::move(plan));else next->failed_transfers.emplace(transfer.first,error);
        }
    }
    next->failed_keys.reserve(next->failed.size());for(const auto& failure:next->failed)next->failed_keys.push_back(failure.first);
    ControlLock lock(a->mutex,a->cost);
    ++a->stats.poll_builds;
    if(a->color!=color||a->depth!=depth||mode!=static_cast<int>(na::settings::Get().j2_scale_mode.load())||a->group_revision!=group_revision){a->worker_revision=0;return;}
    if(!next->prototype){a->failed_group_revision=group_revision;a->failed_group_mode=mode;a->failed_group_optimal=optimal_available;
        a->stats.failed_plans=1;a->work_pending.store(false);a->note(error.empty()?"Preparing scene bindings.":error.c_str());return;}
    a->prepared=std::move(next);a->stats.generation=a->prepared->group->Generation();a->stats.render_w=render.width;a->stats.render_h=render.height;
    a->stats.out_w=output.width;a->stats.out_h=output.height;a->stats.members=UINT(members.size());a->stats.plans=UINT(a->prepared->bindings.size());
    a->stats.failed_plans=a->prepared->failed.size()+a->prepared->failed_transfers.size();if(!same)++a->stats.group_builds;
    if(a->stats.failed_plans)a->note(!a->prepared->failed.empty()?a->prepared->failed.begin()->second.c_str():a->prepared->failed_transfers.begin()->second.c_str());
    else a->note("Reduced scene prepared; applying at the next frame boundary.");
    bool more=!a->pending.empty();for(const auto& transfer:a->observed_transfers)more|=!a->prepared->transfers.count(transfer.first)&&!a->prepared->failed_transfers.count(transfer.first);
    a->work_pending.store(more);
}
bool AutomaticSceneWorkPending(){auto a=automatic();return a&&a->work_pending.load();}
void AutomaticScenePresent(){auto a=automatic();if(!a)return;uint64_t depth=0,color=0;na::observe::GetLiveSelection(&depth,&color);
    ControlLock lock(a->mutex,a->cost);if(a->current&&a->current->redirected&&!a->current->aborted)++a->redirected_frames;
    ++a->epoch;a->current_recording=false;if(a->current)a->retired_runs.push_back(std::move(a->current));if(color&&depth)a->select(color,depth);
    if(na::settings::Get().j2_scale_mode.load()==0){if(a->built)a->retired_profiles.push_back(std::move(a->built));a->stats.active=false;a->note("Rendering at native resolution.");}
    else if(a->prepared&&a->built!=a->prepared){if(a->built)a->retired_profiles.push_back(std::move(a->built));a->built=a->prepared;}}
AutomaticSceneStats GetAutomaticSceneStats(){auto a=automatic();if(!a)return {};std::shared_lock<std::shared_mutex> lock(a->mutex);auto result=a->stats;
    result.operations=a->operations.load();result.redirected=a->redirected.load();result.fast_hits=a->fast_hits.load();result.snapshots=a->snapshots.load();
    result.constant_copies=a->constant_copies.load();result.constant_reuses=a->constant_reuses.load();
    result.presents=a->epoch-1;result.redirected_frames=a->redirected_frames.load();result.active=a->current&&a->current->redirected&&!a->current->aborted;
    result.control_wait_us=a->cost.wait_ns.load()/1000;result.control_hold_us=a->cost.hold_ns.load()/1000;result.control_max_us=a->cost.max_hold_ns.load()/1000;result.worker_us=a->cost.worker_ns.load()/1000;return result;}
AutomaticSceneSubmission::AutomaticSceneSubmission(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists){
    prior_=submission;auto a=automatic();if(!a){submission.reset();return;}
    auto value=std::make_shared<Submission>();value->queue=queue;value->lists.assign(lists,lists+count);
    ControlLock lock(a->mutex,a->cost);
    for(UINT i=0;i<count;++i){const auto key=std::make_pair(na::observe::IdOf(lists[i]),na::observe::ListRecording(lists[i]));
        const uint64_t queue_id=na::observe::IdOf(queue);const auto old=a->list_queues.find(key.first);
        if(old!=a->list_queues.end()&&old->second!=queue_id)old->second=0;else if(old==a->list_queues.end())a->list_queues.emplace(key.first,queue_id);
        if(!a->execution_queue&&queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&a->color&&a->depth&&na::observe::ListRendersTo(lists[i],a->color,a->depth))a->execution_queue=queue;
        const auto found=a->bound.find(key);
        if(found==a->bound.end())continue;if(value->run&&value->run!=found->second.run){value->run.reset();break;}value->run=found->second.run;}
    if(value->run){if(a->execution_queue&&a->execution_queue.Get()!=queue)value->run->aborted=true;else a->execution_queue=queue;}
    submission=std::move(value);
}
AutomaticSceneSubmission::~AutomaticSceneSubmission(){submission=std::static_pointer_cast<Submission>(prior_);}
}
