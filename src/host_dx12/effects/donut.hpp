#pragma once
#include <d3d12.h>
#include <cstdint>
#include <memory>
namespace na::donut {
struct Options {
    bool tone=false,bloom=false;
    float amount=1,exposure_bias=-.5f,white=3,speed_up=1,speed_down=.5f;
    float low=.8f,high=.95f,min_luminance=.02f,max_luminance=.5f;
    float bloom_amount=.1f,bloom_sigma=16,bloom_threshold=1;
    uint32_t curve=0; // Donut luminance Reinhard / Falcor fitted filmic
};
struct Inputs {
    ID3D12Resource* color=nullptr;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint32_t width=0,height=0,x=0,y=0;
    bool srgb=false,preserve_extent=false,reset=false;
    float seconds=0; // zero measures elapsed recording time; nonzero is an explicit test/host delta
    uint64_t stream=0;
};
// Scene-linear creative grade before the game's final display transform. No
// output transfer function is imposed; the original encoding/alpha is retained.
// Known queue: Record calls must execute in order on that queue and the result
// must be consumed before the next Record. Unknown queue: independent scratch
// and instantaneous histogram adaptation, no unsynchronized shared GPU history.
class Frame {
public:
    ~Frame();
    bool Record(ID3D12GraphicsCommandList*);
    ID3D12Resource* Color() const;
private:
    friend class Pass;struct Work;std::unique_ptr<Work> work_;Frame();
};
struct Stats {uint64_t recorded=0,allocations=0,bindings=0,busy=0;bool compiling=false;char reason[160]{};};
class Pass {
public:
    Pass();~Pass();
    std::shared_ptr<Frame> Prepare(ID3D12Device*,ID3D12CommandQueue*,const Inputs&,const Options&);
    Stats GetStats() const;
    bool WaitPrepared(uint32_t milliseconds); // offline tests only
private:
    struct State;std::unique_ptr<State> state_;
};
Pass& ScenePass();Options CurrentOptions();
class NativeScope {
public:
    NativeScope(ID3D12GraphicsCommandList*,const void* parameters);
    ~NativeScope();
private:
    const void* parameters_=nullptr;ID3D12Resource* original_=nullptr;
};
}
