#include "host_dx12/overlay/freeze_watchdog.hpp"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>

namespace na::freeze {

namespace {

// x64 unwinding with the kernel's own tables: no dbghelp, no symbol
// handler, no loader lock. Safe to run while the target thread is suspended.
uint32_t walk_context(CONTEXT& ctx, uint64_t* frames, uint32_t capacity)
{
    uint32_t n = 0;
    while (n < capacity && ctx.Rip) {
        frames[n++] = ctx.Rip;
        DWORD64 base = 0;
        RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!rf) { // leaf function: the return address is at RSP
            if (!ctx.Rsp) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &handler_data, &establisher, nullptr);
    }
    return n;
}

} // namespace

uint32_t CaptureAllThreads(Capture* out)
{
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            ++out->threads_total;
            if (out->thread_count >= kMaxThreads) continue;
            ThreadStack& t = out->threads[out->thread_count];
            t.thread_id = te.th32ThreadID;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) { ++out->thread_count; continue; }
            if (SuspendThread(h) != static_cast<DWORD>(-1)) {
                t.suspended = 1;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(h, &ctx)) {
                    __try {
                        t.frame_count = walk_context(ctx, t.frames, kMaxFrames);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        // a torn stack: keep what was read
                    }
                }
                ResumeThread(h);
            }
            CloseHandle(h);
            ++out->thread_count;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return out->thread_count;
}

uint32_t FormatStack(const ThreadStack& t, char* out, uint32_t capacity)
{
    if (!out || capacity < 32) return 0;
    int n = snprintf(out, capacity, "tid=%u%s:", t.thread_id, t.suspended ? "" : " (not suspended)");
    if (n < 0) return 0;
    uint32_t pos = static_cast<uint32_t>(n);
    HMODULE last = nullptr;
    char last_name[40] = {};
    for (uint32_t i = 0; i < t.frame_count; ++i) {
        HMODULE m = nullptr;
        char frame[96];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(t.frames[i]), &m) && m) {
            if (m != last) {
                wchar_t path[MAX_PATH] = {};
                GetModuleFileNameW(m, path, MAX_PATH);
                const wchar_t* name = path;
                for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') name = p + 1;
                WideCharToMultiByte(CP_ACP, 0, name, -1, last_name, sizeof(last_name), nullptr, nullptr);
                last_name[sizeof(last_name) - 1] = 0;
                last = m;
            }
            snprintf(frame, sizeof(frame), "%s%s+0x%llX", i ? " > " : " ", last_name,
                     static_cast<unsigned long long>(t.frames[i] - reinterpret_cast<uint64_t>(m)));
        } else {
            snprintf(frame, sizeof(frame), "%s?0x%llX", i ? " > " : " ", static_cast<unsigned long long>(t.frames[i]));
        }
        size_t fl = strlen(frame);
        if (pos + fl + 1 >= capacity) break;
        memcpy(out + pos, frame, fl + 1);
        pos += static_cast<uint32_t>(fl);
    }
    return pos;
}

bool WriteMiniDump(const wchar_t* path, EXCEPTION_POINTERS* exception)
{
    if (!path) return false;
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return false;
    using PfnWrite = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                   PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto write = reinterpret_cast<PfnWrite>(GetProcAddress(dbg, "MiniDumpWriteDump"));
    if (!write) { FreeLibrary(dbg); return false; }
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { FreeLibrary(dbg); return false; }
    auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules |
                                           MiniDumpWithHandleData);
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
    BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
    FreeLibrary(dbg);
    return ok != FALSE;
}

bool Watchdog::Check(uint64_t game_presents, uint64_t now_ms)
{
    if (game_presents != last_presents_) {
        if (last_presents_ != 0 || game_presents != 0) seen_progress_ = true;
        last_presents_ = game_presents;
        last_change_ms_ = now_ms;
        tripped_ = false;
        return false;
    }
    if (!seen_progress_ || tripped_ || !last_change_ms_) return false;
    if (now_ms - last_change_ms_ < stall_ms_) return false;
    tripped_ = true;
    return true;
}

} // namespace na::freeze
