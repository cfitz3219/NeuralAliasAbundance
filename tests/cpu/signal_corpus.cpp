// Offline fixture transport only: bounded text on stdin, no labels/truth IDs.
#include "providers/classifier/content_checks.hpp"
#include <iostream>
using namespace na::classifier;
int main() {
    SampleIdentity key{{1,2,3,4,5,6},1,1,0,0,0,true,true,true};
    unsigned candidates{}, witness_count{};
    std::cin >> key.width >> key.height >> candidates >> witness_count;
    if (!std::cin || !Ready(key) || key.width > 256 || key.height > 256 ||
        candidates > 64 || witness_count > 4) return 2;
    const size_t count = size_t(key.width)*key.height;
    Matrix vp{}; double jx{}, jy{};
    for (auto& v : vp) std::cin >> v;
    std::cin >> jx >> jy;
    std::vector<Vec3> world(count);
    for (auto& p : world) for (auto& v : p) std::cin >> v;
    std::vector<CompositionWitness> witnesses(witness_count);
    for (auto& w : witnesses) {
        w.source = key; w.composite.resize(count); w.foreground.resize(count); w.alpha.resize(count);
        for (size_t i = 0; i < count; ++i) {
            std::cin >> w.alpha[i];
            for (auto& v : w.foreground[i]) std::cin >> v;
            for (auto& v : w.composite[i]) std::cin >> v;
        }
    }
    for (unsigned c = 0; c < candidates; ++c) {
        uint64_t id{}; unsigned kind{}, reverse{};
        std::cin >> id >> kind >> reverse;
        if (!std::cin || kind > 1 || reverse > 1) return 2;
        ContentReport result{};
        if (kind == 0) {
            std::vector<double> values(count);
            for (auto& v : values) std::cin >> v;
            result = CheckDepth(key,key,values,world,vp,jx,jy,reverse != 0);
        } else {
            std::vector<Vec3> values(count);
            for (auto& p : values) for (auto& v : p) std::cin >> v;
            result = CheckColor(key,values,witnesses);
        }
        if (!std::cin) return 2;
        std::cout << id << ' ' << static_cast<int>(result.result) << ' ' << result.checked
            << ' ' << result.unresolved << ' ' << result.mismatches << '\n';
    }
    std::cin >> std::ws;
    return std::cin.eof() ? 0 : 2;
}
