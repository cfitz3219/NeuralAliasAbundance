#include "test_support.hpp"
#include "host_dx12/adapters/tsushima/apply.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include <array>
namespace na::tsushima {
void ConfigureForTest(bool(*)(void*),void(*)(void*,const void*,bool),uintptr_t);
bool DispatchForTest(void*,uintptr_t);
}
namespace {
std::array<unsigned char,0xb00> engine{};
int requests=0,dispatches=0;
void queue(void* c,const void* settings,bool deferred) {
    CHECK(deferred);CHECK(c==engine.data());
    CHECK(na::tsushima::GetApplyStatus().stage==na::tsushima::Stage::Rebuilding);
    std::memcpy(engine.data()+0x8f0,settings,0x148);engine[0xa3a]=1;++requests;
}
bool original(void*) {
    ++dispatches;
    if(engine[0xa3a]){std::memcpy(engine.data()+0x7a8,engine.data()+0x8f0,0x148);engine[0xa3a]=0;}
    return false; // result must be forwarded, not fabricated success
}
void setup(unsigned mode) {
    engine={};requests=dispatches=0;
    na::tsushima::GraphicsSettings settings{};uint32_t method=1;
    std::memcpy(settings.data()+0xb0,&method,4);
    std::memcpy(engine.data()+0x7a8,settings.data(),settings.size());
    auto& s=na::settings::Get();s.enabled=1;s.follow_game_resolution=0;s.dlss_mode=mode;s.preset=-1;
    na::tsushima::ConfigureForTest(original,queue,0xabc);
}
}
int main(){
    using namespace na::tsushima;
    for(unsigned mode=0;mode<5;++mode){
        GraphicsSettings s; s.fill(0x5a);auto before=s;
        CHECK(SetQuality(s,mode));
        CHECK(Read(s,0x4c)==0);CHECK(Read(s,0xb0)==(mode?1u:0u));
        CHECK(mode?Read(s,0xb4)==4-mode:Read(s,0x88)==4);
        for(size_t i=0;i<s.size();++i){
            bool changed=(i>=0x4c&&i<0x50)||(i>=0xb0&&i<0xb4)||
                         (mode?(i>=0xb4&&i<0xb8):(i>=0x88&&i<0x8c));
            if(!changed)CHECK(s[i]==before[i]);
        }
        setup(mode);CHECK(RequestApply({mode,1920,1080,-1}));
        CHECK(!RequestApply({mode,1920,1080,-1}));
        CHECK(!DispatchForTest(engine.data(),0xdef));CHECK(requests==0);
        CHECK(!DispatchForTest(engine.data(),0xabc));CHECK(requests==1);
        CHECK(GetApplyStatus().stage==Stage::Rebuilding); // engine dispatch alone isn't confirmation
        CHECK(!DispatchForTest(engine.data(),0xabc));CHECK(requests==1);
    }
    setup(1);CHECK(RequestApply({1,1920,1080,-1}));
    engine[0xa3a]=1;std::memcpy(engine.data()+0x8f0,engine.data()+0x7a8,0x148);
    CHECK(!DispatchForTest(engine.data(),0xabc));CHECK(requests==0);
    CHECK(!DispatchForTest(engine.data(),0xabc));CHECK(requests==1);
    setup(2);CHECK(RequestApply({2,1920,1080,-1}));na::settings::Get().dlss_mode=1;
    CHECK(!DispatchForTest(engine.data(),0xabc));CHECK(requests==0);CHECK(GetApplyStatus().stage==Stage::Cancelled);
    setup(3);CHECK(RequestApply({3,1920,1080,-1}));na::settings::Get().follow_game_resolution=1;
    CHECK(!DispatchForTest(engine.data(),0xabc));CHECK(requests==0);
    setup(1);engine[0x7a8+0xb0]=2;CHECK(RequestApply({1,1920,1080,-1}));
    CHECK(!DispatchForTest(engine.data(),0xabc));CHECK(requests==0);CHECK(GetApplyStatus().stage==Stage::Failed);
    return test::finish("tsushima_apply_test");
}
