#include "shader_dimensions.hpp"
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <tuple>
namespace na::observe {
namespace {
std::string normalized(const char* name) {
    std::string s; if(name)for(const unsigned char* c=reinterpret_cast<const unsigned char*>(name);*c;++c)
        if(std::isalnum(*c))s+=static_cast<char>(std::tolower(*c));
    if(s.size()>1&&s[0]=='g')s.erase(0,1); return s;
}
void variable(ID3D12ShaderReflectionType* type,const char* name,uint32_t offset,
              uint32_t reg,uint32_t space,uint32_t bytes,std::vector<ShaderDimension>& out,unsigned depth=0) {
    D3D12_SHADER_TYPE_DESC t{};if(!type||depth>4||FAILED(type->GetDesc(&t))||t.Elements>1)return;
    if(t.Class==D3D_SVC_STRUCT){for(UINT i=0;i<t.Members;++i){auto* member=type->GetMemberTypeByIndex(i);D3D12_SHADER_TYPE_DESC m{};
        if(member&&SUCCEEDED(member->GetDesc(&m)))variable(member,type->GetMemberTypeName(i),offset+m.Offset,reg,space,bytes,out,depth+1);}return;}
    if(t.Rows!=1||(t.Type!=D3D_SVT_FLOAT&&t.Type!=D3D_SVT_UINT))return;
    const auto n=normalized(name);int first=-1;
    const bool size=n=="screensize"||n=="viewportsize"||n=="rendertargetsize"||n=="rendersize"||n=="renderresolution"||n=="screenresolution"||n=="viewsizeandinversesize"||n=="viewsizeandinvsize";
    const bool inverse=n=="invscreensize"||n=="invviewportsize"||n=="invrendertargetsize"||n=="inversescreensize"||n=="inverserendertargetsize";
    if(t.Columns>=2&&(size||inverse))first=inverse?2:0;
    else if(t.Columns==1){
        if(n=="screenwidth"||n=="viewportwidth"||n=="renderwidth")first=0;
        else if(n=="screenheight"||n=="viewportheight"||n=="renderheight")first=1;
        else if(n=="invscreenwidth"||n=="invviewportwidth"||n=="invrenderwidth")first=2;
        else if(n=="invscreenheight"||n=="invviewportheight"||n=="invrenderheight")first=3;
    }
    if(first<0)return;const UINT count=t.Columns>=2?2:1;
    for(UINT i=0;i<count;++i)if(offset+4*i+4<=bytes&&!(t.Type==D3D_SVT_UINT&&first>=2))
        out.push_back({reg,space,offset+4*i,bytes,static_cast<uint32_t>(first+i),t.Type==D3D_SVT_UINT});
    if(t.Columns==4&&t.Type==D3D_SVT_FLOAT&&(n=="viewsizeandinvsize"||n=="viewsizeandinversesize")){
        for(UINT i=2;i<4;++i)if(offset+4*i+4<=bytes)out.push_back({reg,space,offset+4*i,bytes,i,false});
    }else if(t.Columns==4&&t.Type==D3D_SVT_FLOAT&&(size||inverse)){
        for(UINT i=0;i<2;++i)if(offset+8+4*i+4<=bytes){ShaderDimension field{reg,space,offset+8+4*i,bytes,(inverse?0u:2u)+i,false};field.optional=true;out.push_back(field);}
    }
}
}
void ReflectDimensions(const D3D12_SHADER_BYTECODE& code,std::vector<ShaderDimension>& out,uint32_t visibility) {
    if(!code.pShaderBytecode||!code.BytecodeLength)return;
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> shader;
    if(FAILED(D3DReflect(code.pShaderBytecode,code.BytecodeLength,IID_PPV_ARGS(&shader))))return;
    D3D12_SHADER_DESC desc{};if(FAILED(shader->GetDesc(&desc)))return;
    const size_t start=out.size();
    for(UINT i=0;i<desc.ConstantBuffers;++i){auto* cb=shader->GetConstantBufferByIndex(i);D3D12_SHADER_BUFFER_DESC b{};
        if(!cb||FAILED(cb->GetDesc(&b))||b.Type!=D3D_CT_CBUFFER||b.Size>65536)continue;
        D3D12_SHADER_INPUT_BIND_DESC binding{};if(FAILED(shader->GetResourceBindingDescByName(b.Name,&binding)))continue;
        for(UINT j=0;j<b.Variables;++j){auto* v=cb->GetVariableByIndex(j);D3D12_SHADER_VARIABLE_DESC value{};
            if(v&&SUCCEEDED(v->GetDesc(&value)))variable(v->GetType(),value.Name,value.StartOffset,binding.BindPoint,binding.Space,b.Size,out);}
    }
    for(size_t i=start;i<out.size();++i)out[i].visibility=visibility;
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return std::tie(a.visibility,a.space,a.shader_register,a.offset,a.dimension)<std::tie(b.visibility,b.space,b.shader_register,b.offset,b.dimension);});
    out.erase(std::unique(out.begin(),out.end(),[](const auto& a,const auto& b){return a.visibility==b.visibility&&a.space==b.space&&a.shader_register==b.shader_register&&a.offset==b.offset&&a.dimension==b.dimension&&a.integer==b.integer;}),out.end());
}
}
