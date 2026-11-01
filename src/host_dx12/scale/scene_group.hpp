// Layer 2: owned reduced scene allocations. Native game resources are never
// resized or replaced. A caller selects an already identified dependency set;
// this allocator does not infer scene roles from dimensions or resource flags.
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace na::scale {
struct Extent {
    uint32_t width = 0, height = 0;
    bool operator==(const Extent& b) const { return width == b.width && height == b.height; }
};
enum class Role { Color, Depth, Intermediate, History };
struct MemberRequest {
    uint64_t id = 0; // current native resource identity, never a cross-run key
    ID3D12Resource* native = nullptr;
    Role role = Role::Intermediate;
    Extent reduced; // explicit per member; half/quarter resources are not omitted
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
};
struct Member {
    uint64_t id = 0;
    Role role = Role::Intermediate;
    Microsoft::WRL::ComPtr<ID3D12Resource> native, reduced;
    D3D12_RESOURCE_DESC native_desc{}, reduced_desc{};
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
};
class SceneGroup {
public:
    // Worker/setup operation, not a per-draw allocation. On ANY failure, all
    // temporary allocations are released and no candidate is returned. The
    // original game's resources/heaps/descriptors remain completely untouched.
    // This allocates storage only. Initialization/history contents and a coherent
    // operation plan must be prepared before publishing it for scene rendering.
    static std::shared_ptr<const SceneGroup> Create(ID3D12Device* device, uint64_t generation,
        Extent output, Extent render, const std::vector<MemberRequest>& members, std::string& error);
    const Member* Find(uint64_t id) const;
    uint64_t Generation() const { return generation_; }
    Extent Output() const { return output_; }
    Extent Render() const { return render_; }
    const std::vector<Member>& Members() const { return members_; }
private:
    uint64_t generation_ = 0;
    Extent output_{}, render_{};
    std::vector<Member> members_;
};

// Publication is all-or-nothing. Acquire ONCE at a coherent frame/recording
// boundary and retain that snapshot through the existing recording/submission
// leases. Disable/Publish never changes a snapshot used by an in-flight list.
// This class is not a GPU retirement service and must not be used as one.
class GroupSelection {
public:
    void Publish(std::shared_ptr<const SceneGroup> group);
    void Disable();
    std::shared_ptr<const SceneGroup> Acquire() const;
private:
    std::shared_ptr<const SceneGroup> current_;
};

bool ScaleViewport(const D3D12_VIEWPORT& native, Extent from, Extent to, D3D12_VIEWPORT& reduced);
bool ScaleScissor(const D3D12_RECT& native, Extent from, Extent to, D3D12_RECT& reduced);
// Only for a known screen-space dispatch whose thread-group layout is known.
// Never infer this layout by scaling arbitrary observed dispatch counts.
bool DispatchForExtent(Extent extent, uint32_t threads_x, uint32_t threads_y, UINT& x, UINT& y);

enum class Scalar { Float, UInt };
enum class Dimension { Width, Height, InverseWidth, InverseHeight };
struct ConstantPatch {
    uint32_t offset = 0;
    Scalar scalar = Scalar::Float;
    Dimension dimension = Dimension::Width;
    Extent native, reduced; // input and output constants can have different extents
};
// Explicit, per-operation patch plan. No search/replace of coincidentally equal
// bytes. Validates ALL entries first; source and destination stay unchanged on
// failure. Returned bytes belong to the caller and must be bound in an owned
// upload allocation, never written back into a shared game constant buffer.
bool PatchConstants(const void* source, size_t bytes, const std::vector<ConstantPatch>& patches,
    std::vector<uint8_t>& destination, std::string& error);
} // namespace na::scale
