#include "host_dx12/settings.hpp"

#include <shlobj.h>
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <algorithm>
#include <vector>
#include <string>

// Persistence: games under Program Files run unelevated, so writes next to
// the game DLL fail silently (A17 finding). Defaults are read from the
// deploy-time ini beside the DLL; user changes are saved to and preferred
// from %LOCALAPPDATA%\NeuralAlias\NeuralAliasSL.ini.
namespace na::settings {
namespace {
#define NA_PERSISTED_FIELDS(F) F(enabled) F(dlss_mode) F(follow_game_resolution) F(preset) F(rr_preset) F(text_bold) F(win_x) F(win_y) F(win_w) F(win_h) F(nr_enabled) F(nr_intensity) F(nr_structure) F(nr_tone) F(nr_skin) F(nr_paper_white) F(nr_hdr_mode) F(nr_model) F(motion_enabled) F(motion_grid) F(motion_perf) F(motion_downscale) F(j2_sharpness) F(j2_preset) F(j2_mip_bias_x100) F(reflex_mode) F(fg_enabled) F(fg_mode) F(fg_preset) F(fg_multiplier) F(fg_target_fps) F(fg_frozen_at) F(fg_profile_override) F(native_fg_mode) F(native_fg_frames)
Settings g_settings;
Settings g_loaded_values;
std::atomic<bool> g_nr_user_touched{false};
std::atomic<bool> g_renderer_managed{false},g_renderer_ready{false},g_primary{false};
std::atomic<SaveDispatcher> g_save_dispatcher{nullptr};
std::atomic<uint32_t> g_failure_pending{0},g_safe_reason_code{0};
HANDLE g_renderer_journal=INVALID_HANDLE_VALUE;
HANDLE g_journal_election=nullptr;

wchar_t g_ini_defaults[MAX_PATH] = {};
wchar_t g_ini_user[MAX_PATH] = {};
std::atomic<uint32_t> g_last_save{0};
std::mutex g_nr_write_mutex;
NrWrite g_nr_writes[16]{};
uint64_t g_nr_write_sequence=0;
std::atomic<bool> g_session_started{false},g_session_failed{false},g_safe_start{false};
std::atomic<uint32_t> g_safe_overrides{0};
uint32_t g_saved_switches[6]{};
bool g_safe_profile=false;
wchar_t g_session_marker[MAX_PATH]{};
const char* reason_text(uint32_t code){return code==2?"freeze":code==3?"crash":code?"unclean":"none";}
uint32_t reason_code(const char* text){return text && !strcmp(text,"freeze")?2u:text && !strcmp(text,"crash")?3u:1u;}
uint32_t PersistedSwitch(uint32_t slot,uint32_t current) {
    const uint32_t mask=1u<<slot;
    if(!(g_safe_overrides.load()&mask))return current;
    if(current){g_safe_overrides.fetch_and(~mask);return current;}
    return g_saved_switches[slot]; // saving another control must not save our forced OFF
}

void resolve_user_ini()
{
    if (g_ini_user[0]) return;
    wchar_t* local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        wchar_t dir[MAX_PATH];
        swprintf_s(dir, L"%s\\NeuralAlias", local);
        CreateDirectoryW(dir, nullptr);
        wchar_t scope[64]{};
        if (g_ini_defaults[0]) GetPrivateProfileStringW(L"settings", L"scope", L"", scope, 64, g_ini_defaults);
        bool valid = scope[0] != 0;
        for (const wchar_t* c = scope; *c; ++c)
            valid &= (*c >= L'a' && *c <= L'z') || (*c >= L'A' && *c <= L'Z') ||
                     (*c >= L'0' && *c <= L'9') || *c == L'-' || *c == L'_';
        if (valid) {
            swprintf_s(dir, L"%s\\NeuralAlias\\%s", local, scope);
            CreateDirectoryW(dir, nullptr);
        }
        swprintf_s(g_ini_user, L"%s\\NeuralAliasSL.ini", dir);
        CoTaskMemFree(local);
    }
}

void load_from(const wchar_t* ini, bool only_if_present,Settings& target=g_settings)
{
    if (!ini || !ini[0]) return;
    if (only_if_present && GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) return;
    target.enabled.store(GetPrivateProfileIntW(L"dlss", L"enabled", 1, ini) ? 1u : 0u);
    uint32_t mode = GetPrivateProfileIntW(L"dlss", L"mode", 0, ini);
    if (mode >= static_cast<uint32_t>(DlssMode::Count)) mode = 0;
    target.dlss_mode.store(mode);
    target.follow_game_resolution.store(GetPrivateProfileIntW(L"dlss", L"follow_game_resolution", 0, ini) ? 1u : 0u);
    int preset = static_cast<int>(
        GetPrivateProfileIntW(L"dlss", L"preset", static_cast<UINT>(-1), ini));
    target.preset.store((preset >= 1 && preset <= 13) ? preset : -1);
    const int rr_preset = static_cast<int>(GetPrivateProfileIntW(L"dlss", L"rr_preset", 0, ini));
    target.rr_preset.store(rr_preset >= 4 && rr_preset <= 6 ? rr_preset : 0);
    target.text_bold.store(
        GetPrivateProfileIntW(L"overlay", L"text_bold", 0, ini) ? 1u : 0u);
    target.win_x.store(static_cast<int32_t>(GetPrivateProfileIntW(L"overlay", L"win_x", 0, ini)));
    target.win_y.store(static_cast<int32_t>(GetPrivateProfileIntW(L"overlay", L"win_y", 0, ini)));
    target.win_w.store(static_cast<int32_t>(GetPrivateProfileIntW(L"overlay", L"win_w", 0, ini)));
    target.win_h.store(static_cast<int32_t>(GetPrivateProfileIntW(L"overlay", L"win_h", 0, ini)));
    auto pct = [&](const wchar_t* key, uint32_t def) {
        int v = static_cast<int>(GetPrivateProfileIntW(L"nr", key, def, ini));
        return static_cast<uint32_t>(v < 0 ? 0 : (v > 100 ? 100 : v));
    };
    const bool nr=GetPrivateProfileIntW(L"nr",L"enabled",0,ini)!=0;
    if(&target==&g_settings)SetNrEnabled(nr,only_if_present?NrWriteSource::UserIni:NrWriteSource::DefaultIni);
    else target.nr_enabled.store(nr?1u:0u);
    target.nr_intensity.store(pct(L"intensity", 100));
    {
        const int v = static_cast<int>(GetPrivateProfileIntW(L"j2", L"sharpness", 100, ini));
        target.j2_sharpness.store(static_cast<uint32_t>(v < 0 ? 0 : (v > 200 ? 200 : v))); // 101..200 adds NIS
        wchar_t mb[32] = {};
        GetPrivateProfileStringW(L"j2", L"mip_bias", L"-1.0", mb, 32, ini);
        double mbv = _wtof(mb);
        mbv = mbv < -4.0 ? -4.0 : (mbv > 0.0 ? 0.0 : mbv);
        target.j2_mip_bias_x100.store(static_cast<int32_t>(mbv * 100.0 + (mbv < 0 ? -0.5 : 0.5)));
        // [j2] preset: a letter J..M (NGX value = letter - 'A' + 1), "default", or a number.
        wchar_t pz[16] = {};
        GetPrivateProfileStringW(L"j2", L"preset", L"L", pz, 16, ini);
        uint32_t pv = 12;
        if (pz[0] >= L'0' && pz[0] <= L'9') pv = static_cast<uint32_t>(_wtoi(pz));
        else if (!_wcsicmp(pz, L"default")) pv = 0;
        else if (pz[0] && !pz[1]) { const wchar_t c = towupper(pz[0]); pv = (c >= L'A' && c <= L'Z') ? static_cast<uint32_t>(c - L'A' + 1) : 12; }
        target.j2_preset.store(pv > 26 ? 12 : pv);
    }
    target.nr_structure.store(pct(L"structure", 100));
    target.nr_tone.store(pct(L"tone", 0));
    target.nr_skin.store(pct(L"skin", 100));
    int pw = static_cast<int>(GetPrivateProfileIntW(L"nr", L"paper_white", 200, ini));
    target.nr_paper_white.store(static_cast<uint32_t>(pw < 80 ? 80 : (pw > 1000 ? 1000 : pw)));
    uint32_t hm = GetPrivateProfileIntW(L"nr", L"hdr_mode", 0, ini);
    target.nr_hdr_mode.store(hm > 2 ? 0 : hm);
    target.nr_model.store(GetPrivateProfileIntW(L"nr", L"model", 0, ini) == 1 ? 1u : 0u);
    target.motion_enabled.store(GetPrivateProfileIntW(L"motion", L"enabled", 0, ini) ? 1u : 0u);
    uint32_t mg = GetPrivateProfileIntW(L"motion", L"grid", 4, ini);
    target.motion_grid.store(mg == 1 || mg == 2 || mg == 4 ? mg : 4);
    uint32_t mp = GetPrivateProfileIntW(L"motion", L"perf", 5, ini);
    target.motion_perf.store(mp == 5 || mp == 10 || mp == 20 ? mp : 5);
    uint32_t mdz = GetPrivateProfileIntW(L"motion", L"downscale", 2, ini);
    target.motion_downscale.store(mdz == 1 || mdz == 2 || mdz == 4 ? mdz : 2);
    int reflex = static_cast<int>(GetPrivateProfileIntW(L"latency", L"reflex", 1, ini));
    target.reflex_mode.store(reflex >= 0 && reflex <= 2 ? reflex : 1);
    target.fg_enabled.store(GetPrivateProfileIntW(L"fg", L"enabled", 0, ini) ? 1u : 0u);
    const UINT fg_preset = GetPrivateProfileIntW(L"fg", L"preset", 1, ini);
    target.fg_preset.store(fg_preset >= 1 && fg_preset <= 3 ? fg_preset : 1u);
    target.fg_mode.store(GetPrivateProfileIntW(L"fg", L"mode", 1, ini) ? 1u : 0u);
    uint32_t fm = GetPrivateProfileIntW(L"fg", L"multiplier", 2, ini);
    target.fg_multiplier.store(fm < 2 ? 2 : (fm > 6 ? 6 : fm));
    uint32_t ft = GetPrivateProfileIntW(L"fg", L"target_fps", 0, ini);
    target.fg_target_fps.store(ft > 1000 ? 0 : ft);
    target.fg_frozen_at.store(GetPrivateProfileIntW(L"fg", L"frozen_at", 0, ini)); // A69
    const UINT nfm = GetPrivateProfileIntW(L"native_fg", L"mode", 0, ini);
    target.native_fg_mode.store(nfm <= 3 ? nfm : 0u);
    const UINT nff = GetPrivateProfileIntW(L"native_fg", L"frames", 0, ini);
    target.native_fg_frames.store(nff <= 5 ? nff : 0u);
}
} // namespace

