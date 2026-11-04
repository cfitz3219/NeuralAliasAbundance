#include "neural_bridge.hpp"
#include "neural_transport.hpp"
#include <d3dcompiler.h>
namespace dx11_enhancement {
using namespace neural;
struct NeuralBridge::Impl {
    ComPtr<ID3D11Device> device;Texture11 color,depth,motion,output;Fence11 input_ready,output_ready;
    std::unique_ptr<Metadata> metadata;Child child;Handle ready,stop;
    UINT width=0,height=0,sequence=0;uint64_t startup=GetTickCount64();double wait_ms=0;
    ComPtr<ID3D11DeviceContext> deferred;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> copy,restore;
    ComPtr<ID3D11RenderTargetView> color_rt;ComPtr<ID3D11ShaderResourceView> input_view,output_view;
    ~Impl(){if(stop.value)SetEvent(stop.value);if(child.process.hProcess)WaitForSingleObject(child.process.hProcess,100);}
    void blit(ID3D11DeviceContext* context,ID3D11ShaderResourceView* source,ID3D11RenderTargetView* target,bool returning){
        deferred->ClearState();D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};deferred->RSSetViewports(1,&vp);
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);deferred->VSSetShader(vs.Get(),nullptr,0);
        deferred->PSSetShader(returning?restore.Get():copy.Get(),nullptr,0);
        ID3D11ShaderResourceView* views[]={source,returning?input_view.Get():nullptr};deferred->PSSetShaderResources(0,2,views);
        deferred->OMSetRenderTargets(1,&target,nullptr);deferred->Draw(3,0);
        ComPtr<ID3D11CommandList> commands;require(deferred->FinishCommandList(FALSE,&commands),"NR blit recording");context->ExecuteCommandList(commands.Get(),TRUE);
    }
    ComPtr<ID3D11Resource> validate_view(ID3D11ShaderResourceView* view,DXGI_FORMAT expected=DXGI_FORMAT_UNKNOWN){
        if(!view)throw std::runtime_error("missing NR input");
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);ComPtr<ID3D11Resource> r;view->GetResource(&r);
        ComPtr<ID3D11Texture2D> t;require(r.As(&t),"NR input texture");D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);
        if(d.Width!=width || d.Height!=height || d.ArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 ||
            v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || v.Texture2D.MostDetailedMip!=0 ||
            (expected!=DXGI_FORMAT_UNKNOWN && v.Format!=expected))throw std::runtime_error("NR input layout mismatch");
        return r;
    }
};
NeuralBridge::NeuralBridge()=default;NeuralBridge::~NeuralBridge()=default;
bool NeuralBridge::started()const{return bool(impl_);}void NeuralBridge::stop(){impl_.reset();}
double NeuralBridge::last_wait_ms()const{return impl_?impl_->wait_ms:0;}
void NeuralBridge::start(ID3D11DeviceContext* context,UINT w,UINT h,const std::filesystem::path& helper,const std::filesystem::path& runtime,const std::filesystem::path& cache,NeuralSettings settings){
    settings.validate();
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE || !w || !h || w>4096 || h>4096)throw std::runtime_error("NR unsupported context/size");
    stop();auto s=std::make_unique<Impl>();context->GetDevice(&s->device);s->width=w;s->height=h;
    ComPtr<IDXGIDevice> dxgi;require(s->device.As(&dxgi),"NR DXGI device");ComPtr<IDXGIAdapter> adapter;require(dxgi->GetAdapter(&adapter),"NR adapter");DXGI_ADAPTER_DESC info{};require(adapter->GetDesc(&info),"NR adapter info");
    const auto name=L"Local\\DX11Neural-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    s->color.create(s->device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,name+L".color");s->depth.create(s->device.Get(),w,h,DXGI_FORMAT_R32_FLOAT,name+L".depth");
    s->motion.create(s->device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT,name+L".motion");s->output.create(s->device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,name+L".output");
    s->input_ready.create(s->device.Get(),name+L".input-ready");s->output_ready.create(s->device.Get(),name+L".output-ready");
    require(s->device->CreateRenderTargetView(s->color.image.Get(),nullptr,&s->color_rt),"NR color RTV");
    require(s->device->CreateShaderResourceView(s->color.image.Get(),nullptr,&s->input_view),"NR input SRV");
    require(s->device->CreateShaderResourceView(s->output.image.Get(),nullptr,&s->output_view),"NR output SRV");
    require(s->device->CreateDeferredContext(0,&s->deferred),"NR conversion context");
    const char shader[]=R"(Texture2D<float4> source:register(t0);Texture2D<float4> original:register(t1);
