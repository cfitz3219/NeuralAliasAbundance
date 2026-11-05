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

bool initialize_api() {
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
bool load_api() {
    // Menu refresh and the FG profile worker may run concurrently. Publish
    // immutable function pointers once; don't rewrite them on every refresh.
    static std::once_flag once;
    static bool ready=false;
    std::call_once(once,[]{ready=initialize_api();});
    return ready;
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
    if (_wcsicmp(name, L"tlou-i.exe") && !settings::Get().fg_profile_override.load() && !settings::SafeProfileRestoreRequired()) return;
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
    if(settings::SafeStart()) {
        uint32_t stored=0;bool changed=false;
        const auto result=apply(desired.load(),stored,changed,false,true);
        std::lock_guard<std::mutex> lock(mutex);status.result=result;status.stored=stored;status.requested=desired.load();
    } else apply_requested();
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

namespace {
std::mutex upscaler_mutex;
UpscalerOverrides upscaler_overrides;
bool read_dword(NvDRSSessionHandle session, NvDRSProfileHandle profile, NvU32 id, uint32_t& value, bool& inherited) {
    NVDRS_SETTING s{}; s.version = NVDRS_SETTING_VER;
    if (get(session, profile, id, &s) != NVAPI_OK) return false;
    value = s.u32CurrentValue;
    inherited = s.settingLocation == NVDRS_GLOBAL_PROFILE_LOCATION || s.settingLocation == NVDRS_BASE_PROFILE_LOCATION;
    return true;
}
}
void ReadUpscalerOverrides() {
    UpscalerOverrides out{};
    if (!load_api()) { std::lock_guard<std::mutex> lock(upscaler_mutex); upscaler_overrides = out; return; }
    using GetGlobal = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle*);
    auto module = GetModuleHandleW(L"nvapi64.dll");
    auto query = module ? reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(module, "nvapi_QueryInterface"))) : nullptr;
    auto get_global = query ? reinterpret_cast<GetGlobal>(query(0x617bff9f)) : nullptr; // NvAPI_DRS_GetCurrentGlobalProfile
    NvDRSSessionHandle session = nullptr;
    if (create(&session) != NVAPI_OK) {std::lock_guard<std::mutex> lock(upscaler_mutex);upscaler_overrides=out;return;}
    if (load(session) == NVAPI_OK) {
        out.available = true;
        wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
        auto name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
        NvAPI_UnicodeString exe{}; wcscpy_s(reinterpret_cast<wchar_t*>(exe), NVAPI_UNICODE_STRING_MAX, name);
        NvDRSProfileHandle profiles[2] = {};
        NVDRS_APPLICATION app{}; app.version = NVDRS_APPLICATION_VER;
        if (find(session, exe, &profiles[0], &app) != NVAPI_OK) profiles[0] = nullptr;
        if (!get_global || get_global(session, &profiles[1]) != NVAPI_OK) profiles[1] = nullptr;
        // GetSetting on the game profile returns its effective inherited value.
        const int i=profiles[0]?0:1;
        if (profiles[i]) {
            uint32_t v = 0; bool user = false;
            if (read_dword(session, profiles[i], NGX_DLSS_SR_OVERRIDE_ID, v, user)) { out.sr_enabled = v; out.sr_global = i == 1 || user; }
            if (read_dword(session, profiles[i], NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID, v, user)) out.sr_preset = v;
            if (read_dword(session, profiles[i], NGX_DLSS_RR_OVERRIDE_ID, v, user)) { out.rr_enabled = v; out.rr_global = i == 1 || user; }
            if (read_dword(session, profiles[i], NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_ID, v, user)) out.rr_preset = v;
        }
    }
    destroy(session);
    std::lock_guard<std::mutex> lock(upscaler_mutex); upscaler_overrides = out;
}
UpscalerOverrides GetUpscalerOverrides() { std::lock_guard<std::mutex> lock(upscaler_mutex); return upscaler_overrides; }
namespace {
std::atomic<HANDLE> refresh_event{nullptr};
std::atomic<ULONGLONG> last_refresh{0};
std::once_flag refresh_once;
DWORD WINAPI refresh_worker(void*) {
    for (;;) {
        HANDLE e = refresh_event.load();
        if (!e || WaitForSingleObject(e, INFINITE) != WAIT_OBJECT_0) return 0;
        ReadUpscalerOverrides();
        ReadAnisoOverride();
    }
}
}
void RequestUpscalerOverridesRefresh() {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG prev = last_refresh.load();
    if (now - prev < 2000 || !last_refresh.compare_exchange_strong(prev, now)) return;
    std::call_once(refresh_once, [] {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!e) return;
        refresh_event.store(e);
        HANDLE t = CreateThread(nullptr, 0, refresh_worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t); else { refresh_event.store(nullptr); CloseHandle(e); }
    });
    if (HANDLE e = refresh_event.load()) SetEvent(e);
}
}