Settings& Get() { return g_settings; }
void SetNrEnabled(bool enabled,NrWriteSource source,uint32_t input_flags) {
    std::lock_guard<std::mutex> lock(g_nr_write_mutex);
    if(source==NrWriteSource::Overlay){g_safe_overrides.fetch_and(~2u);g_nr_user_touched.store(true);} // explicit Off is also a user choice
    const auto before=g_settings.nr_enabled.exchange(enabled?1u:0u);
    const auto sequence=++g_nr_write_sequence;
    g_nr_writes[(sequence-1)%16]={sequence,GetTickCount64(),GetCurrentThreadId(),before,enabled?1u:0u,source,input_flags};
}
NrWriteHistory GetNrWrites() {
    std::lock_guard<std::mutex> lock(g_nr_write_mutex);
    NrWriteHistory out;
    out.count=static_cast<uint32_t>((std::min)(g_nr_write_sequence,uint64_t{16}));
    const auto first=g_nr_write_sequence-out.count;
    for(uint32_t i=0;i<out.count;++i) out.entries[i]=g_nr_writes[(first+i)%16];
    return out;
}

void SetIniPath(const wchar_t* path)
{
    if (path) wcsncpy_s(g_ini_defaults, path, _TRUNCATE);
}

const wchar_t* UserIniPath()
{
    resolve_user_ini();
    return g_ini_user;
}

