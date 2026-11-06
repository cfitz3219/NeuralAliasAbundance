#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

namespace na::j2::native_hud {
// One exact game picture. Caller owns this packet through its source recordings
// and Present completion. No queue, readback, fence wait or CPU image verdict.
class Picture {
    struct Impl;
    std::unique_ptr<Impl> p_;
public:
    struct ProofReceipt {uint32_t tested=0,matched=0,picture_matched=0,hud_pixels=0,separated_pixels=0;float maximum_error=0;};
    Picture();
    ~Picture();
    static std::shared_ptr<Picture> Create(ID3D12Device*,const D3D12_RESOURCE_DESC& source,uint32_t ow,uint32_t oh);
    bool Matches(ID3D12Device*,const D3D12_RESOURCE_DESC&,uint32_t ow,uint32_t oh)const;
    void BeginCycle(); // only after ALL source-recording and Present owners release it
    bool Prepared() const;
    // Another Present of the same submitted picture gets independent mutable
    // descriptors/output resources, while reusing the immutable source images.
    std::shared_ptr<Picture> ForkSnapshot(ID3D12GraphicsCommandList*);
    bool SnapshotScene(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES);
    bool SnapshotFull(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES);
    // Optional original-size counterfactuals: verify the actual blend model
    // against the captured pre-HUD scene, including opaque pixels and clipping.
    void SetValidationLayers(ID3D12Resource* low,ID3D12Resource* high,bool source_srgb=false);
    void SetReplayValidation(ID3D12Resource* original_size_check); // existing native draw replay, NPSR
    // Discovery only: compare an immutable canvas with this exact final frame.
    // Neither image is altered. Caller supplies its existing completion fence.
    bool Probe(ID3D12GraphicsCommandList*,ID3D12Resource* frame,DXGI_FORMAT frame_view);
    // frame: NPSR, motion: NPSR, layers: RT. All are returned in those states.
    // The GPU verifies this frame against the captured transfer source BEFORE
    // choosing clean scene or untouched complete colour. No historical licence
    // to remove HUD on a later, structurally different frame.
    bool Prepare(ID3D12GraphicsCommandList*,ID3D12Resource* frame,DXGI_FORMAT frame_view,
                 ID3D12Resource* motion,ID3D12Resource* n0,ID3D12Resource* n1,
                 DXGI_FORMAT layer_view,float grey,bool srgb,bool clean_allowed,DXGI_FORMAT output_format,bool tint=false);
    ID3D12Resource* Input() const; // NPSR, RGBA16F; same colour encoding as frame
    ID3D12Resource* Mask() const;  // NPSR, R16F; exact draw-derived protection
    ID3D12Resource* Scene() const;
    ID3D12Resource* Full() const;
    bool ProofAfterCompletion(bool& identity,bool& clean,ProofReceipt* receipt=nullptr); // caller MUST prove GPU completion; consumed once
    // out is UAV and returned UAV. Identity transfer + complete replay =>
    // native HUD composition; partial replay => current-picture protection.
    bool Composite(ID3D12GraphicsCommandList*,ID3D12Resource* out);
    bool SeedNativeAlpha(ID3D12GraphicsCommandList*,ID3D12Resource* out);
    // A UI-only canvas starts from its own transparent/background pixels,
    // independently of the reconstructed world. out: NPSR -> PRESENT.
    bool SeedNativeScene(ID3D12GraphicsCommandList*,ID3D12Resource* out,DXGI_FORMAT source_view);
    // Both the final handoff AND original-size replay must match. Native and
    // check are NPSR; out is UAV. Otherwise show the untouched final picture.
    bool PresentNative(ID3D12GraphicsCommandList*,ID3D12Resource* frame,DXGI_FORMAT view,
                       ID3D12Resource* native,ID3D12Resource* check,ID3D12Resource* out,bool tint=false);
};
}
