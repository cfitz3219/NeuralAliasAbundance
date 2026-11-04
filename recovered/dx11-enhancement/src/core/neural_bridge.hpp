#pragma once
#include "bridge_protocol.hpp"
#include "neural_settings.hpp"
#include <d3d11.h>
#include <filesystem>
#include <memory>
namespace dx11_enhancement {
class NeuralBridge {
public:
    NeuralBridge();~NeuralBridge();
    void start(ID3D11DeviceContext*,UINT width,UINT height,const std::filesystem::path& helper,
        const std::filesystem::path& runtime,const std::filesystem::path& cache,NeuralSettings settings={});
    bool started()const;bool ready();void stop();
    // Inputs must be same-extent textures. Color is display-encoded; no tone mapping here.
    void process(ID3D11DeviceContext*,ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,
        ID3D11ShaderResourceView* motion,ID3D11RenderTargetView* output,BridgeFrame);
    double last_wait_ms()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
