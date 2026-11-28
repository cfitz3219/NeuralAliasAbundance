#pragma once
#include "core/referee_world_check.hpp"
#include <d3d11_1.h>
#include <vector>
#include <wrl/client.h>

namespace na::native11 {

// Private raster surfaces for one game-owned colour/depth pair. Both transfers
// are prepared before the first redirected draw. A game operation can always
// recover its native images without waiting for DLSS or reading anything back
// to the CPU.
class WorldRaster {
public:
  bool Prepare(ID3D11RenderTargetView *color, ID3D11DepthStencilView *depth,
               UINT w, UINT h);
  void Seed(ID3D11DeviceContext *ctx) const {
    ctx->ExecuteCommandList(seed_.Get(), TRUE);
  }
  void Restore(ID3D11DeviceContext *ctx, bool stencil_written) const {
    auto *list = !stencil_written && restore_depth_only_
                     ? restore_depth_only_.Get()
                     : restore_.Get();
    ctx->ExecuteCommandList(list, TRUE);
  }
  ID3D11Texture2D *Color() const { return color_.Get(); }
  ID3D11Texture2D *Depth() const { return depth_.Get(); }
  ID3D11RenderTargetView *Target() const { return rtv_.Get(); }
  ID3D11DepthStencilView *DepthView() const { return dsv_.Get(); }
  // The small depth through a view with the game's own read-only flags (a draw that
  // tests depth while a shader reads it). Made on first use; null when it cannot be.
  ID3D11DepthStencilView *DepthView(UINT flags);
  UINT width = 0, height = 0, native_width = 0, native_height = 0;

private:
  template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
  Ptr<ID3D11Texture2D> color_, depth_;
  Ptr<ID3D11RenderTargetView> rtv_;
  Ptr<ID3D11DepthStencilView> dsv_;
  Ptr<ID3D11DepthStencilView> dsv_flags_[4];
  bool dsv_flags_tried_[4]{};
  D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc_{};
  Ptr<ID3D11CommandList> seed_, restore_, restore_depth_only_;
};

// Full-size pictures a world draw reads by pixel position (the frame copied for
// water, depth for soft edges). While the world is drawn small such a draw is at
// small pixel positions, so it is given a full-size copy whose top-left corner
// holds the picture made small: position p then reads what p shows. One copy per
// source view; refilled when the source may have changed (the caller's `run`).
class CornerCopies {
public:
  ID3D11ShaderResourceView *Get(ID3D11DeviceContext *ctx,
                                ID3D11ShaderResourceView *source, UINT small_w,
                                UINT small_h, uint64_t run, uint64_t frame,
                                const char **why);
  // Whether such a copy can be had for this source (made, not filled).
  bool Can(ID3D11ShaderResourceView *source, UINT small_w, UINT small_h,
           uint64_t frame, const char **why);
  void Trim(uint64_t frame);
  void Clear() { entries_.clear(); }
  uint64_t fills = 0;

private:
  template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
  struct Entry {
    Ptr<ID3D11ShaderResourceView> source, view;
    Ptr<ID3D11Texture2D> texture;
    Ptr<ID3D11CommandList> fill;
    UINT w = 0, h = 0;
    uint64_t run = ~uint64_t{0}, used = 0;
  };
  bool Make(ID3D11ShaderResourceView *source, UINT w, UINT h, Entry &e,
            const char **why);
  Entry *Find(ID3D11ShaderResourceView *source, UINT w, UINT h,
              const char **why);
  std::vector<Entry> entries_;
  Ptr<ID3D11VertexShader> vs_;
  Ptr<ID3D11PixelShader> ps_;
  Ptr<ID3D11RasterizerState> rs_;
  bool shaders_failed_ = false;
};

// The Referee's eye on the small picture (core/referee_world_check.hpp): a coarse
// brightness grid of it, drawn on the GPU and read back a few frames later without
// ever waiting for the GPU.
class WorldProbe {
public:
  bool Prepare(ID3D11Device *device, ID3D11Texture2D *picture,
               DXGI_FORMAT view_format);
  // Samples the picture as it is now. False: nothing was sampled (no free slot).
  bool Note(ID3D11DeviceContext *ctx, uint64_t tag);
  // One finished sample, oldest first. Call once a frame.
  bool Poll(ID3D11DeviceContext *ctx, uint64_t &tag,
            na::referee::WorldGrid &cells);
  bool Ready() const { return list_ != nullptr; }

private:
  template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
  static constexpr unsigned kSlots = 4;
  struct Slot {
    Ptr<ID3D11Texture2D> staging;
    uint64_t tag = 0;
    unsigned polls = 0;
    uint64_t order = 0;
  };
  Slot slots_[kSlots];
  uint64_t order_ = 0;
  Ptr<ID3D11Texture2D> thumb_;
  Ptr<ID3D11ShaderResourceView> thumb_view_;
  Ptr<ID3D11CommandList> list_;
};
} // namespace na::native11
