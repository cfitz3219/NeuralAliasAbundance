#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <windows.h>
#include <atomic>
#include "host_dx12/fg/preset_profile.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/sl_boundary/sl_boundary.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"
#include "host_dx12/adapters/mode_control.hpp"

namespace na::host {
namespace {
using ExitFn=void(WINAPI*)(UINT);
ExitFn original=nullptr;
std::atomic<bool> started{false},evidence{false},ready{false},save_pending{false},ending{false};
std::atomic<uint32_t> exit_code{0};
std::atomic<bool> clean_exit_hook{false};
HANDLE wake=nullptr,finished=nullptr;
bool request_save(){
    if(ending.load())return false;
    if(ready.load() && !settings::CanPersist())return settings::SaveFromWorker(); // read-only: no disk writes
    save_pending.store(true);settings::SetSavePending();if(wake)SetEvent(wake);return true;
}
void end(uint32_t code){
    exit_code.store(code);ending.store(true);if(wake)SetEvent(wake);
    if(finished)WaitForSingleObject(finished,2000); // teardown only; never a live frame wait
}
void WINAPI Exit(UINT code){end(code);original(code);}
}
void StartSession(){
    if(started.exchange(true))return;
    wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);finished=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    settings::DeferForRenderer(&request_save);
    auto target=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ExitProcess");
    if(target && hooking::BeginInstallWait("session clean exit")){
        if(hooking::PrepareHook(reinterpret_cast<void*>(target),reinterpret_cast<void*>(&Exit),reinterpret_cast<void**>(&original)))clean_exit_hook.store(hooking::CommitInstall());
        else hooking::AbandonInstall();
    }
}
void RendererObserved(){
    if(started.load(std::memory_order_acquire) && !evidence.exchange(true,std::memory_order_acq_rel) && wake)SetEvent(wake);
}
bool SessionReady(){return ready.load(std::memory_order_acquire);}
bool CleanExitObserved(){return clean_exit_hook.load(std::memory_order_acquire);}
void RunSessionWorker(RendererReadyFn activate){
    // Keep the existing initialization worker for renderer election, journals
    // and queued settings. Helpers never receive real renderer evidence.
    while(!ending.load(std::memory_order_acquire)){
        if(evidence.load(std::memory_order_acquire) && !ready.load()){
            settings::BeginRendererSession(clean_exit_hook.load());
            settings::PublishRendererReady(); // recovery has completed before any saved effect
            if(activate)activate();
            ready.store(true,std::memory_order_release);
        }
        if(ready.load()){
            settings::FlushSessionFailure();
            if(save_pending.exchange(false))settings::SaveFromWorker();
        }
        if(wake)WaitForSingleObject(wake,250);else Sleep(250);
    }
    if(ready.load()){
        if(save_pending.exchange(false))settings::SaveFromWorker();
        settings::EndSession(exit_code.load());
    }
    ready.store(false);if(finished)SetEvent(finished);
}
void StopSessionForTests(uint32_t code){end(code);}
void ActivateSavedRequests(const wchar_t* ini){
    const bool safe=settings::SafeStart();auto& st=settings::Get();
    if(settings::CanPersist())na::fg::preset::Initialize();
    slb::SetFgRequest(safe?slb::FgRequest{}:slb::FgRequest{static_cast<slb::FgMode>(st.native_fg_mode.load()),st.native_fg_frames.load()});
    j2::SetPaused(safe);
    if(j2::Configured())observe::SetSamplerMipBias(static_cast<float>(st.j2_mip_bias_x100.load())/100.f);
    if(GetPrivateProfileIntW(L"fsr",L"enabled",1,ini)==1)
        ffx::SetMode(!safe && GetPrivateProfileIntW(L"fsr",L"replace",0,ini)==1?ffx::Mode::ReplaceWithDlss:ffx::Mode::Observe);
    if(GetPrivateProfileIntW(L"xess",L"enabled",1,ini)==1)
        xess::SetMode(!safe && GetPrivateProfileIntW(L"xess",L"replace",0,ini)==1?xess::Mode::ReplaceWithDlss:xess::Mode::Observe);
    if(safe || !st.enabled.load())return;
    if(st.preset.load()>0 || st.rr_preset.load()>0)ngx::RequestReapply();
    const auto known=ngx::GetStats();
    if(!st.follow_game_resolution.load() && known.last_sr_out_w && known.last_sr_out_h &&
       known.last_sr_out_w!=0xffffffffu && known.last_sr_out_h!=0xffffffffu){
        na::mode::Request request;
        if(na::mode::BuildSavedRequest(st.dlss_mode.load(),st.preset.load(),known.last_sr_out_w,known.last_sr_out_h,request) &&
           (request.render_w!=known.last_sr_in_w || request.render_h!=known.last_sr_in_h))na::mode::RequestApply(request);
    }
}
}
