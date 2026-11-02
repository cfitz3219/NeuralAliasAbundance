#pragma once
#include "scene_group.hpp"
#include "host_dx12/observe/texture_operation.hpp"
namespace na::scale {
struct TransferSpec {
    std::shared_ptr<const SceneGroup> group;
    na::observe::TextureKind kind = na::observe::TextureKind::Copy;
    uint64_t source = 0, destination = 0;
    D3D12_RENDER_TARGET_VIEW_DESC color{};
    D3D12_DEPTH_STENCIL_VIEW_DESC depth{};
    DXGI_FORMAT resolve_format = DXGI_FORMAT_UNKNOWN;
};
// Worker-prepared transfer/clear. Both endpoints of a copy/resolve must belong
// to the same admitted group. Outside-group consumers are handled by promotion,
// never by copying a smaller resource into a differently sized native texture.
class PreparedTransfer : public std::enable_shared_from_this<PreparedTransfer> {
public:
    static std::shared_ptr<const PreparedTransfer> Create(ID3D12Device*, TransferSpec, std::string&);
    bool Record(ID3D12GraphicsCommandList*, const na::observe::TextureCall&, const char** reason) const;
private:
    TransferSpec spec_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> clear_;
};
class TransferProvider {
public:
    virtual ~TransferProvider() = default;
    virtual std::shared_ptr<const PreparedTransfer> Acquire(ID3D12GraphicsCommandList*, const na::observe::TextureCall&) noexcept = 0;
    virtual void Refused(ID3D12GraphicsCommandList*, const na::observe::TextureCall&, const char*) noexcept = 0;
};
class TransferHook final : public na::observe::TextureOperationHook {
public:
    explicit TransferHook(std::shared_ptr<TransferProvider> p) : provider_(std::move(p)) {}
    na::observe::ShaderScope Before(ID3D12GraphicsCommandList*, const na::observe::TextureCall&) noexcept override;
    void After(ID3D12GraphicsCommandList*, const na::observe::TextureCall&, na::observe::ShaderScope&) noexcept override {}
private:
    std::shared_ptr<TransferProvider> provider_;
};
} // namespace na::scale