void Load()
{
    load_from(g_ini_defaults, false);
    g_settings.fg_profile_override.store(g_ini_defaults[0] && GetPrivateProfileIntW(L"fg", L"profile_override", 0, g_ini_defaults) == 1);
    resolve_user_ini();
    load_from(g_ini_user, true); // user choices win when present
    // Insertion mode is an explicit per-game deployment choice, never inherited
    // from another game's global menu preferences.
    g_settings.j2_inline.store(g_ini_defaults[0] && GetPrivateProfileIntW(L"j2", L"inline", 0, g_ini_defaults) != 0);
    // Route choice is per game too: automatic unless this game's ini forces one (diagnosis).
    {
        wchar_t rt[16] = {};
        if (g_ini_defaults[0]) GetPrivateProfileStringW(L"j2", L"route", L"auto", rt, 16, g_ini_defaults);
        g_settings.j2_route.store(!_wcsicmp(rt, L"between") ? 1u : (!_wcsicmp(rt, L"inline") ? 2u : 0u));
    }
    if (g_ini_defaults[0]) { // evidence capture: per game, diagnostic
        const UINT f = GetPrivateProfileIntW(L"j2", L"capture_frames", 30, g_ini_defaults);
        g_settings.j2_capture_frames.store(f < 1 ? 1u : (f > 240 ? 240u : f));
        g_settings.j2_capture_key.store(GetPrivateProfileIntW(L"j2", L"capture_key", 0x77, g_ini_defaults) & 0xFFu);
        g_settings.j2_capture_after_s.store(GetPrivateProfileIntW(L"j2", L"capture_after_seconds", 0, g_ini_defaults));
    }
#define REMEMBER(member) g_loaded_values.member.store(g_settings.member.load());
    NA_PERSISTED_FIELDS(REMEMBER)
#undef REMEMBER

}

