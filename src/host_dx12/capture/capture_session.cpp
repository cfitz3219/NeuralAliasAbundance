#include "host_dx12/capture/capture_session.hpp"
#include "core/capture_retention.hpp"
#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <cwchar>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace na::capture {
namespace {
void json_escape(const wchar_t* in, char* out, size_t n)
{
    size_t o = 0;
    for (; in && *in && o + 7 < n; ++in) {
        const wchar_t c = *in;
        if (c == L'\\' || c == L'"') { out[o++] = '\\'; out[o++] = static_cast<char>(c); }
        else if (c < 0x20 || c > 0x7e) o += static_cast<size_t>(std::snprintf(out + o, n - o, "\\u%04x", static_cast<unsigned>(c)));
        else out[o++] = static_cast<char>(c);
    }
    out[o < n ? o : n - 1] = 0;
}

// ---- the captures folder's budget ----
struct Retention {
    std::mutex m;                 // ini, log
    wchar_t ini[MAX_PATH] = {};
    RetentionLog log = nullptr;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> passes{0}, removed_folders{0}, removed_bytes{0}, remove_errors{0}, refused{0};
    std::atomic<uint64_t> last_total{0}, last_free{0};
};
Retention& R() { static Retention r; return r; }

void retention_log(const char* fmt, ...)
{
    RetentionLog log;
    { std::lock_guard<std::mutex> lock(R().m); log = R().log; }
    if (!log) return;
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log(line);
}

void retention_limits(uint64_t* keep_bytes, uint64_t* min_free_bytes)
{
    wchar_t ini[MAX_PATH];
    { std::lock_guard<std::mutex> lock(R().m); wcscpy_s(ini, R().ini); }
    const UINT keep = ini[0] ? GetPrivateProfileIntW(L"capture", L"keep_gb", 60, ini) : 60u;
    const UINT floor_gb = ini[0] ? GetPrivateProfileIntW(L"capture", L"min_free_gb", 20, ini) : 20u;
    *keep_bytes = static_cast<uint64_t>(keep) << 30;
    *min_free_bytes = static_cast<uint64_t>(floor_gb) << 30;
}

uint64_t filetime(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// Bytes under `dir`. A link to somewhere else (reparse point) is counted as nothing and never entered.
uint64_t tree_bytes(const std::wstring& dir)
{
    uint64_t total = 0;
    WIN32_FIND_DATAW f{};
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += tree_bytes(dir + L"\\" + f.cFileName);
        else total += (static_cast<uint64_t>(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return total;
}

// Removes `dir` and everything in it. A link is unlinked, never followed. False when anything stayed.
bool remove_tree(const std::wstring& dir)
{
    bool ok = true;
    WIN32_FIND_DATAW f{};
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            const std::wstring path = dir + L"\\" + f.cFileName;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), f.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ok = RemoveDirectoryW(path.c_str()) && ok;
                else ok = remove_tree(path) && ok;
            } else ok = DeleteFileW(path.c_str()) && ok;
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) && ok;
}

// One pass over LocalAppData\NeuralAlias\captures, on its own low-priority thread.
void retention_pass(std::wstring base, uint64_t keep_bytes, uint64_t process_started)
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    struct Folder { std::wstring name; };
    constexpr uint64_t kRecent = 2ull * 3600ull * 10000000ull; // two hours, in FILETIME units
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now = filetime(now_ft);
    std::vector<Folder> folders;
    std::vector<retention::Entry> entries;
    WIN32_FIND_DATAW f{};
    const HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            retention::Entry e;
            e.created = filetime(f.ftCreationTime);
            // This process's folders, and any made in the last two hours (another game still writing, or
            // the run that has only just ended and is about to be read).
            e.current_run = (process_started && e.created >= process_started) || e.created + kRecent >= now;
            e.bytes = tree_bytes(base + L"\\" + f.cFileName);
            folders.push_back({f.cFileName});
            entries.push_back(e);
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    const std::unique_ptr<bool[]> remove(new bool[entries.size() + 1]);
    const retention::Plan plan = retention::Decide(entries.data(), entries.size(), keep_bytes, remove.get());
    uint64_t removed = 0, removed_bytes = 0, errors = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!remove[i]) continue;
        char name[300];
        json_escape(folders[i].name.c_str(), name, sizeof name);
        if (remove_tree(base + L"\\" + folders[i].name)) {
            ++removed;
            removed_bytes += entries[i].bytes;
            retention_log("removed folder=%s mb=%llu why=captures_over_budget_oldest_first", name,
                          static_cast<unsigned long long>(entries[i].bytes >> 20));
        } else {
            ++errors;
            retention_log("not_removed folder=%s mb=%llu why=delete_failed error=%lu", name,
                          static_cast<unsigned long long>(entries[i].bytes >> 20), GetLastError());
        }
    }
    Retention& r = R();
    r.removed_folders.fetch_add(removed);
    r.removed_bytes.fetch_add(removed_bytes);
    r.remove_errors.fetch_add(errors);
    r.last_total.store(plan.total_before - removed_bytes);
    r.passes.fetch_add(1);
    retention_log("pass folders=%zu total_mb=%llu budget_mb=%llu removed=%llu removed_mb=%llu errors=%llu current_run_folders=%u current_run_mb=%llu after_mb=%llu%s",
                  entries.size(), static_cast<unsigned long long>(plan.total_before >> 20),
                  static_cast<unsigned long long>(keep_bytes >> 20), static_cast<unsigned long long>(removed),
                  static_cast<unsigned long long>(removed_bytes >> 20), static_cast<unsigned long long>(errors),
                  plan.protected_count, static_cast<unsigned long long>(plan.protected_bytes >> 20),
                  static_cast<unsigned long long>((plan.total_before - removed_bytes) >> 20),
                  !keep_bytes ? " why=budget_off" : (plan.over_after ? " why=still_over_only_this_run_is_left" : ""));
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    r.running.store(false);
}
}

