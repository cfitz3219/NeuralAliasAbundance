#pragma once
// Supported Unreal console ABI, independent of executable hash/name/RVAs.
// Reuse the existing bounded PE reader and HDE relocation normalization.
#include "host_dx12/adapters/tsushima/locator.hpp"
#include <algorithm>
#include <cwchar>
namespace na::unreal::locator {
namespace pe=na::tsushima::locator;
using pe::Image;using pe::Signature;using pe::Function;
enum Part {FloatSet,IntSet,FloatCtor,IntCtor,Sink,ManagerInit,Changed,Priority,ManagerGet,Count};
using Signatures=std::array<Signature,Count>;
inline constexpr Signatures supported{{
 {0x82,0x637f4aca6bf6110cULL},{0x74,0xd358aa7bfa62d975ULL},
 {0x86,0x2c091fa621a41fbdULL},{0x85,0x5a63191bebe436a0ULL},
 {0x72,0x90935aa863e61499ULL},{0x1bb,0xe04910ab23553a63ULL},
 {0x4e,0x22adc1cfe62b5800ULL},{0x2f,0x8cf18747dfa2c5d1ULL},
 {0x1c4,0x19010f1d3d9c3ab3ULL}
}};
struct Result {
 uint32_t variables[2]{},tables[2]{},registration_tables[2]{},setters[2]{};
 uint32_t manager_global=0,manager_table=0,sink=0,caller=0;
 const char* reason="Unreal console interface not recognized.";
 explicit operator bool()const{return variables[0]&&variables[1]&&tables[0]&&tables[1]&&sink&&caller;}
};
inline bool bytes(const Image& im,uint32_t r,const char* b,size_t n){return im.contains(r,n,true)&&!std::memcmp(im.bytes+r,b,n);}
inline uint32_t relative(const Image& im,uint32_t r,unsigned disp,unsigned length) {
 if(!im.contains(r,length,true))return 0;
 const int64_t t=int64_t(r)+length+im.read<int32_t>(r+disp);
 return t>0 && uint64_t(t)<im.size?uint32_t(t):0;
}
inline uint32_t pointer(const Image& im,uint32_t at) {
 if(!im.contains(at,8,false))return 0;
 const auto v=im.read<uint64_t>(at);
 return v>=im.address && v-im.address<im.size?uint32_t(v-im.address):0;
}
inline bool matches(const Image& im,uint32_t at,Part part,const Signatures& sig) {
 return at && pe::fingerprint(im,{at,sig[part].size})==sig[part].hash;
}
inline std::vector<uint32_t> strings(const Image& im,const wchar_t* text) {
 std::vector<uint32_t> out;const size_t n=(std::wcslen(text)+1)*sizeof(wchar_t);
 for(auto r:im.ranges)if(!r.executable && r.size>=n) {
  for(uint32_t off=0;off<=r.size-n;off+=2)
   if(im.bytes[r.start+off]==uint8_t(text[0])&&!std::memcmp(im.bytes+r.start+off,text,n))out.push_back(r.start+off);
 }
 return out;
}
inline bool has(const std::vector<uint32_t>& v,uint32_t r){return std::find(v.begin(),v.end(),r)!=v.end();}
inline Result locate(const Image& im,const Signatures& sig=supported) {
 Result result;
 const auto float_names=strings(im,L"r.ScreenPercentage"),int_names=strings(im,L"r.DynamicRes.OperationMode"),tick_names=strings(im,L"FEngineLoop::Tick.Benchmarking");
 if(float_names.empty()||int_names.empty()||tick_names.empty())return result;
 uint32_t constructors[2]{};unsigned registrations[2]{};
 std::vector<uint32_t> tick_refs;
 // Registration sequence binds the string and static object to a verified
 // constructor; merely adjacent strings/globals are insufficient.
 for(auto range:im.ranges)if(range.executable && range.size>=19) {
  for(uint32_t off=0;off+19<=range.size;++off) {
   const auto r=range.start+off;
   if(!bytes(im,r,"\x48\x8d\x15",3))continue;
   const auto name=relative(im,r,3,7);
   if(has(tick_names,name))tick_refs.push_back(r);
   const int kind=has(float_names,name)?0:has(int_names,name)?1:-1;
   if(kind<0 || !bytes(im,r+7,"\x48\x8d\x0d",3)||im.bytes[r+14]!=0xe8)continue;
   const auto object=relative(im,r+7,3,7),ctor=relative(im,r+14,1,5);
   if(!im.contains(object,24,false)||!matches(im,ctor,kind==0?FloatCtor:IntCtor,sig))continue;
   if(result.variables[kind]!=object){++registrations[kind];result.variables[kind]=object;constructors[kind]=ctor;}
  }
 }
 if(registrations[0]!=1||registrations[1]!=1){result.reason="Unreal resolution variable registration is missing or ambiguous.";return {};}
 // Both constructors must obtain the SAME console manager and typed storage.
 uint32_t getter=0;
 for(unsigned k=0;k<2;++k) {
  auto c=constructors[k];
  if(!bytes(im,c+0x21,"\xe8",1))return {};
  const auto g=relative(im,c+0x21,1,5);
  if(!matches(im,g,ManagerGet,sig)||(getter&&getter!=g))return {};
  getter=g;
  const unsigned table_offset=k==0?0x56:0x55;
  if(!bytes(im,c+table_offset,"\x48\x8d\x1d",3))return {};
  result.registration_tables[k]=relative(im,c+table_offset,3,7);
  if(!im.contains(result.registration_tables[k],8,false))return {};
 }
 if(!bytes(im,getter+4,"\x48\x8b\x05",3)||!bytes(im,getter+0x3a,"\x48\x8d\x05",3))return {};
 result.manager_global=relative(im,getter+4,3,7);
 result.manager_table=relative(im,getter+0x3a,3,7);
 if(!im.contains(result.manager_global,8,false)||!im.contains(result.manager_table,0x60,false))return {};
 result.sink=pointer(im,result.manager_table+0x58);
 if(!matches(im,result.sink,Sink,sig))return {};
 // The normal tick call must reference that manager singleton, using its
 // verified initialization path. Never choose an unrelated sink invocation.
 unsigned callers=0;
 for(auto ref:tick_refs) {
  for(auto f:im.functions)if(ref>=f.start && ref-f.start<f.size) {
   for(uint32_t off=0;off+18<=f.size;++off) {
    const auto r=f.start+off;
    if(!bytes(im,r,"\xe8",1)||!bytes(im,r+5,"\x48\x8b\x0d",3)||
       !bytes(im,r+12,"\x48\x8b\x01\xff\x50\x58",6))continue;
    const auto init=relative(im,r,1,5);
    if(relative(im,r+5,3,7)!=result.manager_global || !matches(im,init,ManagerInit,sig))continue;
    if(relative(im,init+4,3,8)!=result.manager_global || relative(im,init+0x1a0,3,7)!=result.manager_global ||
       relative(im,init+0x38,3,7)!=result.manager_table)continue;
    if(result.caller!=r+18){result.caller=r+18;++callers;}
   }
  }
 }
 if(callers!=1)return {};
 // Unwind entries anchor function starts even for compiler-split functions;
 // full supported body signatures include storage, flags and callback paths.
 for(unsigned k=0;k<2;++k) {
  unsigned found=0;
  for(auto f:im.functions) {
   if(!bytes(im,f.start,"\x48\x89\x6c\x24\x10\x48\x89\x74",8))continue;
   if(matches(im,f.start,k==0?FloatSet:IntSet,sig)) {++found;result.setters[k]=f.start;}
  }
  if(found!=1)return {};
  const auto priority=relative(im,result.setters[k]+0x2d,1,5);
  if(!bytes(im,result.setters[k]+0x2d,"\xe8",1)||!matches(im,priority,Priority,sig))return {};
  unsigned tables=0;
  for(auto range:im.ranges)if(!range.executable && range.size>=0x118) {
   for(uint32_t off=0;off+0x118<=range.size;off+=8) {
    const auto t=range.start+off;
    if(pointer(im,t+0xb0)!=result.setters[k])continue;
    const auto storage=pointer(im,t+(k==0?0x70:0x68)),other=pointer(im,t+(k==0?0x68:0x70));
    if(!bytes(im,storage,"\x48\x8d\x41\x50\xc3",5)||!bytes(im,other,"\x33\xc0\xc3",3)||
       !matches(im,pointer(im,t+0x110),Changed,sig))continue;
    ++tables;result.tables[k]=t;
   }
  }
  if(tables!=1)return {};
 }
 result.reason="Compatible Unreal console setters and game-thread sink located.";return result;
}
} // namespace na::unreal::locator