bool SaveFromWorker()
{
    if(g_renderer_managed.load() && !g_primary.load()){g_last_save.store(2);return false;}
    resolve_user_ini();
    const wchar_t* ini = g_ini_user[0] ? g_ini_user : g_ini_defaults;
    if (!ini[0]) {
        g_last_save.store(2);
        return false;
    }
    wchar_t v[32];
    bool ok = true;
    swprintf_s(v, L"%u", PersistedSwitch(0,g_settings.enabled.load()));
    ok &= WritePrivateProfileStringW(L"dlss", L"enabled", v, ini) != 0;
    swprintf_s(v, L"%u", g_settings.dlss_mode.load());
    ok &= WritePrivateProfileStringW(L"dlss", L"mode", v, ini) != 0;
    swprintf_s(v, L"%u", g_settings.follow_game_resolution.load());
    ok &= WritePrivateProfileStringW(L"dlss", L"follow_game_resolution", v, ini) != 0;
    swprintf_s(v, L"%d", g_settings.preset.load());
    ok &= WritePrivateProfileStringW(L"dlss", L"preset", v, ini) != 0;
    swprintf_s(v, L"%d", g_settings.rr_preset.load());
    ok &= WritePrivateProfileStringW(L"dlss", L"rr_preset", v, ini) != 0;
    swprintf_s(v, L"%u", g_settings.text_bold.load());
    ok &= WritePrivateProfileStringW(L"overlay", L"text_bold", v, ini) != 0;
    struct { const wchar_t* key; int32_t value; } win[] = {
        {L"win_x", g_settings.win_x.load()}, {L"win_y", g_settings.win_y.load()},
        {L"win_w", g_settings.win_w.load()}, {L"win_h", g_settings.win_h.load()}};
    for (const auto& kv : win) {
        swprintf_s(v, L"%d", kv.value);
        ok &= WritePrivateProfileStringW(L"overlay", kv.key, v, ini) != 0;
    }
    struct { const wchar_t* key; uint32_t value; } nr[] = {
        {L"enabled", PersistedSwitch(1,g_settings.nr_enabled.load())},   {L"intensity", g_settings.nr_intensity.load()},
        {L"structure", g_settings.nr_structure.load()}, {L"tone", g_settings.nr_tone.load()},
        {L"skin", g_settings.nr_skin.load()},         {L"paper_white", g_settings.nr_paper_white.load()},
        {L"hdr_mode", g_settings.nr_hdr_mode.load()}, {L"model", g_settings.nr_model.load()}};
    for (const auto& kv : nr) {
        swprintf_s(v, L"%u", kv.value);
        ok &= WritePrivateProfileStringW(L"nr", kv.key, v, ini) != 0;
    }
    struct { const wchar_t* key; uint32_t value; } motion[] = {
        {L"enabled", PersistedSwitch(2,g_settings.motion_enabled.load())}, {L"grid", g_settings.motion_grid.load()}, {L"perf", g_settings.motion_perf.load()},
        {L"downscale", g_settings.motion_downscale.load()}};
    for (const auto& kv : motion) {
        swprintf_s(v, L"%u", kv.value);
        ok &= WritePrivateProfileStringW(L"motion", kv.key, v, ini) != 0;
    }
    swprintf_s(v, L"%u", g_settings.j2_sharpness.load());
    ok &= WritePrivateProfileStringW(L"j2", L"sharpness", v, ini) != 0;
    {
        const uint32_t pv = g_settings.j2_preset.load();
        if (pv >= 1 && pv <= 26) swprintf_s(v, L"%c", static_cast<wchar_t>(L'A' + pv - 1));
        else swprintf_s(v, L"default");
        ok &= WritePrivateProfileStringW(L"j2", L"preset", v, ini) != 0;
        swprintf_s(v, L"%.2f", static_cast<int32_t>(PersistedSwitch(5,static_cast<uint32_t>(g_settings.j2_mip_bias_x100.load()))) / 100.0);
        ok &= WritePrivateProfileStringW(L"j2", L"mip_bias", v, ini) != 0;
    }
    swprintf_s(v, L"%d", static_cast<int32_t>(PersistedSwitch(4,static_cast<uint32_t>(g_settings.reflex_mode.load()))));
    ok &= WritePrivateProfileStringW(L"latency", L"reflex", v, ini) != 0;
    struct { const wchar_t* key; uint32_t value; } fg[] = {
        {L"enabled", PersistedSwitch(3,g_settings.fg_enabled.load())}, {L"mode", g_settings.fg_mode.load()}, {L"preset", g_settings.fg_preset.load()},
        {L"multiplier", g_settings.fg_multiplier.load()}, {L"target_fps", g_settings.fg_target_fps.load()},
        {L"frozen_at", g_settings.fg_frozen_at.load()}};
    for (const auto& kv : fg) {
        swprintf_s(v, L"%u", kv.value);
        ok &= WritePrivateProfileStringW(L"fg", kv.key, v, ini) != 0;
    }
    swprintf_s(v, L"%u", g_settings.native_fg_mode.load());
    ok &= WritePrivateProfileStringW(L"native_fg", L"mode", v, ini) != 0;
    swprintf_s(v, L"%u", g_settings.native_fg_frames.load());
    ok &= WritePrivateProfileStringW(L"native_fg", L"frames", v, ini) != 0;
    g_last_save.store(ok ? 1u : 2u);
    return ok;
}

