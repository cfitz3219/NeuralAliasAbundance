#include "host_dx12/adapters/unreal/locator.hpp"
#include "test_support.hpp"
#include "host_dx12/adapters/unreal/screen_percentage.hpp"
#include "host_dx12/adapters/mode_control.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <intrin.h>
#include <array>
#include <atomic>
namespace na::unreal {
void ConfigureForTest(void*,void*,void(*)(void*,const wchar_t*,const void*),void(*)(void*,const wchar_t*,const void*));
void ProcessForTest(uintptr_t);
void DiscoveryForTest(void(*)());
bool AttachSinkForTest(void**,void*,uintptr_t,char*,size_t);
bool RestoreSinkForTest(void**);
}
namespace {
std::array<uint8_t,0x58> float_object{},int_object{};
int calls=0;DWORD setter_thread=0;
std::atomic<int> discovery_calls{0};
uintptr_t observed_sink_caller=0;LONG tick_returns=0;int forwarded_sinks=0;
using TestSink=void(*)(void*);
__declspec(noinline) void probe_sink(void*){
    observed_sink_caller=reinterpret_cast<uintptr_t>(_ReturnAddress());++forwarded_sinks;
    // Fixture's original engine sink propagates the changed value. Production
    // never writes this shadow directly; its native sink must be called.
    memcpy(float_object.data()+0x54,float_object.data()+0x50,sizeof(float));
}
__declspec(noinline) void invoke_tick(void* manager){
    auto** table=*static_cast<void***>(manager);
    const auto sink=*reinterpret_cast<TestSink volatile*>(table+11);
    sink(manager);InterlockedIncrement(&tick_returns); // preserve a real call, not a tail jump
}
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
void discover(){setup();++discovery_calls;}
void discover_pending(){++discovery_calls;}
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
    {
        // Exercise actual vtable -> detour -> native setter -> original sink.
        // A valid one-byte RET function cannot host MinHook's inline patch;
        // the engine's existing virtual dispatch must work without such a patch.
        setup();
        auto* code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));REQUIRE(code);
        memset(code,0xf1,4096);code[128]=0xc3;DWORD protect=0;
        REQUIRE(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&protect));FlushInstructionCache(GetCurrentProcess(),code,4096);
        void* tiny=code+128;void* unused=nullptr;
        REQUIRE(na::hooking::Initialize());REQUIRE(na::hooking::BeginInstall());
        CHECK(!na::hooking::PrepareHook(tiny,reinterpret_cast<void*>(&probe_sink),&unused));
        na::hooking::AbandonInstall();na::hooking::Shutdown();
        auto** table=static_cast<void**>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));REQUIRE(table);
        for(unsigned n=0;n<16;++n)table[n]=reinterpret_cast<void*>(&probe_sink);
        struct Manager{void** table;} manager{table};invoke_tick(&manager);REQUIRE(observed_sink_caller);
        const uintptr_t tick_callsite=observed_sink_caller;
        table[11]=tiny;REQUIRE(VirtualProtect(table,4096,PAGE_READONLY,&protect));
        char error[200]{};REQUIRE(AttachSinkForTest(table+11,tiny,tick_callsite,error,sizeof error));
        MEMORY_BASIC_INFORMATION mbi{};REQUIRE(VirtualQuery(table,&mbi,sizeof mbi));CHECK(mbi.Protect==PAGE_READONLY);
        CHECK(RequestScreenPercentage(66.7f,3440,1440));
        reinterpret_cast<TestSink>(table[11])(&manager);CHECK(calls==0); // unrelated caller cannot write
        invoke_tick(&manager);CHECK(calls==2);CHECK(setter_thread==GetCurrentThreadId());
        CHECK(get<float>(float_object,0x50)==66.7f && get<int>(int_object,0x50)==0);
        CHECK(GetScreenPercentageStatus().sink_calls==2 && GetScreenPercentageStatus().game_thread_calls==1);
        CHECK(HandBackScreenPercentage());invoke_tick(&manager);CHECK(calls==4);
        CHECK(get<float>(float_object,0x50)==50.f && get<int>(int_object,0x50)==2);
        CHECK(RestoreSinkForTest(table+11));CHECK(table[11]==tiny);CHECK(code[128]==0xc3 && code[127]==0xf1 && code[129]==0xf1);
        CHECK(!AttachSinkForTest(table+11,reinterpret_cast<void*>(&probe_sink),tick_callsite,error,sizeof error));
        CHECK(table[11]==tiny);for(unsigned n=0;n<16;++n)if(n!=11)CHECK(table[n]==reinterpret_cast<void*>(&probe_sink));
        REQUIRE(VirtualProtect(table,4096,PAGE_READWRITE,&protect));table[11]=reinterpret_cast<void*>(&probe_sink);
        REQUIRE(VirtualProtect(table,4096,PAGE_READONLY,&protect));setup();const int forwarded_before=forwarded_sinks;
        REQUIRE(AttachSinkForTest(table+11,reinterpret_cast<void*>(&probe_sink),tick_callsite,error,sizeof error));
        REQUIRE(RequestScreenPercentage(58.f,3440,1440));invoke_tick(&manager);
        CHECK(calls==2 && forwarded_sinks==forwarded_before+1);CHECK(get<float>(float_object,0x54)==58.f);
        CHECK(HandBackScreenPercentage());invoke_tick(&manager);
        CHECK(calls==4 && forwarded_sinks==forwarded_before+2);CHECK(get<float>(float_object,0x54)==50.f);
        CHECK(RestoreSinkForTest(table+11));CHECK(table[11]==reinterpret_cast<void*>(&probe_sink));
        VirtualFree(table,0,MEM_RELEASE);VirtualFree(code,0,MEM_RELEASE);
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
    // A recovery start must remain deferred, but explicit overlay Apply must
    // discover the adapter before choosing its fallback methods.
    DiscoveryForTest(discover);ConfigureScreenPercentage(true,true);
    CHECK(discovery_calls==0 && !GetScreenPercentageStatus().available);
    na::mode::Start();na::mode::Request request;
    request.mode=1;request.out_w=3440;request.out_h=1440;request.render_w=2296;request.render_h=960;request.unreal_percent=66.7f;
    CHECK(na::mode::RequestApply(request));
    const ULONGLONG until=GetTickCount64()+1500;
    while(GetTickCount64()<until && GetScreenPercentageStatus().state!=SpState::Applying)Sleep(10);
    CHECK(discovery_calls==1);
    CHECK((na::mode::GetStatus().available_mask & (1u<<uint32_t(na::mode::Layer::Unreal)))!=0);
    CHECK(GetScreenPercentageStatus().state==SpState::Applying);
    if(discovery_calls==1){ProcessForTest(0xabc);CHECK(calls==2);}
    na::mode::Cancel(true);Sleep(80);if(discovery_calls==1)ProcessForTest(0xabc);
    CHECK(!ResumeScreenPercentageForApply());CHECK(discovery_calls==1); // already initialized, no second scan
    // Cancelling while the worker waits for discovery must not enqueue an
    // engine change when discovery becomes ready afterwards.
    DiscoveryForTest(discover_pending);ConfigureScreenPercentage(true,true);
    CHECK(na::mode::RequestApply(request));
    const ULONGLONG pending_until=GetTickCount64()+1500;
    while(discovery_calls<2 && GetTickCount64()<pending_until)Sleep(10);
    CHECK(discovery_calls==2);na::mode::Cancel(true);setup();Sleep(80);
    ProcessForTest(0xabc);CHECK(calls==0);CHECK(na::mode::GetStatus().state==na::mode::State::Cancelled);
    DiscoveryForTest(discover);ConfigureScreenPercentage(false,true);
    CHECK(!ResumeScreenPercentageForApply());CHECK(discovery_calls==2);CHECK(!GetScreenPercentageStatus().available);
    ConfigureScreenPercentage(true,false);CHECK(discovery_calls==3);CHECK(calls==0); // ordinary startup remains supported
    return test::finish("unreal_apply_test");
}
