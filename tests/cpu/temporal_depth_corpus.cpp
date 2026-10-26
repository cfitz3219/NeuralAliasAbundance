// Bounded offline transport; only depth and camera samples, no world/visibility oracle.
#include "providers/classifier/temporal_depth.hpp"
#include <iostream>
using namespace na::classifier;
int main() {
    uint32_t width{},height{}; unsigned reset{};
    std::cin>>width>>height>>reset;
    if(!std::cin || !width || !height || width>256 || height>256 || reset>1) return 2;
    DepthFrame frames[2];
    for(unsigned n=0;n<2;++n) {
        auto& f=frames[n]; f.width=width;f.height=height;f.resource=n+1;
        f.camera.session=1;f.camera.epoch=2;f.camera.queue=3;f.camera.ticket=n+1;f.camera.frame=n+1;
        f.camera.view_projection_unjittered=true;f.camera.qualified=f.camera.complete=true;f.camera.reset=n && reset;
        for(auto& v:f.camera.view_projection) std::cin>>v;
        std::cin>>f.camera.jitter_x>>f.camera.jitter_y;
        f.depth.resize(size_t(width)*height);for(auto& d:f.depth) std::cin>>d;
    }
    if(!std::cin) return 2;
    std::cin>>std::ws;
    if(!std::cin.eof()) return 2;
    const auto r=CheckTemporalDepth(frames[0],frames[1]);
    std::cout<<static_cast<int>(r.verdict)<<' '<<r.checked<<' '<<r.inliers<<' '<<r.excluded<<' '<<r.coverage<<'\n';
}
