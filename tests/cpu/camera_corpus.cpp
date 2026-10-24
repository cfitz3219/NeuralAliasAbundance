#include "providers/camera/matrix_candidates.hpp"
#include <iostream>
#include <iomanip>
using namespace na::camera;
int main() {
    size_t count{}; std::cin>>count;
    if (!std::cin || count>262144) return 2;
    Snapshot s{1,2,3,4,5,0,Sync::AppSyncUnknown,true,true,{}};
    s.bytes.resize(count*4);
    for (size_t i=0;i<count;++i) {
        float v{}; std::cin>>v; std::memcpy(s.bytes.data()+i*4,&v,4);
    }
    if (!std::cin) return 2;
    std::cin>>std::ws; if (!std::cin.eof()) return 2;
    const auto scan=FindMatrices(s);
    if (scan.unavailable || scan.capacity_exceeded) return 3;
    std::cout<<std::setprecision(12);
    for (const auto& c:scan.candidates)
        std::cout<<c.byte_offset<<' '<<static_cast<int>(c.kind)<<' '<<static_cast<int>(c.layout)<<' '
            <<c.near_plane<<' '<<c.far_plane<<' '<<c.position[0]<<' '<<c.position[1]<<' '<<c.position[2]<<' '
            <<c.matrix[2]<<' '<<c.matrix[6]<<'\n';
}
