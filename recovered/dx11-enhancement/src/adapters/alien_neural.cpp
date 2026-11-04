#include "alien_neural.hpp"
#include <reshade.hpp>
#include <fstream>
#include <stdexcept>
#include <cstdio>

namespace alien_isolation {
using Microsoft::WRL::ComPtr;
static void check(HRESULT result,const char* message){if(FAILED(result))throw std::runtime_error(message);}
void NeuralTrial::reset(){
    bridge_.stop();view_.Reset();color_.Reset();last_frame_=UINT64_MAX;last_time_=0;frames_=0;waits_=0;
    if(!failed_)status_=requested_?"Neural rendering: waiting for DLAA gameplay.":"Neural rendering: off.";
}
void NeuralTrial::request(bool enabled,dx11_enhancement::NeuralSettings settings){
    if(enabled==requested_ && settings==settings_)return;
    settings_=settings;
    requested_=enabled;failed_=false;reset();
    reshade::log::message(reshade::log::level::info,status_.c_str());
}
void NeuralTrial::render(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,
    ID3D11ShaderResourceView* depth,ID3D11ShaderResourceView* motion,uint64_t frame,bool current_dlaa){
    if(!requested_ || failed_)return;
    if(!current_dlaa){status_="Neural rendering: waiting for DLAA gameplay.";return;}
    try{
        if(!target || !depth || !motion)throw std::runtime_error("missing current-frame inputs");
        D3D11_RENDER_TARGET_VIEW_DESC vd{};target->GetDesc(&vd);
        if(vd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice!=0 ||
            (vd.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && vd.Format!=DXGI_FORMAT_B8G8R8A8_UNORM))
            throw std::runtime_error("first trial requires an SDR UNORM backbuffer view");
        ComPtr<ID3D11Resource> resource;target->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;check(resource.As(&source),"backbuffer is not a texture");
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if(desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1)
            throw std::runtime_error("unsupported backbuffer layout");
        if(color_){
            D3D11_TEXTURE2D_DESC old{};color_->GetDesc(&old);
            if(old.Width!=desc.Width || old.Height!=desc.Height || old.Format!=vd.Format)reset();
        }
        ComPtr<ID3D11Device> device;context->GetDevice(&device);
        if(!color_){
            desc.Format=vd.Format;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags=0;desc.MiscFlags=0;
            check(device->CreateTexture2D(&desc,nullptr,&color_),"final-color copy allocation failed");
            check(device->CreateShaderResourceView(color_.Get(),nullptr,&view_),"final-color view failed");
        }
        if(!bridge_.started()){
            wchar_t exe[32768]{};if(!GetModuleFileNameW(nullptr,exe,32768))throw std::runtime_error("game path unavailable");
            std::ifstream config(std::filesystem::path(exe).parent_path()/L"dx11-neural-paths.txt");
            std::string helper,runtime,cache;
            if(!std::getline(config,helper) || !std::getline(config,runtime) || !std::getline(config,cache))
                throw std::runtime_error("helper configuration missing");
            bridge_.start(context,desc.Width,desc.Height,std::filesystem::u8path(helper),std::filesystem::u8path(runtime),
                std::filesystem::u8path(cache)/std::to_string(GetTickCount64()),settings_);
        }
        if(!bridge_.ready()){status_="Neural rendering: starting helper; DLAA continues.";return;}
        // Copy encoded SDR values without an sRGB decode. Bridge preserves original alpha.
        context->CopyResource(color_.Get(),source.Get());
        dx11_enhancement::BridgeFrame metadata;
        metadata.width=desc.Width;metadata.height=desc.Height;metadata.source_frame=frame;
        metadata.motion_scale_x=-float(desc.Width);metadata.motion_scale_y=-float(desc.Height);
        const auto now=GetTickCount64(),elapsed=last_time_?now-last_time_:0;
        metadata.reset=last_frame_==UINT64_MAX || frame!=last_frame_+1 || elapsed>250;
        metadata.frame_ms=elapsed && elapsed<=250?float(elapsed):16.6667f;
        bridge_.process(context,view_.Get(),depth,motion,target,metadata);
        last_frame_=frame;last_time_=now;++frames_;waits_+=bridge_.last_wait_ms();
        status_="Neural rendering: active (experimental; includes game HUD).";
        if(frames_==1){
            char line[256]{};std::snprintf(line,sizeof(line),
                "[DX11Neural] First result: final SDR %ux%u format=%u; before ReShade effects; HUD included; controls recorded in helper session settings.txt.",
                desc.Width,desc.Height,static_cast<unsigned>(vd.Format));
            reshade::log::message(reshade::log::level::info,line);
        }
        if(frames_%120==0){
            char line[192]{};std::snprintf(line,sizeof(line),"[DX11Neural] 120-frame mean CPU output wait %.3fms; not GPU timing or FPS.",waits_/120);
            reshade::log::message(reshade::log::level::info,line);waits_=0;
        }
    }catch(const std::exception& error){
        failed_=true;reset();status_=std::string("Neural rendering off: ")+error.what()+". Uncheck/check to retry.";
        reshade::log::message(reshade::log::level::warning,status_.c_str());
    }
}
}
