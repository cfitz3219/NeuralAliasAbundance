#pragma once
#include <cstdint>
#include <string>

namespace abundance {
enum class Route { none, native_upscaler, adapter, optical_flow_experimental };
enum class Feature { reconstruction, neural_rendering, frame_generation };
enum class ColorSpace { unknown, linear_hdr, linear_sdr, srgb_sdr };
enum class MotionSpace { unknown, pixels_current_to_previous, normalized_current_to_previous };
struct Extent {
    uint32_t width=0, height=0;
    bool valid() const { return width>0 && height>0; }
    bool operator==(Extent b) const { return width==b.width && height==b.height; }
};
// Non-owning identity only. Providers must keep actual GPU resources alive until
// their submission fence completes; this metadata is not permission to release them.
struct Image {
    uint64_t identity=0, frame=0, generation=0;
    Extent extent{};
    bool valid() const { return identity!=0 && extent.valid(); }
};
struct Frame {
    uint64_t id=0, generation=0;
    Route route=Route::none;
    Image color{}, depth{}, motion{};
    Extent output{};
    ColorSpace color_space=ColorSpace::unknown;
    MotionSpace motion_space=MotionSpace::unknown;
    float jitter_x=0, jitter_y=0;
    bool jitter_known=false, exposure_policy_known=false;
    bool clean_scene=false, motion_excludes_jitter=false;
    bool reset=false, camera_cut=false;
};
struct ProviderEvidence {
    Route route=Route::none;
    // Merely finding a DLL is deliberately insufficient.
    bool evaluation_observed=false, inputs_validated=false, adapter_matched=false;
};
Route choose_route(ProviderEvidence native, ProviderEvidence adapter);
struct BackendSupport {
    bool reconstruction=false, neural_rendering=false;
    // Exact NR contract must be qualified separately from SR's requirements.
    bool neural_contract_qualified=false;
};
struct Decision { bool ready=false; bool reset=false; std::string reason; };
Decision validate_inputs(const Frame&, Feature);
Decision evaluate(const Frame&, Feature, const BackendSupport&);
class Continuity {
public:
    Decision observe(const Frame&, Feature, const BackendSupport&);
    void invalidate() { valid_=false; }
private:
    bool valid_=false;
    Frame previous_{};
};
const char* route_name(Route);
}
