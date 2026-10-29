#include "host_dx12/fg/preset_profile.hpp"
#include "host_dx12/settings.hpp"
#include <windows.h>
#include <nvapi.h>
#include <NvApiDriverSettings.h>
#include <mutex>
#include <atomic>
#include <cwchar>
#include <cstdio>

namespace na::fg::preset {
namespace {
using Query = void*(__cdecl*)(unsigned);
decltype(&NvAPI_DRS_CreateSession) create = nullptr;
decltype(&NvAPI_DRS_DestroySession) destroy = nullptr;
decltype(&NvAPI_DRS_LoadSettings) load = nullptr;
decltype(&NvAPI_DRS_SaveSettings) save = nullptr;
decltype(&NvAPI_DRS_FindApplicationByName) find = nullptr;
decltype(&NvAPI_DRS_GetSetting) get = nullptr;
decltype(&NvAPI_DRS_SetSetting) set = nullptr;
decltype(&NvAPI_DRS_DeleteProfileSetting) erase = nullptr;
wchar_t target_exe[MAX_PATH] = {};
wchar_t backup_path[MAX_PATH] = {};
std::mutex mutex;
Status status;
HANDLE wake = nullptr;
bool changed_this_run = false;
std::atomic<uint32_t> desired{1};
constexpr NvU32 ids[] = {NGX_DLSS_FG_OVERRIDE_ID, NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID};

bool load_api() {
    auto module = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    auto query = reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(module, "nvapi_QueryInterface")));
    if (!query) return false;
    create = reinterpret_cast<decltype(create)>(query(0x0694d52e)); destroy = reinterpret_cast<decltype(destroy)>(query(0xdad9cff8));
    load = reinterpret_cast<decltype(load)>(query(0x375dbd6b)); save = reinterpret_cast<decltype(save)>(query(0xfcbc7e14));
    find = reinterpret_cast<decltype(find)>(query(0xeee566b2)); get = reinterpret_cast<decltype(get)>(query(0x73bf8338));
    set = reinterpret_cast<decltype(set)>(query(0x577dd202));
    erase = reinterpret_cast<decltype(erase)>(query(0xe4a26362));
    return create && destroy && load && save && find && get && set && erase;
}

