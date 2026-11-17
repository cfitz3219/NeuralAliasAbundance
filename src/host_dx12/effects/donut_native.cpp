#include "donut.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/nr/fg_states.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/observe/producers.hpp"
#include <wrl/client.h>
#if defined(NA_HAVE_NGX_SDK)
#include <nvsdk_ngx.h>
#endif
namespace na::donut {
NativeScope::NativeScope(ID3D12GraphicsCommandList* list,const void* parameters) {
#if defined(NA_HAVE_NGX_SDK)
    const auto options=CurrentOptions();
    if((!options.tone&&!options.bloom)||!na::settings::EffectsReady()||!list||!parameters)return;
    na::ngx::EvalParams ep{};if(!na::ngx::ReadEvalParams(parameters,&ep)||!ep.valid||!ep.color)return;
    Inputs in;in.color=reinterpret_cast<ID3D12Resource*>(ep.color);auto td=in.color->GetDesc();in.format=td.Format;
    switch(in.format){
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:in.format=DXGI_FORMAT_R16G16B16A16_FLOAT;break;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:in.format=DXGI_FORMAT_R32G32B32A32_FLOAT;break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:in.format=DXGI_FORMAT_R8G8B8A8_UNORM;break;
    default:break;}
    in.width=ep.subrect_width!=UINT32_MAX?ep.subrect_width:ep.in_width;
    in.height=ep.subrect_height!=UINT32_MAX?ep.subrect_height:ep.in_height;
    in.x=ep.color_base_x!=UINT32_MAX?ep.color_base_x:0;in.y=ep.color_base_y!=UINT32_MAX?ep.color_base_y:0;
    in.srgb=in.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||
        (in.format==DXGI_FORMAT_R8G8B8A8_UNORM&&(ep.create_flags==UINT32_MAX||!(ep.create_flags&1u)));in.preserve_extent=true;in.reset=ep.reset==1;
    D3D12_RESOURCE_STATES state;
    const auto evidence=na::nr::states::Inspect(list,in.color,state);
    if(evidence==na::nr::states::Evidence::NoTransition)state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    else if(evidence!=na::nr::states::Evidence::Known)return;
    if(!na::nr::states::CanTransitionOn(list->GetType(),state))return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return;
    auto frame=ScenePass().Prepare(device.Get(),nullptr,in,options);if(!frame)return;
    if(!na::nr::native::RetainRecording(list,frame))return;
    na::observe::HostWorkScope own;
    auto transition=[&](D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={in.color,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&barrier);};
    transition(state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);const bool recorded=frame->Record(list);
    transition(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,state);if(na::observe::ListStateRestorable(list))na::observe::RestoreListState(list);
    if(!recorded)return;
    auto* p=const_cast<NVSDK_NGX_Parameter*>(static_cast<const NVSDK_NGX_Parameter*>(parameters));
    ID3D12Resource* original=nullptr;if(NVSDK_NGX_FAILED(p->Get(NVSDK_NGX_Parameter_Color,&original))||original!=in.color)return;
    original_=original;parameters_=parameters;p->Set(NVSDK_NGX_Parameter_Color,frame->Color());
#else
    (void)list;(void)parameters;
#endif
}
NativeScope::~NativeScope(){
#if defined(NA_HAVE_NGX_SDK)
    if(parameters_)const_cast<NVSDK_NGX_Parameter*>(static_cast<const NVSDK_NGX_Parameter*>(parameters_))->Set(NVSDK_NGX_Parameter_Color,original_);
#endif
}
}
