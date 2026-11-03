#pragma once
#include <cstdint>

namespace na::overlay {
// Caller serializes. A qualified game-facing route owns only its associated
// base chain, including asynchronous generated Presents. No global latch and
// no frame-count heuristic. Resize preserves ownership; final Release removes
// it, so another chain or a reused address is never suppressed accidentally.
class BoundaryRoutes {
public:
    bool Select(uintptr_t proxy,uintptr_t base) {
        if(!proxy || !base || proxy==base)return false;
        for(auto& r:routes_)if(r.proxy==proxy){r.base=base;return true;}
        for(auto& r:routes_)if(!r.proxy){r={proxy,base};return true;}
        return false;
    }
    bool OwnsProxy(uintptr_t proxy) const {
        for(const auto& r:routes_)if(r.proxy && r.proxy==proxy)return true;
        return false;
    }
    bool Suppresses(uintptr_t chain) const {
        for(const auto& r:routes_)if(r.proxy && (r.base==chain || r.proxy==chain))return true;
        return false;
    }
    void Forget(uintptr_t chain) {
        for(auto& r:routes_)if(r.proxy==chain || r.base==chain)r={};
    }
    void Reset(){*this=BoundaryRoutes{};}
private:
    struct Route {uintptr_t proxy=0,base=0;};
    Route routes_[8]{};
};
}
