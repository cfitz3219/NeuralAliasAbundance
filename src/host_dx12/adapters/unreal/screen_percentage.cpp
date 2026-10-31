#include "host_dx12/adapters/unreal/screen_percentage.hpp"

#include "host_dx12/ngx_observe/ngx_observe.hpp"

#include <windows.h>
#include "host_dx12/adapters/unreal/locator.hpp"
#include <intrin.h>
#include <array>
#include "host_dx12/hooking/vtable_hook.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

namespace na::unreal {
namespace {

struct Section { const uint8_t* base = nullptr; size_t size = 0; bool exec = false, write = false; };
struct Image { const uint8_t* base = nullptr; size_t size = 0; std::vector<Section> sections; };

bool load_image(Image& img)
{
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    img.base = base;
    img.size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if(!(sh[i].Characteristics & IMAGE_SCN_MEM_READ) || (sh[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE))continue;
        if(sh[i].VirtualAddress>img.size || sh[i].Misc.VirtualSize>img.size-sh[i].VirtualAddress)return false;
        Section s;
        s.base = base + sh[i].VirtualAddress;
        s.size = sh[i].Misc.VirtualSize;
        s.exec = (sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        s.write = (sh[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        img.sections.push_back(s);
    }
    return true;
}

bool region_ok(const void* p, size_t n, bool need_write)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD wr = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
    if (!(mbi.Protect & (need_write ? wr : rd))) return false;
    return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

bool sane_percent(float v) { return std::isfinite(v) && v >= 1.f && v <= 400.f; }
// Supported engine ABI is discovered from code/data relationships, not a
// title, executable hash, or fixed address. See locator.hpp.
using Sink=void(*)(void*);
using Setter=void(*)(void*,const wchar_t*,const void*);
Sink g_sink=nullptr;
Setter g_float_set=nullptr,g_int_set=nullptr;
void* g_float=nullptr;void* g_int=nullptr;
uintptr_t g_caller=0;
std::mutex g_m;
SpStatus g_status;
std::atomic<bool> g_started{false};
struct Pending {bool set=false;float percent=0;uint32_t ow=0,oh=0;uint64_t generation=0;} g_pending;
uint64_t g_generation=0,g_confirm=0;
bool g_handback=false,g_executing=false;
struct Ownership {bool held=false;float before=0,ours=0;int drs=0;uint32_t float_priority=0,int_priority=0;} g_owner;
struct Values {float percent;int drs;uint32_t fp,ip;bool valid;};
Values values() {
    Values v{};
    __try {
        v.percent=*reinterpret_cast<const float*>(static_cast<const uint8_t*>(g_float)+0x50);
        v.drs=*reinterpret_cast<const int*>(static_cast<const uint8_t*>(g_int)+0x50);
        v.fp=*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(g_float)+0x18)&0xff000000u;
        v.ip=*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(g_int)+0x18)&0xff000000u;
        v.valid=sane_percent(v.percent)&&v.drs>=0&&v.drs<=2;
    } __except(EXCEPTION_EXECUTE_HANDLER) {v.valid=false;}
    return v;
}
void set_value(Setter set,void* variable,const wchar_t* value,uint32_t priority) {
    // This build takes a packed flags + FName parameter record, NOT uint32
    // flags by value. Preserve set-by precedence; zero FName denotes None.
    alignas(8) uint8_t args[24]{};std::memcpy(args,&priority,4);
    set(variable,value,args); // engine owns callback/sink/render-shadow propagation
}
void restore(const Ownership& owned) {
    if(!owned.held)return;
    const auto v=values();if(!v.valid)return;
    wchar_t text[32]{};
    // Do not overwrite a subsequent menu/engine change, including priority.
    if(std::fabs(v.percent-owned.ours)<0.01f && v.fp==owned.float_priority) {
        swprintf_s(text,L"%.9g",double(owned.before));set_value(g_float_set,g_float,text,v.fp);
    }
    if(v.drs==0 && v.ip==owned.int_priority) {
        swprintf_s(text,L"%d",owned.drs);set_value(g_int_set,g_int,text,v.ip);
    }
}
void process(uintptr_t caller) {
    Pending req;Ownership handback;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if(caller!=g_caller || g_executing)return;
        g_executing=true;
        if(g_handback){handback=g_owner;g_owner={};g_handback=false;}
        req=g_pending;g_pending={};
    }
    restore(handback); // no host mutex held across engine callbacks
    if(req.set) {
        const auto v=values();
        uint64_t confirm=0;bool admitted=false;
        {
            std::lock_guard<std::mutex> lock(g_m);
            admitted=v.valid&&req.generation==g_generation&&!g_handback;
            if(admitted) {
                if(!g_owner.held || std::fabs(v.percent-g_owner.ours)>0.01f || v.fp!=g_owner.float_priority || v.drs!=0 || v.ip!=g_owner.int_priority)
                    g_owner={true,v.percent,req.percent,v.drs,v.fp,v.ip};
                g_owner.ours=req.percent;
                g_status.requested=req.percent;g_status.request_id=req.generation;
                g_status.expect_w=ExpectedExtent(req.ow,req.percent);g_status.expect_h=ExpectedExtent(req.oh,req.percent);
                confirm=na::ngx::ArmLiveSizeConfirmation(g_status.expect_w,g_status.expect_h,req.ow,req.oh);
                g_confirm=confirm;
            }
        }
        if(admitted) {
            wchar_t text[32]{};swprintf_s(text,L"%.9g",double(req.percent));
            set_value(g_int_set,g_int,L"0",v.ip);
            set_value(g_float_set,g_float,text,v.fp);
            na::ngx::MarkSizeConfirmationRestored(confirm);
            const auto after=values();
            std::lock_guard<std::mutex> lock(g_m);
            if(req.generation==g_generation) {
                if(!after.valid || after.drs!=0 || std::fabs(after.percent-req.percent)>0.01f) {
                    g_status.state=SpState::NotApplied;g_handback=true;
                    strcpy_s(g_status.reason,"The engine did not accept the requested resolution settings.");
                    if(na::ngx::GetSizeConfirmation().id==confirm)na::ngx::CancelSizeConfirmation();
                } else strcpy_s(g_status.reason,"Engine setting applied; checking actual DLSS frames.");
            }
        }
    }
    const auto v=values();
    {
        std::lock_guard<std::mutex> lock(g_m);
        if(v.valid)g_status.current=v.percent;
        if(g_status.state==SpState::Applying && g_confirm) {
            const auto c=na::ngx::GetSizeConfirmation();
            g_status.seen_w=c.last_w;g_status.seen_h=c.last_h;
            if(!v.valid || v.drs!=0 || std::fabs(v.percent-g_status.requested)>0.01f) {
                g_status.state=SpState::GameChanged;
                strcpy_s(g_status.reason,"The game's settings changed; its choice is kept.");
                if(c.id==g_confirm)na::ngx::CancelSizeConfirmation();
            } else if(c.id!=g_confirm || c.state==4) {
                g_status.state=SpState::NotApplied;g_handback=true;
                strcpy_s(g_status.reason,"The requested resolution was not confirmed on rendered DLSS frames.");
            } else if(c.state==3) {
                g_status.state=SpState::Confirmed;
                snprintf(g_status.reason,sizeof(g_status.reason),"Applied: the game renders %ux%u (%u confirmed frames).",c.last_w,c.last_h,c.streak);
            }
        }
        g_executing=false;
    }
}
void sink_detour(void* manager) {
    process(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    g_sink(manager); // newly changed cvars participate in this ordinary sink pass
}
bool image_readable(const Image& image) {
    for(const auto& s:image.sections) {
        size_t at=0;
        while(at<s.size) {
            MEMORY_BASIC_INFORMATION m{};
            if(!VirtualQuery(s.base+at,&m,sizeof(m)) || m.AllocationBase!=image.base ||
               m.State!=MEM_COMMIT || (m.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
            const auto end=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
            const auto current=reinterpret_cast<uintptr_t>(s.base)+at;
            if(end<=current)return false;
            at+=std::min(size_t(end-current),s.size-at);
        }
    }
    return true;
}
bool bind_variables(const Image& image,const locator::Result& found) {
    void* objects[2]{};
    for(unsigned k=0;k<2;++k) {
        const auto* reg=image.base+found.variables[k];
        if(!region_ok(reg,24,false))return false;
        const auto* words=reinterpret_cast<const uintptr_t*>(reg);
        if(words[0]!=reinterpret_cast<uintptr_t>(image.base)+found.registration_tables[k])return false;
        auto* object=reinterpret_cast<void*>(words[1]);
        if(!region_ok(object,0x58,false) ||
           *static_cast<const uintptr_t*>(object)!=reinterpret_cast<uintptr_t>(image.base)+found.tables[k] ||
           words[2]!=reinterpret_cast<uintptr_t>(object)+0x50)return false;
        objects[k]=object;
    }
    g_float=objects[0];g_int=objects[1];return values().valid;
}
DWORD WINAPI worker(LPVOID) {
    Sleep(5000);
    Image image;
    locator::Result found;
    bool valid=load_image(image) && image_readable(image);
    if(valid)found=locator::locate(locator::pe::mapped_image(image.base,image.size,reinterpret_cast<uintptr_t>(image.base)));
    valid=valid && bool(found);
    const char* why=found.reason;
    if(valid) {
        // Registration can finish later than module load. Resolve once, then
        // retry the cheap object binding without calling any engine code.
        valid=false;
        for(unsigned attempt=0;attempt<30;++attempt) {
            if(bind_variables(image,found)){valid=true;break;}Sleep(100);
        }
        if(!valid)why="Unreal console objects are not initialized with the supported layout.";
    }
    if(valid) {
        g_float_set=reinterpret_cast<Setter>(const_cast<uint8_t*>(image.base)+found.setters[0]);
        g_int_set=reinterpret_cast<Setter>(const_cast<uint8_t*>(image.base)+found.setters[1]);
        g_caller=reinterpret_cast<uintptr_t>(image.base)+found.caller;
        if(na::hooking::BeginInstall()) {
            valid=na::hooking::PrepareHook(const_cast<uint8_t*>(image.base)+found.sink,reinterpret_cast<void*>(&sink_detour),reinterpret_cast<void**>(&g_sink));
            if(valid)valid=na::hooking::CommitInstall();else na::hooking::AbandonInstall();
        } else valid=false;
        if(!valid)why="Unreal console interface found, but its game-thread hook could not attach.";
    }
    std::lock_guard<std::mutex> lock(g_m);g_status.available=valid;g_status.candidates=found?1:0;
    g_status.state=valid?SpState::Ready:SpState::NotFound;
    strcpy_s(g_status.reason,valid?"Unreal engine resolution control is ready.":why);
    return 0;
}
} // namespace
uint32_t ExpectedExtent(uint32_t out,float percent) {
    if(!sane_percent(percent)||out==0xffffffffu)return 0;
    const double value=std::ceil(double(out)*double(percent)/100.-1e-6);
    return value<=UINT32_MAX?uint32_t(value):0;
}
void StartScreenPercentage() {if(g_started.exchange(true))return;if(HANDLE t=CreateThread(nullptr,0,worker,nullptr,0,nullptr))CloseHandle(t);}
bool RequestScreenPercentage(float percent,uint32_t ow,uint32_t oh) {
    std::lock_guard<std::mutex> lock(g_m);
    if(!g_status.available || !sane_percent(percent)||!ow||!oh||!ExpectedExtent(ow,percent)||!ExpectedExtent(oh,percent)||g_executing||g_pending.set||g_status.state==SpState::Applying||g_handback)return false;
    g_pending={true,percent,ow,oh,++g_generation};g_confirm=0;g_status.state=SpState::Applying;
    strcpy_s(g_status.reason,"Queued for the engine's game-thread settings update.");return true;
}
SpStatus GetScreenPercentageStatus(){std::lock_guard<std::mutex> lock(g_m);return g_status;}
void CancelScreenPercentage() {
    std::lock_guard<std::mutex> lock(g_m);++g_generation;g_pending={};
    if(g_confirm && na::ngx::GetSizeConfirmation().id==g_confirm)na::ngx::CancelSizeConfirmation();g_confirm=0;
    if(g_status.state==SpState::Applying)g_status.state=SpState::Cancelled;
}
bool HandBackScreenPercentage() {
    CancelScreenPercentage();std::lock_guard<std::mutex> lock(g_m);g_handback=g_owner.held;return g_handback;
}
#ifdef NA_UNREAL_APPLY_TEST
bool BindForTest(const uint8_t* base,const locator::Result& result){Image image;image.base=base;return bind_variables(image,result);}
void ConfigureForTest(void* f,void* i,Setter fs,Setter is) {
    std::lock_guard<std::mutex> lock(g_m);g_float=f;g_int=i;g_float_set=fs;g_int_set=is;g_caller=0xabc;
    g_status={};g_status.available=true;g_pending={};g_owner={};g_confirm=0;g_executing=g_handback=false;
}
void ProcessForTest(uintptr_t caller){process(caller);}
#endif
} // namespace na::unreal
