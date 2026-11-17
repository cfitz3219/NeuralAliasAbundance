#pragma once
#include "operation_snapshot.hpp"
namespace na::observe {
// Standard compiler reflection, at PSO creation, never a scan for coincidentally
// matching float values. DXIL PSV0 supplies binding metadata even without named
// constants; optional constant names are never invented. Cached by actual bytes.
bool ReflectDimensions(const D3D12_SHADER_BYTECODE&, std::vector<ShaderDimension>&, uint32_t visibility = D3D12_SHADER_VISIBILITY_ALL,
    std::vector<ShaderResource>* resources = nullptr,bool* pixel_position=nullptr);
// Standard DXBC executable declarations remain available when optional RDEF
// data was stripped. Does not pretend that named constants were recovered.
enum ShaderRejection : uint32_t {ShaderUnknown=1,ShaderUav=2,ShaderDirectHeap=4,ShaderStreamOutput=8};
bool ExecutableReadOnly(const D3D12_SHADER_BYTECODE&,bool* pixel_position=nullptr,uint32_t* rejection=nullptr);
struct ShaderInspectionStats {uint64_t hits=0,misses=0,disassemblies=0,dxil=0,microseconds=0,bytes=0,evictions=0;};
ShaderInspectionStats GetShaderInspectionStats();
}
