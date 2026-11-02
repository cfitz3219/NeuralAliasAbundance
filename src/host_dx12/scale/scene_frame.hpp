#pragma once
#include "depth_restore.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include <mutex>

namespace na::scale {
struct FrameMemberState {
    uint64_t member = 0;
    D3D12_RESOURCE_STATES native_state = D3D12_RESOURCE_STATE_COMMON;
    // Only history members are seeded from the previous full-size native image.
    bool seed_history = false;
    // The seed boundary can have a different native state from the finish boundary.
    // Also required for D32S8 stencil initialization, even without colour history.
    D3D12_RESOURCE_STATES seed_state = static_cast<D3D12_RESOURCE_STATES>(~0u);
};
struct FrameSpec {
    std::shared_ptr<const SceneGroup> group;
    uint64_t color = 0, depth = 0;
    std::vector<FrameMemberState> members;
};
// One recognized scene frame, not a discovery algorithm. Preparation owns all
// promotions/seeds; recording performs no shader compilation or GPU readback.
// The admitting route supplies the complete dependency set and exact native
// states at its finish boundary, on its ordered queue. PRIVATE inputs are COMMON.
class SceneFrame : public std::enable_shared_from_this<SceneFrame> {
public:
    static std::shared_ptr<SceneFrame> Create(ID3D12Device*, FrameSpec, std::string&);
    // Call once before the first private scene operation: initializes histories
    // and the inherited stencil plane. Repeated calls are harmless.
    bool SeedHistories(ID3D12GraphicsCommandList*);
    void Written(uint64_t member);
    void StencilCleared(uint64_t member);
    void Ready();
    // Nonterminal native-consumer/list-end checkpoint. Inputs stay available
    // for the existing J2 insertion, while the native renderer has a current
    // full-size picture even when J2 skips. Only newly written members promote.
    bool Checkpoint(ID3D12GraphicsCommandList*, const std::vector<FrameMemberState>& current);
    na::j2::PhysicalLookup Input(std::shared_ptr<const na::j2::PhysicalInput>*);
    void Outcome(na::j2::PhysicalOutcome);
    // Called before the first native consumer, including an unavailable/failed
    // DLSS replacement. Returns the current-frame picture to full-size natives;
    // a successful DLSS colour is preserved, other written dependencies restored.
    bool Finish(ID3D12GraphicsCommandList*);
    uint64_t Generation() const { return spec_.group->Generation(); }
    bool Matches(uint64_t color,uint64_t depth) const { return color==spec_.color&&depth==spec_.depth; }
private:
    struct Promotion {
        FrameMemberState state;
        const Member* member = nullptr;
        std::shared_ptr<const ColorRestore> color, seed;
        std::shared_ptr<const DepthRestore> depth;
        Microsoft::WRL::ComPtr<ID3D12Resource> plane;
        Microsoft::WRL::ComPtr<ID3D12Resource> stencil_small, stencil_full;
        std::shared_ptr<const ColorRestore> stencil, seed_stencil;
        bool written = false;
        bool pending = false, stencil_initialized = false;
    };
    FrameSpec spec_;
    bool Promote(ID3D12GraphicsCommandList*, bool final);
    std::vector<Promotion> promotions_;
    void Stencil(ID3D12GraphicsCommandList*,Promotion&,bool seed);
    std::mutex mutex_;
    bool dirty_ = false, ready_ = false, finished_ = false, seeded_ = false;
    na::j2::PhysicalOutcome outcome_ = na::j2::PhysicalOutcome::Unavailable;
};

// Adapter to the EXISTING J2 physical-input callback. Bind an admitted frame to
// its exact executable recording. A SubmissionScope selects that frame for J2's
// between-lists route; inline lookups use their exact list/generation instead.
// The host installs Callbacks() while quiescent, and clears it before destruction.
class PhysicalInputBridge {
public:
    bool Bind(ID3D12GraphicsCommandList*, std::shared_ptr<SceneFrame>);
    na::j2::PhysicalInputProvider Callbacks();
    class SubmissionScope {
    public:
        SubmissionScope(PhysicalInputBridge&,ID3D12GraphicsCommandList*);
        ~SubmissionScope();
    private:
        PhysicalInputBridge* prior_bridge_ = nullptr;
        std::shared_ptr<SceneFrame> prior_;
    };
private:
    struct Bound {
        ID3D12GraphicsCommandList* list = nullptr;
        uint64_t id = 0, recording = 0;
        std::shared_ptr<SceneFrame> frame;
        na::nr::native::RecordingLease lease;
    };
    std::shared_ptr<SceneFrame> Find(ID3D12GraphicsCommandList*);
    static na::j2::PhysicalLookup Lookup(void*,uint64_t,uint64_t,ID3D12GraphicsCommandList*,std::shared_ptr<const na::j2::PhysicalInput>*);
    static void Report(void*,uint64_t,na::j2::PhysicalOutcome,const char*);
    std::mutex mutex_;
    std::vector<Bound> bound_;
    static thread_local PhysicalInputBridge* current_bridge_;
    static thread_local std::shared_ptr<SceneFrame> current_, reporting_;
};
} // namespace na::scale
