#include "test_support.hpp"
#include "host_dx12/adapters/tsushima/locator.hpp"
#include <fstream>
#include <iterator>
using namespace na::tsushima::locator;
namespace {
struct Fixture {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(0x5000);
    Image image;
    std::array<Signature,5> sig{};
    uint32_t shift;
    template<class T> void put(uint32_t at,T value){std::memcpy(bytes.data()+at,&value,sizeof(value));}
    void data(uint32_t at,const char* v,size_t n){std::memcpy(bytes.data()+at,v,n);}
    Fixture(uint32_t move=0,uint64_t base=0x140000000):shift(move) {
        image={bytes.data(),bytes.size(),base,{{0x1000,0x2000,true},{0x3000,0x2000,false}},
          {{0x1100+move,6},{0x1200+move,2},{0x1300+move,13},{0x1400+move,128},{0x1500+move,3}}};
        // synthetic native request calls another valid code address
        data(0x1100+move,"\xe8\x00\x00\x00\x00\xc3",6);
        put<int32_t>(0x1101+move,int32_t(0x2000-(0x1105+move)));
        data(0x1200+move,"\x90\xc3",2);
        data(0x1300+move,"\x48\x8b\x03\x48\x8b\xcb\xff\x90\x30\x01\x00\x00\xc3",13);
        const char* names[]={"DynamicResolutionTargetFPS","AA","UpscaleMethod","UpscaleQuality"};
        const uint32_t offsets[]={0x4c,0x88,0xb0,0xb4};
        for(unsigned i=0;i<4;++i) {
            const uint32_t r=0x1400+move+i*32,t=0x3400+move+i*64;
            data(t,names[i],std::strlen(names[i])+1);
            data(r,"\x48\x8d\x0d",3);put<int32_t>(r+3,int32_t(t-r-7));
            data(r+7,"\x48\x89\x4c\x24\x28\x48\x8b\xc7\xc7\x44\x24\x20\x02\0\0\0\xc7\x44\x24\x38",20);
            put<uint32_t>(r+27,offsets[i]);bytes[r+31]=0x90;
        }
        data(0x1500+move,"\x31\xc0\xc3",3);
        table(0x3000+move);
        for(unsigned i=0;i<5;++i)sig[i]={image.functions[i].size,fingerprint(image,image.functions[i])};
    }
    void table(uint32_t at) {
        put<uint64_t>(at+0x58,image.address+0x1300+shift);
        put<uint64_t>(at+0x120,image.address+0x1100+shift);
        put<uint64_t>(at+0x130,image.address+0x1200+shift);
    }
};
// Optional read-only real-executable check. Map section bytes ourselves: no
// LoadLibrary, DllMain, imports, hooks or game execution.
int inspect(const char* path) {
    std::ifstream in(path,std::ios::binary);
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),{});
    if(raw.size()<sizeof(IMAGE_DOS_HEADER))return 2;
    IMAGE_DOS_HEADER dos{};std::memcpy(&dos,raw.data(),sizeof(dos));
    if(dos.e_lfanew<0 || size_t(dos.e_lfanew)+sizeof(IMAGE_NT_HEADERS64)>raw.size())return 2;
    IMAGE_NT_HEADERS64 nt{};std::memcpy(&nt,raw.data()+dos.e_lfanew,sizeof(nt));
    if(nt.OptionalHeader.SizeOfImage>1024*1024*1024 || nt.OptionalHeader.SizeOfHeaders>raw.size())return 2;
    std::vector<uint8_t> mapped(nt.OptionalHeader.SizeOfImage);
    if(nt.OptionalHeader.SizeOfHeaders>mapped.size())return 2;
    std::memcpy(mapped.data(),raw.data(),nt.OptionalHeader.SizeOfHeaders);
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        const size_t at=size_t(dos.e_lfanew)+sizeof(nt)+i*sizeof(IMAGE_SECTION_HEADER);
        if(at+sizeof(IMAGE_SECTION_HEADER)>raw.size())return 2;
        IMAGE_SECTION_HEADER s{};std::memcpy(&s,raw.data()+at,sizeof(s));
        if(uint64_t(s.PointerToRawData)+s.SizeOfRawData>raw.size() || uint64_t(s.VirtualAddress)+s.SizeOfRawData>mapped.size())return 2;
        std::memcpy(mapped.data()+s.VirtualAddress,raw.data()+s.PointerToRawData,s.SizeOfRawData);
    }
    auto image=mapped_image(mapped.data(),mapped.size(),nt.OptionalHeader.ImageBase);
    auto r=locate(image);
    std::printf("Read-only Ghost locator: request=%x dispatch=%x caller=%x: %s\n",r.request,r.dispatch,r.caller,r.reason);
    if(!r) {
        for(auto f:image.functions)for(auto s:signatures)if(f.size==s.size)
            std::printf("candidate %x size=%x fingerprint=%llx\n",f.start,f.size,static_cast<unsigned long long>(fingerprint(image,f)));
    }
    return r?0:1;
}
}
int main(int argc,char** argv) {
    if(argc==2)return inspect(argv[1]);
    Fixture f;CHECK(locate(f.image,f.sig));
    Fixture moved(0x100,0x7ff600000000); // ASLR, function/data/table moves, new relative call displacement
    CHECK(locate(moved.image,f.sig));CHECK(locate(moved.image,f.sig).caller==0x140c);
    // A string can move independently of code, as happens when read-only data
    // is relinked. A same-shaped reference to the wrong string must not pass.
    Fixture strings;
    strings.data(0x3c00,"UpscaleQuality",15);
    strings.put<int32_t>(0x1400+3*32+3,0x3c00-(0x1400+3*32+7));
    CHECK(locate(strings.image,f.sig));
    strings.bytes[0x3c00]='X';CHECK(!locate(strings.image,f.sig));
    f.bytes[0x3000+0x120]^=8;CHECK(!locate(f.image,f.sig)); // wrong class linkage
    Fixture duplicate;duplicate.table(0x3800);CHECK(!locate(duplicate.image,duplicate.sig));
    Fixture changed;changed.put<uint32_t>(0x1400+2*32+27,0xb8);
    changed.sig[3].hash=fingerprint(changed.image,changed.image.functions[3]);
    CHECK(!locate(changed.image,changed.sig)); // schema mismatch even if function fingerprint accepted
    Fixture text;text.bytes[0x3400+3*64]='X';CHECK(!locate(text.image,text.sig));
    Fixture instruction;instruction.bytes[0x1200]=0xcc;CHECK(!locate(instruction.image,instruction.sig));
    Fixture modes;modes.bytes[0x1500]=0x33;CHECK(!locate(modes.image,modes.sig));
    Fixture ambiguous;ambiguous.image.functions.push_back(ambiguous.image.functions[0]);CHECK(!locate(ambiguous.image,ambiguous.sig));
    Fixture bounds;bounds.image.functions[0].size=UINT32_MAX;CHECK(!fingerprint(bounds.image,bounds.image.functions[0]));
    Fixture call;call.put<int32_t>(0x1101,INT32_MAX);CHECK(!locate(call.image,call.sig));
    CHECK(!mapped_image(f.bytes.data(),12,0).bytes);
    return test::finish("tsushima_locator_test");
}
