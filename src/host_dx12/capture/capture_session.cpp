#include "host_dx12/capture/capture_session.hpp"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cwchar>
#include <mutex>

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
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const Session& s = ThisProcess();
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