bool Save(){
    if(auto dispatch=g_save_dispatcher.load(std::memory_order_acquire))return dispatch();
    return SaveFromWorker();
}
void SetSavePending(){g_last_save.store(3);}
bool EffectsReady(){return !g_renderer_managed.load(std::memory_order_acquire) || g_renderer_ready.load(std::memory_order_acquire);}
bool CanPersist(){return !g_renderer_managed.load() || g_primary.load();}
void DeferForRenderer(SaveDispatcher dispatcher){
    g_renderer_ready.store(false);g_primary.store(false);g_renderer_managed.store(true,std::memory_order_release);
    g_save_dispatcher.store(dispatcher,std::memory_order_release);
}
void PublishRendererReady(){g_renderer_ready.store(true,std::memory_order_release);}
uint32_t LastSaveResult() { return g_last_save.load(); }

namespace {
void apply_recovery(bool unclean,uint32_t reason){
    g_safe_reason_code.store(unclean?reason:0);g_safe_start.store(unclean);g_safe_overrides.store(unclean?63u:0u);g_safe_profile=false;
    if(!unclean)return;
    g_saved_switches[0]=g_settings.enabled.exchange(0);g_saved_switches[1]=g_settings.nr_enabled.load();
    SetNrEnabled(false,NrWriteSource::SafeStartup);
    g_saved_switches[2]=g_settings.motion_enabled.exchange(0);g_saved_switches[3]=g_settings.fg_enabled.exchange(0);
    g_saved_switches[4]=static_cast<uint32_t>(g_settings.reflex_mode.exchange(0));
    g_safe_profile=g_settings.fg_profile_override.exchange(0)!=0;
    g_saved_switches[5]=static_cast<uint32_t>(g_settings.j2_mip_bias_x100.exchange(0));
}
template<class T> void refresh_field(std::atomic<T>& current,const std::atomic<T>& loaded,const std::atomic<T>& fresh){
    if(static_cast<const void*>(&current)==static_cast<const void*>(&g_settings.nr_enabled) && g_nr_user_touched.load())return;
    T expected=loaded.load();current.compare_exchange_strong(expected,fresh.load());
}
void refresh_unedited(){
    Settings fresh;load_from(g_ini_defaults,false,fresh);load_from(g_ini_user,true,fresh);
    fresh.fg_profile_override.store(g_ini_defaults[0] && GetPrivateProfileIntW(L"fg",L"profile_override",0,g_ini_defaults)==1);
#define REFRESH(member) refresh_field(g_settings.member,g_loaded_values.member,fresh.member);
    NA_PERSISTED_FIELDS(REFRESH)
#undef REFRESH
}
uint32_t read_reason(HANDLE file,bool* primary=nullptr){
    char bytes[96]{};DWORD read=0;ReadFile(file,bytes,sizeof(bytes)-1,&read,nullptr);
    if(primary)*primary=strstr(bytes,"primary=0")==nullptr;
    return strstr(bytes,"freeze")?2u:strstr(bytes,"crash")?3u:1u;
}
void write_journal(uint32_t reason){
    if(g_renderer_journal==INVALID_HANDLE_VALUE)return;
    char bytes[80]{};const int n=snprintf(bytes,sizeof(bytes),"primary=%u\n%s\n",g_primary.load()?1u:0u,reason_text(reason));
    SetFilePointer(g_renderer_journal,0,nullptr,FILE_BEGIN);DWORD written=0;
    WriteFile(g_renderer_journal,bytes,static_cast<DWORD>(n),&written,nullptr);SetEndOfFile(g_renderer_journal);
    FlushFileBuffers(g_renderer_journal);
}
} // namespace

