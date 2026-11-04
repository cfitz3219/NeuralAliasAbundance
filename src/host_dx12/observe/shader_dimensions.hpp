#pragma once
#include "operation_snapshot.hpp"
namespace na::observe {
// Standard compiler reflection, at PSO creation, never a scan for coincidentally
// matching float values. Stripped/SM6 reflection is simply absent here.
bool ReflectDimensions(const D3D12_SHADER_BYTECODE&, std::vector<ShaderDimension>&, uint32_t visibility = D3D12_SHADER_VISIBILITY_ALL,
    std::vector<ShaderResource>* resources = nullptr,bool* pixel_position=nullptr);
// Standard DXBC executable declarations remain available when optional RDEF
// data was stripped. Does not pretend that named constants were recovered.
bool ExecutableReadOnly(const D3D12_SHADER_BYTECODE&,bool* pixel_position=nullptr);
}
