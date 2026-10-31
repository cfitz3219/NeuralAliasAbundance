#pragma once
// Ghost's inspected settings ABI, located by code/schema/vtable relationships.
// Fingerprints retain layout offsets and control flow, but ignore linker-fixed
// RIP references and direct-call displacements. This supports relocated code,
// not arbitrary compiler rewrites or unverified Nixxes titles.
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>
#include "../../../../third_party/minhook/src/hde/hde64.h"

namespace na::tsushima::locator {
struct Range { uint32_t start=0, size=0; bool executable=false; };
struct Function { uint32_t start=0, size=0; };
struct Image {
    const uint8_t* bytes=nullptr;
    size_t size=0;
    uint64_t address=0; // runtime base, or preferred base for read-only file inspection
    std::vector<Range> ranges;
    std::vector<Function> functions;
    bool contains(uint32_t r, size_t n, bool code) const {
        if(!bytes || r>size || n>size-r)return false;
        for(const auto& s:ranges)
            if(s.executable==code && r>=s.start && r-s.start<=s.size && n<=s.size-(r-s.start))return true;
        return false;
    }
    template<class T> T read(uint32_t r) const {T v{};if(r<=size && sizeof(v)<=size-r)std::memcpy(&v,bytes+r,sizeof(v));return v;}
};
struct Signature { uint32_t size; uint64_t hash; };
// Parse an already mapped PE image, without loading or executing game code.
inline Image mapped_image(const uint8_t* bytes, size_t size, uint64_t address) {
    Image image{bytes,size,address,{},{}};
    if(!bytes || size<sizeof(IMAGE_DOS_HEADER))return {};
    const auto dos=image.read<IMAGE_DOS_HEADER>(0);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || size_t(dos.e_lfanew)>size || sizeof(IMAGE_NT_HEADERS64)>size-size_t(dos.e_lfanew))return {};
    const auto nt=image.read<IMAGE_NT_HEADERS64>(uint32_t(dos.e_lfanew));
    if(nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
       nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) || nt.OptionalHeader.SizeOfImage!=size || nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_EXCEPTION)return {};
    const size_t sections=size_t(dos.e_lfanew)+sizeof(IMAGE_NT_HEADERS64);
    if(sections>size || nt.FileHeader.NumberOfSections>(size-sections)/sizeof(IMAGE_SECTION_HEADER))return {};
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        const auto s=image.read<IMAGE_SECTION_HEADER>(uint32_t(sections+i*sizeof(IMAGE_SECTION_HEADER)));
        if(s.VirtualAddress>size || s.Misc.VirtualSize>size-s.VirtualAddress)return {};
        if((s.Characteristics&IMAGE_SCN_MEM_READ) && !(s.Characteristics&IMAGE_SCN_MEM_DISCARDABLE))image.ranges.push_back({s.VirtualAddress,s.Misc.VirtualSize,(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)!=0});
    }
    const auto d=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if(!d.Size || d.Size%sizeof(RUNTIME_FUNCTION) || !image.contains(d.VirtualAddress,d.Size,false))return {};
    for(uint32_t off=0;off<d.Size;off+=sizeof(RUNTIME_FUNCTION)) {
        const auto f=image.read<RUNTIME_FUNCTION>(d.VirtualAddress+off);
        if(f.EndAddress<=f.BeginAddress || !image.contains(f.BeginAddress,f.EndAddress-f.BeginAddress,true))return {};
        image.functions.push_back({f.BeginAddress,f.EndAddress-f.BeginAddress});
    }
    return image;
}
inline constexpr std::array<Signature,5> signatures{{
    {0x2ce,0x718660056740405cULL}, // native deferred settings request
    {0x1c7,0xfdbee16f8e977b87ULL}, // native settings dispatcher
    {0x1db,0x45571fa05ffe4e2fULL}, // ordinary frame update
    {0x1514,0xfa3bf89ecca5f3bfULL}, // GraphicsSettings schema registration
    {0x10c,0x340524704825f1ffULL} // quality enum / DLSS mode conversion
}};
struct Result { uint32_t request=0, dispatch=0, caller=0; const char* reason="Ghost settings layout was not recognized."; explicit operator bool() const{return request&&dispatch&&caller;} };

