#pragma once
// The fold's D3D12 work (core/fold_shading.hpp), shared by both routes that run DLSS on our D3D12
// device: the DX12 route (j2/live_dlaa.cpp) and the native DX11 route (native_d3d11/live_dlaa.cpp).
// One pixel of every coarse-shaded block is kept from the scene's colour, depth and motion, and the
// smaller picture is described for DLSS; the full-size output is the same one DLAA writes. Each route
// owns one Pass (its own device, planes, history views and waiting period).
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/fold_shading.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

namespace na::fold {

struct Planes {
    Microsoft::WRL::ComPtr<ID3D12Resource> color, depth, motion;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
};

class Pass {
public:
    // The compute program and a descriptor ring of `ring` slots (two tables a slot: shown and warm-up).
    bool Init(ID3D12Device* device, uint32_t ring);
    bool Ok() const { return ok_; }
    // The folded planes of mode m, in UNORDERED_ACCESS between uses. Replaced only when the picture's size
    // or format changes. (ow, oh, out_format are no longer used: nothing is run unseen any more.)
    bool Targets(Mode m, uint32_t w, uint32_t h, DXGI_FORMAT color_format, uint32_t ow, uint32_t oh, DXGI_FORMAT out_format);
    // Mode m's planes are not needed any more (no level, or another one): they are given back a few seconds
    // later (Collect), when no frame in flight can still read them.
    void Release(Mode m);
    // Once a frame: frees released planes whose time has come.
    void Collect();
    // What DLSS will be given for mode m on a picture described by `base` (as Record would describe it, without
    // any work or resources): the DLSS feature a level needs is made from this before the level is shown.
    static void Describe(const dlss::EvalDesc& base, Mode m, uint64_t view_base, uint32_t preset, dlss::EvalDesc* out);
    // The name this pass's items carry in the video memory log ("dx12", "dx11").
    void Tag(const char* tag) { tag_ = tag; }
    // Keeps pixel `pick` of each `blk` block of colour, depth and motion into `planes` (descriptor table `table`).
    // motion_ready: the motion plane already holds the kept vectors (left as it is).
    void Dispatch(ID3D12GraphicsCommandList* l, ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
                  const Planes& planes, Block blk, Block pick, uint32_t table, bool motion_ready);
    // Folds `base`'s colour, depth and motion for mode m and describes the result for DLSS in *out (view
    // view_base + m, preset from `preset`). The planes are left readable; Done hands them back.
    void Record(ID3D12GraphicsCommandList* l, const dlss::EvalDesc& base, Mode m, uint32_t slot, bool warming,
                dlss::EvalDesc* out, uint64_t view_base, uint32_t preset, bool motion_ready = false);
    void Done(ID3D12GraphicsCommandList* l, Mode m);
    Planes& PlanesFor(Mode m) { return planes_[static_cast<uint32_t>(m)]; }
    uint32_t Ring() const { return ring_; }
    // Every resource the pass owns (a recording keeps them alive until it has run).
    template <class F> void ForEachResource(F f) const
    {
        for (const auto& p : planes_) {
            if (p.color) f(p.color);
            if (p.depth) f(p.depth);
            if (p.motion) f(p.motion);
        }
        for (const auto& r : released_) {
            if (r.planes.color) f(r.planes.color);
            if (r.planes.depth) f(r.planes.depth);
            if (r.planes.motion) f(r.planes.motion);
        }
    }
    // Everything released (device change, shutdown); the waiting period starts over.
    void Reset()
    {
        for (uint32_t i = 0; i < kModes; ++i) if (planes_[i].color) Release(static_cast<Mode>(i));
        released_.clear();
        root_.Reset(); pso_.Reset(); heap_.Reset(); device_.Reset();
        tried_ = ok_ = false;
        hand = Handover{};
    }

    Handover hand;

private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Planes planes_[kModes];
    struct Released { Planes planes; uint64_t at_ms = 0; };
    std::vector<Released> released_;
    const char* tag_ = "fold";
    uint32_t inc_ = 0, ring_ = 1;
    bool tried_ = false, ok_ = false;
};

// The plain-word state of one route's level, for the menu and the log.
struct Status {
    std::atomic<uint32_t> asked{0}, shown{0}, warming{0}, good{0}, refused{0}, shading{0}, w{0}, h{0};
    std::mutex why_m;
    char why[160]{};
    // Sets the reason (logged once when it changes under `source`, e.g. "fold" or "fold11").
    void Why(const char* source, const char* text);
    // The Home menu's line for this route.
    void Format(char* out, size_t n);
};

// A level that cannot save the game's shading is refused (Cort: a level that is not clearly faster
// than DLAA is a fail). The reason, in plain words, for both routes.
constexpr const char* kNoCoarseShading =
    "this graphics card or driver cannot shade the game's picture in blocks (coarse shading), so the level would not be faster than DLAA";

} // namespace na::fold
