#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

namespace na::native11 {

class DepthReadTwin {
public:
    bool Refresh(ID3D11DeviceContext* context, ID3D11Resource* depth, float jitter_x, float jitter_y,
                 bool expanded, UINT active_width, UINT active_height);
    ID3D11ShaderResourceView* ViewFor(ID3D11ShaderResourceView* original) const;
    ID3D11Resource* SourceForCopy(ID3D11Resource* destination) const;
    const char* Reason() const { return reason_; }

private:
    bool Prepare(ID3D11Resource* depth);
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Resource> source_;
    Ptr<ID3D11Texture2D> twin_;
    Ptr<ID3D11ShaderResourceView> input_, output_;
    Ptr<ID3D11DepthStencilView> target_;
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11CommandList> draw_;
    Ptr<ID3D11VertexShader> vs_;
    Ptr<ID3D11PixelShader> ps_;
    Ptr<ID3D11Buffer> constants_;
    Ptr<ID3D11DepthStencilState> depth_state_;
    Ptr<ID3D11RasterizerState> raster_;
    D3D11_TEXTURE2D_DESC desc_{};
    DXGI_FORMAT view_format_ = DXGI_FORMAT_UNKNOWN;
    const char* reason_ = "waiting for scene depth";
};

} // namespace na::native11
