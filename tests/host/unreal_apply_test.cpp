#include "host_dx12/adapters/unreal/locator.hpp"
#include "test_support.hpp"
#include "host_dx12/adapters/unreal/screen_percentage.hpp"
#include <array>
namespace na::unreal {
void ConfigureForTest(void*,void*,void(*)(void*,const wchar_t*,const void*),void(*)(void*,const wchar_t*,const void*));
void ProcessForTest(uintptr_t);
}
namespace {
std::array<uint8_t,0x58> float_object{},int_object{};
int calls=0;DWORD setter_thread=0;
template<class T> void put(std::array<uint8_t,0x58>& b,size_t at,T v){std::memcpy(b.data()+at,&v,sizeof(v));}
template<class T> T get(const std::array<uint8_t,0x58>& b,size_t at){T v{};std::memcpy(&v,b.data()+at,sizeof(v));return v;}
void set(void* object,const wchar_t* text,const void* params) {
    ++calls;setter_thread=GetCurrentThreadId();
    // Real setter reentry must not deadlock on the adapter status mutex.
    (void)na::unreal::GetScreenPercentageStatus();
    uint32_t flags{};std::memcpy(&flags,params,4);CHECK(flags==0x08000000);
    const auto* p=static_cast<const uint8_t*>(params);for(unsigned n=4;n<24;++n)CHECK(p[n]==0);
    if(object==float_object.data())put<float>(float_object,0x50,float(wcstod(text,nullptr)));
    else {CHECK(object==int_object.data());put<int>(int_object,0x50,_wtoi(text));}
    // Deliberately do not propagate the render shadow: that's engine work.
}
void setup(){
    float_object={};int_object={};calls=0;setter_thread=0;
    put<float>(float_object,0x50,50.0f);put<float>(float_object,0x54,50.0f);put<int>(int_object,0x50,2);
    put<uint32_t>(float_object,0x18,0x08000020);put<uint32_t>(int_object,0x18,0x08000000);
    na::unreal::ConfigureForTest(float_object.data(),int_object.data(),set,set);
}
}
namespace na::unreal {bool BindForTest(const uint8_t*,const locator::Result&);}
int main(){
    using namespace na::unreal;
    {
        std::array<uint8_t,0x800> image{};locator::Result resolved;
        resolved.variables[0]=0x10;resolved.variables[1]=0x30;
        resolved.registration_tables[0]=0x100;resolved.registration_tables[1]=0x200;
        resolved.tables[0]=0x300;resolved.tables[1]=0x400;
        setup();
        auto write=[&](size_t at,uintptr_t v){std::memcpy(image.data()+at,&v,sizeof(v));};
        const auto base=reinterpret_cast<uintptr_t>(image.data());
        write(0x10,base+0x100);write(0x18,reinterpret_cast<uintptr_t>(float_object.data()));write(0x20,reinterpret_cast<uintptr_t>(float_object.data())+0x50);
        write(0x30,base+0x200);write(0x38,reinterpret_cast<uintptr_t>(int_object.data()));write(0x40,reinterpret_cast<uintptr_t>(int_object.data())+0x50);
        put<uintptr_t>(float_object,0,base+0x300);put<uintptr_t>(int_object,0,base+0x400);
        CHECK(BindForTest(image.data(),resolved));CHECK(calls==0);
        write(0x40,reinterpret_cast<uintptr_t>(float_object.data())+0x50);CHECK(!BindForTest(image.data(),resolved));
        write(0x40,reinterpret_cast<uintptr_t>(int_object.data())+0x50);put<uintptr_t>(int_object,0,base+0x300);CHECK(!BindForTest(image.data(),resolved));
        put<uintptr_t>(int_object,0,base+0x400);write(0x10,base+0x200);CHECK(!BindForTest(image.data(),resolved));CHECK(calls==0);
    }
    setup();CHECK(RequestScreenPercentage(66.7f,3440,1440));CHECK(calls==0);
    CHECK(!RequestScreenPercentage(58,3440,1440));
    ProcessForTest(0xdef);CHECK(calls==0);
    ProcessForTest(0xabc);CHECK(calls==2);CHECK(setter_thread==GetCurrentThreadId());
    CHECK(get<int>(int_object,0x50)==0);CHECK(get<float>(float_object,0x50)==66.7f);CHECK(get<float>(float_object,0x54)==50.0f);
    CHECK(GetScreenPercentageStatus().state==SpState::Applying); // Set isn't rendered evidence
    CHECK(HandBackScreenPercentage());CHECK(calls==2);ProcessForTest(0xabc);
    CHECK(calls==4);CHECK(get<int>(int_object,0x50)==2);CHECK(get<float>(float_object,0x50)==50.0f);
    setup();CHECK(RequestScreenPercentage(58,3440,1440));CancelScreenPercentage();ProcessForTest(0xabc);CHECK(calls==0);
    setup();CHECK(RequestScreenPercentage(58,3440,1440));ProcessForTest(0xabc);
    put<float>(float_object,0x50,75.0f);put<int>(int_object,0x50,1);CHECK(HandBackScreenPercentage());ProcessForTest(0xabc);
    CHECK(get<float>(float_object,0x50)==75.0f);CHECK(get<int>(int_object,0x50)==1);CHECK(calls==2);
    setup();CHECK(!RequestScreenPercentage(0,3440,1440));CHECK(!RequestScreenPercentage(58,UINT32_MAX,1440));
    return test::finish("unreal_apply_test");
}
