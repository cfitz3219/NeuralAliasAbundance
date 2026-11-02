#pragma once
#include "operation_snapshot.hpp"
namespace na::observe {
// Standard compiler reflection, at PSO creation, never a scan for coincidentally
// matching float values. Stripped/SM6 reflection is simply absent here.
void ReflectDimensions(const D3D12_SHADER_BYTECODE&, std::vector<ShaderDimension>&);
}