bool BeginRendererSession(bool journal_allowed){
    // Only the init/control worker calls this after real renderer evidence.
    resolve_user_ini();
    if(!g_ini_user[0])return false; // never create relative journals in a game/work directory
    uint64_t hash=1469598103934665603ull;
    for(const wchar_t* c=g_ini_user;*c;++c){hash^=static_cast<uint64_t>(towlower(*c));hash*=1099511628211ull;}
    wchar_t mutex_name[96]{};swprintf_s(mutex_name,L"Local\\NeuralAliasSettings-%016llx",static_cast<unsigned long long>(hash));
    HANDLE election=CreateMutexW(nullptr,FALSE,mutex_name);
    const DWORD wait=election?WaitForSingleObject(election,2000):WAIT_FAILED;
    if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED){if(election)CloseHandle(election);return false;}
    // Election is short-lived. Per-process journal handles, not PID existence,
    // distinguish live renderers from files left by a terminated renderer.
    std::wstring legacy=std::wstring(g_ini_user)+L".session";
    std::wstring pattern=legacy+L".*";
    std::vector<std::wstring> stale;bool live_primary=false;uint32_t recovery=0;
    auto inspect=[&](const std::wstring& path){
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE){recovery=(std::max)(recovery,read_reason(file));CloseHandle(file);stale.push_back(path);return;}
        const DWORD error=GetLastError();if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return;
        file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        bool primary=true;if(file!=INVALID_HANDLE_VALUE){read_reason(file,&primary);CloseHandle(file);}
        live_primary|=primary;
    };
    inspect(legacy);
    WIN32_FIND_DATAW data{};HANDLE find=FindFirstFileW(pattern.c_str(),&data);
    if(find!=INVALID_HANDLE_VALUE){
        auto dir=std::wstring(g_ini_user);dir.resize(dir.find_last_of(L"\\/")+1);
        do{
            const wchar_t* suffix=wcsrchr(data.cFileName,L'.');bool numeric=suffix && *++suffix;
            if(numeric)for(const wchar_t* c=suffix;*c;++c)numeric&=*c>=L'0' && *c<=L'9';
            if(numeric && !(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))inspect(dir+data.cFileName);
        }while(FindNextFileW(find,&data));FindClose(find);
    }
    swprintf_s(g_session_marker,L"%s.session.%lu",g_ini_user,GetCurrentProcessId());
    g_renderer_journal=journal_allowed?CreateFileW(g_session_marker,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr):INVALID_HANDLE_VALUE;
    const bool created=g_renderer_journal!=INVALID_HANDLE_VALUE;
    g_primary.store(created && !live_primary);g_session_started.store(created);g_session_failed.store(false);
    if(g_primary.load()){
        // Refresh again under election: another primary may have saved and
        // exited since bootstrap; preserve fields the local UI already edited.
        refresh_unedited();apply_recovery(recovery!=0,recovery?recovery:1);
        for(const auto& path:stale)if(path!=g_session_marker)DeleteFileW(path.c_str());
    }else {refresh_unedited();apply_recovery(!live_primary && recovery!=0,recovery?recovery:1);}
    if(created)write_journal(1);
    ReleaseMutex(election);
    if(created)g_journal_election=election;else CloseHandle(election);
    return g_primary.load();
}
void FlushSessionFailure(){
    const auto pending=g_failure_pending.exchange(0);
    if(pending && g_renderer_journal!=INVALID_HANDLE_VALUE)write_journal(pending);
}

