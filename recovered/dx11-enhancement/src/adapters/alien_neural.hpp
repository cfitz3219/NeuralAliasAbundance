#pragma once
#include "core/neural_bridge.hpp"
#include <wrl/client.h>
#include <string>

namespace alien_isolation {
// Experimental final SDR stage: game HUD pixels are included, without a UI mask.
class NeuralTrial {
public:
    void request(bool enabled,dx11_enhancement::NeuralSettings settings);
    void reset();
    void render(ID3D11DeviceContext*,ID3D11RenderTargetView*,ID3D11ShaderResourceView* depth,
        ID3D11ShaderResourceView* motion,uint64_t frame,bool current_dlaa);
    const std::string& status()const{return status_;}
private:
    dx11_enhancement::NeuralBridge bridge_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    dx11_enhancement::NeuralSettings settings_;
    bool requested_=false,failed_=false;
    uint64_t last_frame_=UINT64_MAX,last_time_=0,frames_=0;
    double waits_=0;
    std::string status_="Neural rendering: off.";
};
}
