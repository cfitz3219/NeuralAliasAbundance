#pragma once
#include <d3d12.h>
namespace na::nr::states {
bool InstallEnhancedInvalidation();
void Observe(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
void Reset(ID3D12GraphicsCommandList*);
bool Read(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES&);
}