void SetRetention(const wchar_t* ini, RetentionLog log)
{
    std::lock_guard<std::mutex> lock(R().m);
    if (ini) wcsncpy_s(R().ini, ini, _TRUNCATE);
    R().log = log;
}

RetentionStats GetRetentionStats()
{
    Retention& r = R();
    RetentionStats s;
    s.passes = r.passes.load();
    s.removed_folders = r.removed_folders.load();
    s.removed_bytes = r.removed_bytes.load();
    s.remove_errors = r.remove_errors.load();
    s.refused_disk_full = r.refused.load();
    s.last_total_bytes = r.last_total.load();
    s.last_free_bytes = r.last_free.load();
    return s;
}

const Session& ThisProcess()
{
    static Session s;
    static std::once_flag once;
    std::call_once(once, [] {
        s.pid = GetCurrentProcessId();
        GetModuleFileNameW(nullptr, s.path, static_cast<DWORD>(sizeof s.path / sizeof s.path[0]));
        const wchar_t* name = std::wcsrchr(s.path, L'\\');
        name = name ? name + 1 : s.path;
        size_t i = 0;
        for (; name[i] && name[i] != L'.' && i + 1 < sizeof s.exe / sizeof s.exe[0]; ++i) {
            const wchar_t c = name[i];
            s.exe[i] = (c == L' ' || c == L'-' || c == L'\\' || c == L'/' || c == L':') ? L'_' : c; // folder-name safe
        }
        s.exe[i] = 0;
        if (!s.exe[0]) wcscpy_s(s.exe, L"game");
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            s.started_utc = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            SYSTEMTIME t{};
            FileTimeToSystemTime(&created, &t);
            std::snprintf(s.started_iso, sizeof s.started_iso, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay,
                          t.wHour, t.wMinute, t.wSecond);
        }
    });
    return s;
}

bool MakeDir(const wchar_t* kind, wchar_t* out, size_t out_chars)
{
    if (!out || !out_chars) return false;
    out[0] = 0;
    wchar_t* local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local) return false;
    wchar_t base[MAX_PATH];
    swprintf_s(base, L"%s\\NeuralAlias", local);
    CreateDirectoryW(base, nullptr);
    swprintf_s(base, L"%s\\NeuralAlias\\captures", local);
    CreateDirectoryW(base, nullptr);
    CoTaskMemFree(local);
    const Session& s = ThisProcess();
    {   // The budget: no capture onto a nearly full drive, and the oldest folders make room.
        uint64_t keep_bytes = 0, min_free_bytes = 0;
        retention_limits(&keep_bytes, &min_free_bytes);
        ULARGE_INTEGER free_bytes{};
        if (GetDiskFreeSpaceExW(base, &free_bytes, nullptr, nullptr)) {
            R().last_free.store(free_bytes.QuadPart);
            if (retention::DiskTooFull(free_bytes.QuadPart, min_free_bytes)) {
                R().refused.fetch_add(1);
                char k[64];
                json_escape(kind ? kind : L"capture", k, sizeof k);
                retention_log("capture_refused kind=%s why=drive_nearly_full free_mb=%llu floor_mb=%llu", k,
                              static_cast<unsigned long long>(free_bytes.QuadPart >> 20),
                              static_cast<unsigned long long>(min_free_bytes >> 20));
                // Old folders may be what filled it: make room for the next request.
                if (keep_bytes && !R().running.exchange(true)) std::thread(retention_pass, std::wstring(base), keep_bytes, s.started_utc).detach();
                return false;
            }
        }
        if (keep_bytes && !R().running.exchange(true)) std::thread(retention_pass, std::wstring(base), keep_bytes, s.started_utc).detach();
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    swprintf_s(out, out_chars, L"%s\\%s-%04u%02u%02u-%02u%02u%02u-%s-p%u", base, kind ? kind : L"capture", t.wYear, t.wMonth,
               t.wDay, t.wHour, t.wMinute, t.wSecond, s.exe, s.pid);
    CreateDirectoryW(out, nullptr);
    return true;
}

void TagDir(const wchar_t* dir, const char* kind)
{
    if (!dir || !dir[0]) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\session.json", dir);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    const Session& s = ThisProcess();
    char exe[200], full[700];
    json_escape(s.exe, exe, sizeof exe);
    json_escape(s.path, full, sizeof full);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"wb") != 0 || !fp) return;
    std::fprintf(fp, "{\n  \"format\": \"na-capture-session-1\",\n  \"kind\": \"%s\",\n  \"exe\": \"%s\",\n  \"path\": \"%s\",\n"
                     "  \"pid\": %u,\n  \"process_started_utc\": \"%s\",\n  \"process_started_filetime\": %llu\n}\n",
                 kind ? kind : "capture", exe, full, s.pid, s.started_iso, static_cast<unsigned long long>(s.started_utc));
    std::fclose(fp);
}
}
