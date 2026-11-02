#pragma once
#include "gpu_constants.hpp"
#include "host_dx12/observe/shader_operation.hpp"
#include <mutex>

namespace na::observe{struct ShaderState;}
namespace na::scale {
// Setup-worker allocation only. Immutable slices share a group-owned heap;
// no descriptor is recycled while an executable recording may reference it.
class ShaderDescriptorArena {
public:
    bool Allocate(ID3D12Device*,UINT count,Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>&,UINT& first,std::string* error=nullptr);
private:
    struct Block {Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;UINT used=0,capacity=0;};
    std::mutex mutex_;
    Block current_; // older heaps are owned only by their immutable plans/recording leases
};
// Prepared from the recognized operation's real view/layout metadata, on the
// setup worker under HostWorkScope. No shader-visible heap is used as a CPU
// descriptor-copy source. Every descriptor in this private heap is created
// from its typed description and retained resource.
struct ShaderView {
    enum class Kind { Srv, Uav, PatchedCbv, NativeCbv } kind = Kind::Srv;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource, counter;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    bool default_view = false;
    uint32_t constant = 0; // PatchedCbv: index in BindingSpec::constants
};
struct ConstantUse {
    std::shared_ptr<const GpuConstants> buffer;
    D3D12_RESOURCE_STATES source_state = D3D12_RESOURCE_STATE_GENERIC_READ;
};
struct ResourceUse {
    uint64_t member = 0;
    uint32_t subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
struct ColorTarget { uint64_t member = 0; D3D12_RENDER_TARGET_VIEW_DESC view{}; };
struct DepthTarget { uint64_t member = 0; D3D12_DEPTH_STENCIL_VIEW_DESC view{}; };
struct BindingSpec {
    std::shared_ptr<const SceneGroup> group;
    std::shared_ptr<ShaderDescriptorArena> descriptors;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    bool compute = false;
    // If any private resource heap is needed, all native resource tables must
    // be rebased. Empty views/tables leave native material tables in place.
    // Native sampler tables are retained/rebound from the host shadow.
    struct Table { uint32_t param, first; };
    struct RootCbv { uint32_t param, constant; };
    std::vector<ShaderView> views;
    std::vector<Table> tables;
    std::vector<RootCbv> root_cbvs;
    std::vector<ConstantUse> constants;
    // Private resources enter/leave each operation in COMMON. The group's
    // owner supplies queue ordering; these barriers do not synchronize queues.
    std::vector<ResourceUse> uses;
    std::vector<ColorTarget> colors;
    DepthTarget depth;
    Extent native_extent, render_extent;
    std::vector<D3D12_VIEWPORT> viewports;
    std::vector<D3D12_RECT> scissors;
    // Only set for a recognized screen-space compute pass with known numthreads.
    uint32_t threads_x = 0, threads_y = 0;
};
class PreparedBindings {
public:
    static std::shared_ptr<const PreparedBindings> Create(ID3D12Device*, BindingSpec, std::string& error);
    const std::vector<uint64_t>& WrittenMembers()const{return written_;}
    const std::vector<ResourceUse>& Uses()const{return spec_.uses;}
    const std::vector<ConstantUse>& Constants()const{return spec_.constants;}
    const BindingSpec& Spec()const{return spec_;}
    const std::vector<D3D12_VIEWPORT>& Viewports()const{return viewports_;}
    const std::vector<D3D12_RECT>& Scissors()const{return scissors_;}
    ID3D12DescriptorHeap* ShaderHeap()const{return views_.Get();}
    UINT ShaderOffset()const{return views_first_;}
private:
    BindingSpec spec_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views_, rtvs_, dsv_;
    std::vector<D3D12_RESOURCE_BARRIER> enter_, leave_;
    std::vector<D3D12_VIEWPORT> viewports_;
    std::vector<D3D12_RECT> scissors_;
    std::vector<uint64_t> written_;
    UINT dispatch_x_ = 0, dispatch_y_ = 0;
    uint64_t tables_mask_ = 0, cbv_mask_ = 0;
    UINT increment_ = 0;
    UINT views_first_ = 0;
    friend class BindingHook;
};
class BindingProvider {
public:
    virtual ~BindingProvider() = default;
    virtual void Closing(ID3D12GraphicsCommandList*) noexcept {}
    virtual void NativeBoundary(ID3D12GraphicsCommandList*) noexcept {}
    // An automatic owner may retain its immutable Built (and thus every plan)
    // once per recording, and batch immutable-upload CB copies until Close.
    virtual bool RecordingOwnsPlans()const noexcept{return false;}
    struct Changes{bool targets=true,viewports=true,scissors=true;uint64_t cbvs=~uint64_t(0);};
    virtual bool KeepNativeBindings()const noexcept{return false;}
    virtual Changes BindingChanges(const std::shared_ptr<const PreparedBindings>&,const na::observe::ShaderState&,const na::observe::ShaderScope&)noexcept{return {};}
    virtual void OperationScope(ID3D12GraphicsCommandList*,na::observe::ShaderScope&)noexcept{}
    virtual bool RecordConstants(ID3D12GraphicsCommandList*,const std::vector<ConstantUse>&,const na::observe::ShaderScope&)noexcept{return false;}
    virtual void RestoreConstants(ID3D12GraphicsCommandList*,const std::vector<ConstantUse>&)noexcept{}
    virtual bool RecordResources(ID3D12GraphicsCommandList*,const std::vector<ResourceUse>&,const na::observe::ShaderScope&)noexcept{return false;}
    // Current recording/op recognition and coherent group admission belong to
    // the provider. It must never infer authority from resolution alone. Return
    // null for unrelated operations. A plan is immutable for all recorded uses.
    virtual std::shared_ptr<const PreparedBindings> Acquire(ID3D12GraphicsCommandList*,
        const na::observe::ShaderCall&) noexcept = 0;
    // Called before forwarding if an acquired plan cannot be bound. The owner
    // must promote any already redirected scene needed by this native consumer
    // and restore native state here. A stale native allocation is not fallback.
    // No commands from this rejected plan have been recorded at this point.
    virtual void Refused(ID3D12GraphicsCommandList*, const na::observe::ShaderCall&, const char* reason) noexcept = 0;
    // Actual private draw/dispatch was recorded and native state restored. Mark
    // its written dependencies here, not in Acquire (which can still refuse).
    virtual void Applied(ID3D12GraphicsCommandList*, const na::observe::ShaderCall&,
        const std::shared_ptr<const PreparedBindings>&) noexcept {}
    virtual void AppliedScope(ID3D12GraphicsCommandList* list,const na::observe::ShaderCall& call,
        const std::shared_ptr<const PreparedBindings>& plan,const na::observe::ShaderScope&)noexcept{Applied(list,call,plan);}
};
// Actual adapter for the existing Bindings shader-operation lane. Retains the
// prepared payload through the EXISTING executable-recording/submission lease,
// preserving discard and replay, then restores native bindings after each op.
// Install the reset/submission tracking during host setup, as on other lease
// consumers. Not installed automatically: the same-launch scene map selects it.
class BindingHook final : public na::observe::ShaderOperationHook {
public:
    explicit BindingHook(std::shared_ptr<BindingProvider> provider) : provider_(std::move(provider)) {}
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList*, na::observe::ShaderCall&) noexcept override;
    void After(ID3D12GraphicsCommandList*, const na::observe::ShaderCall&, na::observe::ShaderScope&) noexcept override;
    void Closing(ID3D12GraphicsCommandList* list) noexcept override { if(provider_)provider_->Closing(list); }
    void NativeBoundary(ID3D12GraphicsCommandList* list) noexcept override { if(provider_)provider_->NativeBoundary(list); }
private:
    std::shared_ptr<BindingProvider> provider_;
};
} // namespace na::scale
