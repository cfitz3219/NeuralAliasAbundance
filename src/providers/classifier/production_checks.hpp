// Diagnostics-only confirmations using observable execution/content evidence.
// Supported means a corroborated candidate, never permission to feed an effect.
#pragma once
#include "temporal_depth.hpp"
#include "motion_checks.hpp"

namespace na::classifier {
enum class Confirmation { Unavailable, Contradicted, Supported };
struct ConfirmationReport {
    Confirmation result{Confirmation::Unavailable};
    size_t checked{}, mismatches{};
    double coverage{}, agreement{};
    std::string reason;
};
inline ConfirmationReport ConfirmDepth(const Window& window,const Observation& observation,
    const DepthFrame& previous,const DepthFrame& current) {
    ConfirmationReport out{};
    out.reason="Acquire matching main-scene depth, completed versions and camera association";
    const auto candidate=Describe(Signal::Depth,window,observation);
    const auto& c=current.camera;
    if(candidate.confidence!=Confidence::Candidate || observation.last_write!=window.scene_end ||
        current.resource!=observation.resource || current.width!=observation.width || current.height!=observation.height ||
        c.session!=observation.scope.session || c.epoch!=observation.scope.epoch ||
        c.queue!=observation.scope.queue || c.ticket!=observation.scope.submission) return out;
    if(current.depth.empty()) return out;
    const auto range=std::minmax_element(current.depth.begin(),current.depth.end());
    if(*range.second-*range.first<1e-5) { out.reason="Flat/clear depth cannot identify scene geometry"; return out; }
    const auto evidence=CheckTemporalDepth(previous,current);
    out.checked=evidence.checked;out.mismatches=evidence.checked-evidence.inliers;
    out.coverage=evidence.coverage;out.agreement=evidence.inlier_fraction;
    if(evidence.verdict==TemporalVerdict::Consistent) {
        out.result=Confirmation::Supported;
        out.reason="Main-scene write and independent-frame depth/camera reprojection agree; diagnostic candidate only";
    } else if(evidence.verdict==TemporalVerdict::Contradicted) {
        out.result=Confirmation::Contradicted;out.reason="Depth versions contradict camera reprojection";
    } else out.reason="Depth needs more camera motion, overlap or intact adjacent-frame evidence";
    return out;
}

struct ColorFrame { SampleIdentity identity; std::vector<Vec3> color; };
struct OverlayRect { uint32_t left{},top{},right{},bottom{}; };
struct ColorBoundary {
    // Identity of the preserved source read by the observed final composition.
    // Comes from resolved H2 bindings/copy lineage, not an image similarity guess.
    SampleIdentity consumed_source;
    ColorFrame final_color;
    uint64_t composition_op{},first_overlay_op{},last_overlay_op{};
    bool complete{},source_preserved{},presentation_linked{},no_other_writes{};
    // Conservative union of observed viewport/scissor bounds for later
    // depth-disabled blended draws. No HUD alpha/foreground oracle is required.
    std::vector<OverlayRect> overlays;
};
// This confirms the preserved display-space composition source, NOT the
// render-resolution pre-post-processing DLSS color input. That separate input
// requires render-pass/post-process lineage not established by image agreement.
inline ConfirmationReport ConfirmPreHudComposite(const Window& window,const Observation& observation,
    const ColorFrame& source,const ColorBoundary& boundary,double tolerance=0.001) {
    ConfirmationReport out{};out.reason="Acquire preserved scene source, presentation lineage and bounded overlay writes";
    const auto candidate=Describe(Signal::SceneColor,window,observation);
    const auto& s=source.identity;const auto& f=boundary.final_color.identity;
    if(candidate.confidence!=Confidence::Candidate || !Ready(s) || !Ready(f) || !Ready(boundary.consumed_source) ||
        !SameScope(s.scope,observation.scope) || s.resource!=observation.resource || s.version!=observation.content_version ||
        s.subresource!=observation.subresource || !SameSample(s,boundary.consumed_source) ||
        s.width!=observation.width || s.height!=observation.height || f.width!=s.width || f.height!=s.height ||
        !SameScope(f.scope,s.scope) || f.resource==s.resource || !Tolerance(tolerance) ||
        !boundary.complete || !boundary.source_preserved || !boundary.presentation_linked || !boundary.no_other_writes ||
        !Known(boundary.composition_op) || boundary.composition_op<=observation.last_write ||
        boundary.first_overlay_op!=window.hud_begin || boundary.first_overlay_op<=boundary.composition_op ||
        boundary.last_overlay_op<boundary.first_overlay_op || !Known(boundary.last_overlay_op) ||
        boundary.overlays.empty() || boundary.overlays.size()>128) return out;
    const size_t n=size_t(s.width)*s.height;
    if(source.color.size()!=n || boundary.final_color.color.size()!=n) return out;
    std::vector<uint8_t> overlay(n);
    for(const auto& rect:boundary.overlays) {
        if(rect.left>=rect.right || rect.top>=rect.bottom || rect.right>s.width || rect.bottom>s.height) return out;
        for(uint32_t y=rect.top;y<rect.bottom;++y) for(uint32_t x=rect.left;x<rect.right;++x)
            overlay[size_t(y)*s.width+x]=1;
    }
    size_t changed_overlay=0;double low=1e300,high=-1e300;
    for(size_t i=0;i<n;++i) {
        if(!Finite(source.color[i]) || !Finite(boundary.final_color.color[i])) return ConfirmationReport{};
        double error=0;
        for(size_t channel=0;channel<3;++channel) {
            error=std::max(error,std::abs(source.color[i][channel]-boundary.final_color.color[i][channel]));
            low=std::min(low,source.color[i][channel]);high=std::max(high,source.color[i][channel]);
        }
        if(overlay[i]) { if(error>tolerance) ++changed_overlay; }
        else { ++out.checked; if(error>tolerance) ++out.mismatches; }
    }
    out.coverage=double(out.checked)/double(n);
    if(out.checked) out.agreement=1-double(out.mismatches)/double(out.checked);
    if(out.checked<16 || out.coverage<0.75 || !changed_overlay || high-low<1e-5) {
        out.reason="Overlay footprint/flat content does not expose enough independently comparable scene";return out;
    }
    if(out.agreement<0.99) {
        out.result=Confirmation::Contradicted;out.reason="Candidate differs from presentation outside observed overlay writes";
    } else {
        out.result=Confirmation::Supported;
        out.reason="Preserved composition source agrees outside later overlay writes; diagnostic pre-overlay candidate, not trusted effect input";
    }
    return out;
}

struct ProductionMotionReport {
    ConfirmationReport evidence;
    motion_discovery::Report motion;
};
inline ProductionMotionReport ConfirmMotion(const DepthFrame& previous_depth,const DepthFrame& current_depth,
    motion_discovery::Frame previous_color,motion_discovery::Frame current_color,
    const motion_discovery::Motion& vectors,const motion_discovery::Convention& convention) {
    ProductionMotionReport out{};out.evidence.reason="Acquire matching depth/camera visibility and adjacent scene colors";
    const auto matches=[](const DepthFrame& d,const motion_discovery::Frame& c) {
        return d.camera.session==c.session && d.camera.epoch==c.epoch && d.camera.frame==c.frame &&
            d.width==c.width && d.height==c.height && d.camera.jitter_x==c.jitter_x && d.camera.jitter_y==c.jitter_y;
    };
    if(!matches(previous_depth,previous_color) || !matches(current_depth,current_color) ||
        previous_color.color.size()!=previous_depth.depth.size() || current_color.color.size()!=current_depth.depth.size()) return out;
    const auto geometry=CheckTemporalDepth(previous_depth,current_depth);
    if(geometry.verdict!=TemporalVerdict::Consistent) return out;
    // The mask derives from depth/camera, never from candidate vectors or a
    // fixture visibility oracle. Motion evidence covers only those surfaces.
    previous_color.stable=geometry.previous_agreement;current_color.stable=geometry.current_agreement;
    out.motion=motion_discovery::Evaluate(previous_color,current_color,vectors,convention);
    out.evidence.checked=out.motion.checked;
    out.evidence.coverage=double(out.motion.checked)/double(current_color.color.size());
    out.evidence.agreement=out.motion.inlier_fraction;
    if(out.evidence.coverage<0.75) { out.evidence.reason="Depth-derived visibility covers too little of this frame";return out; }
    if(out.motion.verdict==motion_discovery::Verdict::Consistent) {
        out.evidence.result=Confirmation::Supported;
        out.evidence.reason="Temporal color warp agrees on depth/camera-visible surfaces; moving/transparent exclusions remain unknown";
    } else if(out.motion.verdict==motion_discovery::Verdict::Contradicted) {
        out.evidence.result=Confirmation::Contradicted;out.evidence.reason="Motion contradicts temporal color evidence";
    } else out.evidence.reason="Temporal evidence is sparse or cannot distinguish motion conventions";
    return out;
}
} // namespace na::classifier