inline uint64_t fingerprint(const Image& image, Function f) {
    if(!image.contains(f.start,f.size,true))return 0;
    uint64_t hash=14695981039346656037ULL;
    for(uint32_t at=0;at<f.size;) {
        // HDE reads ahead: give it a padded instruction-sized local buffer.
        uint8_t b[32]{};const size_t remaining=f.size-at;
        std::memcpy(b,image.bytes+f.start+at,remaining<15?remaining:15);
        hde64s h{};hde64_disasm(b,&h);
        if(!h.len || h.len>remaining || (h.flags&F_ERROR))return 0;
        const unsigned imm=(h.flags&F_IMM64)?8u:(h.flags&F_IMM32)?4u:(h.flags&F_IMM16)?2u:(h.flags&F_IMM8)?1u:0u;
        const bool rip=(h.flags&F_MODRM)&&h.modrm_mod==0&&h.modrm_rm==5&&!h.p_67;
        if(rip || h.opcode==0xe8) {
            const unsigned tail=rip?imm:0;
            if(h.len<tail+4)return 0;
            const unsigned off=h.len-tail-4;
            int32_t displacement{};std::memcpy(&displacement,b+off,4);
            const int64_t target=int64_t(f.start)+at+h.len+displacement;
            if(target<0 || uint64_t(target)>=image.size)return 0;
            if(h.opcode==0xe8 && !image.contains(uint32_t(target),1,true))return 0;
            std::memset(b+off,0,4);
        }
        for(unsigned i=0;i<h.len;++i)hash=(hash^b[i])*1099511628211ULL;
        at+=h.len;
    }
    return hash;
}

// The four fields we mutate must belong to the SAME schema function. Checking
// strings alone would accept unrelated UI text or a changed structure layout.
inline bool schema_matches(const Image& image, Function f) {
    struct Field {const char* name;uint32_t offset;};
    constexpr Field fields[]={{"DynamicResolutionTargetFPS",0x4c},{"AA",0x88},{"UpscaleMethod",0xb0},{"UpscaleQuality",0xb4}};
    unsigned found[4]{};
    for(uint32_t at=0;at+31<=f.size;++at) {
        const auto r=f.start+at;const auto* b=image.bytes+r;
        if(std::memcmp(b,"\x48\x8d\x0d",3))continue;
        const int64_t target=int64_t(r)+7+image.read<int32_t>(r+3);
        if(target<0 || uint64_t(target)>UINT32_MAX)continue;
        // lea rcx,name; mov [rsp+28],rcx; mov rax,rdi;
        // mov dword [rsp+20],2; mov dword [rsp+38],fieldOffset.
        constexpr uint8_t registration[]={0x48,0x89,0x4c,0x24,0x28,0x48,0x8b,0xc7,0xc7,0x44,0x24,0x20,2,0,0,0,0xc7,0x44,0x24,0x38};
        if(std::memcmp(b+7,registration,sizeof(registration)))continue;
        for(unsigned i=0;i<4;++i) {
            const size_t n=std::strlen(fields[i].name)+1;
            if(image.contains(uint32_t(target),n,false)&&!std::memcmp(image.bytes+target,fields[i].name,n)&&image.read<uint32_t>(r+27)==fields[i].offset)++found[i];
        }
    }
    for(auto n:found)if(n!=1)return false;
    return true;
}

inline Result locate(const Image& image, const std::array<Signature,5>& expected=signatures) {
    std::array<Function,5> functions{};unsigned counts[5]{};
    for(auto f:image.functions)for(unsigned i=0;i<5;++i)
        if(f.size==expected[i].size && fingerprint(image,f)==expected[i].hash){functions[i]=f;++counts[i];}
    for(auto count:counts)if(count!=1)return {};
    if(!schema_matches(image,functions[3]))return {0,0,0,"Ghost graphics-settings schema changed."};
    unsigned tables=0;
    // In this ABI: frame update +58, deferred request +120, dispatch +130.
    for(auto r:image.ranges) {
        if(r.executable || !image.contains(r.start,r.size,false))continue;
        for(uint32_t at=0;uint64_t(at)+0x138<=r.size;at+=8) {
            const auto t=r.start+at;
            if(image.read<uint64_t>(t+0x58)==image.address+functions[2].start &&
               image.read<uint64_t>(t+0x120)==image.address+functions[0].start &&
               image.read<uint64_t>(t+0x130)==image.address+functions[1].start)++tables;
        }
    }
    if(tables!=1)return {0,0,0,"Ghost settings functions do not share one verified dispatch table."};
    uint32_t caller=0;unsigned calls=0;
    const auto frame=functions[2];
    constexpr uint8_t site[]={0x48,0x8b,0x03,0x48,0x8b,0xcb,0xff,0x90,0x30,0x01,0x00,0x00};
    for(uint32_t at=0;at+sizeof(site)<=frame.size;++at)
        if(!std::memcmp(image.bytes+frame.start+at,site,sizeof(site))){caller=frame.start+at+uint32_t(sizeof(site));++calls;}
    if(calls!=1)return {0,0,0,"Ghost normal-frame dispatch call changed."};
    return {functions[0].start,functions[1].start,caller,"Ghost settings schema and native dispatch located."};
}
} // namespace na::tsushima::locator
