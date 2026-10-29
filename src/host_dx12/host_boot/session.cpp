#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <windows.h>
namespace na::host {
namespace {
using ExitFn=void(WINAPI*)(UINT);
ExitFn original=nullptr;
void WINAPI Exit(UINT code) { settings::EndSession(code);original(code); }
}
void StartSession() {
    // Called on initialization workers, never under the loader lock. Forced
    // termination and crashes do not take this clean ExitProcess(0) path.
    settings::BeginSession();
    if(!original) {
        auto target=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"ExitProcess");
        if(target)hooking::HookFunction(reinterpret_cast<void*>(target),reinterpret_cast<void*>(Exit),reinterpret_cast<void**>(&original));
    }
}
}
