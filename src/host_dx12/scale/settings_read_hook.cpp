#include "host_dx12/scale/settings_read_hook.hpp"
#include "host_dx12/scale/render_scale_patch.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>

namespace na::scale {
namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

CreateFileWFn o_create_w = nullptr;
CreateFileAFn o_create_a = nullptr;
ReadFileFn o_read = nullptr;
WriteFileFn o_write = nullptr;

constexpr uint64_t kMaxFileBytes = 1u << 20;

struct Tracked {
    std::wstring path;
    bool read = false, write = false;
};
struct Patched { // per file: what we changed, to put back in the game's own writes
    std::string original, ours; // value text, same length
    std::string key;
};

std::mutex g_m;
double g_fraction = 0;
std::wstring g_log;
std::map<HANDLE, Tracked> g_handles;
std::map<std::wstring, Patched> g_patched; // lower-cased path
ReadHookStats g_stats;
bool g_installed = false;
thread_local int t_inside = 0; // our own file work (the log) is not observed
// Lock-free fast path: ReadFile/WriteFile run for every file the game touches, so untracked
// handles return after scanning these few slots, without the mutex. Stale slots are harmless
// (still_same() re-checks the file) and are overwritten round-robin.
constexpr int kSlots = 16;
std::atomic<HANDLE> g_slot[kSlots];
std::atomic<uint32_t> g_slot_next{0};
bool tracked_fast(HANDLE h)
{
    for (auto& x : g_slot) if (x.load(std::memory_order_acquire) == h) return true;
    return false;
}
void track_slot(HANDLE h)
{
    if (tracked_fast(h)) return;
    g_slot[g_slot_next.fetch_add(1) % kSlots].store(h, std::memory_order_release);
}

std::wstring lower(std::wstring s)
{
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}
bool candidate(const std::wstring& p)
{
    const size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos || p.find_last_of(L"\\/") > dot) return false;
    static const wchar_t* const exts[] = {L".ini", L".cfg", L".json", L".xml", L".txt", L".yaml", L".yml", L".conf", L".config"};
    const std::wstring e = lower(p.substr(dot));
    for (const wchar_t* x : exts) if (e == x) return true;
    return false;
}
std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
void log_line(const char* text)
{
    if (g_log.empty() || !o_create_w || !o_write) return;
    ++t_inside;
    const HANDLE f = o_create_w(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[700];
        const int n = std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u render_scale: %s\r\n", st.wYear, st.wMonth, st.wDay,
                                    st.wHour, st.wMinute, st.wSecond, text);
        DWORD w = 0;
        if (n > 0) o_write(f, buf, static_cast<DWORD>((std::min)(n, static_cast<int>(sizeof buf) - 1)), &w, nullptr);
        CloseHandle(f);
    }
    --t_inside;
}

void on_open(HANDLE h, std::wstring path, DWORD access)
{
    if (h == INVALID_HANDLE_VALUE || t_inside || !candidate(path)) return;
    const bool rd = (access & (GENERIC_READ | FILE_READ_DATA | GENERIC_ALL)) != 0;
    const bool wr = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_ALL)) != 0;
    std::lock_guard<std::mutex> lk(g_m);
    if (!(g_fraction > 0)) return;
    if (rd && !wr) {
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || static_cast<uint64_t>(sz.QuadPart) > kMaxFileBytes) return;
        ++g_stats.files_opened;
    }
    if (g_handles.size() > 256) g_handles.clear(); // bounded: stale entries only
    g_handles[h] = Tracked{lower(std::move(path)), rd, wr};
    track_slot(h);
}

HANDLE WINAPI create_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    const HANDLE h = o_create_w(name, access, share, sa, disp, flags, tmpl);
    if (name && !t_inside) on_open(h, name, access);
    return h;
}
HANDLE WINAPI create_a(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    ++t_inside; // CreateFileA may forward to CreateFileW: observe once, here
    const HANDLE h = o_create_a(name, access, share, sa, disp, flags, tmpl);
    --t_inside;
    if (name && !t_inside) {
        wchar_t w[MAX_PATH * 2] = {};
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH * 2) > 0) on_open(h, w, access);
    }
    return h;
}

// The tracked entry still names the same file (handle values are reused after CloseHandle).
bool still_same(HANDLE h, const std::wstring& path)
{
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetFinalPathNameByHandleW(h, buf, MAX_PATH * 2, FILE_NAME_NORMALIZED);
    if (!n || n >= MAX_PATH * 2) return false;
    std::wstring fin = lower(buf);
    if (fin.rfind(L"\\\\?\\", 0) == 0) fin = fin.substr(4);
    const size_t a = fin.find_last_of(L"\\/"), b = path.find_last_of(L"\\/");
    return fin.substr(a == std::wstring::npos ? 0 : a + 1) == path.substr(b == std::wstring::npos ? 0 : b + 1);
}

