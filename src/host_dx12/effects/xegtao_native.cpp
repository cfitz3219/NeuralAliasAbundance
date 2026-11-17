#include "xegtao.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/fg/camera.hpp"
#include "host_dx12/nr/fg_states.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/observe/producers.hpp"
#include <wrl/client.h>
#if defined(NA_HAVE_NGX_SDK)
#include <nvsdk_ngx.h>
#endif

namespace na::xegtao {
namespace {
#if defined(NA_HAVE_NGX_SDK)
bool read_state(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES& state) {
    const auto evidence=na::nr::states::Inspect(l,r,state);
    if(evidence==na::nr::states::Evidence::Known)return na::nr::states::CanTransitionOn(l->GetType(),state);
    // NGX's named SR input resources have an NPSR read contract. It supplies
    // an entry state only when observation has no contrary/split/enhanced fact.
    if(evidence==na::nr::states::Evidence::NoTransition){state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;return true;}
    return false;
}
void barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    if(from==to)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};l->ResourceBarrier(1,&b);
}
#endif
}
NativeScope::NativeScope(ID3D12GraphicsCommandList* list,const void* parameters) {
#if defined(NA_HAVE_NGX_SDK)
    const auto options=CurrentOptions();
    if(!options.enabled||!options.amount||!na::settings::EffectsReady()||!list||!parameters)return;
    na::ngx::EvalParams ep{};
    if(!na::ngx::ReadEvalParams(parameters,&ep)||!ep.valid||!ep.color||!ep.depth)return;
    na::fg::Camera camera;
    if(!na::fg::FindCamera(na::fg::EvaluationFrame(),na::fg::EvaluationViewport(),camera)||camera.ortho){
        ScenePass().Unavailable("Waiting for this scene's camera.");return;
    }
    Inputs in;in.color=reinterpret_cast<ID3D12Resource*>(ep.color);in.depth=reinterpret_cast<ID3D12Resource*>(ep.depth);
    in.width=ep.subrect_width!=UINT32_MAX?ep.subrect_width:ep.in_width;
    in.height=ep.subrect_height!=UINT32_MAX?ep.subrect_height:ep.in_height;
    // Optional NGX bases default to zero by its input-subrect contract.
    in.color_x=ep.color_base_x==UINT32_MAX?0:ep.color_base_x;in.color_y=ep.color_base_y==UINT32_MAX?0:ep.color_base_y;
    in.depth_x=ep.depth_base_x==UINT32_MAX?0:ep.depth_base_x;in.depth_y=ep.depth_base_y==UINT32_MAX?0:ep.depth_base_y;
    const auto cd=in.color->GetDesc(),dd=in.depth->GetDesc();in.color_format=cd.Format;
    if(cd.Width>UINT32_MAX)return;
    switch(cd.Format){
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:in.color_format=DXGI_FORMAT_R16G16B16A16_FLOAT;break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:in.color_format=DXGI_FORMAT_R8G8B8A8_UNORM;break;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:in.color_format=DXGI_FORMAT_R32G32B32A32_FLOAT;break;
    default:break;
    }
    switch(dd.Format){
    case DXGI_FORMAT_D32_FLOAT:case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_R32_FLOAT:in.depth_format=DXGI_FORMAT_R32_FLOAT;break;
    case DXGI_FORMAT_D16_UNORM:case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_R16_UNORM:in.depth_format=DXGI_FORMAT_R16_UNORM;break;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:case DXGI_FORMAT_R24G8_TYPELESS:in.depth_format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:case DXGI_FORMAT_R32G8X24_TYPELESS:in.depth_format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;break;
    default:return;
    }
    double column[16];for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)column[r*4+c]=camera.view_to_clip[c*4+r];
    if(!FromViewProjection(column,in.projection))return;
    in.jitter_x=ep.jitter_x;in.jitter_y=ep.jitter_y;in.clear_depth=camera.inverted?0.f:1.f;
    in.copy_depth=true;in.preserve_color_extent=true;in.srgb=cd.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    D3D12_RESOURCE_STATES color_state;
    if(!read_state(list,in.color,color_state)||!read_state(list,in.depth,in.depth_state))return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return;
    // The recording's owning queue may not be known yet. Independent scratch
    // avoids assuming the overlay queue or another game's global last queue.
    auto frame=ScenePass().Prepare(device.Get(),nullptr,in,options);if(!frame)return;
    if(!na::nr::native::RetainRecording(list,frame))return;
    na::observe::HostWorkScope own;
    constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier(list,in.color,color_state,read);
    const bool recorded=frame->Record(list);
    barrier(list,in.color,read,color_state);
    if(na::observe::ListStateRestorable(list))na::observe::RestoreListState(list);
    if(!recorded)return;
    auto* p=const_cast<NVSDK_NGX_Parameter*>(static_cast<const NVSDK_NGX_Parameter*>(parameters));
    ID3D12Resource* original=nullptr;
    if(NVSDK_NGX_FAILED(p->Get(NVSDK_NGX_Parameter_Color,&original))||original!=in.color)return;
    original_=original;parameters_=parameters;p->Set(NVSDK_NGX_Parameter_Color,frame->Color());
#else
    (void)list;(void)parameters;
#endif
}
NativeScope::~NativeScope() {
#if defined(NA_HAVE_NGX_SDK)
    if(parameters_)const_cast<NVSDK_NGX_Parameter*>(static_cast<const NVSDK_NGX_Parameter*>(parameters_))->Set(NVSDK_NGX_Parameter_Color,original_);
#endif
}
} // namespace na::xegtao
