#include "test_support.hpp"
#include "host_dx12/adapters/unreal/screen_percentage.hpp"
#include <array>
namespace na::unreal {
void ConfigureForTest(void*,void*,void(*)(void*,const wchar_t*,const void*),void(*)(void*,const wchar_t*,const void*));
void ProcessForTest(uintptr_t);
}
namespace {
std::array<uint8_t,0x58> f{},i{};
int calls=0;DWORD setter_thread=0;
template<class T> void put(std::array<uint8_t,0x58>& b,size_t at,T v){std::memcpy(b.data()+at,&v,sizeof(v));}
template<class T> T get(const std::array<uint8_t,0x58>& b,size_t at){T v{};std::memcpy(&v,b.data()+at,sizeof(v));return v;}
void set(void* object,const wchar_t* text,const void* params) {
    ++calls;setter_thread=GetCurrentThreadId();
    // Real setter reentry must not deadlock on the adapter status mutex.
    (void)na::unreal::GetScreenPercentageStatus();
    uint32_t flags{};std::memcpy(&flags,params,4);CHECK(flags==0x08000000);
    const auto* p=static_cast<const uint8_t*>(params);for(unsigned n=4;n<24;++n)CHECK(p[n]==0);
    if(object==f.data())put<float>(f,0x50,float(wcstod(text,nullptr)));
    else {CHECK(object==i.data());put<int>(i,0x50,_wtoi(text));}
    // Deliberately do not propagate the render shadow: that's engine work.
}
void setup(){
    f={};i={};calls=0;setter_thread=0;
    put<float>(f,0x50,50.f);put<float>(f,0x54,50.f);put<int>(i,0x50,2);
    put<uint32_t>(f,0x18,0x08000020);put<uint32_t>(i,0x18,0x08000000);
    na::unreal::ConfigureForTest(f.data(),i.data(),set,set);
}
}
int main(){
    using namespace na::unreal;
    setup();CHECK(RequestScreenPercentage(66.7f,3440,1440));CHECK(calls==0);
    CHECK(!RequestScreenPercentage(58,3440,1440));
    ProcessForTest(0xdef);CHECK(calls==0);
    ProcessForTest(0xabc);CHECK(calls==2);CHECK(setter_thread==GetCurrentThreadId());
    CHECK(get<int>(i,0x50)==0);CHECK(get<float>(f,0x50)==66.7f);CHECK(get<float>(f,0x54)==50.f);
    CHECK(GetScreenPercentageStatus().state==SpState::Applying); // Set isn't rendered evidence
    CHECK(HandBackScreenPercentage());CHECK(calls==2);ProcessForTest(0xabc);
    CHECK(calls==4);CHECK(get<int>(i,0x50)==2);CHECK(get<float>(f,0x50)==50.f);
    setup();CHECK(RequestScreenPercentage(58,3440,1440));CancelScreenPercentage();ProcessForTest(0xabc);CHECK(calls==0);
    setup();CHECK(RequestScreenPercentage(58,3440,1440));ProcessForTest(0xabc);
    put<float>(f,0x50,75.f);put<int>(i,0x50,1);CHECK(HandBackScreenPercentage());ProcessForTest(0xabc);
    CHECK(get<float>(f,0x50)==75.f);CHECK(get<int>(i,0x50)==1);CHECK(calls==2);
    setup();CHECK(!RequestScreenPercentage(0,3440,1440));CHECK(!RequestScreenPercentage(58,UINT32_MAX,1440));
    return test::finish("unreal_apply_test");
}
