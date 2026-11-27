// Live Frame Generation: interface that moves with the scene (map names, unit markers drawn after the
// hand-over and anchored to the map) placed on each generated picture at that picture's time, instead
// of at the newer real frame's place (coordinator 2026-11-27; Hearts of Iron IV: city names lagged
// half a real-frame step behind the panning map on every generated picture, snapping back when the pan
// stopped).
//
// VARIANT: hudpaste/moves-with-scene (default) | hudpaste/screen-fixed (the T2 paste for everything)
//
// The interface is what the T2 paste pastes (fg/hud_paste.hpp): pixels where the real frame differs
// from its HUD-free picture by more than 4/255 in display terms, with a one-pixel apron. For each
// 16x16 tile of real frame n, the interface pixels are tested against real frame n-1 two ways:
//   - moves with the scene: the shown colour and the HUD mask at p + v in frame n-1 match frame n at p,
//     v being the scene's own motion at p (the motion Frame Generation gets: the interface is not in the
//     motion plane, so v is the scene's under it), sampled exactly and at the nearest pixel;
//   - screen-fixed: frame n-1 has the same interface pixel at p.
// A tile votes over its 3x3 tile neighbourhood (a label spans tiles); a score per tile, carried from
// frame to frame along the motion (a moving label's score moves with it), clamped to +-3, decides the
// class with hysteresis. Scene motion under one display pixel is the dead zone: pasted as before.
// On a generated picture at time t (k/(N+1) of the way from frame n-1 to n), a pixel q shows the moving
// interface pixel p = q - (1 - t) v when p is interface in a moving tile (the newer frame's shown pixels,
// sampled at p, so the scene under translucent text moves with it). Screen-fixed interface is pasted
// exactly as T2 and wins on top. Where a moving label sat in frame n but is not placed, the generated
// HUD-free picture shows. Without a valid pair (a cut, a gap, a relearned HUD) the caller uses T2.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <cstdint>

namespace na::fg {

class HudMove {
public:
    static constexpr uint32_t kTile = 16;
    static constexpr uint32_t kPages = 32;
    static constexpr uint32_t kEncodings = 3;
    // Counters of what the pass decided, read back a few frames late (for the log and RESULTS).
    struct Stats {
        uint64_t frames = 0, pictures = 0;
        uint64_t tiles_moving = 0, tiles_fixed = 0;      // interface tiles of each class, summed over frames
        uint64_t pixels_moving = 0, pixels_fixed = 0;    // interface pixels in tiles of each class
        double shift_px_sum = 0;                         // placement shift of the moving tiles (|(1-t) v|)
        uint64_t shift_n = 0;
        uint64_t fallback{0};                            // frames pasted the T2 way (reasons below)
        uint64_t fallback_pair = 0, fallback_reset = 0, fallback_motion = 0, fallback_off = 0, fallback_size = 0;
    };

    bool Init(ID3D12Device* device);
    bool Ready() const { return ready_; }
    const char* Error() const { return error_.c_str(); } // the shader compiler's words when Init failed
    // A new pair is not continuous (cut, gap, relearn, resize): the carried scores start over.
    void Forget() { forget_ = true; }
    // Frame n: classify its interface against frame n-1 (call once per real frame with a valid pair).
    // All pictures in NON_PIXEL_SHADER_RESOURCE; `motion` is Frame Generation's (render size, decoded
    // to render pixels by mv_scale); w x h is the display size.
    bool Classify(ID3D12GraphicsCommandList* list, ID3D12Resource* real, ID3D12Resource* hudfree, ID3D12Resource* real_prev,
                  ID3D12Resource* hudfree_prev, ID3D12Resource* motion, float mv_scale_x, float mv_scale_y, uint32_t w, uint32_t h,
                  DXGI_FORMAT format, uint32_t encoding, uint64_t frame_id);
    // Generated picture at time t in (0, 1) of frame n's classification: moving interface placed at t,
    // screen-fixed interface pasted on top. `generated` in UNORDERED_ACCESS.
    bool Paste(ID3D12GraphicsCommandList* list, ID3D12Resource* generated, ID3D12Resource* real, ID3D12Resource* hudfree,
               ID3D12Resource* motion, float mv_scale_x, float mv_scale_y, float t, uint32_t w, uint32_t h, DXGI_FORMAT format,
               uint32_t encoding);
    // Counters read back so far (the GPU's are at most kPages frames old).
    Stats Read();
    void NoteFallback(const char* why);
    // Tests: the class decided for a tile in the last classified frame (read back; -1 unknown).
    int TileClassForTests(uint32_t tx, uint32_t ty);

private:
    bool make_buffers(uint32_t tiles_w, uint32_t tiles_h);
    void collect();
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> classify_[kEncodings], decide_, paste_[kEncodings];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> tile_stats_, score_[2], tile_class_, counters_, counters_rb_, class_rb_;
    uint32_t inc_ = 0, page_ = 0, tw_ = 0, th_ = 0, ping_ = 0, last_page_ = 0;
    bool ready_ = false, forget_ = true;
    uint64_t submitted_[kPages] = {}, classified_ = 0;
    Stats stats_{};
    std::string error_;
    std::atomic<uint64_t> fb_pair_{0}, fb_reset_{0}, fb_motion_{0}, fb_off_{0}, fb_size_{0};
};

} // namespace na::fg
