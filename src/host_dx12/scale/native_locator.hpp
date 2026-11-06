#pragma once
// Reuses Ghost's mapped-PE/HDE reader. Addresses come from named-setting and
// instruction relationships, never an executable hash or per-build RVA table.
#include "host_dx12/adapters/tsushima/locator.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace na::scale::native::locator {
using Image=na::tsushima::locator::Image;
using Function=na::tsushima::locator::Function;
using na::tsushima::locator::mapped_image;
struct Ins {
    uint32_t at=0,rip=0,call=0; hde64s h{};
    unsigned reg()const{return h.modrm_reg+(h.rex_r?8u:0u);}
    unsigned rm()const{return h.modrm_rm+(h.rex_b?8u:0u);}
    bool op(unsigned second)const{return h.opcode==0x0f&&h.opcode2==second;}
    bool scalar(unsigned second)const{return h.p_rep==0xf3&&op(second);}
};
struct Result {
    uint32_t setting=0,cache=0,active=0,context=0,apply=0,tick=0;
    uint32_t full_w=0,full_h=0,scene_w=0,scene_h=0,update=0;
    uint32_t upscale_setting=0,upscale_cache=0;
    const char* reason="Native render-scale settings/apply relationship was not recognized.";
    explicit operator bool()const{return setting&&cache&&active&&context&&apply&&tick&&full_w&&full_h&&scene_w&&scene_h&&update;}
};
inline Function owner(const Image& im,uint32_t at){
    auto it=std::upper_bound(im.functions.begin(),im.functions.end(),at,[](uint32_t a,Function f){return a<f.start;});
    if(it==im.functions.begin())return {};--it;
    return at-it->start<it->size?*it:Function{};
}
inline std::vector<Ins> decode(const Image& im,Function f,bool stop_ret=false){
    std::vector<Ins> out;
    if(!f.size||!im.contains(f.start,f.size,true))return out;
    for(uint32_t p=f.start;p<f.start+f.size;){
        uint8_t bytes[32]{};std::memcpy(bytes,im.bytes+p,std::min<size_t>(15,f.start+f.size-p));
        Ins i;i.at=p;hde64_disasm(bytes,&i.h);
        if(!i.h.len||i.h.flags&F_ERROR||i.h.len>f.start+f.size-p)return {};
        if((i.h.flags&F_MODRM)&&i.h.modrm_mod==0&&i.h.modrm_rm==5&&!i.h.p_67){
            const int64_t r=int64_t(p)+i.h.len+int32_t(i.h.disp.disp32);
            if(r>=0&&uint64_t(r)<im.size)i.rip=uint32_t(r);
        }
        if(i.h.opcode==0xe8){const int64_t r=int64_t(p)+i.h.len+int32_t(i.h.imm.imm32);if(r>=0&&uint64_t(r)<im.size)i.call=uint32_t(r);}
        out.push_back(i);p+=i.h.len;if(stop_ret&&i.h.opcode==0xc3)break;
    }
    return out;
}
inline uint32_t primary(const Image& im,uint32_t at){
    auto f=owner(im,at);if(!f.size)return 0;
    const auto dos=im.read<IMAGE_DOS_HEADER>(0);const auto nt=im.read<IMAGE_NT_HEADERS64>(uint32_t(dos.e_lfanew));
    const auto d=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    uint32_t unwind=0;
    for(uint32_t p=0;p<d.Size;p+=sizeof(RUNTIME_FUNCTION)){
        auto r=im.read<RUNTIME_FUNCTION>(d.VirtualAddress+p);if(r.BeginAddress==f.start){unwind=r.UnwindData;break;}}
    for(unsigned depth=0;unwind&&depth<16;++depth){
        if(!im.contains(unwind,4,false))return 0;
        const auto flags=im.bytes[unwind]>>3;if(!(flags&UNW_FLAG_CHAININFO))return f.start;
        const auto tail=unwind+4+((im.bytes[unwind+2]+1u)&~1u)*2;
        if(!im.contains(tail,sizeof(RUNTIME_FUNCTION),false))return 0;
        auto r=im.read<RUNTIME_FUNCTION>(tail);f.start=r.BeginAddress;unwind=r.UnwindData;
    }
    return 0;
}
inline std::vector<Ins> refs(const Image& im,uint32_t target){
    std::map<uint32_t,Ins> found;std::map<uint32_t,std::vector<Ins>> decoded;
    for(auto range:im.ranges)if(range.executable&&im.contains(range.start,range.size,true)){
        for(uint32_t p=range.start;p+5<=range.start+range.size;++p){
            if((im.bytes[p]&0xc7)!=5)continue;
            const auto candidate=int64_t(p)+5+im.read<int32_t>(p+1);
            if(candidate!=target)continue;
            auto f=owner(im,p);if(!f.size)continue;
            auto it=decoded.find(f.start);if(it==decoded.end())it=decoded.emplace(f.start,decode(im,f)).first;
            for(const auto& i:it->second)if(i.at<=p&&p<i.at+i.h.len){if(i.rip==target)found.emplace(i.at,i);break;}
        }
    }
    std::vector<Ins> out;for(auto v:found)out.push_back(v.second);return out;
}
inline uint32_t anchor(const Image& im,const void* bytes,size_t count){
    uint32_t result=0;unsigned found=0;
    for(auto range:im.ranges)if(!range.executable&&im.contains(range.start,range.size,false)){
        const auto first=im.bytes+range.start,last=first+range.size;
        for(auto at=first;at<last;){
            auto match=std::search(at,last,static_cast<const uint8_t*>(bytes),static_cast<const uint8_t*>(bytes)+count);
            if(match==last)break;result=uint32_t(match-im.bytes);++found;at=match+1;
        }
    }
    return found==1?result:0;
}
inline size_t index(const std::vector<Ins>& code,uint32_t at){
    for(size_t n=0;n<code.size();++n)if(code[n].at==at)return n;return code.size();
}
inline bool quantizer(const Image& im,const std::vector<Ins>& code,size_t end){
    std::vector<float> values;
    for(size_t n=end>32?end-32:0;n<end;++n)if(code[n].scalar(0x59)&&im.contains(code[n].rip,4,false))values.push_back(im.read<float>(code[n].rip));
    return values.size()==2&&std::abs(values[0]-20.f)<1e-6f&&std::abs(values[1]-.05f)<1e-7f;
}
struct Chain{uint32_t active=0,fw=0,fh=0,sw=0,sh=0,update=0;explicit operator bool()const{return active&&fw&&fh&&sw&&sh&&update;}};
inline Chain dimensions(const Image& im,const std::vector<Ins>& code,unsigned scale,uint32_t cache){
    struct Value{uint32_t origin=0;bool scaled=false;};
    std::array<Value,16> integer{},floating{};std::vector<std::array<uint32_t,3>> dims;std::set<uint32_t> scales;
    for(auto i:code){
        if(i.scalar(0x11)&&i.rip&&i.reg()==scale&&i.rip!=cache)scales.insert(i.rip);
        if(i.h.opcode==0x8b)integer[i.reg()]={i.rip,false};
        else if(i.scalar(0x2a)&&i.h.modrm_mod==3)floating[i.reg()]=integer[i.rm()];
        else if(i.scalar(0x59)&&i.h.modrm_mod==3&&i.rm()==scale)floating[i.reg()].scaled=true;
        else if(i.scalar(0x2c)&&i.h.modrm_mod==3)integer[i.reg()]=floating[i.rm()];
        else if(i.h.opcode==0x89&&i.rip){auto v=integer[i.reg()];if(v.origin&&v.scaled)dims.push_back({v.origin,i.rip,i.at});}
    }
    if(dims.size()!=2||scales.size()!=1)return {};
    Chain c{*scales.begin(),dims[0][0],dims[1][0],dims[0][1],dims[1][1],0};
    if(c.fw==c.fh||c.sw==c.sh)return {};
    for(auto i:code)if(i.at>dims.back()[2]&&i.call){c.update=i.call;break;}
    for(auto p:{c.active,c.fw,c.fh,c.sw,c.sh})if(!im.contains(p,4,false))return {};
    return c;
}
inline Result locate(const Image& im){
    if(!im.bytes||im.functions.empty())return {};
    constexpr wchar_t name[]=L"Frontend.Options.Display.RenderScale";constexpr char key[]="RenderScale";
    const auto ui=anchor(im,name,sizeof(name)),parser=anchor(im,key,sizeof(key));if(!ui||!parser)return {};
    std::set<uint32_t> settings;
    for(auto ref:refs(im,ui)){
        if(ref.h.opcode!=0x8d||!ref.h.rex_w||ref.reg()!=1)continue;
        auto code=decode(im,owner(im,ref.at));auto k=index(code,ref.at);
        for(size_t n=k>6?k-6:0;n<k;++n)if(code[n].h.opcode==0x8d&&code[n].h.rex_w&&code[n].reg()==0&&im.contains(code[n].rip,4,false))settings.insert(code[n].rip);
    }
    if(settings.size()!=1)return {};const auto setting=*settings.begin();bool parsed=false;
    for(auto ref:refs(im,parser)){auto code=decode(im,owner(im,ref.at));for(size_t n=0;n<code.size();++n)
        if(code[n].scalar(0x11)&&code[n].rip==setting&&quantizer(im,code,n))parsed=true;}
    if(!parsed)return {};
    std::vector<Result> results;
    const auto setting_refs=refs(im,setting);
    for(auto mirror:setting_refs){
        if(!mirror.scalar(0x11))continue;auto code=decode(im,owner(im,mirror.at));const auto k=index(code,mirror.at);
        for(size_t r=k>4?k-4:0;r<k;++r){
            const auto load=code[r];if(!load.scalar(0x10)||load.reg()!=mirror.reg()||!load.rip||load.rip==setting)continue;
            for(size_t s=0;s<r;++s){
                const auto store=code[s];if(!store.scalar(0x11)||store.rip!=load.rip)continue;
                auto accept=[&](Chain c,uint32_t helper){
                    if(!c)return;Result result;result.setting=setting;result.cache=load.rip;result.apply=primary(im,mirror.at);
                    result.active=c.active;result.full_w=c.fw;result.full_h=c.fh;result.scene_w=c.sw;result.scene_h=c.sh;result.update=c.update;
                    // Find the normal settings tick, its quantizer and actual RCX
                    // context argument to this same native settings-apply entry.
                    for(auto read:setting_refs){
                        if(!read.scalar(0x10))continue;auto tick=decode(im,owner(im,read.at));bool normalized=false;
                        for(size_t q=0;q<tick.size();++q)if(tick[q].scalar(0x11)&&tick[q].rip==setting&&quantizer(im,tick,q))normalized=true;
                        if(!normalized)continue;
                        for(size_t q=1;q<tick.size();++q)if(tick[q].call==result.apply&&tick[q-1].h.opcode==0x8d&&tick[q-1].reg()==1&&tick[q-1].rip){
                            result.tick=primary(im,read.at);result.context=tick[q-1].rip;}
                    }
                    if(result.setting<result.context||result.setting-result.context>0x2000||!result.tick)return;
                    if(helper){
                        // SE5's helper reads a DWORD native-upscale mode before
                        // its scalar fallback. Prove its settings-apply mirror.
                        auto body=decode(im,{helper,0x800},true);
                        for(auto i:body)if(i.h.opcode==0x8b&&i.rip){
                            for(size_t z=1;z<code.size();++z)if(code[z].h.opcode==0x89&&code[z].rip&&code[z].rip!=setting){
                                // The mirror reads the same native cache and
                                // writes the pending field, after helper CALL.
                                if(code[z-1].h.opcode==0x8b&&code[z-1].rip==i.rip&&code[z-1].reg()==code[z].reg()&&code[z].rip>=result.context&&code[z].rip-result.context<0x2000){
                                    result.upscale_setting=code[z].rip;result.upscale_cache=i.rip;break;}
                            }
                            if(result.upscale_setting)break;
                        }
                        if(!result.upscale_setting)return;
                    }
                    result.reason="Native named settings, exact-scale apply and scene-size update located.";
                    results.push_back(result);
                };
                std::vector<Ins> tail(code.begin()+s+1,code.begin()+r);
                auto inline_chain=dimensions(im,tail,store.reg(),load.rip);
                if(inline_chain){accept(inline_chain,0);continue;}
                for(size_t n=s+1;n<r;++n)if(code[n].call){
                    const auto target=code[n].call;auto body=decode(im,{target,0x800},true);if(body.empty()||body.back().h.opcode!=0xc3)continue;
                    for(auto source:body)if(source.scalar(0x10)&&source.rip==load.rip)accept(dimensions(im,body,source.reg(),load.rip),target);
                }
            }
        }
    }
    return results.size()==1?results.front():Result{};
}
} // namespace na::scale::native::locator