bool SafeStart(){return g_safe_start.load();}
bool SafeProfileRestoreRequired(){return SafeStart() && g_safe_profile;}
const char* SafeStartReason(){return reason_text(g_safe_reason_code.load());}
void MarkSessionFailed(const char* reason) {
    if(!g_session_started.load())return;
    g_session_failed.store(true);const auto code=reason_code(reason);
    if(g_renderer_managed.load()){g_failure_pending.store(code);return;}
    HANDLE file=CreateFileW(g_session_marker,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD written=0;const char* text=reason_text(code);WriteFile(file,text,static_cast<DWORD>(strlen(text)),&written,nullptr);FlushFileBuffers(file);CloseHandle(file);}
}
void BeginSession(const wchar_t* marker_path) {
    // Legacy standalone test/host API. Managed production uses renderer evidence.
    if(g_session_started.exchange(true))return;
    if(marker_path)wcsncpy_s(g_session_marker,marker_path,_TRUNCATE);
    else {resolve_user_ini();swprintf_s(g_session_marker,L"%s.session",g_ini_user);}
    HANDLE previous=CreateFileW(g_session_marker,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    const bool unclean=previous!=INVALID_HANDLE_VALUE;uint32_t reason=1;
    if(unclean){reason=read_reason(previous);CloseHandle(previous);}
    apply_recovery(unclean,reason);MarkSessionFailed("unclean");g_session_failed.store(false);
}
void EndSession(uint32_t exit_code) {
    g_renderer_ready.store(false,std::memory_order_release);
    if(!g_session_started.exchange(false)){g_primary.store(false);return;}
    bool elected=false;
    if(g_renderer_managed.load()){
        const auto wait=g_journal_election?WaitForSingleObject(g_journal_election,2000):WAIT_FAILED;
        elected=wait==WAIT_OBJECT_0 || wait==WAIT_ABANDONED;
        FlushSessionFailure();
        if(g_renderer_journal!=INVALID_HANDLE_VALUE){CloseHandle(g_renderer_journal);g_renderer_journal=INVALID_HANDLE_VALUE;}
    }
    if(exit_code==0 && !g_session_failed.load() && g_session_marker[0] && (!g_renderer_managed.load() || elected))DeleteFileW(g_session_marker);
    if(g_journal_election){if(elected)ReleaseMutex(g_journal_election);CloseHandle(g_journal_election);g_journal_election=nullptr;}
    g_primary.store(false);
}

} // namespace na::settings
