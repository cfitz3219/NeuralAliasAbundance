#include "host_dx12/scale/native_control.hpp"
#include "host_dx12/scale/native_locator.hpp"
#include "host_dx12/scale/settings_read_hook.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <atomic>
#include <cstdio>
#include <mutex>

namespace na::scale::native {
namespace {
using Tick=void(*)();using Apply=void(*)(void*);
struct State {
    uint8_t* base=nullptr;locator::Result found;Tick tick=nullptr;Apply apply=nullptr;
    std::atomic<bool> available{false},enabled{false},seen{false};
    std::atomic<uint32_t> mode{0};std::atomic<uint64_t> revision{1},evaluation{0};
    std::mutex mutex;Stats stats;uint64_t consumed=0;
    std::atomic<uint64_t> armed_evaluation{0};
    std::atomic<bool> controlled{false};std::atomic<uint32_t> owner{0};
};
State& g=*new State;
thread_local bool inside=false;
template<class T>T read(uint32_t r){T v{};std::memcpy(&v,g.base+r,sizeof v);return v;}
template<class T>void put(uint32_t r,T v){std::memcpy(g.base+r,&v,sizeof v);}
void note(const char* text){std::lock_guard<std::mutex> lock(g.mutex);std::snprintf(g.stats.reason,sizeof g.stats.reason,"%s",text);}
void apply_native(void* context,bool override_scale){
    if(!override_scale){g.apply(context);return;}
    // Only the located native settings object, on its ordinary update thread.
    // Preserve pending/user settings so the original save operation sees them.
    auto& f=g.found;
    const float pending=read<float>(f.setting);
    const uint32_t upscale=f.upscale_setting?read<uint32_t>(f.upscale_setting):0;
    const auto mode=g.mode.load(std::memory_order_acquire);
    const float fraction=static_cast<float>(FractionForMode(static_cast<int>(mode)));
    struct Restore {
        uint32_t scale,up;float pending;uint32_t upscale;
        ~Restore(){put(scale,pending);if(up)put(up,upscale);}
    } restore{f.setting,f.upscale_setting,pending,upscale};
    put(f.setting,fraction);if(f.upscale_setting)put<uint32_t>(f.upscale_setting,0);
    // A deferred/inhibited native update can already have staged the desired
    // cache without committing the renderer size. Reconcile that staging with
    // the actual committed scale, then let native apply perform its own retry.
    const float active=read<float>(f.active),cached=read<float>(f.cache);
    if(cached==fraction&&active!=fraction&&std::isfinite(active)&&active>=.25f&&active<=2.f)put(f.cache,active);
    inside=true;g.apply(context);inside=false;
    std::lock_guard<std::mutex> lock(g.mutex);
    ++g.stats.applies;g.stats.controlled=true;g.stats.confirmed=false;g.stats.fraction=fraction;
}
void apply_hook(void* context){
    const bool ours=!inside&&g.controlled&&g.enabled.load(std::memory_order_acquire)&&
        GetCurrentThreadId()==g.owner&&context==g.base+g.found.context;
    apply_native(context,ours);
}
void tick_hook(){
    // Preserve the original tick/save/update ordering. Native menu changes are
    // still handled normally; their apply call observes the current override.
    g.tick();
    if(inside||!g.seen.load(std::memory_order_acquire))return;
    const auto thread=GetCurrentThreadId();
    uint32_t no_owner=0;g.owner.compare_exchange_strong(no_owner,thread);
    if(g.owner!=thread)return;
    const auto revision=g.revision.load(std::memory_order_acquire);
    const bool enabled=g.enabled.load(std::memory_order_acquire);
    const auto& f=g.found;
    const auto mode=g.mode.load(std::memory_order_acquire);
    const float wanted=static_cast<float>(FractionForMode(static_cast<int>(mode)));
    const float active=read<float>(f.active);
    const uint32_t fw=read<uint32_t>(f.full_w),fh=read<uint32_t>(f.full_h);
    if(!fw||!fh||fw>32768||fh>32768)return; // renderer has not initialized
    if(enabled&&(revision!=g.consumed||!g.controlled||active!=wanted)){
        // Directly reuses the game's complete settings apply, including SER's
        // inlined size update, AFTER the tick's 5% pending-setting normalizer.
        apply_native(g.base+f.context,true);g.controlled=true;g.consumed=revision;
        g.armed_evaluation=g.evaluation.load(std::memory_order_acquire);
        note("Native scene size applied; checking DLSS input and full-size output.");
    }else if(!enabled&&g.controlled){
        // The preserved pending settings are the game's own requested values.
        apply_native(g.base+f.context,false);g.controlled=false;g.consumed=revision;
        note("Game render scale restored; NeuralAlias is paused.");
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    ++g.stats.ticks;g.stats.thread=thread;g.stats.mode=mode;g.stats.controlled=g.controlled;
    g.stats.render_w=read<uint32_t>(f.scene_w);g.stats.render_h=read<uint32_t>(f.scene_h);
    g.stats.out_w=fw;g.stats.out_h=fh;
}
bool readable(const uint8_t* start,size_t size,const void* base){
    for(size_t at=0;at<size;){MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(start+at,&m,sizeof m)||m.AllocationBase!=base||m.State!=MEM_COMMIT||m.Protect&(PAGE_NOACCESS|PAGE_GUARD))return false;
        const auto end=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
        const auto current=reinterpret_cast<uintptr_t>(start)+at;if(end<=current)return false;
        const auto step=end-current;if(step>=size-at)return true;at+=step;}
    return true;
}
} // namespace
bool Start(int mode,const wchar_t* log_path){
    if(g.available.load())return true;
    auto* base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if(!base||!readable(base,4096,base))return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<0||dos->e_lfanew>3072)return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64)return false;
    const auto size=nt->OptionalHeader.SizeOfImage;
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned n=0;n<nt->FileHeader.NumberOfSections;++n){const auto& s=sections[n];
        if((s.Characteristics&IMAGE_SCN_MEM_READ)&&!(s.Characteristics&IMAGE_SCN_MEM_DISCARDABLE)&&
           (s.VirtualAddress>size||s.Misc.VirtualSize>size-s.VirtualAddress||!readable(base+s.VirtualAddress,s.Misc.VirtualSize,base)))return false;}
    auto im=locator::mapped_image(base,size,reinterpret_cast<uintptr_t>(base));const auto found=locator::locate(im);
    if(log_path){FILE* file=nullptr;if(!_wfopen_s(&file,log_path,L"a")){
        std::fprintf(file,"native_scale located=%u setting=%x cache=%x active=%x context=%x tick=%x apply=%x full=%x/%x scene=%x/%x upscale=%x/%x reason=\"%s\"\n",
            bool(found),found.setting,found.cache,found.active,found.context,found.tick,found.apply,found.full_w,found.full_h,found.scene_w,found.scene_h,found.upscale_setting,found.upscale_cache,found.reason);std::fclose(file);}}
    if(!found){note(found.reason);return false;}
    g.base=base;g.found=found;g.mode.store(mode>=0&&mode<=4?uint32_t(mode):0);
    if(!na::hooking::Initialize()){note("Native settings hook could not initialize.");return false;}
    if(!na::hooking::BeginInstallWait("native render-scale control")){na::hooking::Shutdown();return false;}
    if(!na::hooking::PrepareHook(base+found.apply,reinterpret_cast<void*>(&apply_hook),reinterpret_cast<void**>(&g.apply))||
       !na::hooking::PrepareHook(base+found.tick,reinterpret_cast<void*>(&tick_hook),reinterpret_cast<void**>(&g.tick))){na::hooking::AbandonInstall();return false;}
    if(!na::hooking::CommitInstall())return false;
    g.available.store(true,std::memory_order_release);
    {std::lock_guard<std::mutex> lock(g.mutex);g.stats.available=true;}
    note("Native render-scale control ready; waiting for the game's scene.");return true;
}
bool Available(){return g.available.load(std::memory_order_acquire);}
void Request(int mode){
    if(!Available()||mode<0||mode>4)return;g.mode.store(uint32_t(mode),std::memory_order_release);g.revision.fetch_add(1,std::memory_order_acq_rel);
    {std::lock_guard<std::mutex> lock(g.mutex);++g.stats.requests;g.stats.confirmed=false;}
    note("Waiting for the game's native settings update.");
}
void Observe(bool enabled,uint32_t rw,uint32_t rh,uint32_t ow,uint32_t oh,uint64_t evaluation,bool evaluated){
    if(!Available())return;
    g.evaluation.store(evaluation,std::memory_order_release);
    if(rw&&rh)g.seen.store(true,std::memory_order_release);
    const bool changed=g.enabled.exchange(enabled,std::memory_order_acq_rel)!=enabled;
    if(changed)g.revision.fetch_add(1,std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(g.mutex);auto& s=g.stats;s.evaluation=evaluation;
    if(changed)s.confirmed=false;
    const float f=static_cast<float>(FractionForMode(static_cast<int>(g.mode.load())));
    // Native SSE conversion and compiler float contraction can differ at an
    // integer boundary by one pixel. Confirm the EXACT committed dimensions,
    // and independently require them to match the requested scale within that
    // rounding allowance; never accept a width-only percentage tolerance.
    const bool ratio=std::abs(double(s.render_w)-double(s.out_w)*f)<=1.01&&std::abs(double(s.render_h)-double(s.out_h)*f)<=1.01;
    if(enabled&&s.controlled&&ratio&&evaluated&&evaluation>g.armed_evaluation&&ow==s.out_w&&oh==s.out_h&&s.render_w==rw&&s.render_h==rh){
        s.confirmed=true;std::snprintf(s.reason,sizeof s.reason,"Native scene %u x %u; DLSS output %u x %u.",rw,rh,ow,oh);}
}
Stats GetStats(){std::lock_guard<std::mutex> lock(g.mutex);return g.stats;}
#ifdef NA_NATIVE_SCALE_TEST
void ConfigureForTest(uint8_t* base,const locator::Result& found,Tick tick,Apply apply){
    g.base=base;g.found=found;g.tick=tick;g.apply=apply;g.available=true;g.enabled=false;g.seen=false;g.mode=0;g.revision=1;g.evaluation=0;
    g.consumed=g.armed_evaluation=0;g.controlled=false;g.owner=0;g.stats={};g.stats.available=true;inside=false;
}
void TickForTest(){tick_hook();}
void ApplyForTest(void* context){apply_hook(context);}
#endif
} // namespace na::scale::native
