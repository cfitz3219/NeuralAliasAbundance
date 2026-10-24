// C7: shared DLL-load watcher. See module_watch.hpp.
#include "host_dx12/upscalers/module_watch.hpp"

#include <atomic>
#include <cwchar>
#include <mutex>
#include <vector>

namespace na::upscalers {
namespace {

struct UnicodeStr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct LdrLoaded { ULONG Flags; const UnicodeStr* FullDllName; const UnicodeStr* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using LdrNotifyFn = VOID(CALLBACK*)(ULONG reason, const LdrLoaded* data, PVOID ctx);
using LdrRegisterFn = LONG(NTAPI*)(ULONG, LdrNotifyFn, PVOID, PVOID*);
using LdrUnregisterFn = LONG(NTAPI*)(PVOID);
constexpr ULONG kLdrLoaded = 1;

struct Watch { const wchar_t* const* names; InstallFn install; };
struct Pending { HMODULE module; InstallFn install; };

std::mutex g_m; // watches + pending (never held while installing)
std::vector<Watch> g_watches;
std::vector<Pending> g_pending;
PVOID g_cookie = nullptr;
HANDLE g_thread = nullptr, g_event = nullptr;
std::atomic<bool> g_stop{false};

VOID CALLBACK LdrNotify(ULONG reason, const LdrLoaded* data, PVOID)
{
    if (reason != kLdrLoaded || !data || !data->BaseDllName || !data->BaseDllName->Buffer) return;
    const size_t len = data->BaseDllName->Length / sizeof(wchar_t);
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const Watch& w : g_watches)
            if (NameMatches(data->BaseDllName->Buffer, len, w.names)) {
                g_pending.push_back({static_cast<HMODULE>(data->DllBase), w.install});
                queued = true;
            }
    }
    if (queued) SetEvent(g_event);
}

DWORD WINAPI Worker(LPVOID)
{
    while (!g_stop.load(std::memory_order_acquire)) {
        WaitForSingleObject(g_event, 1000);
        std::vector<Pending> work;
        {
            std::lock_guard<std::mutex> lock(g_m);
            work.swap(g_pending);
        }
        for (const Pending& p : work) {
            HMODULE pinned = nullptr; // keep the module mapped while hooked
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(p.module), &pinned))
                p.install(pinned);
        }
    }
    return 0;
}

bool start_locked_free()
{
    if (g_thread) return true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = ntdll ? reinterpret_cast<LdrRegisterFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrRegisterDllNotification"))) : nullptr;
    if (!reg) return false;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_event) return false;
    g_stop.store(false, std::memory_order_release);
    if (reg(0, &LdrNotify, nullptr, &g_cookie) != 0) {
        CloseHandle(g_event);
        g_event = nullptr;
        return false;
    }
    g_thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!g_thread) {
        StopModuleWatches();
        return false;
    }
    return true;
}

std::mutex g_start_m;

} // namespace

bool NameMatches(const wchar_t* name, size_t len, const wchar_t* const* names)
{
    if (!name || !names) return false;
    size_t start = len;
    while (start > 0 && name[start - 1] != L'\\' && name[start - 1] != L'/') --start;
    const wchar_t* base = name + start;
    const size_t n = len - start;
    for (const wchar_t* const* k = names; *k; ++k) {
        const size_t kl = wcslen(*k);
        if (n == kl && _wcsnicmp(base, *k, kl) == 0) return true;
    }
    return false;
}

bool AddModuleWatch(const wchar_t* const* names, InstallFn install)
{
    if (!names || !install) return false;
    std::lock_guard<std::mutex> start_lock(g_start_m);
    if (!start_locked_free()) return false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_watches.push_back({names, install});
        for (const wchar_t* const* k = names; *k; ++k)
            if (HMODULE m = GetModuleHandleW(*k)) g_pending.push_back({m, install});
    }
    SetEvent(g_event);
    return true;
}

void StopModuleWatches()
{
    if (g_cookie) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (auto unreg = ntdll ? reinterpret_cast<LdrUnregisterFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrUnregisterDllNotification"))) : nullptr)
            unreg(g_cookie);
        g_cookie = nullptr;
    }
    if (g_thread) {
        g_stop.store(true, std::memory_order_release);
        SetEvent(g_event);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_event) {
        CloseHandle(g_event);
        g_event = nullptr;
    }
    std::lock_guard<std::mutex> lock(g_m);
    g_watches.clear();
    g_pending.clear();
}

} // namespace na::upscalers
