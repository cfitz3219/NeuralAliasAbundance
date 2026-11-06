#pragma once
#include <cstdint>
#include <memory>
struct ID3D12GraphicsCommandList;
namespace na::observe {
enum class ShaderKind { Draw, DrawIndexed, Dispatch };
struct ShaderCall {
    ShaderKind kind = ShaderKind::Dispatch;
    // Native arguments in API order (BaseVertexLocation remains signed).
    int64_t args[5]{};
};
enum class ShaderAction { Forward, Replaced };
struct ShaderScope {
    ShaderAction action = ShaderAction::Forward;
    std::shared_ptr<void> state;
    // Exact outer operation context survives nested runtime-internal calls.
    // No allocation: the provider supplies its already-retained recording owner.
    std::shared_ptr<void> context;
    void* opaque=nullptr;
    uint64_t list_id=0,recording=0;
};
class ShaderOperationHook {
public:
    virtual ~ShaderOperationHook() = default;
    // No observer locks held; runs in HostWorkScope. Modify forwarded counts or
    // bind prepared owned state. Replaced means the COMPLETE valid output has
    // already been recorded. Otherwise forward exactly once. A failed attempt
    // must restore its changes before returning Forward.
    virtual ShaderScope Before(ID3D12GraphicsCommandList*, ShaderCall&) noexcept = 0;
    // Called in reverse order for every hook whose Before ran, including the
    // replacing hook. Restore game state here, even if registration changed in
    // Before. This CPU scope does NOT replace recording/submission GPU leases.
    virtual void After(ID3D12GraphicsCommandList*, const ShaderCall&, ShaderScope&) noexcept = 0;
    // Same recording lane, before the native Close. A reduced scene can return
    // current dependencies to native allocations before another list consumes
    // them. Runs without registry locks, under HostWorkScope.
    virtual void Closing(ID3D12GraphicsCommandList*) noexcept {}
    virtual void NativeBoundary(ID3D12GraphicsCommandList*) noexcept {}
};
// Reconstruction first; Bindings only if forwarding remains necessary. Keeping
// two explicit lanes lets Layer1 and Layer2 share the existing host hook rather
// than patching the same draw/dispatch function twice.
enum class ShaderLane { Reconstruction = 0, Bindings = 1 };
void SetShaderOperationHook(ShaderLane lane, std::shared_ptr<ShaderOperationHook> hook);
} // namespace na::observe
