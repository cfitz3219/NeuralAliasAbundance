#include "host_dx12/capture/capture_session.hpp"
#include "core/capture_retention.hpp"
#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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

struct Limits { uint32_t trigger_builds = 10, keep_builds = 5; uint64_t min_free_bytes = 20ull << 30; };
Limits retention_limits()
{
    wchar_t ini[MAX_PATH];
    { std::lock_guard<std::mutex> lock(R().m); wcscpy_s(ini, R().ini); }
    Limits l;
    if (!ini[0]) return l;
    l.trigger_builds = GetPrivateProfileIntW(L"capture", L"builds_trigger", 10, ini);
    l.keep_builds = GetPrivateProfileIntW(L"capture", L"builds_keep", 5, ini);
    if (l.keep_builds < 1) l.keep_builds = 1;
    if (l.trigger_builds && l.trigger_builds <= l.keep_builds) l.trigger_builds = l.keep_builds + 1;
    l.min_free_bytes = static_cast<uint64_t>(GetPrivateProfileIntW(L"capture", L"min_free_gb", 20, ini)) << 30;
    return l;
}

// This build's commit ([host] build_commit, written by the installer); "" when the INI does not say.
void this_build(char* out, size_t n)
{
    out[0] = 0;
    wchar_t ini[MAX_PATH], build[64] = {};
    { std::lock_guard<std::mutex> lock(R().m); wcscpy_s(ini, R().ini); }
    if (ini[0]) GetPrivateProfileStringW(L"host", L"build_commit", L"", build, 64, ini);
    size_t o = 0;
    for (const wchar_t* c = build; *c && o + 1 < n; ++c)
        if ((*c >= L'0' && *c <= L'9') || (*c >= L'a' && *c <= L'f') || (*c >= L'A' && *c <= L'F')) out[o++] = static_cast<char>(*c);
    out[o] = 0;
}

