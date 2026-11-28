#pragma once
#include <d3d11_1.h>
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
  UINT width = 0, height = 0, native_width = 0, native_height = 0;

private:
  template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
  Ptr<ID3D11Texture2D> color_, depth_;
  Ptr<ID3D11RenderTargetView> rtv_;
  Ptr<ID3D11DepthStencilView> dsv_;
  Ptr<ID3D11CommandList> seed_, restore_, restore_depth_only_;
};
} // namespace na::native11