BOOL WINAPI read_file(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    const BOOL ok = o_read(h, buf, n, got, ov);
    if (!ok || !buf || t_inside || ov || !got || !*got || !tracked_fast(h)) return ok;
    std::lock_guard<std::mutex> lk(g_m);
    if (!(g_fraction > 0) || g_handles.empty()) return ok;
    const auto it = g_handles.find(h);
    if (it == g_handles.end() || !it->second.read) return ok;
    if (!still_same(h, it->second.path)) { g_handles.erase(it); return ok; }
    char* d = static_cast<char*>(buf);
    Match m;
    if (!FindRenderScale(d, *got, &m)) return ok;
    const std::string original(d + m.value_pos, m.value_len);
    const int k = PatchRenderScale(d, *got, g_fraction, &m);
    char line[600];
    if (k > 0) {
        ++g_stats.reads_patched;
        g_stats.original = m.current;
        std::snprintf(g_stats.key, sizeof g_stats.key, "%s", m.key);
        const std::string file = utf8(it->second.path);
        std::snprintf(g_stats.file, sizeof g_stats.file, "%s", file.c_str());
        Patched& p = g_patched[it->second.path];
        p.original = original;
        p.ours.assign(d + m.value_pos, m.value_len);
        p.key = m.key;
        std::snprintf(g_stats.note, sizeof g_stats.note, "%s %s -> %s (%d value%s)", m.key, original.c_str(), p.ours.c_str(), k, k == 1 ? "" : "s");
        std::snprintf(line, sizeof line, "patched read of %s: %s", file.c_str(), g_stats.note);
    } else {
        ++g_stats.reads_unpatched;
        std::snprintf(g_stats.note, sizeof g_stats.note, "%s = %s found, but %.4f does not fit its width", m.key, original.c_str(), g_fraction);
        std::snprintf(line, sizeof line, "not patched (%s): %s", utf8(it->second.path).c_str(), g_stats.note);
    }
    log_line(line);
    return ok;
}

BOOL WINAPI write_file(HANDLE h, LPCVOID buf, DWORD n, LPDWORD done, LPOVERLAPPED ov)
{
    if (!buf || !n || t_inside || !tracked_fast(h)) return o_write(h, buf, n, done, ov);
    std::string fixed;
    {
        std::lock_guard<std::mutex> lk(g_m);
        const auto it = g_handles.find(h);
        if (it != g_handles.end() && it->second.write) {
            const auto p = g_patched.find(it->second.path);
            if (p != g_patched.end()) {
                // Put the game's original value back where it writes exactly our value.
                fixed.assign(static_cast<const char*>(buf), n);
                Match m;
                bool changed = false;
                for (size_t from = 0; FindRenderScale(fixed.data(), fixed.size(), &m, from); from = m.value_pos + m.value_len) {
                    if (m.value_len == p->second.ours.size() && fixed.compare(m.value_pos, m.value_len, p->second.ours) == 0) {
                        fixed.replace(m.value_pos, m.value_len, p->second.original);
                        changed = true;
                    }
                }
                if (changed) {
                    ++g_stats.writes_restored;
                    char line[400];
                    std::snprintf(line, sizeof line, "game saved %s: kept the original %s = %s", utf8(it->second.path).c_str(),
                                  p->second.key.c_str(), p->second.original.c_str());
                    log_line(line);
                } else {
                    fixed.clear();
                }
            }
        }
    }
    if (!fixed.empty()) return o_write(h, fixed.data(), static_cast<DWORD>(fixed.size()), done, ov);
    return o_write(h, buf, n, done, ov);
}

} // namespace

void Configure(double fraction, const wchar_t* log_path)
{
    std::lock_guard<std::mutex> lk(g_m);
    g_fraction = fraction > 0 && fraction <= 2.0 ? fraction : 0;
    g_log = log_path ? log_path : L"";
}
double ConfiguredFraction()
{
    std::lock_guard<std::mutex> lk(g_m);
    return g_fraction;
}
double FractionForMode(int mode)
{
    switch (mode) {
    case 1: return 1.0 / 1.5;
    case 2: return 1.0 / 1.72;
    case 3: return 0.5;
    case 4: return 1.0 / 3.0;
    default: return 1.0;
    }
}

bool InstallSettingsReadHook()
{
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (g_installed) return true;
    }
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (!kb) kb = GetModuleHandleW(L"kernel32.dll");
    if (!kb) return false;
    void* cw = reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileW"));
    void* ca = reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileA"));
    void* rf = reinterpret_cast<void*>(GetProcAddress(kb, "ReadFile"));
    void* wf = reinterpret_cast<void*>(GetProcAddress(kb, "WriteFile"));
    if (!cw || !ca || !rf || !wf || !na::hooking::Initialize()) return false;
    if (!na::hooking::BeginInstallWait("render-scale read hook")) { na::hooking::Shutdown(); return false; }
    // Originals first (publish-before-enable); the detours never run before they are set.
    if (!na::hooking::PrepareHook(cw, reinterpret_cast<void*>(&create_w), reinterpret_cast<void**>(&o_create_w)) ||
        !na::hooking::PrepareHook(ca, reinterpret_cast<void*>(&create_a), reinterpret_cast<void**>(&o_create_a)) ||
        !na::hooking::PrepareHook(rf, reinterpret_cast<void*>(&read_file), reinterpret_cast<void**>(&o_read)) ||
        !na::hooking::PrepareHook(wf, reinterpret_cast<void*>(&write_file), reinterpret_cast<void**>(&o_write))) {
        na::hooking::AbandonInstall();
        na::hooking::Shutdown();
        return false;
    }
    if (!na::hooking::CommitInstall()) { na::hooking::Shutdown(); return false; }
    std::lock_guard<std::mutex> lk(g_m);
    g_installed = true; // hooks persist for the process lifetime (vtable_hook.hpp threading rules)
    return true;
}

ReadHookStats GetReadHookStats()
{
    std::lock_guard<std::mutex> lk(g_m);
    return g_stats;
}

} // namespace na::scale
