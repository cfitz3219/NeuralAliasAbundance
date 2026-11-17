#pragma once
#include <d3d12.h>
#include <cstdint>
#include <memory>

namespace na::xegtao {
// Camera geometry in the existing column-vector VP convention. No game-specific offsets.
struct Projection { float sx=0, sy=0, dx=0, dy=0, a=0, b=0; };
bool FromViewProjection(const double* column_vector_vp, Projection&);
struct Options {
    bool enabled=false;
    uint32_t amount=100, quality=2; // percent; Intel default High (0..3 Low..Ultra)
    float radius=.5f;             // view/world units, adjustable per game
    uint32_t denoise=1;
    float radius_multiplier=1.457f,falloff=.615f,sample_distribution=2.f,thin_occluder=0.f,final_power=2.2f,depth_mip_offset=3.3f;
    // Share of the previous frames' shading kept per frame (0 = off). Needs
    // Inputs::motion; reprojects with the motion vectors and jitter change,
    // rejects history where the depth differs, clamps to the current 3x3.
    float temporal=.85f;
};
struct Inputs {
    ID3D12Resource* color=nullptr;
    ID3D12Resource* depth=nullptr;
    DXGI_FORMAT color_format=DXGI_FORMAT_UNKNOWN, depth_format=DXGI_FORMAT_R32_FLOAT;
    uint32_t width=0,height=0,color_x=0,color_y=0,depth_x=0,depth_y=0;
    Projection projection;
    float jitter_x=0,jitter_y=0,clear_depth=0;
    bool srgb=false;
    // Optional temporal input (render pixels, previous = current + motion,
    // NON_PIXEL_SHADER_RESOURCE, same extent as width x height). Without it
    // the shading is computed from this frame alone, as before.
    ID3D12Resource* motion=nullptr;
    // Motion maps this frame to motion_reference_frame, which can be older
    // than the last recorded AO when a scene pass or evaluation was skipped.
    uint64_t frame=0,motion_reference_frame=0,scene_epoch=0;
    bool reset=false; // camera cut / resize: start the history again
    bool preserve_color_extent=false; // native NGX keeps its original subrect origins
    // Native depth may forbid SRVs. Copy its compatible single-mip surface,
    // preserving the caller-declared state, before extracting the subrect.
    bool copy_depth=false;
    D3D12_RESOURCE_STATES depth_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
};
// Inputs must be NON_PIXEL_SHADER_RESOURCE when Record is called. They are never
// written or transitioned. The owned result ends in NON_PIXEL_SHADER_RESOURCE.
// Retain the Frame through GPU completion / executable recording retirement,
// using the caller's EXISTING fence ring or native RecordingLease.
// With a known queue, scratch is shared in GPU order: consume Color in this
// recording before the next AO pass, or copy it for a delayed consumer.
class Frame {
public:
    ~Frame();
    bool Record(ID3D12GraphicsCommandList*);
    ID3D12Resource* Color() const;
    ID3D12Resource* Occlusion() const; // spatially denoised, before history
    ID3D12Resource* CompositedVisibility() const; // after Record, read before the next AO pass
    ID3D12Resource* ViewDepthForTests() const;
    ID3D12Resource* NormalsForTests() const;
private:
    friend class Pass;
    struct Work;
    std::unique_ptr<Work> work_;
    Frame();
};
struct Stats { uint64_t recorded=0, allocations=0, busy=0, bindings=0; bool compiling=false; char reason[160]{}; };
class Pass {
public:
    Pass();
    ~Pass();
    // Off/zero returns immediately without compiling or allocating. The first
    // enabled request prepares shaders/PSOs on a worker and leaves the picture
    // unchanged until ready. Prepare itself records no GPU commands.
    // A known queue allows scratch sharing in GPU execution order. Without one,
    // recordings get independent scratch (never guess a process-wide queue).
    std::shared_ptr<Frame> Prepare(ID3D12Device*,ID3D12CommandQueue*,const Inputs&,const Options&);
    Stats GetStats() const;
    void Unavailable(const char* reason);
    bool WaitPrepared(uint32_t milliseconds); // offline tests only
private:
    struct State;
    std::unique_ptr<State> state_;
};
// One existing scene processor, shared by all J2 placement modes.
Pass& ScenePass();
Options CurrentOptions();
// Existing native-DLSS boundary; does nothing unless explicitly enabled and
// this evaluation supplies a matched camera and observed resource states.
class NativeScope {
public:
    NativeScope(ID3D12GraphicsCommandList*,const void* parameters);
    ~NativeScope();
    NativeScope(const NativeScope&)=delete;
    NativeScope& operator=(const NativeScope&)=delete;
private:
    const void* parameters_=nullptr;
    ID3D12Resource* original_=nullptr;
};
} // namespace na::xegtao
