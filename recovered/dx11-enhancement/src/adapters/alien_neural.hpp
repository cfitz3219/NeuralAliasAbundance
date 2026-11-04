#pragma once
#include "core/neural_bridge.hpp"
#include "camera_cut.hpp"
#include <wrl/client.h>
#include <string>

namespace alien_isolation {
// Candidate post-sharpen SDR stage. Never falls back to the HUD-composited backbuffer.
class NeuralTrial {
public:
    void request(bool enabled,dx11_enhancement::NeuralSettings settings);
    void reset();
    void render(ID3D11DeviceContext*,ID3D11RenderTargetView*,ID3D11ShaderResourceView* depth,
        ID3D11ShaderResourceView* motion,uint64_t frame,bool current_dlaa,
        const std::array<float,16>& view_projection,bool camera_valid);
    bool needs_camera()const{return requested_ && !failed_;}
    void missing_stage(){if(needs_camera())status_="Neural rendering: waiting for identified post-sharpen scene boundary.";}
    const std::string& status()const{return status_;}
private:
    dx11_enhancement::NeuralBridge bridge_;
    CameraCutDetector cuts_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    dx11_enhancement::NeuralSettings settings_;
    bool requested_=false,failed_=false;
    uint64_t last_frame_=UINT64_MAX,last_time_=0,frames_=0;
    double waits_=0;
    std::string status_="Neural rendering: off.";
};
}