float4 vs(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}
float4 ps(float4 p:SV_Position):SV_Target{return source.Load(int3(int2(p.xy),0));}
float4 restore(float4 p:SV_Position):SV_Target{return float4(source.Load(int3(int2(p.xy),0)).rgb,original.Load(int3(int2(p.xy),0)).a);})";
    ComPtr<ID3DBlob> code,errors;
    require(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"NR vertex compile");require(s->device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&s->vs),"NR vertex");
    for(bool returning:{false,true}){code.Reset();errors.Reset();require(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,returning?"restore":"ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"NR pixel compile");
        require(s->device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,returning?&s->restore:&s->copy),"NR pixel");}
    s->metadata=std::make_unique<Metadata>(name,true);
    s->ready=Handle(CreateEventW(nullptr,TRUE,FALSE,(name+L".ready").c_str()));s->stop=Handle(CreateEventW(nullptr,TRUE,FALSE,(name+L".stop").c_str()));
    if(!s->ready.value || !s->stop.value)throw std::runtime_error("NR lifecycle events");std::filesystem::create_directories(cache);settings.save(cache/L"settings.txt");
    std::wstring command=L"\""+helper.wstring()+L"\" "+name+L" "+std::to_wstring(info.AdapterLuid.LowPart)+L" "+std::to_wstring(info.AdapterLuid.HighPart)+L" "+std::to_wstring(w)+L" "+std::to_wstring(h)+L" \""+runtime.wstring()+L"\" \""+cache.wstring()+L"\" "+std::to_wstring(GetCurrentProcessId())+L" 1";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    if(!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&s->child.process))throw std::runtime_error("NR helper launch");
    impl_=std::move(s);
}
bool NeuralBridge::ready(){if(!impl_)return false;
    if(WaitForSingleObject(impl_->child.process.hProcess,0)!=WAIT_TIMEOUT)throw std::runtime_error("NR helper exited; inspect session failure.txt");
    if(WaitForSingleObject(impl_->ready.value,0)==WAIT_OBJECT_0)return true;
    if(GetTickCount64()-impl_->startup>15000)throw std::runtime_error("NR startup deadline");return false;
}
void NeuralBridge::process(ID3D11DeviceContext* context,ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* depth,ID3D11ShaderResourceView* motion,ID3D11RenderTargetView* target,BridgeFrame frame){
    if(!impl_ || !ready() || !target)throw std::runtime_error("NR not ready");auto& s=*impl_;
    s.validate_view(color);auto z=s.validate_view(depth,DXGI_FORMAT_R32_FLOAT),m=s.validate_view(motion,DXGI_FORMAT_R16G16_FLOAT);
    ComPtr<ID3D11Resource> target_resource;target->GetResource(&target_resource);ComPtr<ID3D11Texture2D> target_texture;
    require(target_resource.As(&target_texture),"NR target texture");D3D11_TEXTURE2D_DESC target_desc{};target_texture->GetDesc(&target_desc);
    if(target_desc.Width!=s.width || target_desc.Height!=s.height || target_desc.SampleDesc.Count!=1)throw std::runtime_error("NR target layout mismatch");
    frame.sequence=s.sequence;validate(frame,s.sequence,s.width,s.height);
    s.blit(context,color,s.color_rt.Get(),false);context->CopyResource(s.depth.image.Get(),z.Get());context->CopyResource(s.motion.image.Get(),m.Get());
    std::memcpy(s.metadata->data,&frame,sizeof(frame));MemoryBarrier();s.input_ready.signal(context,UINT64(s.sequence)+1);
    const auto start=std::chrono::steady_clock::now();s.output_ready.wait(UINT64(s.sequence)+1,s.child.process.hProcess,100);
    s.wait_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    s.blit(context,s.output_view.Get(),target,true);++s.sequence;
}
}
