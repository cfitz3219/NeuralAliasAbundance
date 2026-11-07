#include "test_support.hpp"
#include "host_dx12/scale/native_locator.hpp"
#include "host_dx12/scale/native_control.hpp"
#include <fstream>
#include <iterator>
#include <thread>
using namespace na::scale::native;
namespace {
struct Fixture {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(0x6000);
    locator::Image image;uint32_t shift=0;
    uint32_t setting=0,cache=0,active=0,context=0,fw=0,fh=0,sw=0,sh=0,up=0,upcache=0;
    uint32_t bind=0,parse=0,tick=0,apply=0,helper=0,update=0;
    template<class T>void put(uint32_t at,T v){std::memcpy(bytes.data()+at,&v,sizeof v);}
    template<class T>T get(uint32_t at)const{T v{};std::memcpy(&v,bytes.data()+at,sizeof v);return v;}
    void data(uint32_t at,const void* p,size_t n){std::memcpy(bytes.data()+at,p,n);}
    struct Code {
        Fixture& f;uint32_t start,at;
        void raw(std::initializer_list<uint8_t> v){for(auto b:v)f.bytes[at++]=b;}
        void rip(std::initializer_list<uint8_t> v,uint32_t target){raw(v);f.put<int32_t>(at,int32_t(target-at-4));at+=4;}
        void finish(){raw({0xc3});f.image.functions.push_back({start,at-start});}
    };
    Code code(uint32_t at){return {*this,at,at};}
    Fixture(bool inlined=false,uint32_t move=0,uint64_t base=0x140000000):shift(move){
        image={bytes.data(),bytes.size(),base,{{0x1000,0x2000,true},{0x3000,0x3000,false}},{}};
        bind=0x1100+move;parse=0x1300+move;tick=0x1500+move;apply=0x1800+move;helper=0x1c00+move;update=0x2000+move;
        context=0x3500+move;setting=context+0x438;cache=0x3a00+move;active=0x3b00+move;
        fw=active+4;fh=active+8;sw=active+12;sh=active+16;up=context+0x428;upcache=cache+16;
        const uint32_t ui=0x4000+move,key=0x4100+move,twenty=0x4200+move,step=twenty+4;
        constexpr wchar_t name[]=L"Frontend.Options.Display.RenderScale";data(ui,name,sizeof name);data(key,"RenderScale",12);
        put(twenty,20.f);put(step,.05f);put(0x4300,1.000001f); // numeric decoy
        auto b=code(bind);b.rip({0x48,0x8d,0x05},setting);b.rip({0x48,0x8d,0x0d},ui);b.finish();
        auto p=code(parse);p.rip({0x48,0x8d,0x0d},key);p.rip({0xf3,0x0f,0x59,0x0d},twenty);p.rip({0xf3,0x0f,0x59,0x0d},step);p.rip({0xf3,0x0f,0x11,0x0d},setting);p.finish();
        auto t=code(tick);t.rip({0xf3,0x0f,0x10,0x0d},setting);t.rip({0xf3,0x0f,0x59,0x0d},twenty);t.rip({0xf3,0x0f,0x59,0x0d},step);
        t.rip({0xf3,0x0f,0x11,0x0d},setting);t.rip({0x48,0x8d,0x0d},context);t.rip({0xe8},apply);t.finish();
        const auto dims=[&](Code& c){
            c.rip({0xf3,0x0f,0x11,0x0d},active);
            c.rip({0x8b,0x05},fw);c.raw({0xf3,0x48,0x0f,0x2a,0xc0,0xf3,0x0f,0x59,0xc1,0xf3,0x48,0x0f,0x2c,0xc0});c.rip({0x89,0x05},sw);
            c.rip({0x8b,0x05},fh);c.raw({0xf3,0x48,0x0f,0x2a,0xc0,0xf3,0x0f,0x59,0xc1,0xf3,0x48,0x0f,0x2c,0xc0});c.rip({0x89,0x05},sh);c.rip({0xe8},update);
        };
        auto a=code(apply);
        if(!inlined){a.rip({0x8b,0x05},upcache);a.rip({0x89,0x05},up);}
        a.rip({0xf3,0x0f,0x11,0x0d},cache);
        if(inlined)dims(a);else a.rip({0xe8},helper);
        a.rip({0xf3,0x0f,0x10,0x05},cache);a.raw({0xb0,0x01});a.rip({0xf3,0x0f,0x11,0x05},setting);a.finish();
        if(!inlined){auto h=code(helper);h.rip({0x8b,0x05},upcache);h.rip({0xf3,0x0f,0x10,0x0d},cache);dims(h);h.finish();}
        auto u=code(update);u.finish();
        IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x80;put(0,dos);
        IMAGE_NT_HEADERS64 nt{};nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]={0x4800,uint32_t(image.functions.size()*sizeof(RUNTIME_FUNCTION))};put(0x80,nt);
        for(unsigned n=0;n<image.functions.size();++n){auto f=image.functions[n];RUNTIME_FUNCTION rf{f.start,f.start+f.size,0x4a00};put(0x4800+n*uint32_t(sizeof rf),rf);}bytes[0x4a00]=1;
        put(setting,1.f);put(cache,1.f);put(active,1.f);put(fw,3440u);put(fh,1440u);put(sw,3440u);put(sh,1440u);put(up,3u);put(upcache,3u);
    }
};
Fixture* game=nullptr;bool dirty=false,inhibited=false,clamp_half=false;uint32_t native_calls=0,native_thread=0;float saved=0;
void native_apply(void*){
    ++native_calls;native_thread=GetCurrentThreadId();
    const float scale=game->get<float>(game->setting);game->put(game->cache,scale);game->put(game->upcache,game->get<uint32_t>(game->up));
    if(inhibited)return;
    if(clamp_half&&scale<.5f){game->put(game->active,.5f);return;} // an engine that refuses below 50%
    game->put(game->active,scale);game->put(game->sw,uint32_t(float(game->get<uint32_t>(game->fw))*scale));game->put(game->sh,uint32_t(float(game->get<uint32_t>(game->fh))*scale));
}
void native_tick(){
    if(!dirty)return;
    const auto scale=std::floor(game->get<float>(game->setting)*20.f+.5f)*.05f;
    game->put(game->setting,scale);ApplyForTest(game->bytes.data()+game->context);saved=game->get<float>(game->setting);dirty=false;
}
int inspect(const char* path){
    std::ifstream in(path,std::ios::binary);std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),{});
    if(raw.size()<sizeof(IMAGE_DOS_HEADER))return 2;IMAGE_DOS_HEADER dos{};std::memcpy(&dos,raw.data(),sizeof dos);
    if(dos.e_lfanew<0||size_t(dos.e_lfanew)+sizeof(IMAGE_NT_HEADERS64)>raw.size())return 2;
    IMAGE_NT_HEADERS64 nt{};std::memcpy(&nt,raw.data()+dos.e_lfanew,sizeof nt);
    if(nt.OptionalHeader.SizeOfImage>1024*1024*1024||nt.OptionalHeader.SizeOfHeaders>raw.size())return 2;
    std::vector<uint8_t> mapped(nt.OptionalHeader.SizeOfImage);if(nt.OptionalHeader.SizeOfHeaders>mapped.size())return 2;
    std::memcpy(mapped.data(),raw.data(),nt.OptionalHeader.SizeOfHeaders);
    for(unsigned n=0;n<nt.FileHeader.NumberOfSections;++n){const size_t at=size_t(dos.e_lfanew)+sizeof nt+n*sizeof(IMAGE_SECTION_HEADER);if(at+sizeof(IMAGE_SECTION_HEADER)>raw.size())return 2;
        IMAGE_SECTION_HEADER s{};std::memcpy(&s,raw.data()+at,sizeof s);if(uint64_t(s.PointerToRawData)+s.SizeOfRawData>raw.size()||uint64_t(s.VirtualAddress)+s.SizeOfRawData>mapped.size())return 2;
        std::memcpy(mapped.data()+s.VirtualAddress,raw.data()+s.PointerToRawData,s.SizeOfRawData);}
    auto im=locator::mapped_image(mapped.data(),mapped.size(),nt.OptionalHeader.ImageBase);const auto r=locator::locate(im);
    std::printf("native locator: setting=%x cache=%x active=%x context=%x apply=%x tick=%x full=%x/%x scene=%x/%x update=%x upscale=%x/%x: %s\n",r.setting,r.cache,r.active,r.context,r.apply,r.tick,r.full_w,r.full_h,r.scene_w,r.scene_h,r.update,r.upscale_setting,r.upscale_cache,r.reason);
    return r?0:1;
}
}
int main(int argc,char** argv){
    if(argc==2)return inspect(argv[1]);
    for(bool inline_apply:{false,true})for(uint32_t move:{0u,0x80u}){
        Fixture f(inline_apply,move,move?0x7ff600000000:0x140000000);const auto r=locator::locate(f.image);
        CHECK(r);CHECK(r.setting==f.setting);CHECK(r.context==f.context);CHECK(r.apply==f.apply);CHECK(r.tick==f.tick);
        CHECK(r.scene_w==f.sw);CHECK(r.scene_h==f.sh);CHECK(r.upscale_setting==(inline_apply?0:f.up));
        f.bytes[f.bind]=0xcc;CHECK(!locator::locate(f.image));
    }
    Fixture f;auto r=locator::locate(f.image);game=&f;ConfigureForTest(f.bytes.data(),r,&native_tick,&native_apply);
    Request(1);CHECK(native_calls==0);TickForTest();CHECK(native_calls==0); // no scene yet; UI/setup never calls native
    Observe(true,3440,1440,3440,1440,1,true);
    std::thread engine([]{TickForTest();});engine.join();
    auto st=GetStats();CHECK(st.controlled);CHECK(!st.confirmed);CHECK(st.thread==native_thread);CHECK(st.render_w==2293);CHECK(st.render_h==960);
    CHECK(f.get<float>(f.setting)==1.f);CHECK(f.get<uint32_t>(f.up)==3);CHECK(f.get<uint32_t>(f.upcache)==3); // pending/user settings intact
    // 2026-11-07: SE5 saved our 0.333 to its ini at exit. After a committed
    // apply the native cache (what the game saves) holds the USER's values again.
    CHECK(f.get<float>(f.cache)==1.f);CHECK(std::abs(f.get<float>(f.active)-2.f/3.f)<1e-6f);
    Observe(true,2293,960,2293,960,2,true);CHECK(!GetStats().confirmed); // reduced DLAA is not full-output DLSS
    Observe(true,2293,959,3440,1440,2,true);CHECK(!GetStats().confirmed); // both dimensions
    Observe(true,2293,960,3440,1440,1,true);CHECK(!GetStats().confirmed); // old evaluation
    Observe(true,2293,960,3440,1440,3,true);CHECK(GetStats().confirmed);
    Observe(true,2293,960,3440,1440,3,false);CHECK(!GetStats().confirmed);
    Observe(true,2293,960,3440,1440,3,true);CHECK(!GetStats().confirmed); // no new submission
    Observe(true,2293,960,3440,1440,4,true);CHECK(GetStats().confirmed);
    Observe(true,2293,959,3440,1440,5,true);CHECK(!GetStats().confirmed);
    // Subsequent updates execute on the same simulated engine thread. Configure
    // resets ownership only between isolated fixtures, never in production.
    ConfigureForTest(f.bytes.data(),r,&native_tick,&native_apply);Observe(true,3440,1440,3440,1440,3,true);Request(2);
    dirty=true;TickForTest();CHECK(saved==1.f);CHECK(f.get<float>(f.setting)==1.f);CHECK(GetStats().render_w>=1999&&GetStats().render_w<=2000);CHECK(GetStats().render_h==837);
    Request(3);inhibited=true;TickForTest();CHECK(f.get<float>(f.cache)==.5f);inhibited=false;TickForTest();CHECK(GetStats().render_w==1720);CHECK(GetStats().render_h==720);
    // a0fe517 run (ini saved RenderScale=0.5): the game commits a deferred scale on its own,
    // after our apply. The next tick puts the user's value back into the cache it saves.
    Request(2);inhibited=true;TickForTest();const float deferred=f.get<float>(f.cache);CHECK(deferred<1.f&&deferred>.25f);inhibited=false;
    f.put(f.active,deferred);f.put(f.sw,uint32_t(3440.f*deferred));f.put(f.sh,uint32_t(1440.f*deferred));
    TickForTest();CHECK(f.get<float>(f.cache)==1.f);CHECK(f.get<float>(f.active)==deferred);
    Request(4);TickForTest();CHECK(GetStats().render_w==1146);CHECK(GetStats().render_h==480);
    Request(0);TickForTest();CHECK(GetStats().render_w==3440);CHECK(GetStats().render_h==1440);
    Observe(false,3440,1440,3440,1440,4,true);TickForTest();CHECK(!GetStats().controlled);CHECK(f.get<uint32_t>(f.upcache)==3);CHECK(f.get<float>(f.cache)==1.f);
    // Audit 2026-11-06: an engine that keeps another scale must not be resized
    // on every tick forever (visible as a constant hitch): bounded retries.
    ConfigureForTest(f.bytes.data(),r,&native_tick,&native_apply);Observe(true,3440,1440,3440,1440,5,true);
    clamp_half=true;Request(4);
    for(int i=0;i<200;++i)TickForTest();
    CHECK(GetStats().applies<=31);CHECK(GetStats().refusals==1);
    for(int i=0;i<300;++i)TickForTest();
    CHECK(GetStats().applies<=33);
    clamp_half=false;Request(1);TickForTest();CHECK(std::abs(f.get<float>(f.active)-2.f/3.f)<1e-6f); // a new request tries at once
    // The game's own apply from a different worker thread keeps our scale
    // (no resize back to the user's 100% and forward again: a flip-flop).
    std::thread other([]{dirty=true;TickForTest();});other.join();
    CHECK(std::abs(f.get<float>(f.active)-2.f/3.f)<1e-6f);CHECK(f.get<float>(f.setting)==1.f);
    const auto settled=GetStats().applies;
    TickForTest();CHECK(GetStats().applies==settled); // nothing to redo on the next tick
    CHECK(GetStats().game_applies>=1);
    // Pause during an inhibited update must retain restoration ownership and
    // retry after the native cache has staged the original value already.
    for(uint32_t restore_up:{0u,3u}){
        f.put(f.up,restore_up);f.put(f.upcache,restore_up);f.put(f.active,1.f);f.put(f.cache,1.f);f.put(f.sw,3440u);f.put(f.sh,1440u);
        ConfigureForTest(f.bytes.data(),r,&native_tick,&native_apply);Observe(true,3440,1440,3440,1440,6,true);Request(1);TickForTest();
        inhibited=true;Observe(false,2293,960,3440,1440,6,false);TickForTest();
        CHECK(GetStats().controlled);CHECK(f.get<float>(f.cache)==1.f);CHECK(f.get<float>(f.active)!=1.f);
        inhibited=false;TickForTest();CHECK(!GetStats().controlled);CHECK(f.get<float>(f.active)==1.f);
        CHECK(f.get<uint32_t>(f.upcache)==restore_up);CHECK(GetStats().render_w==3440&&GetStats().render_h==1440);
    }
    return test::finish("native_scale_test");
}
