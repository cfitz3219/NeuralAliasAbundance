#include "scene_frame.hpp"
#include "host_dx12/observe/producers.hpp"
#include <algorithm>
namespace na::scale {
namespace {
void transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b)return;D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};l->ResourceBarrier(1,&x);
}
}
std::shared_ptr<SceneFrame> SceneFrame::Create(ID3D12Device* dev,FrameSpec spec,std::string& error) {
    error.clear();const auto fail=[&](const char* why)->std::shared_ptr<SceneFrame>{error=why;return {};};
    if(!dev||!spec.group||!spec.color||!spec.depth||spec.color==spec.depth||spec.members.size()!=spec.group->Members().size())
        return fail("frame requires the complete admitted scene group");
    const auto* color=spec.group->Find(spec.color);const auto* depth=spec.group->Find(spec.depth);
    if(!color||!depth||color->role!=Role::Color||depth->role!=Role::Depth)return fail("frame scene roles are not identified");
    const auto render=spec.group->Render(),output=spec.group->Output();
    if(color->reduced_desc.Width!=render.width||color->reduced_desc.Height!=render.height||
       depth->reduced_desc.Width!=render.width||depth->reduced_desc.Height!=render.height||
       color->native_desc.Width!=output.width||color->native_desc.Height!=output.height)
        return fail("frame active inputs and output do not match their group extents");
    auto p=std::shared_ptr<SceneFrame>(new SceneFrame);p->spec_=std::move(spec);
    for(const auto& state:p->spec_.members) {
        const auto* m=p->spec_.group->Find(state.member);
        if(!m||m->initial_state!=D3D12_RESOURCE_STATE_COMMON||std::any_of(p->promotions_.begin(),p->promotions_.end(),[&](const auto& x){return x.member==m;}))
            return fail("frame state membership is missing or duplicated");
        const auto& d=m->reduced_desc;
        if(d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1||m->native_desc.MipLevels!=1||m->native_desc.DepthOrArraySize!=1)
            return fail("frame promotion needs resolved plain scene members");
        Promotion x;x.state=state;x.member=m;
        if(m->role==Role::Depth) {
            if(d.Format!=DXGI_FORMAT_D32_FLOAT&&d.Format!=DXGI_FORMAT_R32_TYPELESS)
                return fail("frame depth promotion currently requires the identified 32-bit plane");
            D3D12_RESOURCE_DESC plane=d;plane.Format=DXGI_FORMAT_R32_FLOAT;plane.Flags=D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            if(FAILED(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&plane,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&x.plane))))
                return fail("frame depth plane allocation failed");
            x.depth=DepthRestore::Create(dev,x.plane.Get(),m->native.Get(),0,error);if(!x.depth)return {};
        } else {
            x.color=ColorRestore::Create(dev,m->reduced.Get(),m->native.Get(),0,d.Format,error);if(!x.color)return {};
        }
        if(state.seed_history) {
            if(m->role!=Role::History||state.seed_state==static_cast<D3D12_RESOURCE_STATES>(~0u))return fail("history seed needs its identified role and actual entry state");
            x.seed=ColorRestore::Create(dev,m->native.Get(),m->reduced.Get(),0,d.Format,error);if(!x.seed)return {};
        }
        p->promotions_.push_back(std::move(x));
    }
    return p;
}
bool SceneFrame::SeedHistories(ID3D12GraphicsCommandList* l) {
    const na::observe::HostWorkScope host;
    if(!l||!na::observe::ListStateRestorable(l))return false;
    if(!na::nr::native::RetainRecording(l,shared_from_this()))return false;
    for(auto& x:promotions_)if(x.seed) {
        transition(l,x.member->native.Get(),x.state.seed_state,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(l,x.member->reduced.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
        x.seed->Record(l);
        transition(l,x.member->reduced.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        transition(l,x.member->native.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,x.state.seed_state);
    }
    return na::observe::RestoreListState(l);
}
void SceneFrame::Written(uint64_t member) {
    std::lock_guard<std::mutex> lock(mutex_);if(finished_)return;
    for(auto& x:promotions_)if(x.state.member==member){x.written=true;dirty_=true;return;}
}
void SceneFrame::Ready(){std::lock_guard<std::mutex> lock(mutex_);ready_=true;}
na::j2::PhysicalLookup SceneFrame::Input(std::shared_ptr<const na::j2::PhysicalInput>* out) {
    if(!out)return na::j2::PhysicalLookup::Unavailable;
    std::lock_guard<std::mutex> lock(mutex_);out->reset();
    if(!dirty_||finished_)return na::j2::PhysicalLookup::None;
    auto input=std::make_shared<na::j2::PhysicalInput>();
    input->generation=Generation();input->logical_color=spec_.color;input->logical_depth=spec_.depth;input->owner=shared_from_this();
    *out=input; // includes the generation even when the redirected frame is unavailable
    bool have_color=false,have_depth=false;
    for(const auto& x:promotions_){if(x.state.member==spec_.color)have_color=x.written;if(x.state.member==spec_.depth)have_depth=x.written;}
    if(!ready_||!have_color||!have_depth)return na::j2::PhysicalLookup::Unavailable;
    const auto* c=spec_.group->Find(spec_.color);const auto* d=spec_.group->Find(spec_.depth);
    input->color=c->reduced.Get();input->depth=d->reduced.Get();input->output=c->native.Get();
    input->color_state=input->depth_state=D3D12_RESOURCE_STATE_COMMON;
    input->render_w=spec_.group->Render().width;input->render_h=spec_.group->Render().height;
    for(const auto& x:promotions_)if(x.member==c)input->output_state=x.state.native_state;
    return na::j2::PhysicalLookup::Provided;
}
void SceneFrame::Outcome(na::j2::PhysicalOutcome x){std::lock_guard<std::mutex> lock(mutex_);if(!finished_)outcome_=x;}
bool SceneFrame::Finish(ID3D12GraphicsCommandList* l) {
    const na::observe::HostWorkScope host;
    std::lock_guard<std::mutex> lock(mutex_);
    if(finished_||!dirty_)return true;
    if(!l||!na::observe::ListRasterStateRestorable(l)||!na::nr::native::RetainRecording(l,shared_from_this()))return false;
    for(auto& x:promotions_) {
        if(!x.written||(x.state.member==spec_.color&&outcome_==na::j2::PhysicalOutcome::Evaluated))continue;
        auto* reduced=x.member->reduced.Get();auto* native=x.member->native.Get();
        if(x.depth) {
            transition(l,reduced,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(l,x.plane.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=reduced;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=x.plane.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            transition(l,reduced,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
            transition(l,x.plane.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(l,native,x.state.native_state,D3D12_RESOURCE_STATE_DEPTH_WRITE);x.depth->Record(l);
            transition(l,native,D3D12_RESOURCE_STATE_DEPTH_WRITE,x.state.native_state);
            transition(l,x.plane.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        } else {
            transition(l,reduced,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(l,native,x.state.native_state,D3D12_RESOURCE_STATE_RENDER_TARGET);x.color->Record(l);
            transition(l,native,D3D12_RESOURCE_STATE_RENDER_TARGET,x.state.native_state);
            transition(l,reduced,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        }
    }
    if(!na::observe::RestoreListState(l))return false;
    finished_=true;return true;
}

thread_local PhysicalInputBridge* PhysicalInputBridge::current_bridge_=nullptr;
thread_local std::shared_ptr<SceneFrame> PhysicalInputBridge::current_,PhysicalInputBridge::reporting_;
bool PhysicalInputBridge::Bind(ID3D12GraphicsCommandList* l,std::shared_ptr<SceneFrame> frame) {
    if(!l||!frame)return false;
    const auto id=na::observe::IdOf(l),recording=na::observe::ListRecording(l);if(!id||!recording)return false;
    na::nr::native::Maintain();
    std::lock_guard<std::mutex> lock(mutex_);
    bound_.erase(std::remove_if(bound_.begin(),bound_.end(),[](const Bound& x){return x.lease.Retired();}),bound_.end());
    for(const auto& x:bound_)if(x.id==id&&x.recording==recording)return x.frame==frame;
    if(bound_.size()>=128)return false;
    auto lease=na::nr::native::RetainRecording(l,frame);if(!lease)return false;
    bound_.push_back({l,id,recording,std::move(frame),std::move(lease)});return true;
}
std::shared_ptr<SceneFrame> PhysicalInputBridge::Find(ID3D12GraphicsCommandList* l) {
    if(!l)return {};const auto id=na::observe::IdOf(l),recording=na::observe::ListRecording(l);
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& x:bound_)if(x.list==l&&x.id==id&&x.recording==recording&&!x.lease.Retired())return x.frame;return {};
}
na::j2::PhysicalLookup PhysicalInputBridge::Lookup(void* ctx,uint64_t color,uint64_t depth,ID3D12GraphicsCommandList* l,std::shared_ptr<const na::j2::PhysicalInput>* out) {
    auto* b=static_cast<PhysicalInputBridge*>(ctx);reporting_.reset();if(out)out->reset();
    auto f=l?b->Find(l):current_bridge_==b?current_:nullptr;
    if(!f||!f->Matches(color,depth))return na::j2::PhysicalLookup::None;
    const auto result=f->Input(out);if(result!=na::j2::PhysicalLookup::None)reporting_=f;return result;
}
void PhysicalInputBridge::Report(void*,uint64_t generation,na::j2::PhysicalOutcome result,const char*) {
    auto f=std::move(reporting_);if(f&&f->Generation()==generation)f->Outcome(result);
}
na::j2::PhysicalInputProvider PhysicalInputBridge::Callbacks(){return {&Lookup,&Report,this};}
PhysicalInputBridge::SubmissionScope::SubmissionScope(PhysicalInputBridge& b,ID3D12GraphicsCommandList* l)
    :prior_bridge_(current_bridge_),prior_(std::move(current_)){current_bridge_=&b;current_=b.Find(l);}
PhysicalInputBridge::SubmissionScope::~SubmissionScope(){current_bridge_=prior_bridge_;current_=std::move(prior_);}
} // namespace na::scale