// The build recorded in <dir>\\session.json; "" when the folder has none (made before 2026-11-17).
std::string folder_build(const std::wstring& dir)
{
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (dir + L"\\session.json").c_str(), L"rb") != 0 || !fp) return {};
    char text[4096];
    const size_t n = std::fread(text, 1, sizeof text - 1, fp);
    std::fclose(fp);
    text[n] = 0;
    const char* key = std::strstr(text, "\"build_commit\"");
    if (!key) return {};
    const char* q = std::strchr(key + 14, '"');
    if (!q) return {};
    const char* e = std::strchr(q + 1, '"');
    return e ? std::string(q + 1, e) : std::string();
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
void retention_pass(std::wstring base, Limits limits, uint64_t process_started)
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    struct Folder { std::wstring name; std::string build; };
    std::vector<std::string> builds{std::string()}; // number 0 = not recorded
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
            const std::string build = folder_build(base + L"\\" + f.cFileName);
            size_t b = 0;
            while (b < builds.size() && builds[b] != build) ++b;
            if (b == builds.size()) builds.push_back(build);
            e.build = static_cast<uint32_t>(b);
            folders.push_back({f.cFileName, build});
            entries.push_back(e);
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    const std::unique_ptr<bool[]> remove(new bool[entries.size() + 1]);
    const retention::Plan plan = retention::Decide(entries.data(), entries.size(), limits.trigger_builds, limits.keep_builds, remove.get());
    uint64_t removed = 0, removed_bytes = 0, errors = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!remove[i]) continue;
        char name[300];
        json_escape(folders[i].name.c_str(), name, sizeof name);
        if (remove_tree(base + L"\\" + folders[i].name)) {
            ++removed;
            removed_bytes += entries[i].bytes;
            retention_log("removed folder=%s build=%s mb=%llu why=older_than_the_newest_%u_builds", name,
                          folders[i].build.empty() ? "not_recorded" : folders[i].build.c_str(),
                          static_cast<unsigned long long>(entries[i].bytes >> 20), limits.keep_builds);
        } else {
            ++errors;
            retention_log("not_removed folder=%s build=%s mb=%llu why=delete_failed error=%lu", name,
                          folders[i].build.empty() ? "not_recorded" : folders[i].build.c_str(),
                          static_cast<unsigned long long>(entries[i].bytes >> 20), GetLastError());
        }
    }
    Retention& r = R();
    r.removed_folders.fetch_add(removed);
    r.removed_bytes.fetch_add(removed_bytes);
    r.remove_errors.fetch_add(errors);
    r.last_total.store(plan.total_before - removed_bytes);
    r.passes.fetch_add(1);
    retention_log("pass folders=%zu builds=%u total_mb=%llu trigger_builds=%u keep_builds=%u removed_builds=%u removed=%llu removed_mb=%llu errors=%llu after_mb=%llu why=%s",
                  entries.size(), plan.builds_before, static_cast<unsigned long long>(plan.total_before >> 20),
                  limits.trigger_builds, limits.keep_builds, plan.removed_builds, static_cast<unsigned long long>(removed),
                  static_cast<unsigned long long>(removed_bytes >> 20), static_cast<unsigned long long>(errors),
                  static_cast<unsigned long long>((plan.total_before - removed_bytes) >> 20),
                  !limits.trigger_builds ? "rule_off" : (plan.triggered ? "trigger_reached" : "fewer_builds_than_the_trigger"));
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
    {   // No capture onto a nearly full drive; and captures of old builds make room.
        const Limits limits = retention_limits();
        ULARGE_INTEGER free_bytes{};
        const bool measured = GetDiskFreeSpaceExW(base, &free_bytes, nullptr, nullptr) != 0;
        if (measured) R().last_free.store(free_bytes.QuadPart);
        const bool full = measured && retention::DiskTooFull(free_bytes.QuadPart, limits.min_free_bytes);
        if (limits.trigger_builds && !R().running.exchange(true)) std::thread(retention_pass, std::wstring(base), limits, s.started_utc).detach();
        if (full) {
            R().refused.fetch_add(1);
            char k[64];
            json_escape(kind ? kind : L"capture", k, sizeof k);
            retention_log("capture_refused kind=%s why=drive_nearly_full free_mb=%llu floor_mb=%llu", k,
                          static_cast<unsigned long long>(free_bytes.QuadPart >> 20),
                          static_cast<unsigned long long>(limits.min_free_bytes >> 20));
            return false;
        }
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    swprintf_s(out, out_chars, L"%s\\%s-%04u%02u%02u-%02u%02u%02u-%s-p%u", base, kind ? kind : L"capture", t.wYear, t.wMonth,
               t.wDay, t.wHour, t.wMinute, t.wSecond, s.exe, s.pid);
    CreateDirectoryW(out, nullptr);
    {   // Every capture folder says which build made it: the cleanup counts builds.
        char k[64];
        json_escape(kind ? kind : L"capture", k, sizeof k);
        TagDir(out, k);
    }
    return true;
}

void TagDir(const wchar_t* dir, const char* kind)
{
    if (!dir || !dir[0]) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\session.json", dir);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    const Session& s = ThisProcess();
    char exe[200], full[700], build[48];
    this_build(build, sizeof build);
    json_escape(s.exe, exe, sizeof exe);
    json_escape(s.path, full, sizeof full);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"wb") != 0 || !fp) return;
    std::fprintf(fp, "{\n  \"format\": \"na-capture-session-1\",\n  \"kind\": \"%s\",\n  \"exe\": \"%s\",\n  \"path\": \"%s\",\n"
                     "  \"pid\": %u,\n  \"process_started_utc\": \"%s\",\n  \"process_started_filetime\": %llu,\n  \"build_commit\": \"%s\"\n}\n",
                 kind ? kind : "capture", exe, full, s.pid, s.started_iso, static_cast<unsigned long long>(s.started_utc), build);
    std::fclose(fp);
}
}
