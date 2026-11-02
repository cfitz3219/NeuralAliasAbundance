#include "scoped_bindings.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include <algorithm>
#include <cstdio>
namespace na::scale {
bool ShaderDescriptorArena::Allocate(ID3D12Device* device,UINT count,Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& heap,UINT& first,std::string* error){
    if(!device||!count||count>capacity_){if(error)*error="Invalid private descriptor allocation size.";return false;}
    const na::observe::HostWorkScope host;std::lock_guard<std::mutex> lock(mutex_);
    if(!current_.heap||count>current_.capacity-current_.used){
        Block block;block.capacity=capacity_;
        const bool visible=type_==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV||type_==D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        const D3D12_DESCRIPTOR_HEAP_DESC desc{type_,block.capacity,visible?D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE:D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        const auto result=device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&block.heap));
        if(FAILED(result)){++stats_.failures;stats_.last_hr=unsigned(result);
            if(error){static const char* names[]={"shader","sampler","RTV","DSV"};char text[200];
                std::snprintf(text,sizeof text,"Private %s descriptor heap allocation failed (0x%08X; %u requested, %u capacity, %llu heaps made).",
                    names[unsigned(type_)&3],unsigned(result),count,block.capacity,(unsigned long long)stats_.heaps);*error=text;}return false;}
        current_=std::move(block);++stats_.heaps;}
    heap=current_.heap;first=current_.used;current_.used+=count;stats_.descriptors+=count;return true;
}
std::shared_ptr<const ShaderDescriptorArena::Slice> ShaderDescriptorArena::Find(const std::vector<uint8_t>& content){
    std::lock_guard<std::mutex> lock(mutex_);const auto found=shared_.find(content);if(found==shared_.end())return {};
    auto slice=found->second.lock();if(!slice){shared_.erase(found);return {};}++stats_.shared_hits;return slice;
}
void ShaderDescriptorArena::Remember(const std::vector<uint8_t>& content,const std::shared_ptr<const Slice>& slice){
    std::lock_guard<std::mutex> lock(mutex_);
    if(shared_.size()>=65536)for(auto i=shared_.begin();i!=shared_.end();)i=i->second.expired()?shared_.erase(i):std::next(i);
    shared_[content]=slice;
}
ShaderDescriptorArena::Stats ShaderDescriptorArena::GetStats(){std::lock_guard<std::mutex> lock(mutex_);return stats_;}
namespace {
template<class T>void bytes_of(std::vector<uint8_t>& out,const T& value){const auto* p=reinterpret_cast<const uint8_t*>(&value);out.insert(out.end(),p,p+sizeof value);}
// Byte-identical descriptor contents: kind, retained resource identities,
// typed view description, and for patched CBVs the private buffer address.
// Each plan retains its own resources, so a live slice's pointers cannot be reused.
std::vector<uint8_t> table_content(const BindingSpec& s){
    std::vector<uint8_t> key;key.reserve(s.views.size()*64);
    for(const auto& v:s.views){bytes_of(key,uint32_t(v.kind));bytes_of(key,uint8_t(v.default_view));
        bytes_of(key,uintptr_t(v.resource.Get()));bytes_of(key,uintptr_t(v.counter.Get()));
        if(v.kind==ShaderView::Kind::PatchedCbv){if(v.constant<s.constants.size()&&s.constants[v.constant].buffer){
                bytes_of(key,s.constants[v.constant].buffer->Address());bytes_of(key,uint64_t(s.constants[v.constant].buffer->Bytes()));}}
        else if(v.kind==ShaderView::Kind::NativeCbv)bytes_of(key,v.cbv);
        else if(v.kind==ShaderView::Kind::Srv){if(!v.default_view)bytes_of(key,v.srv);}
        else if(!v.default_view)bytes_of(key,v.uav);}
    return key;
}
}
std::shared_ptr<const PreparedBindings> PreparedBindings::Create(ID3D12Device* device, BindingSpec spec, std::string& error)
{
    error.clear();
    const auto fail = [&](const char* why) -> std::shared_ptr<const PreparedBindings> { error = why; return {}; };
    if (!device || !spec.group || !spec.root || spec.views.size() > 4096 || spec.colors.size() > 8 ||
        spec.viewports.size() > 16 || spec.scissors.size() > 16) return fail("incomplete operation bindings");
    if (spec.compute && (!spec.colors.empty() || spec.depth.member || !spec.viewports.empty() || !spec.scissors.empty()))
        return fail("compute operation contains raster bindings");
    if (!spec.compute && (spec.threads_x || spec.threads_y)) return fail("raster operation contains dispatch dimensions");
    auto p = std::shared_ptr<PreparedBindings>(new PreparedBindings);
    p->spec_ = std::move(spec); const auto& s = p->spec_;
    if (s.views.empty() != s.tables.empty()) return fail("private heap and table bindings must be supplied together");
    for (const auto& t : s.tables) {
        if (t.param >= 64 || t.first >= s.views.size() || (p->tables_mask_ & (uint64_t(1) << t.param)))
            return fail("invalid or duplicate private table binding");
        p->tables_mask_ |= uint64_t(1) << t.param;
    }
    for (size_t i = 0; i < s.constants.size(); ++i) {
        if (!s.constants[i].buffer) return fail("missing prepared constant buffer");
        for (size_t j = 0; j < i; ++j) if (s.constants[i].buffer == s.constants[j].buffer)
            return fail("duplicate GPU copy of one constant buffer in the same scope");
    }
    for (const auto& c : s.root_cbvs) {
        if (c.param >= 64 || c.constant >= s.constants.size() || (p->cbv_mask_ & (uint64_t(1) << c.param)) ||
            (p->tables_mask_ & (uint64_t(1) << c.param))) return fail("invalid root constant-buffer binding");
        p->cbv_mask_ |= uint64_t(1) << c.param;
    }
    for (size_t i = 0; i < s.uses.size(); ++i) {
        const auto& use = s.uses[i]; const auto* member = s.group->Find(use.member);
        if (!member || member->initial_state != D3D12_RESOURCE_STATE_COMMON) return fail("private member is not prepared in COMMON");
        D3D12_FEATURE_DATA_FORMAT_INFO info{member->reduced_desc.Format};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof info)) ||
            (use.subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && use.subresource >=
             uint32_t(member->reduced_desc.MipLevels) * member->reduced_desc.DepthOrArraySize * info.PlaneCount))
            return fail("private resource subresource is outside its allocation");
        if (s.compute && (use.state & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_DEPTH_WRITE |
                                      D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)))
            return fail("compute operation requests graphics-only resource state");
        for (size_t j = 0; j < i; ++j) if (s.uses[j].member == use.member &&
            (s.uses[j].subresource == use.subresource || s.uses[j].subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
             use.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) return fail("overlapping private resource state entries");
        if (use.state != D3D12_RESOURCE_STATE_COMMON) {
            if((use.state&(D3D12_RESOURCE_STATE_RENDER_TARGET|D3D12_RESOURCE_STATE_DEPTH_WRITE|D3D12_RESOURCE_STATE_UNORDERED_ACCESS))&&
                std::find(p->written_.begin(),p->written_.end(),use.member)==p->written_.end())p->written_.push_back(use.member);
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {member->reduced.Get(), use.subresource, D3D12_RESOURCE_STATE_COMMON, use.state};
            p->enter_.push_back(b); std::swap(b.Transition.StateBefore, b.Transition.StateAfter); p->leave_.push_back(b);
        }
    }
    for (const auto& v : s.viewports) { D3D12_VIEWPORT scaled;
        if (!ScaleViewport(v, s.native_extent, s.render_extent, scaled)) return fail("invalid operation viewport");
        p->viewports_.push_back(scaled);
    }
    for (const auto& r : s.scissors) { D3D12_RECT scaled;
        if (!ScaleScissor(r, s.native_extent, s.render_extent, scaled)) return fail("invalid operation scissor");
        p->scissors_.push_back(scaled);
    }
    if ((s.threads_x || s.threads_y) && !DispatchForExtent(s.render_extent, s.threads_x, s.threads_y, p->dispatch_x_, p->dispatch_y_))
        return fail("invalid screen-space compute layout");
    if (!s.views.empty()) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, static_cast<UINT>(s.views.size()), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        p->increment_ = device->GetDescriptorHandleIncrementSize(hd.Type);
        std::vector<uint8_t> content;
        if(s.descriptors){
            for (const auto& v : s.views) if (v.kind == ShaderView::Kind::PatchedCbv && v.constant >= s.constants.size()) return fail("invalid table constant-buffer index");
            content=table_content(s);
            if(auto shared=s.descriptors->Find(content)){p->shared_views_=shared;p->views_=shared->heap;p->views_first_=shared->first;}
        }
        if(!p->views_){
        if(s.descriptors){if(!s.descriptors->Allocate(device,hd.NumDescriptors,p->views_,p->views_first_,&error))return {};}
        else { const auto hr=device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->views_)); if (FAILED(hr)) { char t[96]; std::snprintf(t,sizeof t,"private shader descriptor heap failed (0x%08X)",unsigned(hr)); return fail(t);} }
        auto cpu = p->views_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr+=SIZE_T(p->views_first_)*p->increment_;
        for (const auto& v : s.views) {
            if (v.kind == ShaderView::Kind::PatchedCbv) {
                if (v.constant >= s.constants.size()) return fail("invalid table constant-buffer index");
                const auto& c = s.constants[v.constant].buffer;
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{c->Address(), (c->Bytes() + 255u) & ~255u};
                device->CreateConstantBufferView(&cbv, cpu);
            } else if (v.kind == ShaderView::Kind::NativeCbv) device->CreateConstantBufferView(&v.cbv, cpu);
            else if (v.kind == ShaderView::Kind::Srv) device->CreateShaderResourceView(v.resource.Get(), v.default_view ? nullptr : &v.srv, cpu);
            else device->CreateUnorderedAccessView(v.resource.Get(), v.counter.Get(), v.default_view ? nullptr : &v.uav, cpu);
            cpu.ptr += p->increment_;
        }
        if(s.descriptors){auto slice=std::make_shared<ShaderDescriptorArena::Slice>();slice->heap=p->views_;slice->first=p->views_first_;slice->count=hd.NumDescriptors;
            p->shared_views_=slice;s.descriptors->Remember(content,slice);}
        }
    }
    if (!s.colors.empty()) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, static_cast<UINT>(s.colors.size()), D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const auto inc = device->GetDescriptorHandleIncrementSize(hd.Type);
        if(s.rtv_descriptors){if(!s.rtv_descriptors->Allocate(device,hd.NumDescriptors,p->rtvs_,p->rtvs_first_,&error))return {};}
        else { const auto hr=device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->rtvs_)); if (FAILED(hr)) { char t[80]; std::snprintf(t,sizeof t,"private RTV heap failed (0x%08X)",unsigned(hr)); return fail(t);} }
        auto cpu = p->rtvs_->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(p->rtvs_first_) * inc; p->rtv_increment_ = inc;
        for (const auto& v : s.colors) {
            const auto* m = s.group->Find(v.member); if (!m) return fail("unknown colour member");
            if (!(m->reduced_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || v.view.Format == DXGI_FORMAT_UNKNOWN)
                return fail("colour member lacks an explicit render target view");
            device->CreateRenderTargetView(m->reduced.Get(), &v.view, cpu); cpu.ptr += inc;
        }
    }
    if (s.depth.member) {
        const auto* m = s.group->Find(s.depth.member); if (!m) return fail("unknown depth member");
        if (!(m->reduced_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) || s.depth.view.Format == DXGI_FORMAT_UNKNOWN)
            return fail("depth member lacks an explicit depth target view");
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if(s.dsv_descriptors){if(!s.dsv_descriptors->Allocate(device,1,p->dsv_,p->dsv_first_,&error))return {};}
        else { const auto hr=device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->dsv_)); if (FAILED(hr)) { char t[80]; std::snprintf(t,sizeof t,"private DSV heap failed (0x%08X)",unsigned(hr)); return fail(t);} }
        auto cpu = p->dsv_->GetCPUDescriptorHandleForHeapStart();
        p->dsv_increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        cpu.ptr += SIZE_T(p->dsv_first_) * p->dsv_increment_;
        device->CreateDepthStencilView(m->reduced.Get(), &s.depth.view, cpu);
    }
    return p;
}
na::observe::ShaderScope BindingHook::Before(ID3D12GraphicsCommandList* list, na::observe::ShaderCall& call) noexcept
{
    auto p = provider_ ? provider_->Acquire(list, call) : nullptr;
    if (!p) return {};
    na::observe::ShaderScope scope;scope.state=std::const_pointer_cast<PreparedBindings>(p);provider_->OperationScope(list,scope);
    const auto refused = [&](const char* reason) { provider_->Refused(list, call, reason); return na::observe::ShaderScope{}; };
    const auto& s = p->spec_;
    if (s.compute != (call.kind == na::observe::ShaderKind::Dispatch)) return refused("operation kind changed");
    na::observe::ShaderState native;
    if (!na::observe::ListShaderState(list, s.compute, native) || (!s.compute&&!native.raster_restorable) || native.root != s.root.Get() ||
        (p->cbv_mask_ & native.cbv_params) != p->cbv_mask_) return refused("native root bindings changed");
    uint64_t tables = 0;
    for (uint32_t i = 0; i < native.count; ++i) if (!native.tables[i].sampler) tables |= uint64_t(1) << native.tables[i].param;
    if (p->views_ && tables != p->tables_mask_) return refused("private table map does not cover every native resource table");
    for (const auto& c : s.constants) if (!c.buffer->CanRecord(native.list_type, c.source_state)) return refused("source constant state is not usable");
    auto retained = std::const_pointer_cast<PreparedBindings>(p);
    if (!provider_->RecordingOwnsPlans()&&!na::nr::native::RetainRecording(list, retained)) return refused("recording lifetime is not available");
    if(provider_->RecordingOwnsPlans()){
        if(!provider_->RecordConstants(list,s.constants,scope))return refused("recording constants are not available");
    }else for (const auto& c : s.constants) c.buffer->RecordBefore(list, c.source_state);
    if(provider_->RecordingOwnsPlans()){
        if(!provider_->RecordResources(list,s.uses,scope))return refused("recording resources are not available");
    }else if (!p->enter_.empty()) list->ResourceBarrier(static_cast<UINT>(p->enter_.size()), p->enter_.data());
    const auto changes=provider_->BindingChanges(p,native,scope);
    if (p->views_) {
        ID3D12DescriptorHeap* heaps[]{p->views_.Get(), native.samplers};
        list->SetDescriptorHeaps(native.samplers ? 2 : 1, heaps);
        for (uint32_t i = 0; i < native.count; ++i) if (native.tables[i].sampler) {
            const auto t = native.tables[i];
            if (s.compute) list->SetComputeRootDescriptorTable(t.param, {t.handle});
            else list->SetGraphicsRootDescriptorTable(t.param, {t.handle});
        }
        const auto base = p->views_->GetGPUDescriptorHandleForHeapStart();
        for (const auto& t : s.tables) {
            const D3D12_GPU_DESCRIPTOR_HANDLE gpu{base.ptr + uint64_t(p->views_first_+t.first) * p->increment_};
            if (s.compute) list->SetComputeRootDescriptorTable(t.param, gpu); else list->SetGraphicsRootDescriptorTable(t.param, gpu);
        }
    }
    for (const auto& c : s.root_cbvs) {
        if(!(changes.cbvs&(uint64_t(1)<<c.param)))continue;
        const auto address = s.constants[c.constant].buffer->Address();
        if (s.compute) list->SetComputeRootConstantBufferView(c.param, address); else list->SetGraphicsRootConstantBufferView(c.param, address);
    }
    if (changes.targets&&(p->rtvs_ || p->dsv_)) {
        auto rt = p->rtvs_ ? p->rtvs_->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        auto ds = p->dsv_ ? p->dsv_->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        if (rt.ptr) rt.ptr += SIZE_T(p->rtvs_first_) * p->rtv_increment_;
        if (ds.ptr) ds.ptr += SIZE_T(p->dsv_first_) * p->dsv_increment_;
        list->OMSetRenderTargets(static_cast<UINT>(s.colors.size()), rt.ptr ? &rt : nullptr, TRUE, ds.ptr ? &ds : nullptr);
    }
    if (changes.viewports&&!p->viewports_.empty()) list->RSSetViewports(static_cast<UINT>(p->viewports_.size()), p->viewports_.data());
    if (changes.scissors&&!p->scissors_.empty()) list->RSSetScissorRects(static_cast<UINT>(p->scissors_.size()), p->scissors_.data());
    if (p->dispatch_x_) { call.args[0] = p->dispatch_x_; call.args[1] = p->dispatch_y_; }
    return scope;
}
void BindingHook::After(ID3D12GraphicsCommandList* list, const na::observe::ShaderCall& call, na::observe::ShaderScope& scope) noexcept
{
    const auto p = std::static_pointer_cast<PreparedBindings>(scope.state);
    if (!p) return;
    const bool keep=provider_->KeepNativeBindings();
    if(!keep||p->views_)na::observe::RestoreBindingState(list,p->spec_.compute,!!p->views_,keep?0:p->cbv_mask_,
        !keep&&(!!p->rtvs_||!!p->dsv_),!keep&&!p->viewports_.empty(),!keep&&!p->scissors_.empty());
    if (!provider_->RecordingOwnsPlans()&&!p->leave_.empty()) list->ResourceBarrier(static_cast<UINT>(p->leave_.size()), p->leave_.data());
    if(provider_->RecordingOwnsPlans())provider_->RestoreConstants(list,p->spec_.constants);
    else for (auto c = p->spec_.constants.rbegin(); c != p->spec_.constants.rend(); ++c) c->buffer->RecordAfter(list);
    provider_->AppliedScope(list,call,p,scope);
}
} // namespace na::scale
