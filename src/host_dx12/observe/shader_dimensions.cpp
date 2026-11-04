#include "shader_dimensions.hpp"
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <tuple>
#include <string_view>
#include <cstring>
namespace na::observe {
bool ExecutableReadOnly(const D3D12_SHADER_BYTECODE& code,bool* pixel_position){
    if(pixel_position)*pixel_position=false;
    if(!code.pShaderBytecode||!code.BytecodeLength)return false;
    // Avoid invoking the legacy disassembler on potentially large DXIL code.
    // Only standard DXBC executable chunks can establish this fallback.
    if(code.BytecodeLength<32||std::memcmp(code.pShaderBytecode,"DXBC",4))return false;
    const auto* bytes=static_cast<const unsigned char*>(code.pShaderBytecode);
    const auto word=[&](size_t offset){uint32_t n=0;std::memcpy(&n,bytes+offset,4);return n;};
    const uint32_t chunks=word(28);if(chunks>(code.BytecodeLength-32)/4)return false;
    bool executable=false;
    for(uint32_t i=0;i<chunks;++i){const uint32_t offset=word(32+4*i);
        if(offset>code.BytecodeLength-8||word(offset+4)>code.BytecodeLength-offset-8)return false;
        executable|=!std::memcmp(bytes+offset,"SHDR",4)||!std::memcmp(bytes+offset,"SHEX",4);
    }
    if(!executable)return false;
    Microsoft::WRL::ComPtr<ID3DBlob> text;
    if(FAILED(D3DDisassemble(code.pShaderBytecode,code.BytecodeLength,D3D_DISASM_DISABLE_DEBUG_INFO,nullptr,&text)))return false;
    std::string_view s(static_cast<const char*>(text->GetBufferPointer()),text->GetBufferSize());
    bool profile=false;
    while(!s.empty()){
        const auto end=s.find('\n');auto line=s.substr(0,end);
        while(!line.empty()&&(line.front()==' '||line.front()=='\t'||line.front()=='\r'))line.remove_prefix(1);
        if(!line.empty()&&line.substr(0,2)!="//"){
            if(pixel_position&&line.find("dcl_input_ps_siv")==0&&line.find("position")!=std::string_view::npos)*pixel_position=true;
            if(line.size()>=6&&line[2]=='_'&&(line[3]=='4'||line[3]=='5')&&line[4]=='_'&&
               (line[5]=='0'||line[5]=='1')&&(line.substr(0,2)=="vs"||line.substr(0,2)=="ps"||
                line.substr(0,2)=="gs"||line.substr(0,2)=="hs"||line.substr(0,2)=="ds"||line.substr(0,2)=="cs"))profile=true;
            if(line.find("uav")!=std::string_view::npos)return false;
        }
        if(end==std::string_view::npos)break;s.remove_prefix(end+1);
    }
    return profile; // DXIL/unknown programs remain unqualified, never guessed
}
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
bool ReflectDimensions(const D3D12_SHADER_BYTECODE& code,std::vector<ShaderDimension>& out,uint32_t visibility,std::vector<ShaderResource>* resources,bool* pixel_position) {
    if(!code.pShaderBytecode||!code.BytecodeLength)return false;
    Microsoft::WRL::ComPtr<ID3D12ShaderReflection> shader;
    if(FAILED(D3DReflect(code.pShaderBytecode,code.BytecodeLength,IID_PPV_ARGS(&shader))))return false;
    D3D12_SHADER_DESC desc{};if(FAILED(shader->GetDesc(&desc)))return false;
    if(pixel_position){*pixel_position=false;for(UINT i=0;i<desc.InputParameters;++i){D3D12_SIGNATURE_PARAMETER_DESC p{};
        if(SUCCEEDED(shader->GetInputParameterDesc(i,&p))&&p.SystemValueType==D3D_NAME_POSITION)*pixel_position=true;}}
    if(resources)for(UINT i=0;i<desc.BoundResources;++i){D3D12_SHADER_INPUT_BIND_DESC b{};
        if(FAILED(shader->GetResourceBindingDesc(i,&b)))return false;
        uint32_t type=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        if(b.Type==D3D_SIT_CBUFFER)type=D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        else if(b.Type==D3D_SIT_SAMPLER)type=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        else if(b.Type==D3D_SIT_UAV_RWTYPED||b.Type==D3D_SIT_UAV_RWSTRUCTURED||b.Type==D3D_SIT_UAV_RWBYTEADDRESS||
                b.Type==D3D_SIT_UAV_APPEND_STRUCTURED||b.Type==D3D_SIT_UAV_CONSUME_STRUCTURED||b.Type==D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER)type=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        resources->push_back({type,b.BindPoint,b.BindCount,b.Space,visibility});}
    const size_t start=out.size();
    for(UINT i=0;i<desc.ConstantBuffers;++i){auto* cb=shader->GetConstantBufferByIndex(i);D3D12_SHADER_BUFFER_DESC b{};
        if(!cb||FAILED(cb->GetDesc(&b))||b.Type!=D3D_CT_CBUFFER||b.Size>65536)continue;
        D3D12_SHADER_INPUT_BIND_DESC binding{};if(FAILED(shader->GetResourceBindingDescByName(b.Name,&binding)))continue;
        for(UINT j=0;j<b.Variables;++j){auto* v=cb->GetVariableByIndex(j);D3D12_SHADER_VARIABLE_DESC value{};
            if(v&&SUCCEEDED(v->GetDesc(&value))&&(value.uFlags&D3D_SVF_USED))variable(v->GetType(),value.Name,value.StartOffset,binding.BindPoint,binding.Space,b.Size,out);}
    }
    for(size_t i=start;i<out.size();++i)out[i].visibility=visibility;
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return std::tie(a.visibility,a.space,a.shader_register,a.offset,a.dimension)<std::tie(b.visibility,b.space,b.shader_register,b.offset,b.dimension);});
    out.erase(std::unique(out.begin(),out.end(),[](const auto& a,const auto& b){return a.visibility==b.visibility&&a.space==b.space&&a.shader_register==b.shader_register&&a.offset==b.offset&&a.dimension==b.dimension&&a.integer==b.integer;}),out.end());
    return true;
}
}
