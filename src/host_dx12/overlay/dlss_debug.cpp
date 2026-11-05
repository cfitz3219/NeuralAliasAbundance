#include "host_dx12/overlay/dlss_debug.hpp"
#include "host_dx12/capture/capture_session.hpp"
#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/j2/hud_unblend.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")

namespace na::dlss_debug {
namespace {
// DLSS 310.9.16 (NVIDIA/DLSS v310.9.16): the two nvngx_dlss.dll builds we ship or test with.
// Another version hashes to neither and shows as "unknown build" with its hash.
constexpr const char* kDevSha256 = "90F2F70D4D48978E0314900E85EF2E74DC79113C0A4E6B9841A1E50A1BF2B940";
constexpr const char* kReleaseSha256 = "D00B5B0AB29C3CA101B70C3A5ECB3A01B7EF33766A3383A28A18623083E6123E";
constexpr const wchar_t* kNgxKey = L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore";

std::mutex g_m;
Info g_info;
ULONGLONG g_queried_at = 0;
std::atomic<bool> g_hashing{false};
bool g_hashed = false;
wchar_t g_hashed_path[260] = {};
wchar_t g_status_log[260] = {};
wchar_t g_last_dir[260] = {};

// Held hotkey: the dev DLL polls the keyboard at its own evaluate, so the combination stays
// down for a few presents and is released from Tick (menu open or closed).
std::atomic<int> g_held_vk{0};
std::atomic<uint32_t> g_held_presents{0};

bool sha256_file(const wchar_t* path, char out_hex[65])
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::vector<unsigned char> buf(1 << 20);
    DWORD n = 0;
    while (ok && ReadFile(f, buf.data(), DWORD(buf.size()), &n, nullptr) && n) ok = BCryptHashData(hash, buf.data(), n, 0) >= 0;
    unsigned char digest[32];
    if (ok) ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;
    for (int i = 0; i < 32; ++i) std::snprintf(out_hex + i * 2, 3, "%02X", digest[i]);
    return true;
}

void hash_worker(std::wstring path)
{
    char hex[65] = {};
    const bool ok = sha256_file(path.c_str(), hex);
    std::lock_guard<std::mutex> lock(g_m);
    g_hashed = true;
    wcsncpy_s(g_hashed_path, path.c_str(), _TRUNCATE);
    if (ok) {
        std::memcpy(g_info.sha256_16, hex, 16);
        g_info.sha256_16[16] = 0;
        g_info.build = std::strcmp(hex, kDevSha256) == 0 ? 2 : std::strcmp(hex, kReleaseSha256) == 0 ? 1 : 0;
    }
    g_hashing.store(false);
}

void refresh_locked()
{
    HMODULE m = GetModuleHandleW(L"nvngx_dlss.dll");
    wchar_t path[260] = {};
    if (m && GetModuleFileNameW(m, path, 260)) {
        g_info.loaded = true;
        if (wcscmp(path, g_info.path) != 0) { // a different file than last time: hash again
            wcsncpy_s(g_info.path, path, _TRUNCATE);
            g_hashed = false;
            g_info.build = 0;
            g_info.sha256_16[0] = 0;
            g_info.version[0] = 0;
            g_info.bytes = 0;
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) g_info.bytes = (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(path, &handle);
            if (size) {
                std::vector<unsigned char> vi(size);
                VS_FIXEDFILEINFO* ffi = nullptr;
                UINT len = 0;
                if (GetFileVersionInfoW(path, 0, size, vi.data()) && VerQueryValueW(vi.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi)
                    std::snprintf(g_info.version, sizeof g_info.version, "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                                  HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            }
        }
        if (!g_hashed && !g_hashing.exchange(true)) {
            std::thread(hash_worker, std::wstring(path)).detach();
        }
    } else {
        g_info.loaded = false;
    }
    HKEY k = nullptr;
    g_info.indicator_known = false;
    g_info.indicator = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kNgxKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        DWORD v = 0, cb = sizeof v, type = 0;
        if (RegQueryValueExW(k, L"ShowDlssIndicator", nullptr, &type, reinterpret_cast<BYTE*>(&v), &cb) == ERROR_SUCCESS && type == REG_DWORD) {
            g_info.indicator_known = true;
            g_info.indicator = v;
        } else g_info.indicator_known = true; // the key exists, the value does not: off
        RegCloseKey(k);
    }
}

void send_key(WORD vk, bool up)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
}
} // namespace

Info Query()
{
    std::lock_guard<std::mutex> lock(g_m);
    const ULONGLONG now = GetTickCount64();
    if (!g_queried_at || now - g_queried_at > 2000) {
        g_queried_at = now;
        refresh_locked();
    }
    return g_info;
}

bool PressHotkey(int vk)
{
    if (g_held_vk.load() != 0) return false; // one at a time
    g_held_presents.store(0);
    g_held_vk.store(vk);
    send_key(VK_CONTROL, false);
    send_key(VK_MENU, false);
    send_key(WORD(vk), false);
    return true;
}

void Tick()
{
    const int vk = g_held_vk.load(std::memory_order_relaxed);
    if (!vk) return;
    if (g_held_presents.fetch_add(1) + 1 < 4) return;
    send_key(WORD(vk), true);
    send_key(VK_MENU, true);
    send_key(VK_CONTROL, true);
    g_held_vk.store(0);
}

bool SyntheticKeyAllowed(int vk)
{
    const int held = g_held_vk.load(std::memory_order_relaxed);
    if (!held) return false;
    return vk == held || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
}

bool SetIndicator(bool on)
{
    wchar_t args[512];
    if (on) swprintf_s(args, L"add \"HKLM\\%s\" /v ShowDlssIndicator /t REG_DWORD /d 1024 /f", kNgxKey);
    else swprintf_s(args, L"delete \"HKLM\\%s\" /v ShowDlssIndicator /f", kNgxKey);
    const HINSTANCE r = ShellExecuteW(nullptr, L"runas", L"reg.exe", args, nullptr, SW_HIDE);
    std::lock_guard<std::mutex> lock(g_m);
    g_queried_at = 0; // re-read on the next Query
    return reinterpret_cast<INT_PTR>(r) > 32;
}

void SetStatusLogPath(const wchar_t* path)
{
    std::lock_guard<std::mutex> lock(g_m);
    wcsncpy_s(g_status_log, path ? path : L"", _TRUNCATE);
}

const wchar_t* LastRecordDir() { return g_last_dir; }

bool Record(uint32_t frames, wchar_t* dir_out, size_t dir_chars)
{
    wchar_t dir[MAX_PATH];
    if (!na::capture::MakeDir(L"dlssdebug", dir, MAX_PATH)) return false;
    const Info i = Query();
    wchar_t path[MAX_PATH];
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_status_log[0]) {
            swprintf_s(path, L"%s\\status-log.txt", dir);
            CopyFileW(g_status_log, path, FALSE);
        }
        wcsncpy_s(g_last_dir, dir, _TRUNCATE);
    }
    swprintf_s(path, L"%s\\debug-note.txt", dir);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"wb") == 0 && fp) {
        std::fprintf(fp, "dlss_dll loaded=%d path=%ls bytes=%llu version=%s build=%s sha256_16=%s\r\n", i.loaded ? 1 : 0, i.path,
                     static_cast<unsigned long long>(i.bytes), i.version[0] ? i.version : "-",
                     i.build == 2 ? "development" : i.build == 1 ? "release" : "unknown", i.sha256_16[0] ? i.sha256_16 : "-");
        std::fprintf(fp, "dlss_indicator known=%d value=%u\r\n", i.indicator_known ? 1 : 0, i.indicator);
        std::fprintf(fp, "%s\r\n", na::j2::hud_unblend::StatusLine().c_str());
        std::fprintf(fp, "frames_requested=%u (frameN-backbuffer.raw/.bmp = what you saw, NNNNN-*.raw = DLSS's planes)\r\n", frames);
        std::fclose(fp);
    }
    na::capture::NotePresent("dlssdebug");
    na::capture::RequestPresents(frames, dir, 0);
    na::capture::j2::Request(frames, dir, 0);
    if (dir_out && dir_chars) wcsncpy_s(dir_out, dir_chars, dir, _TRUNCATE);
    return true;
}
} // namespace na::dlss_debug
