// Bounded transport for the combined A5 metadata + production evidence path.
// No candidate labels, world positions, HUD alpha or visibility masks in input.
#include "providers/classifier/production_checks.hpp"
#include <iostream>
using namespace na::classifier;
int main() {
    uint32_t width{},height{},count{},reset{};uint64_t frame{},scene_source{};
    std::cin>>width>>height>>count>>reset>>frame>>scene_source;
    if(!std::cin || !width || !height || width>256 || height>256 || count>32 || reset>1 || frame<2) return 2;
    const size_t n=size_t(width)*height;
    CameraAssociation cameras[2];
    for(unsigned k=0;k<2;++k) {
        auto& c=cameras[k];c.session=1;c.epoch=2;c.queue=4;c.ticket=frame-1+k;c.frame=frame-1+k;
        c.qualified=c.complete=true;c.reset=k && reset;
        for(auto& v:c.view_projection) std::cin>>v;
        std::cin>>c.jitter_x>>c.jitter_y;
    }
    Scope scope{1,2,frame,4,5,frame};
    Window window{scope,width,height,100,120,true,true,true};
    SampleIdentity final_id{scope,99999999,frame,0,width,height,true,true,true};
    ColorBoundary boundary{};boundary.complete=boundary.source_preserved=boundary.presentation_linked=boundary.no_other_writes=true;
    boundary.composition_op=110;boundary.first_overlay_op=120;boundary.last_overlay_op=121;
    uint32_t l{},t{},r{},b{};std::cin>>l>>t>>r>>b;boundary.overlays.push_back({l,t,r,b});
    boundary.final_color.identity=final_id;boundary.final_color.color.resize(n);
    for(auto& p:boundary.final_color.color) for(auto& v:p) std::cin>>v;
    for(uint32_t index=0;index<count;++index) {
        uint64_t id{},write{},version{};uint32_t kind{};
        std::cin>>id>>kind>>write>>version;
        if(!std::cin || kind>1 || !Known(id)) return 2;
        Observation o{};o.scope=scope;o.resource=id;o.content_version=version;o.width=width;o.height=height;
        o.format=kind?Format::ColorFloat:Format::Depth;o.alive=o.external_work=true;
        o.depth_write=!kind;o.color_write=kind!=0;o.first_write=1;o.last_write=write;
        // The input observation's content version is the producing frame.
        // Old buffers cannot inherit this frame's write history.
        if(version!=frame) o.scope.submission=version;
        ConfirmationReport result;
        if(!kind) {
            DepthFrame p{cameras[0],id,width,height,{}},c{cameras[1],id,width,height,{}};
            p.depth.resize(n);c.depth.resize(n);
            for(auto& d:p.depth) std::cin>>d;
            for(auto& d:c.depth) std::cin>>d;
            result=ConfirmDepth(window,o,p,c);
        } else {
            SampleIdentity idn{scope,id,version,0,width,height,true,true,true};
            ColorFrame color{idn,{}};color.color.resize(n);
            for(auto& p:color.color) for(auto& v:p) std::cin>>v;
            boundary.consumed_source={scope,scene_source,frame,0,width,height,true,true,true};
            result=ConfirmSceneColor(window,o,color,boundary);
        }
        if(!std::cin) return 2;
        std::cout<<id<<' '<<static_cast<int>(result.result)<<' '<<result.checked<<' '<<result.mismatches<<'\n';
    }
    std::cin>>std::ws;
    return std::cin.eof()?0:2;
}