NvAPI_Status apply(uint32_t selection, uint32_t& stored, bool& changed, bool read_only = false, bool restore = false) {
    NvDRSSessionHandle session = nullptr;
    auto result = create(&session);
    if (result != NVAPI_OK) return result;
    NvDRSProfileHandle profile = nullptr;
    NVDRS_APPLICATION app{}; app.version = NVDRS_APPLICATION_VER;
    NvAPI_UnicodeString executable{};
    wcscpy_s(reinterpret_cast<wchar_t*>(executable), NVAPI_UNICODE_STRING_MAX, target_exe);
    result = load(session);
    if (result == NVAPI_OK) result = find(session, executable, &profile, &app);
    if (result == NVAPI_OK && restore) {
        wchar_t saved_exe[MAX_PATH]{};
        GetPrivateProfileStringW(L"backup", L"exe", L"", saved_exe, MAX_PATH, backup_path);
        if (GetPrivateProfileIntW(L"backup", L"complete", 0, backup_path) != 1 ||
            _wcsicmp(saved_exe, target_exe)) { destroy(session); return NVAPI_ERROR; }
        for (unsigned i = 0; i < 2 && result == NVAPI_OK; ++i) {
            wchar_t section[32]; swprintf_s(section, L"setting%u", i);
            if (GetPrivateProfileIntW(section, L"id", 0, backup_path) != ids[i]) { result = NVAPI_ERROR; break; }
            if (GetPrivateProfileIntW(section, L"user_value", 0, backup_path)) {
                NVDRS_SETTING setting{}; setting.version = NVDRS_SETTING_VER;
                setting.settingId = ids[i]; setting.settingType = NVDRS_DWORD_TYPE;
                setting.u32CurrentValue = GetPrivateProfileIntW(section, L"value", 0, backup_path);
                result = set(session, profile, &setting);
            } else {
                result = erase(session, profile, ids[i]);
                if (result == NVAPI_SETTING_NOT_FOUND) result = NVAPI_OK;
            }
        }
        if (result == NVAPI_OK) result = save(session);
        destroy(session); return result;
    }
    // No global fallback or speculative profile creation. This installed game
    // has a NVIDIA application profile; report failure if the driver lacks it.
    if (result == NVAPI_OK) {
        NVDRS_SETTING before[2]{};
        bool change = false;
        for (unsigned i = 0; i < 2; ++i) {
            before[i].version = NVDRS_SETTING_VER;
            result = get(session, profile, ids[i], &before[i]);
            if (result != NVAPI_OK && result != NVAPI_SETTING_NOT_FOUND) break;
            if (result == NVAPI_SETTING_NOT_FOUND) before[i].version = 0;
            const NvU32 value = i ? selection : NGX_DLSS_FG_OVERRIDE_ON;
            change |= !before[i].version || before[i].u32CurrentValue != value;
            result = NVAPI_OK;
        }
        if (result == NVAPI_OK && !read_only) {
            // Preserve the original two values once for a targeted rollback;
            // never export the user's complete driver profile database.
            const wchar_t* backup = backup_path;
            if (!backup[0]) result = NVAPI_ERROR;
            if (result == NVAPI_OK && GetFileAttributesW(backup) == INVALID_FILE_ATTRIBUTES) {
                for (unsigned i = 0; i < 2; ++i) {
                    wchar_t section[32], value[96]; swprintf_s(section, L"setting%u", i);
                    swprintf_s(value, L"%u", ids[i]);
                    bool ok = WritePrivateProfileStringW(section, L"id", value, backup) != 0;
                    swprintf_s(value, L"%u", before[i].version && before[i].settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && !before[i].isCurrentPredefined ? 1u : 0u);
                    ok &= WritePrivateProfileStringW(section, L"user_value", value, backup) != 0;
                    swprintf_s(value, L"%u", before[i].u32CurrentValue);
                    ok &= WritePrivateProfileStringW(section, L"value", value, backup) != 0;
                    if (!ok) result = NVAPI_ERROR;
                }
                if (result == NVAPI_OK && !WritePrivateProfileStringW(L"backup", L"exe", target_exe, backup)) result = NVAPI_ERROR;
                if (result == NVAPI_OK && !WritePrivateProfileStringW(L"backup", L"complete", L"1", backup)) result = NVAPI_ERROR;
            }
            if (result == NVAPI_OK && GetPrivateProfileIntW(L"backup", L"complete", 0, backup) != 1) result = NVAPI_ERROR;
            wchar_t saved_exe[MAX_PATH]{};
            GetPrivateProfileStringW(L"backup", L"exe", L"", saved_exe, MAX_PATH, backup);
            // Old TLOU backups predate the executable field; no new game inherits them.
            if (result == NVAPI_OK && ((saved_exe[0] && _wcsicmp(saved_exe, target_exe)) ||
                (!saved_exe[0] && _wcsicmp(target_exe, L"tlou-i.exe")))) result = NVAPI_ERROR;
            for (unsigned i = 0; i < 2 && result == NVAPI_OK && change; ++i) {
                NVDRS_SETTING setting{}; setting.version = NVDRS_SETTING_VER;
                setting.settingId = ids[i]; setting.settingType = NVDRS_DWORD_TYPE;
                setting.u32CurrentValue = i ? selection : NGX_DLSS_FG_OVERRIDE_ON;
                result = set(session, profile, &setting);
            }
            if (result == NVAPI_OK && change) { result = save(session); changed = result == NVAPI_OK; } // changes remain local to session until this succeeds
        }
        if (result == NVAPI_OK) {
            NVDRS_SETTING verify{}; verify.version = NVDRS_SETTING_VER;
            result = get(session, profile, ids[1], &verify);
            if (result == NVAPI_OK) stored = verify.u32CurrentValue;
        }
    }
    destroy(session);
    return result;
}
void apply_requested() {
    const auto choice = desired.load(); uint32_t stored = 0; bool changed = false;
    const auto result = apply(choice, stored, changed);
    std::lock_guard<std::mutex> lock(mutex);
    status.result = result; status.stored = stored; status.requested = desired.load();
    status.pending = choice != status.requested;
    changed_this_run |= changed;
    status.restart = changed_this_run;
}
DWORD WINAPI worker(void*) {
    for (;;) { if (WaitForSingleObject(wake, INFINITE) != WAIT_OBJECT_0) return 0; apply_requested(); }
}
}
void Initialize() {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    auto name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
    if (_wcsicmp(name, L"tlou-i.exe") && !settings::Get().fg_profile_override.load()) return;
    wcscpy_s(target_exe, name);
    wcsncpy_s(backup_path, settings::UserIniPath(), _TRUNCATE);
    auto slash = wcsrchr(backup_path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - backup_path),
             !_wcsicmp(name, L"tlou-i.exe") ? L"TLOU-FG-profile-before.ini" : L"FG-profile-before.ini");
    if (!load_api()) return;
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!wake) return;
    const auto preset = settings::Get().fg_preset.load();
    desired.store(preset >= 1 && preset <= 3 ? preset : 1u);
    { std::lock_guard<std::mutex> lock(mutex); status.available = true; }
    apply_requested();
    HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else { std::lock_guard<std::mutex> lock(mutex); status.available = false; }
}
void Request(uint32_t choice) {
    if (choice < 1 || choice > 3) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (!status.available) return;
    desired.store(choice); status.requested = choice; status.pending = true;
    SetEvent(wake);
}
Status GetStatus() { std::lock_guard<std::mutex> lock(mutex); return status; }
}

#ifdef NA_PROFILE_CLI
int wmain(int argc, wchar_t** argv) {
    // Read-only status; installer apply/rollback share the runtime's exact DRS path.
    if (argc < 3 || argc > 5) { fwprintf(stderr, L"Usage: NAProfile status|apply|restore exe [backup.ini] [1|2|3]\n"); return 2; }
    using namespace na::fg::preset;
    const bool read = !_wcsicmp(argv[1], L"status");
    const bool restore = !_wcsicmp(argv[1], L"restore");
    if (!read && !restore && _wcsicmp(argv[1], L"apply")) return 2;
    if ((!read && argc < 4) || wcslen(argv[2]) >= MAX_PATH || wcschr(argv[2], L'\\') || wcschr(argv[2], L'/')) return 2;
    wcscpy_s(target_exe, argv[2]);
    if (!read) { if (wcslen(argv[3]) >= MAX_PATH) return 2; wcscpy_s(backup_path, argv[3]); }
    const auto preset = argc == 5 ? static_cast<uint32_t>(_wtoi(argv[4])) : 3u;
    if (preset < 1 || preset > 3 || !load_api()) return 2;
    uint32_t stored = 0; bool changed = false;
    const auto result = apply(preset, stored, changed, read, restore);
    wprintf(L"exe=%s result=%d stored=%u changed=%u\n", target_exe, result, stored, changed ? 1u : 0u);
    return result == NVAPI_OK ? 0 : 1;
}
#endif