namespace na::fg::preset {
namespace {
std::mutex aniso_mutex;
AnisoOverride aniso_state;
std::atomic<int> aniso_job{0}; // 1 write APP, 2 restore
std::once_flag aniso_once;
HANDLE aniso_event = nullptr;
bool exe_name(NvAPI_UnicodeString& exe) {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    auto name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
    wcscpy_s(reinterpret_cast<wchar_t*>(exe), NVAPI_UNICODE_STRING_MAX, name);
    return name[0] != 0;
}
bool aniso_backup_path(wchar_t (&out)[MAX_PATH]) {
    wcsncpy_s(out, settings::UserIniPath(), _TRUNCATE);
    auto slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - out), L"AF-profile-before.ini");
    return true;
}
// One DRS session: read the effective selector/level for this executable and optionally
// write (job 1: selector APP, saving the previous values first; job 2: put them back).
int32_t aniso_session(int job, AnisoOverride& out) {
    out.available = false;
    if (!load_api()) return NVAPI_ERROR;
    using GetGlobal = NvAPI_Status(__cdecl*)(NvDRSSessionHandle, NvDRSProfileHandle*);
    auto module = GetModuleHandleW(L"nvapi64.dll");
    auto query = module ? reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(module, "nvapi_QueryInterface"))) : nullptr;
    auto get_global = query ? reinterpret_cast<GetGlobal>(query(0x617bff9f)) : nullptr; // NvAPI_DRS_GetCurrentGlobalProfile
    NvDRSSessionHandle session = nullptr;
    NvAPI_Status r = create(&session);
    if (r != NVAPI_OK) return r;
    r = load(session);
    if (r == NVAPI_OK) {
        NvAPI_UnicodeString exe{}; exe_name(exe);
        NvDRSProfileHandle app_profile = nullptr, global = nullptr;
        NVDRS_APPLICATION app{}; app.version = NVDRS_APPLICATION_VER;
        if (find(session, exe, &app_profile, &app) != NVAPI_OK) app_profile = nullptr;
        if (get_global && get_global(session, &global) != NVAPI_OK) global = nullptr;
        NvDRSProfileHandle profile = app_profile ? app_profile : global;
        out.app_profile = app_profile != nullptr;
        if (profile) {
            out.available = true;
            NVDRS_SETTING sel{}; sel.version = NVDRS_SETTING_VER;
            NVDRS_SETTING lvl{}; lvl.version = NVDRS_SETTING_VER;
            const bool has_sel = get(session, profile, ANISO_MODE_SELECTOR_ID, &sel) == NVAPI_OK;
            const bool has_lvl = get(session, profile, ANISO_MODE_LEVEL_ID, &lvl) == NVAPI_OK;
            out.selector = has_sel ? sel.u32CurrentValue : ANISO_MODE_SELECTOR_DEFAULT;
            out.level = has_lvl ? lvl.u32CurrentValue : ANISO_MODE_LEVEL_DEFAULT;
            wchar_t backup[MAX_PATH]{};
            if (job && app_profile && aniso_backup_path(backup)) {
                if (job == 1) {
                    // Save once what the profile held itself (user value), so restore is exact.
                    if (GetFileAttributesW(backup) == INVALID_FILE_ATTRIBUTES) {
                        const bool own = has_sel && sel.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && !sel.isCurrentPredefined;
                        wchar_t v[32];
                        swprintf_s(v, L"%u", own ? 1u : 0u); WritePrivateProfileStringW(L"selector", L"user_value", v, backup);
                        swprintf_s(v, L"%u", out.selector); WritePrivateProfileStringW(L"selector", L"value", v, backup);
                        WritePrivateProfileStringW(L"backup", L"exe", reinterpret_cast<const wchar_t*>(exe), backup);
                        WritePrivateProfileStringW(L"backup", L"complete", L"1", backup);
                    }
                    if (GetPrivateProfileIntW(L"backup", L"complete", 0, backup) != 1) r = NVAPI_ERROR;
                    if (r == NVAPI_OK) {
                        NVDRS_SETTING w{}; w.version = NVDRS_SETTING_VER; w.settingId = ANISO_MODE_SELECTOR_ID;
                        w.settingType = NVDRS_DWORD_TYPE; w.u32CurrentValue = ANISO_MODE_SELECTOR_APP;
                        r = set(session, app_profile, &w);
                        if (r == NVAPI_OK) r = save(session);
                    }
                } else if (GetPrivateProfileIntW(L"backup", L"complete", 0, backup) == 1) {
                    if (GetPrivateProfileIntW(L"selector", L"user_value", 0, backup)) {
                        NVDRS_SETTING w{}; w.version = NVDRS_SETTING_VER; w.settingId = ANISO_MODE_SELECTOR_ID;
                        w.settingType = NVDRS_DWORD_TYPE; w.u32CurrentValue = GetPrivateProfileIntW(L"selector", L"value", 0, backup);
                        r = set(session, app_profile, &w);
                    } else {
                        r = erase(session, app_profile, ANISO_MODE_SELECTOR_ID);
                        if (r == NVAPI_SETTING_NOT_FOUND) r = NVAPI_OK;
                    }
                    if (r == NVAPI_OK) r = save(session);
                    if (r == NVAPI_OK) DeleteFileW(backup);
                } else r = NVAPI_ERROR;
                if (r == NVAPI_OK) { // read back what the profile now says
                    NVDRS_SETTING again{}; again.version = NVDRS_SETTING_VER;
                    if (get(session, app_profile, ANISO_MODE_SELECTOR_ID, &again) == NVAPI_OK) out.selector = again.u32CurrentValue;
                    else out.selector = ANISO_MODE_SELECTOR_DEFAULT;
                }
            } else if (job) r = NVAPI_EXECUTABLE_NOT_FOUND; // no own profile: we never write the global one
        }
    }
    destroy(session);
    return r;
}
DWORD WINAPI aniso_worker(void*) {
    for (;;) {
        if (WaitForSingleObject(aniso_event, INFINITE) != WAIT_OBJECT_0) return 0;
        const int job = aniso_job.exchange(0);
        AnisoOverride now{};
        const int32_t r = aniso_session(job, now);
        std::lock_guard<std::mutex> lock(aniso_mutex);
        const bool written = aniso_state.written;
        aniso_state = now;
        aniso_state.write_pending = false;
        aniso_state.written = written || (job == 1 && r == NVAPI_OK);
        if (job == 2 && r == NVAPI_OK) aniso_state.written = false;
        if (job) aniso_state.write_result = r;
    }
}
void aniso_start() {
    std::call_once(aniso_once, [] {
        aniso_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!aniso_event) return;
        HANDLE t = CreateThread(nullptr, 0, aniso_worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t); else { CloseHandle(aniso_event); aniso_event = nullptr; }
    });
}
void aniso_queue(int job) {
    aniso_start();
    if (!aniso_event) return;
    { std::lock_guard<std::mutex> lock(aniso_mutex); aniso_state.write_pending = true; }
    aniso_job.store(job);
    SetEvent(aniso_event);
}
}
void ReadAnisoOverride() {
    AnisoOverride now{};
    aniso_session(0, now);
    std::lock_guard<std::mutex> lock(aniso_mutex);
    const bool written = aniso_state.written; const int32_t wr = aniso_state.write_result;
    aniso_state = now; aniso_state.written = written; aniso_state.write_result = wr;
}
AnisoOverride GetAnisoOverride() { std::lock_guard<std::mutex> lock(aniso_mutex); return aniso_state; }
void RequestAppControlledAniso() { aniso_queue(1); }
void RequestRestoreAniso() { aniso_queue(2); }
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
