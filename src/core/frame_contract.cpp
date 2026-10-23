#include "frame_contract.hpp"
#include <cmath>
namespace abundance {
const char* route_name(Route route) {
    switch(route) {
    case Route::native_upscaler:return "Existing upscaler";
    case Route::adapter:return "Engine/game adapter";
    case Route::optical_flow_experimental:return "Experimental optical flow";
    default:return "No validated input provider";
    }
}
Route choose_route(ProviderEvidence native, ProviderEvidence adapter) {
    if(native.route==Route::native_upscaler && native.evaluation_observed && native.inputs_validated)
        return Route::native_upscaler;
    if(adapter.adapter_matched && adapter.inputs_validated &&
        (adapter.route==Route::adapter || adapter.route==Route::optical_flow_experimental))
        return adapter.route;
    return Route::none;
}
Decision validate_inputs(const Frame& f, Feature feature) {
    const auto reject=[](const char* text) { return Decision{false,false,text}; };
    if(feature==Feature::frame_generation) return reject("Frame Generation: not implemented yet");
    if(f.route==Route::none) return reject("No validated input provider");
    if(!f.id || !f.generation || !f.output.valid()) return reject("Invalid frame identity or output extent");
    if(!f.color.valid() || !f.motion.valid()) return reject("Missing color or motion input");
    const auto current=[&](const Image& image) { return image.frame==f.id && image.generation==f.generation; };
    if(!current(f.color) || !current(f.motion)) return reject("Stale color/motion resource");
    if(f.color_space==ColorSpace::unknown || f.motion_space==MotionSpace::unknown)
        return reject("Unknown color or motion convention");
    if(!f.clean_scene) return reject("HUD-free scene has not been validated");
    if(feature==Feature::reconstruction) {
        if(!f.depth.valid() || !current(f.depth)) return reject("Missing or stale depth");
        if(!(f.depth.extent==f.color.extent)) return reject("Depth/color extent mismatch");
        if(!(f.motion.extent==f.color.extent) && !(f.motion.extent==f.output))
            return reject("Unsupported motion extent");
        if(!f.jitter_known || !std::isfinite(f.jitter_x) || !std::isfinite(f.jitter_y))
            return reject("Unknown or invalid camera jitter");
        if(!f.exposure_policy_known) return reject("Exposure policy not validated");
        if(!f.motion_excludes_jitter) return reject("Motion jitter conversion required");
        if(f.output.width<f.color.extent.width || f.output.height<f.color.extent.height)
            return reject("Output smaller than reconstruction input");
        const double aspect=double(f.color.extent.width)*f.output.height /
            (double(f.color.extent.height)*f.output.width);
        if(aspect<.995 || aspect>1.005) return reject("Input/output aspect mismatch");
    }
    return {true,f.reset || f.camera_cut,"Input metadata accepted; resource contents require provider validation"};
}
Decision evaluate(const Frame& f, Feature feature, const BackendSupport& backends) {
    auto result=validate_inputs(f,feature);
    if(!result.ready) return result;
    if(feature==Feature::reconstruction && !backends.reconstruction)
        return {false,false,"Reconstruction backend not connected"};
    if(feature==Feature::neural_rendering && (!backends.neural_rendering || !backends.neural_contract_qualified))
        return {false,false,"Neural Rendering backend/contract not qualified"};
    return result;
}
Decision Continuity::observe(const Frame& f, Feature feature, const BackendSupport& backends) {
    auto result=evaluate(f,feature,backends);
    if(!result.ready) { valid_=false; return result; }
    result.reset=result.reset || !valid_ || f.id<=previous_.id || f.id-previous_.id!=1 ||
        f.generation!=previous_.generation || f.route!=previous_.route ||
        !(f.color.extent==previous_.color.extent) || !(f.output==previous_.output) ||
        f.color_space!=previous_.color_space || f.motion_space!=previous_.motion_space;
    previous_=f;valid_=true;
    return result;
}
}
