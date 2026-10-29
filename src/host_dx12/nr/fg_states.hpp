#pragma once
#include <d3d12.h>
namespace na::nr::states {
inline bool CanTransitionOn(D3D12_COMMAND_LIST_TYPE type,D3D12_RESOURCE_STATES state){
    if(type==D3D12_COMMAND_LIST_TYPE_DIRECT)return true;
    if(type!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return false;
    constexpr unsigned allowed=D3D12_RESOURCE_STATE_UNORDERED_ACCESS|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|
        D3D12_RESOURCE_STATE_COPY_SOURCE|D3D12_RESOURCE_STATE_COPY_DEST|D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    return (static_cast<unsigned>(state)&~allowed)==0; // COMMON/PRESENT (0) is legal
}
bool InstallEnhancedInvalidation();
void Observe(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
void Reset(ID3D12GraphicsCommandList*);
bool Read(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES&);
}
