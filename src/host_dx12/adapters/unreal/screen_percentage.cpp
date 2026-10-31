#include "host_dx12/adapters/unreal/screen_percentage.hpp"

#include "host_dx12/ngx_observe/ngx_observe.hpp"

#include <windows.h>
#include <bcrypt.h>
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
        Section s;
        s.base = base + sh[i].VirtualAddress;
        s.size = sh[i].Misc.VirtualSize;
        s.exec = (sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        s.write = (sh[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        img.sections.push_back(s);
    }
    return true;
}

const Section* section_of(const Image& img, const void* p)
{
    auto* b = static_cast<const uint8_t*>(p);
    for (const auto& s : img.sections)
        if (b >= s.base && b < s.base + s.size) return &s;
    return nullptr;
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

std::vector<const uint8_t*> find_wide(const Image& img, const wchar_t* text)
{
    std::vector<const uint8_t*> hits;
    const size_t n = (wcslen(text) + 1) * sizeof(wchar_t);
    for (const auto& s : img.sections) {
        if (s.exec || s.size < n) continue;
        for (size_t i = 0; i + n <= s.size; i += 2)
            if (s.base[i] == static_cast<uint8_t>(text[0]) && memcmp(s.base + i, text, n) == 0) {
                if (i >= 2 && (s.base[i - 2] || s.base[i - 1])) continue; // must start a string
                hits.push_back(s.base + i);
                if (hits.size() >= 4) return hits;
            }
    }
    return hits;
}

struct Lea { const uint8_t* at; const uint8_t* target; unsigned reg; };
bool decode_lea(const uint8_t* p, Lea& out)
{
    if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) return false;
    int32_t disp;
    memcpy(&disp, p + 3, 4);
    out.at = p;
    out.target = p + 7 + disp;
    out.reg = ((p[0] & 4u) << 1) | ((p[2] >> 3) & 7u);
    return true;
}

bool sane_percent(float v) { return std::isfinite(v) && v >= 1.f && v <= 400.f; }

// Returns the validated TConsoleVariableData<float>* (ShadowedValue[0..1]) for
// the cvar whose static initializer references `name`, or null. `count` gets
// the number of distinct validated candidates (only 1 is accepted).
float* locate_float_cvar(const wchar_t* name, uint32_t* count, char* why, size_t why_cap)
{
    *count = 0;
    Image img;
    if (!load_image(img)) { strncpy_s(why, why_cap, "No 64-bit executable image.", _TRUNCATE); return nullptr; }
    const std::vector<const uint8_t*> strings = find_wide(img, name);
    if (strings.empty()) { strncpy_s(why, why_cap, "r.ScreenPercentage is not in this executable (not an Unreal game).", _TRUNCATE); return nullptr; }
    std::vector<Lea> xrefs;
    for (const auto& s : img.sections) {
        if (!s.exec || s.size < 8) continue;
        for (size_t i = 0; i + 7 <= s.size; ++i) {
            Lea l;
            if (!decode_lea(s.base + i, l)) continue;
            for (const uint8_t* str : strings)
                if (l.target == str && xrefs.size() < 16) xrefs.push_back(l);
        }
    }
    float* found = nullptr;
    for (const Lea& x : xrefs) {
        // The static initializer loads the object's own address into RCX right
        // beside the name (Cort's PBZ probe: +7). Earlier/later objects belong to
        // neighbouring cvars (-63, +87 there), so the window stays narrow.
        const Lea* best = nullptr;
        Lea cand[8];
        uint32_t nc = 0;
        for (const uint8_t* p = x.at - 32; p <= x.at + 48 && nc < 8; ++p) {
            if (p < img.base || p + 7 > img.base + img.size || p == x.at) continue;
            Lea c;
            if (!decode_lea(p, c) || c.reg != 1) continue; // RCX = this
            const Section* cs = section_of(img, c.target);
            if (!cs || !cs->write) continue;
            cand[nc++] = c;
        }
        for (uint32_t i = 0; i < nc; ++i) {
            const uint8_t* obj = cand[i].target;
            if (!region_ok(obj, 24, false)) continue;
            uint64_t q[3];
            memcpy(q, obj, sizeof(q));
            if (!section_of(img, reinterpret_cast<const void*>(q[0]))) continue; // vtable in the image
            auto* target = reinterpret_cast<const uint8_t*>(q[1]);
            if (!region_ok(target, 8, false)) continue;
            uint64_t tv = 0;
            memcpy(&tv, target, 8);
            if (!section_of(img, reinterpret_cast<const void*>(tv))) continue;   // IConsoleVariable has an image vtable
            auto* ref = reinterpret_cast<float*>(q[2]);
            if ((q[2] & 3u) != 0 || !region_ok(ref, 8, true)) continue;
            float v[2];
            memcpy(v, ref, sizeof(v));
            if (!sane_percent(v[0]) || !sane_percent(v[1])) continue;
            if (!best || std::abs(cand[i].at - x.at) < std::abs(best->at - x.at)) best = &cand[i];
        }
        if (!best) continue;
        float* ref = nullptr;
        {
            uint64_t q2 = 0;
            memcpy(&q2, best->target + 16, 8);
            ref = reinterpret_cast<float*>(q2);
        }
        if (found && found != ref) {
            *count = 2;
            strncpy_s(why, why_cap, "r.ScreenPercentage matched more than one storage location; not changing anything.", _TRUNCATE);
            return nullptr;
        }
        found = ref;
        *count = 1;
    }
    if (!found) strncpy_s(why, why_cap, "r.ScreenPercentage's storage could not be validated.", _TRUNCATE);
    return found;
}


// PBZ shipping-console ABI, qualified from the installed executable. The
// generic locator remains read-only: a string/storage match is not an ABI.
constexpr char kPbzHash[]="7b4fcd286a350780faf0824d73aa150b21cbf1bccde883e6a96386cbe09973b4";
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
bool hash_file(const wchar_t* path,char (&out)[65]) {
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    BCRYPT_ALG_HANDLE a=nullptr;BCRYPT_HASH_HANDLE h=nullptr;
    bool ok=BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok)ok=BCryptCreateHash(a,&h,nullptr,0,nullptr,0,0)>=0;
    std::array<uint8_t,65536> b{};DWORD n=0;
    while(ok) {if(!ReadFile(f,b.data(),DWORD(b.size()),&n,nullptr)){ok=false;break;}if(!n)break;ok=BCryptHashData(h,b.data(),n,0)>=0;}
    uint8_t digest[32]{};if(ok)ok=BCryptFinishHash(h,digest,32,0)>=0;
    if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);CloseHandle(f);
    if(ok)for(unsigned i=0;i<32;++i)sprintf_s(out+i*2,65-i*2,"%02x",digest[i]);return ok;
}
DWORD WINAPI worker(LPVOID) {
    Sleep(5000);
    wchar_t path[32768]{};char hash[65]{};
    bool valid=GetModuleFileNameW(nullptr,path,32768)&&hash_file(path,hash)&&!std::strcmp(hash,kPbzHash);
    char why[160]="This Unreal build's engine setting interface is not qualified yet.";
    uint32_t count=0;
    if(valid) {
        auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto ref=locate_float_cvar(L"r.ScreenPercentage",&count,why,sizeof(why));
        if(ref && region_ok(reinterpret_cast<void*>(base+0xdf4a9f8),24,false)&&region_ok(reinterpret_cast<void*>(base+0xdf74238),24,false)) {
            g_float=*reinterpret_cast<void**>(base+0xdf4a9f8+8);g_int=*reinterpret_cast<void**>(base+0xdf74238+8);
            valid=region_ok(g_float,0x58,false)&&region_ok(g_int,0x58,false)&&
                *reinterpret_cast<uintptr_t*>(g_float)==base+0xa685528&&*reinterpret_cast<uintptr_t*>(g_int)==base+0xa6852e8&&
                reinterpret_cast<uintptr_t>(ref)==reinterpret_cast<uintptr_t>(g_float)+0x50;
            if(valid) {
                g_float_set=reinterpret_cast<Setter>(base+0x12c19d0);g_int_set=reinterpret_cast<Setter>(base+0x12c1950);
                g_caller=base+0x47ad724;
                constexpr uint8_t caller[]={0xff,0x50,0x58};
                constexpr uint8_t entry[]={0x40,0x56,0x48,0x83,0xec,0x20};
                valid=!std::memcmp(reinterpret_cast<void*>(g_caller-3),caller,3)&&!std::memcmp(reinterpret_cast<void*>(base+0x12aa200),entry,sizeof(entry));
                if(valid && na::hooking::BeginInstall()) {
                    valid=na::hooking::PrepareHook(reinterpret_cast<void*>(base+0x12aa200),reinterpret_cast<void*>(&sink_detour),reinterpret_cast<void**>(&g_sink));
                    if(valid)valid=na::hooking::CommitInstall();else na::hooking::AbandonInstall();
                } else valid=false;
            }
        } else valid=false;
    }
    std::lock_guard<std::mutex> lock(g_m);g_status.available=valid;g_status.candidates=count;
    g_status.state=valid?SpState::Ready:SpState::NotFound;
    strcpy_s(g_status.reason,valid?"PBZ engine resolution control is ready.":why);
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
void ConfigureForTest(void* f,void* i,Setter fs,Setter is) {
    std::lock_guard<std::mutex> lock(g_m);g_float=f;g_int=i;g_float_set=fs;g_int_set=is;g_caller=0xabc;
    g_status={};g_status.available=true;g_pending={};g_owner={};g_confirm=0;g_executing=g_handback=false;
}
void ProcessForTest(uintptr_t caller){process(caller);}
#endif
} // namespace na::unreal
