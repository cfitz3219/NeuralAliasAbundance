#include "test_support.hpp"
#include "host_dx12/adapters/unreal/locator.hpp"
#include <fstream>
#include <iterator>
using namespace na::unreal::locator;
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
    auto image=pe::mapped_image(mapped.data(),mapped.size(),nt.OptionalHeader.ImageBase);
    auto r=na::unreal::locator::locate(image);
    std::printf("Unreal locator: vars=%x/%x setters=%x/%x tables=%x/%x manager=%x sink=%x caller=%x: %s\n",r.variables[0],r.variables[1],r.setters[0],r.setters[1],r.tables[0],r.tables[1],r.manager_global,r.sink,r.caller,r.reason);
    return r?0:1;
}


namespace {
struct Fixture {
 std::vector<uint8_t> b=std::vector<uint8_t>(0xb000);
 Image image;
 Signatures signatures=supported;
 std::array<uint32_t,Count> fn{};
 uint32_t code_shift,data_shift,string_shift,tick,reg[2],table[2],vars[2],global,manager;
 template<class T>void put(uint32_t at,T v){std::memcpy(b.data()+at,&v,sizeof(v));}
 void data(uint32_t at,const char* bytes,size_t n){std::memcpy(b.data()+at,bytes,n);}
 void rel(uint32_t at,const char* op,unsigned prefix,uint32_t target,unsigned length){data(at,op,prefix);put<int32_t>(at+prefix,int32_t(target)-int32_t(at+length));}
 Fixture(uint32_t cm=0,uint32_t dm=0,uint32_t sm=0,uint64_t base=0x140000000):code_shift(cm),data_shift(dm),string_shift(sm) {
  image={b.data(),b.size(),base,{{0x1000,0x4000,true},{0x6000,0x5000,false}}, {}};
  const uint32_t starts[]={0x1100,0x1200,0x1300,0x1400,0x1500,0x1600,0x1800,0x1900,0x1a00};
  for(unsigned k=0;k<Count;++k){
   fn[k]=starts[k]+cm;
   // Each function moves independently, not just ASLR.
   if(cm)fn[k]+=k*0x100;
   std::memset(b.data()+fn[k],0x90,signatures[k].size);b[fn[k]+signatures[k].size-1]=0xc3;
   image.functions.push_back({fn[k],signatures[k].size});
  }
  vars[0]=0x6100+dm;vars[1]=0x6120+dm;global=0x6140+dm;manager=0x6200+dm;
  table[0]=0x6400+dm;table[1]=0x6600+dm;
  for(unsigned k=0;k<2;++k){
   const auto set=fn[k];
   data(set,"\x48\x89\x6c\x24\x10\x48\x89\x74\x24\x18",10);
   rel(set+0x2d,"\xe8",1,fn[Priority],5);
   const auto ctor=fn[k==0?FloatCtor:IntCtor];
   rel(ctor+0x21,"\xe8",1,fn[ManagerGet],5);
   rel(ctor+(k==0?0x56:0x55),"\x48\x8d\x1d",3,0x6800+dm+k*0x100,7);
  }
  const uint32_t storage=0x3c00+cm,zero=0x3c10+cm;
  data(storage,"\x48\x8d\x41\x50\xc3",5);data(zero,"\x33\xc0\xc3",3);
  for(unsigned k=0;k<2;++k){
   put<uint64_t>(table[k]+0xb0,base+fn[k]);put<uint64_t>(table[k]+0x110,base+fn[Changed]);
   put<uint64_t>(table[k]+(k==0?0x70:0x68),base+storage);
   put<uint64_t>(table[k]+(k==0?0x68:0x70),base+zero);
  }
  put<uint64_t>(manager+0x58,base+fn[Sink]);
  rel(fn[ManagerGet]+4,"\x48\x8b\x05",3,global,7);
  rel(fn[ManagerGet]+0x3a,"\x48\x8d\x05",3,manager,7);
  rel(fn[ManagerInit]+4,"\x48\x83\x3d",3,global,8);b[fn[ManagerInit]+11]=0;
  rel(fn[ManagerInit]+0x38,"\x48\x8d\x05",3,manager,7);
  rel(fn[ManagerInit]+0x1a0,"\x48\x89\x1d",3,global,7);
  const wchar_t* names[]={L"r.ScreenPercentage",L"r.DynamicRes.OperationMode",L"FEngineLoop::Tick.Benchmarking"};
  for(unsigned k=0;k<3;++k)std::memcpy(b.data()+0x7000+sm+k*0x100,names[k],(std::wcslen(names[k])+1)*2);
  for(unsigned k=0;k<2;++k){
   reg[k]=0x3500+cm+k*0x100;
   rel(reg[k],"\x48\x8d\x15",3,0x7000+sm+k*0x100,7);
   rel(reg[k]+7,"\x48\x8d\x0d",3,vars[k],7);
   rel(reg[k]+14,"\xe8",1,fn[k==0?FloatCtor:IntCtor],5);
  }
  tick=0x3200+cm;std::memset(b.data()+tick,0x90,0x100);image.functions.push_back({tick,0x100});
  rel(tick+0x10,"\xe8",1,fn[ManagerInit],5);
  rel(tick+0x15,"\x48\x8b\x0d",3,global,7);
  data(tick+0x1c,"\x48\x8b\x01\xff\x50\x58",6);
  rel(tick+0x80,"\x48\x8d\x15",3,0x7200+sm,7);
  for(unsigned k=0;k<Count;++k)signatures[k].hash=pe::fingerprint(image,{fn[k],signatures[k].size});
 }
 Result locate(const Signatures& sig){return na::unreal::locator::locate(image,sig);}
};
}
int main(int argc,char** argv){
 if(argc==2)return inspect(argv[1]);
 Fixture base;auto found=base.locate(base.signatures);CHECK(found);
 CHECK(found.variables[0]==base.vars[0] && found.tables[1]==base.table[1]);
 CHECK(found.caller==base.tick+0x22 && found.sink==base.fn[Sink]);
 Fixture moved(0x100,0x180,0x600,0x7ff600000000);CHECK(moved.locate(base.signatures));
 moved.b[0xa100]=0x37;CHECK(moved.locate(base.signatures)); // unrelated executable change
 Fixture wrongName;wrongName.b[0x7100]='X';CHECK(!wrongName.locate(base.signatures));
 Fixture wrongCtor;wrongCtor.rel(wrongCtor.reg[0]+14,"\xe8",1,wrongCtor.fn[IntCtor],5);CHECK(!wrongCtor.locate(base.signatures));
 Fixture wrongObject;wrongObject.rel(wrongObject.reg[0]+7,"\x48\x8d\x0d",3,0x1100,7);CHECK(!wrongObject.locate(base.signatures));
 Fixture argument;argument.b[argument.fn[FloatSet]+12]=0xcc;CHECK(!argument.locate(base.signatures));
 Fixture layout;layout.b[0x3c03]=0x58;CHECK(!layout.locate(base.signatures));
 Fixture changed;changed.b[changed.fn[Changed]+10]=0xcc;CHECK(!changed.locate(base.signatures));
 Fixture priority;priority.b[priority.fn[Priority]+8]=0xcc;CHECK(!priority.locate(base.signatures));
 Fixture unrelatedManager;unrelatedManager.rel(unrelatedManager.tick+0x15,"\x48\x8b\x0d",3,0x6180,7);CHECK(!unrelatedManager.locate(base.signatures));
 Fixture wrongSink;wrongSink.put<uint64_t>(wrongSink.manager+0x58,wrongSink.image.address+wrongSink.fn[Priority]);CHECK(!wrongSink.locate(base.signatures));
 Fixture wrongSlot;wrongSlot.b[wrongSlot.tick+0x21]=0x60;CHECK(!wrongSlot.locate(base.signatures));
 Fixture duplicate;std::memcpy(duplicate.b.data()+0x8000,duplicate.b.data()+duplicate.table[0],0x118);CHECK(!duplicate.locate(base.signatures));
 Fixture association;association.put<uint64_t>(association.table[0]+0x70,association.image.address+0x3c10);CHECK(!association.locate(base.signatures));
 CHECK(!na::unreal::locator::locate(Image{}));
 return test::finish("unreal_locator_test");
}
