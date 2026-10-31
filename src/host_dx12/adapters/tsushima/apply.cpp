#include "host_dx12/adapters/tsushima/apply.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/settings.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <mutex>
#include <cstdio>

namespace na::tsushima {
namespace {
constexpr uint32_t kDispatch=0x422760, kRequest=0x421c00, kCaller=0x42596f;
constexpr char kHash[]="ed0931b6f6d3ca3874baf39ebc16d95ff0e93296891182f96c3a790da7109f63";
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
bool hash_file(const wchar_t* path, char (&hex)[65]) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 65536> bytes{};
    DWORD n = 0;
    while (ok) {
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr)) { ok = false; break; }
        if (!n) break;
        ok = BCryptHashData(hash, bytes.data(), n, 0) >= 0;
    }
    std::array<unsigned char, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (ok) for (size_t i = 0; i < digest.size(); ++i) sprintf_s(hex + i * 2, 65 - i * 2, "%02x", digest[i]);
    return ok;
}
bool code_matches(uintptr_t base, uint32_t rva, const unsigned char* bytes, size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    auto* p = reinterpret_cast<const void*>(base + rva);
    if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
        !(mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        (mi.Protect & PAGE_GUARD) || reinterpret_cast<uintptr_t>(mi.AllocationBase) != base) return false;
    if (base + rva + n > reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize) return false;
    return std::memcmp(p, bytes, n) == 0;
}
} // namespace
void InitializeApply() {
    std::call_once(g_init,[]{
        wchar_t path[32768]{};
        DWORD n=GetModuleFileNameW(nullptr,path,32768);
        if(!n || n==32768)return;
        const wchar_t* name=wcsrchr(path,L'\\');name=name?name+1:path;
        if(_wcsicmp(name,L"GhostOfTsushima.exe"))return;
        char hash[65]{};
        if(!hash_file(path,hash) || std::strcmp(hash,kHash))return;
        const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        constexpr unsigned char entry[]={0x40,0x53,0x48,0x81,0xec,0xa0,0x01,0x00,0x00,0x48,0x8b,0x01};
        constexpr unsigned char caller[]={0xff,0x90,0x30,0x01,0x00,0x00};
        if(!code_matches(base,kDispatch,entry,sizeof(entry)) ||
           !code_matches(base,kCaller-sizeof(caller),caller,sizeof(caller)))return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if(!na::hooking::BeginInstall())return;
        g_request=reinterpret_cast<Request>(base+kRequest);g_caller=base+kCaller;
        if(!na::hooking::PrepareHook(reinterpret_cast<void*>(base+kDispatch),reinterpret_cast<void*>(&detour),reinterpret_cast<void**>(&g_dispatch))){
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

