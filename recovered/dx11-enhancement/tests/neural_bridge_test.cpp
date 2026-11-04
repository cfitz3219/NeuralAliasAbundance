#include "core/neural_bridge.hpp"
#include "core/bridge_transport.hpp"
#include <thread>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
using namespace dx11_enhancement;
using namespace dx11_enhancement::transport;
template<class T> void read(const std::filesystem::path& path,std::vector<T>& out){
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()!=static_cast<std::streamoff>(out.size()*sizeof(T)))throw std::runtime_error("test input length");
    f.seekg(0);f.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(out.size()*sizeof(T)));if(!f)throw std::runtime_error("test input read");
}
int wmain(int argc,wchar_t** argv){try{
    if((argc!=6 && argc!=7) || sizeof(void*)!=4)throw std::runtime_error("x86 test: helper runtime prepared-input direct-reference cache");
    const std::filesystem::path prepared=argv[3],reference=argv[4];
    UINT w=0,h=0,count=0;float exposure=0;std::ifstream cfg(prepared/L"replay.txt");cfg>>w>>h>>count>>exposure;
    if(!cfg || count!=2)throw std::runtime_error("replay config");
    Device d;
    auto make=[&](DXGI_FORMAT format,UINT binds){D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.Format=format;
        desc.MipLevels=desc.ArraySize=1;desc.SampleDesc.Count=1;desc.BindFlags=binds;ComPtr<ID3D11Texture2D> t;
        require(d.device->CreateTexture2D(&desc,nullptr,&t),"test texture");return t;};
    auto color=make(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto depth=make(DXGI_FORMAT_R32_FLOAT,D3D11_BIND_SHADER_RESOURCE),motion=make(DXGI_FORMAT_R16G16_FLOAT,D3D11_BIND_SHADER_RESOURCE);
    auto output=make(DXGI_FORMAT_R16G16B16A16_FLOAT,D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11ShaderResourceView> c,z,m;ComPtr<ID3D11RenderTargetView> rt;
    require(d.device->CreateShaderResourceView(color.Get(),nullptr,&c),"test color view");require(d.device->CreateShaderResourceView(depth.Get(),nullptr,&z),"test depth view");
    require(d.device->CreateShaderResourceView(motion.Get(),nullptr,&m),"test motion view");require(d.device->CreateRenderTargetView(output.Get(),nullptr,&rt),"test output view");
    D3D11_TEXTURE2D_DESC desc{};output->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;require(d.device->CreateTexture2D(&desc,nullptr,&staging),"test readback");
    const auto settings=argc==7?NeuralSettings::read(argv[6]):NeuralSettings{};
    NeuralBridge bridge;bridge.start(d.context.Get(),w,h,argv[1],argv[2],argv[5],settings);
    while(!bridge.ready())std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<uint16_t> pixels(size_t(w)*h*4),vectors(size_t(w)*h*2),expected(pixels.size());std::vector<float> depths(size_t(w)*h);
    for(UINT i=0;i<2;++i){
        read(reference/("input"+std::to_string(i)+".rgba16"),pixels);read(reference/("output"+std::to_string(i)+".rgba16"),expected);
        read(prepared/("depth"+std::to_string(i)+".raw"),depths);read(prepared/("motion"+std::to_string(i)+".raw"),vectors);
        d.context->UpdateSubresource(color.Get(),0,nullptr,pixels.data(),w*8,0);d.context->UpdateSubresource(depth.Get(),0,nullptr,depths.data(),w*4,0);d.context->UpdateSubresource(motion.Get(),0,nullptr,vectors.data(),w*4,0);
        D3D11_VIEWPORT vp{7,9,111,113,.1f,.9f};d.context->RSSetViewports(1,&vp);d.context->PSSetShaderResources(0,1,c.GetAddressOf());
        BridgeFrame frame;frame.width=w;frame.height=h;frame.reset=i==0;frame.motion_scale_x=-float(w);frame.motion_scale_y=-float(h);
        bridge.process(d.context.Get(),c.Get(),z.Get(),m.Get(),rt.Get(),frame);
        D3D11_VIEWPORT restored{};UINT n=1;d.context->RSGetViewports(&n,&restored);ComPtr<ID3D11ShaderResourceView> view;d.context->PSGetShaderResources(0,1,&view);
        if(std::memcmp(&vp,&restored,sizeof(vp)) || view!=c)throw std::runtime_error("host graphics state changed");
        d.context->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};require(d.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"readback map");
        std::ofstream result(std::filesystem::path(argv[5])/("output"+std::to_string(i)+".rgba16"),std::ios::binary);
        for(UINT y=0;y<h;++y)result.write(static_cast<const char*>(mapped.pData)+size_t(y)*mapped.RowPitch,w*8);
        uint64_t mismatch=0;for(UINT y=0;y<h;++y){const auto* row=reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData)+size_t(y)*mapped.RowPitch);
            for(UINT x=0;x<w;++x)for(UINT channel=0;channel<4;++channel){const size_t offset=(size_t(y)*w+x)*4+channel;
                mismatch+=row[x*4+channel]!=(channel==3?pixels[offset]:expected[offset]);}}
        d.context->Unmap(staging.Get(),0);if(mismatch && argc==6)throw std::runtime_error("NR shared output differs from direct reference or alpha changed");
        if(argc==7)std::printf("VARIANT frame%u reference-channel differences=%llu; output saved.\n",i,mismatch);
        else std::printf("PASS frame%u: RGB bit-exact versus direct DX12 NR; alpha/viewport/SRV preserved; wait %.3fms (not game FPS).\n",i,bridge.last_wait_ms());
    }
    // Exercise the two encoded 8-bit backbuffer layouts accepted by the live adapter.
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}){
        auto source8=make(format,D3D11_BIND_SHADER_RESOURCE),output8=make(format,D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11ShaderResourceView> view8;ComPtr<ID3D11RenderTargetView> target8;
        require(d.device->CreateShaderResourceView(source8.Get(),nullptr,&view8),"SDR source view");
        require(d.device->CreateRenderTargetView(output8.Get(),nullptr,&target8),"SDR target view");
        std::vector<unsigned char> bytes(size_t(w)*h*4);
        for(size_t pixel=0;pixel<size_t(w)*h;++pixel){
            for(size_t channel=0;channel<3;++channel){
                const auto input_channel=format==DXGI_FORMAT_B8G8R8A8_UNORM?2-channel:channel;
                const float value=DirectX::PackedVector::XMConvertHalfToFloat(pixels[pixel*4+input_channel]);
                bytes[pixel*4+channel]=static_cast<unsigned char>(std::lround(std::clamp(value,0.f,1.f)*255));
            }
            bytes[pixel*4+3]=static_cast<unsigned char>(pixel%256);
        }
        d.context->UpdateSubresource(source8.Get(),0,nullptr,bytes.data(),w*4,0);
        BridgeFrame f;f.width=w;f.height=h;f.reset=1;f.motion_scale_x=-float(w);f.motion_scale_y=-float(h);
        bridge.process(d.context.Get(),view8.Get(),z.Get(),m.Get(),target8.Get(),f);
        output8->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read8;require(d.device->CreateTexture2D(&desc,nullptr,&read8),"SDR staging");
        d.context->CopyResource(read8.Get(),output8.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        require(d.context->Map(read8.Get(),0,D3D11_MAP_READ,0,&mapped),"SDR map");
        uint64_t changed=0,nonzero=0,alpha_errors=0;
        for(UINT y=0;y<h;++y){const auto* row=static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch;
            for(UINT x=0;x<w;++x){const size_t offset=(size_t(y)*w+x)*4;
                for(UINT ch=0;ch<3;++ch){changed+=row[x*4+ch]!=bytes[offset+ch];nonzero+=row[x*4+ch]!=0;}
                alpha_errors+=row[x*4+3]!=bytes[offset+3];}}
        d.context->Unmap(read8.Get(),0);
        if(alpha_errors || (settings.intensity!=0 && changed<100) || nonzero<100)throw std::runtime_error("SDR alpha/output check failed");
        std::printf("PASS SDR format%u: nonempty changed RGB; all 256 alpha levels preserved. Not a visual-quality test.\n",unsigned(format));
    }
    bridge.stop();std::puts("PASS: actual x86 host -> x64 DX12 neural helper -> x86 output.");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
