#include "providers/classifier/motion_checks.hpp"
#include <iostream>
using namespace na::motion_discovery;
int main() {
    Frame p{1,2,1,0,0,true,true,false,0,0,{},{}},c{1,2,2,0,0,true,true,false,0,0,{},{}};
    std::cin>>p.width>>p.height>>p.jitter_x>>p.jitter_y>>c.jitter_x>>c.jitter_y;
    if (!std::cin || !p.width || !p.height || p.width>256 || p.height>256) return 2;
    c.width=p.width; c.height=p.height;
    const size_t n=size_t(p.width)*p.height;
    p.color.resize(n); c.color.resize(n); p.stable.assign(n,1); c.stable.resize(n);
    Motion m{1,2,2,3,4,true,true,{}}; m.values.resize(n);
    for (size_t i=0;i<n;++i) {
        unsigned stable{}; std::cin>>stable; if (stable>1) return 2; c.stable[i]=static_cast<uint8_t>(stable);
        for (auto& v:p.color[i]) std::cin>>v;
        for (auto& v:c.color[i]) std::cin>>v;
        std::cin>>m.values[i][0]>>m.values[i][1];
    }
    if (!std::cin) return 2;
    for (double scale:{1.0,-1.0,0.5,2.0}) {
        const auto r=Evaluate(p,c,m,{scale,scale,false},1e-5);
        std::cout<<scale<<' '<<static_cast<int>(r.verdict)<<' '<<r.checked<<' '<<r.max_error<<'\n';
    }
}
