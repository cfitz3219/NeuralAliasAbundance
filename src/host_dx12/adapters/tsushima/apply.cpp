#include "host_dx12/adapters/tsushima/apply.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/settings.hpp"
#include <windows.h>
#include "host_dx12/adapters/tsushima/locator.hpp"
#include <intrin.h>
#include <mutex>
#include <cstdio>

namespace na::tsushima {
namespace {
using Dispatch=bool(*)(void*);
using Request=void(*)(void*,const void*,bool);
Dispatch g_dispatch=nullptr;
Request g_request=nullptr;
uintptr_t g_caller=0;
std::mutex g_mutex;
std::once_flag g_init;
ApplyStatus g_status;
uint64_t g_deadline=0;
bool g_inside=false;
bool busy(){return g_status.stage==Stage::Queued || g_status.stage==Stage::Rebuilding;}
void refresh() {
    if(!busy())return;
    auto& st=na::settings::Get();
    if(!st.enabled.load() || st.follow_game_resolution.load() ||
       st.dlss_mode.load()!=g_status.choice.mode || st.preset.load()!=g_status.choice.preset) {
        g_status.stage=Stage::Cancelled;
        strcpy_s(g_status.reason,"Selection changed; choose Apply again.");
        if(na::ngx::GetSizeConfirmation().id==g_status.request_id)na::ngx::CancelSizeConfirmation();
        return;
    }
    auto observed=na::ngx::GetSizeConfirmation();
    if(observed.id!=g_status.request_id || observed.state==4 || GetTickCount64()>=g_deadline) {
        g_status.stage=Stage::Failed;
        strcpy_s(g_status.reason,"Ghost has not confirmed the requested rendering resolution. See Apply details.");
    } else if(g_status.stage==Stage::Rebuilding && observed.state==3) {
        g_status.stage=Stage::Confirmed;
        strcpy_s(g_status.reason,"Ghost is rendering at the selected picture mode.");
    }
}
bool dispatch(void* context,uintptr_t caller) {
    uint64_t id=0;
    GraphicsSettings next{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_status.frame_calls;refresh();
        auto* bytes=static_cast<unsigned char*>(context);
        // Only the verified normal frame-loop call may submit. Existing engine
        // requests take precedence; do not overwrite menu/display changes.
        if(caller==g_caller && !g_inside && g_status.stage==Stage::Queued && !bytes[0xa3a]) {
            std::memcpy(next.data(),bytes+0x7a8,next.size());
            const auto method=Read(next,0xb0), aa=Read(next,0x88);
            if(method==1 || (method==0 && aa==4)) {
                SetQuality(next,g_status.choice.mode);
                g_inside=true;id=g_status.request_id;
                g_status.stage=Stage::Rebuilding;++g_status.triggers;
                g_status.trigger_thread=GetCurrentThreadId();
                strcpy_s(g_status.reason,"Ghost is applying its graphics settings; checking rendering resolution.");
            } else {
                g_status.stage=Stage::Failed;
                strcpy_s(g_status.reason,"Select DLSS or DLAA in Ghost before taking control of its quality.");
            }
        }
    }
    // The game stages this POD using its ordinary validation and pending flag.
    // forceDeferred=true avoids its immediate-apply shortcut. Its original
    // frame-loop dispatch owns renderer synchronization, rebuild and persistence.
    // No engine call is made on the overlay thread or under our mutex.
    if(id)g_request(context,next.data(),true);
    bool result=g_dispatch(context);
    if(id){
        na::ngx::MarkSizeConfirmationRestored(id);
        std::lock_guard<std::mutex> lock(g_mutex);g_inside=false;refresh();
    }
    return result;
}
bool detour(void* context){return dispatch(context,reinterpret_cast<uintptr_t>(_ReturnAddress()));}
bool readable_range(const uint8_t* base, const uint8_t* start, size_t size) {
    // The loader may discard sections; never scan an inaccessible allocation.
    size_t at=0;
    while(at<size) {
        MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(start+at,&m,sizeof(m)) || m.AllocationBase!=base ||
           m.State!=MEM_COMMIT || (m.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
        const auto end=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
        const auto current=reinterpret_cast<uintptr_t>(start)+at;
        if(end<=current)return false;
        const size_t step=end-current;
        if(step>=size-at)return true;
        at+=step;
    }
    return true;
}
} // namespace
void InitializeApply() {
    std::call_once(g_init,[]{
        wchar_t path[32768]{};
        DWORD n=GetModuleFileNameW(nullptr,path,32768);
        if(!n || n==32768)return;
        const wchar_t* name=wcsrchr(path,L'\\');name=name?name+1:path;
        if(_wcsicmp(name,L"GhostOfTsushima.exe"))return;
        const auto* bytes=reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if(!bytes || dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>4096)return;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes+dos->e_lfanew);
        const size_t size=nt->OptionalHeader.SizeOfImage;
        std::lock_guard<std::mutex> lock(g_mutex);
        bool readable=true;
        const auto* sections=IMAGE_FIRST_SECTION(nt);
        for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
            const auto& s=sections[i];
            if(!(s.Characteristics&IMAGE_SCN_MEM_READ) || (s.Characteristics&IMAGE_SCN_MEM_DISCARDABLE))continue;
            if(s.VirtualAddress>size || s.Misc.VirtualSize>size-s.VirtualAddress ||
               !readable_range(bytes,bytes+s.VirtualAddress,s.Misc.VirtualSize)){readable=false;break;}
        }
        if(!readable) {
            strcpy_s(g_status.reason,"Ghost executable contains unreadable sections; live controls could not attach.");return;
        }
        const auto base=reinterpret_cast<uintptr_t>(bytes);
        const auto image=locator::mapped_image(bytes,size,base);
        const auto located=locator::locate(image);
        if(!located){strcpy_s(g_status.reason,located.reason);return;}
        if(!na::hooking::BeginInstall())return;
        g_request=reinterpret_cast<Request>(base+located.request);g_caller=base+located.caller;
        if(!na::hooking::PrepareHook(reinterpret_cast<void*>(base+located.dispatch),reinterpret_cast<void*>(&detour),reinterpret_cast<void**>(&g_dispatch))){
            na::hooking::AbandonInstall();return;
        }
        g_status.available=na::hooking::CommitInstall();
        strcpy_s(g_status.reason,g_status.available?"Ghost live picture-mode control is ready.":"Ghost live picture-mode control could not attach.");
    });
}
bool RequestApply(Choice choice) {
    std::lock_guard<std::mutex> lock(g_mutex);refresh();
    auto& st=na::settings::Get();
    if(!g_status.available || busy() || g_inside || choice.mode>4 || !choice.width || !choice.height ||
       !st.enabled.load() || st.follow_game_resolution.load() || st.dlss_mode.load()!=choice.mode || st.preset.load()!=choice.preset)return false;
    g_status.request_id=na::ngx::ArmSizeConfirmation(choice.width,choice.height,30,5000,7000,15000);
    g_status.choice=choice;g_status.stage=Stage::Queued;g_deadline=GetTickCount64()+15000;
    strcpy_s(g_status.reason,"Waiting for Ghost's next graphics-settings update.");
    return true;
}
ApplyStatus GetApplyStatus(){std::lock_guard<std::mutex> lock(g_mutex);refresh();return g_status;}
#ifdef NA_TSUSHIMA_APPLY_TEST
void ConfigureForTest(Dispatch original,Request request,uintptr_t caller){
    g_dispatch=original;g_request=request;g_caller=caller;g_status={};g_status.available=true;g_inside=false;
}
bool DispatchForTest(void* context,uintptr_t caller){return dispatch(context,caller);}
#endif
} // namespace na::tsushima

