#include "shader_dimensions.hpp"
#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <tuple>
#include <string_view>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <chrono>
namespace na::observe {
namespace {
bool legacy_readonly(const D3D12_SHADER_BYTECODE& code,bool* pixel_position,uint32_t* rejection){
    if(rejection)*rejection=ShaderUnknown;
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
            if(line.find("uav")!=std::string_view::npos){if(rejection)*rejection=ShaderUav;return false;}
        }
        if(end==std::string_view::npos)break;s.remove_prefix(end+1);
    }
    if(profile&&rejection)*rejection=0;
    return profile; // DXIL is handled separately from its actual PSV0 metadata
}
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
static bool reflect_legacy(const D3D12_SHADER_BYTECODE& code,std::vector<ShaderDimension>& out,uint32_t visibility,std::vector<ShaderResource>* resources,bool* pixel_position) {
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
namespace {
struct Inspection {
    std::vector<ShaderDimension> dimensions;
    std::vector<ShaderResource> resources;
    bool bindings=false,readonly=false,position=false;
    uint32_t rejection=ShaderUnknown;
};
uint32_t u32(const unsigned char* p){uint32_t v;std::memcpy(&v,p,4);return v;}
struct Part {const unsigned char* data=nullptr;size_t size=0;};
bool dxil_metadata(const D3D12_SHADER_BYTECODE& code,Inspection& result){
    // Microsoft's DXIL container + PSV0 binding metadata, not an LLVM/text guess.
    // Layout: DirectXShaderCompiler/include/dxc/DxilContainer/{DxilContainer,
    // DxilPipelineStateValidation}.h. Native PSO creation has accepted the code.
    const auto* b=static_cast<const unsigned char*>(code.pShaderBytecode);const size_t n=code.BytecodeLength;
    if(n<32||std::memcmp(b,"DXBC",4)||u32(b+24)!=n)return false;
    const UINT count=u32(b+28);if(count>(n-32)/4)return false;
    Part program,psv,features,input;
    for(UINT i=0;i<count;++i){const size_t offset=u32(b+32+4*i);
        if(offset<32+4*count||offset>n-8)return false;
        const size_t size=u32(b+offset+4);if(size>n-offset-8)return false;
        Part* part=!std::memcmp(b+offset,"DXIL",4)?&program:!std::memcmp(b+offset,"PSV0",4)?&psv:
                   !std::memcmp(b+offset,"SFI0",4)?&features:!std::memcmp(b+offset,"ISG1",4)?&input:nullptr;
        if(part){if(part->data)return false;*part={b+offset+8,size};}
    }
    if(!program.data||program.size<24||!psv.data||psv.size<32||!features.data||features.size!=8)return false;
    const UINT version=u32(program.data),stage=version>>16;
    if(((version>>4)&15)!=6||stage>5||u32(program.data+4)>program.size/4||std::memcmp(program.data+8,"DXIL",4))return false;
    const UINT bit_offset=u32(program.data+16),bit_size=u32(program.data+20);
    if(bit_offset<16||bit_offset>program.size-8||bit_size>program.size-8-bit_offset)return false;
    uint64_t flags;std::memcpy(&flags,features.data,8);
    if(flags&D3D_SHADER_REQUIRES_RESOURCE_DESCRIPTOR_HEAP_INDEXING){result.rejection=ShaderDirectHeap;return true;}
    const size_t runtime=u32(psv.data);
    if(runtime<24||runtime>psv.size-8)return false;
    // PSVRuntimeInfo1 follows the 24-byte RuntimeInfo0 and records ShaderStage.
    if(runtime>=36&&psv.data[4+24]!=stage)return false;
    size_t offset=4+runtime;const size_t resources=u32(psv.data+offset);offset+=4;
    std::vector<ShaderResource> bindings;
    if(resources){
        if(offset>psv.size-4)return false;const size_t stride=u32(psv.data+offset);offset+=4;
        if(stride<16||resources>(psv.size-offset)/stride)return false;
        for(size_t i=0;i<resources;++i){const auto* r=psv.data+offset+i*stride;
            const UINT type=u32(r),space=u32(r+4),lo=u32(r+8),hi=u32(r+12);
            if(type<1||type>9||hi<lo)return false;
            const UINT kind=type==1?D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:type==2?D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
                type<=5?D3D12_DESCRIPTOR_RANGE_TYPE_SRV:D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            bindings.push_back({kind,lo,hi==UINT_MAX?UINT_MAX:hi-lo+1,space,D3D12_SHADER_VISIBILITY_ALL});
        }
    }
    bool position=false;
    if(input.data){
        if(input.size<8)return false;const size_t elements=u32(input.data),first=u32(input.data+4);
        if(first<8||first>input.size||elements>(input.size-first)/32)return false;
        for(size_t i=0;i<elements;++i)position|=u32(input.data+first+i*32+12)==D3D_NAME_POSITION;
    }
    result.bindings=true;result.resources=std::move(bindings);result.position=stage==0&&position;
    result.readonly=std::none_of(result.resources.begin(),result.resources.end(),[](const auto& r){return r.type==D3D12_DESCRIPTOR_RANGE_TYPE_UAV;});
    result.rejection=result.readonly?0u:ShaderUav;return true;
}
struct Cached {std::vector<unsigned char> code;std::shared_ptr<const Inspection> result;};
std::mutex inspection_mutex;
std::unordered_multimap<uint64_t,Cached> inspection_cache;
ShaderInspectionStats inspection_stats;
std::shared_ptr<const Inspection> inspect(const D3D12_SHADER_BYTECODE& code){
    if(!code.pShaderBytecode||!code.BytecodeLength||code.BytecodeLength>4*1024*1024)return std::make_shared<Inspection>();
    const auto* bytes=static_cast<const unsigned char*>(code.pShaderBytecode);uint64_t key=1469598103934665603ull;
    for(size_t i=0;i<code.BytecodeLength;++i){key^=bytes[i];key*=1099511628211ull;}
    // Serialize misses, so concurrent PSO creation does not inspect a shared
    // stage thousands of times. Hit identity includes byte equality, not hash only.
    std::lock_guard<std::mutex> lock(inspection_mutex);
    const auto range=inspection_cache.equal_range(key);
    for(auto i=range.first;i!=range.second;++i)if(i->second.code.size()==code.BytecodeLength&&
        !std::memcmp(i->second.code.data(),bytes,code.BytecodeLength)){++inspection_stats.hits;return i->second.result;}
    const auto start=std::chrono::steady_clock::now();auto result=std::make_shared<Inspection>();
    if(dxil_metadata(code,*result))++inspection_stats.dxil;
    else {
        // RDEF is optional: D3DReflect can still return a signature-only object
        // for stripped code. That does not prove its resource bindings are empty.
        bool rdef=false;
        if(code.BytecodeLength>=32&&!std::memcmp(bytes,"DXBC",4)){
            const size_t count=u32(bytes+28);
            if(count<=(code.BytecodeLength-32)/4)for(size_t i=0;i<count;++i){const size_t off=u32(bytes+32+4*i);
                if(off<=code.BytecodeLength-8&&u32(bytes+off+4)<=code.BytecodeLength-off-8&&!std::memcmp(bytes+off,"RDEF",4))rdef=true;}
        }
        if(rdef)result->bindings=reflect_legacy(code,result->dimensions,D3D12_SHADER_VISIBILITY_ALL,&result->resources,&result->position);
        ++inspection_stats.disassemblies;result->readonly=legacy_readonly(code,&result->position,&result->rejection);
        const bool uav=std::any_of(result->resources.begin(),result->resources.end(),[](const auto& r){return r.type==D3D12_DESCRIPTOR_RANGE_TYPE_UAV;});
        if(uav){result->readonly=false;result->rejection=ShaderUav;}
    }
    ++inspection_stats.misses;
    inspection_stats.microseconds+=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    constexpr size_t budget=32*1024*1024;
    if(inspection_stats.bytes+code.BytecodeLength>budget||inspection_cache.size()>=16384){inspection_cache.clear();inspection_stats.bytes=0;++inspection_stats.evictions;}
    inspection_stats.bytes+=code.BytecodeLength;
    inspection_cache.emplace(key,Cached{{bytes,bytes+code.BytecodeLength},result});return result;
}
}
bool ReflectDimensions(const D3D12_SHADER_BYTECODE& code,std::vector<ShaderDimension>& out,uint32_t visibility,std::vector<ShaderResource>* resources,bool* pixel_position){
    const auto info=inspect(code);if(pixel_position)*pixel_position=info->position;
    if(!info->bindings)return false;
    for(auto d:info->dimensions){d.visibility=visibility;out.push_back(d);}
    if(resources)for(auto r:info->resources){r.visibility=visibility;resources->push_back(r);}
    return true;
}
bool ExecutableReadOnly(const D3D12_SHADER_BYTECODE& code,bool* pixel_position,uint32_t* rejection){
    const auto info=inspect(code);if(pixel_position)*pixel_position=info->position;if(rejection)*rejection=info->rejection;return info->readonly;
}
ShaderInspectionStats GetShaderInspectionStats(){std::lock_guard<std::mutex> lock(inspection_mutex);return inspection_stats;}
}
