// Exercises the production transaction against an in-memory DRS; never changes the driver.
#include "host_dx12/fg/preset_profile.cpp"
#include "test_support.hpp"
#include <map>
#include <filesystem>
using namespace na::fg::preset;
namespace {
std::map<NvU32, NVDRS_SETTING> db, pending;
int saves = 0;
bool fail_save = false;
NvAPI_Status __cdecl make(NvDRSSessionHandle* s) { *s = reinterpret_cast<NvDRSSessionHandle>(1); return NVAPI_OK; }
NvAPI_Status __cdecl close(NvDRSSessionHandle) { return NVAPI_OK; }
NvAPI_Status __cdecl read(NvDRSSessionHandle) { pending = db; return NVAPI_OK; }
NvAPI_Status __cdecl write(NvDRSSessionHandle) { if (fail_save) return NVAPI_ERROR; db = pending; ++saves; return NVAPI_OK; }
NvAPI_Status __cdecl lookup(NvDRSSessionHandle, NvAPI_UnicodeString exe, NvDRSProfileHandle* p, NVDRS_APPLICATION*) {
    if (wcscmp(reinterpret_cast<wchar_t*>(exe), L"PBZ.exe")) return NVAPI_EXECUTABLE_NOT_FOUND;
    *p = reinterpret_cast<NvDRSProfileHandle>(2); return NVAPI_OK;
}
NvAPI_Status __cdecl fetch(NvDRSSessionHandle, NvDRSProfileHandle, NvU32 id, NVDRS_SETTING* s) {
    auto it = pending.find(id); if (it == pending.end()) return NVAPI_SETTING_NOT_FOUND;
    *s = it->second; return NVAPI_OK;
}
NvAPI_Status __cdecl put(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING* s) {
    pending[s->settingId] = *s; return NVAPI_OK;
}
NvAPI_Status __cdecl remove(NvDRSSessionHandle, NvDRSProfileHandle, NvU32 id) { pending.erase(id); return NVAPI_OK; }
}
int main() {
    UpscalerOverrides overrides;overrides.available=true;overrides.sr_preset=13;overrides.rr_preset=6;
    CHECK(!overrides.SrForced() && !overrides.RrForced());
    overrides.sr_enabled=overrides.rr_enabled=1;CHECK(overrides.SrForced() && overrides.RrForced());
    overrides.sr_preset=overrides.rr_preset=0;CHECK(!overrides.SrForced() && !overrides.RrForced());
    create=make; destroy=close; load=read; save=write; find=lookup; get=fetch; set=put; erase=remove;
    wcscpy_s(target_exe, L"PBZ.exe");
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    swprintf_s(backup_path,L"%sNA-profile-test-%lu.ini",temp,GetCurrentProcessId());
    DeleteFileW(backup_path);
    NVDRS_SETTING b{}; b.version=NVDRS_SETTING_VER; b.settingId=ids[1]; b.settingType=NVDRS_DWORD_TYPE;
    b.settingLocation=NVDRS_CURRENT_PROFILE_LOCATION; b.u32CurrentValue=2; db[ids[1]]=b;
    uint32_t stored=0; bool changed=false;
    CHECK(apply(3,stored,changed,true)==NVAPI_OK && stored==2 && saves==0);
    CHECK(GetFileAttributesW(backup_path)==INVALID_FILE_ATTRIBUTES);
    CHECK(apply(3,stored,changed)==NVAPI_OK && stored==3 && changed);
    CHECK(db[ids[0]].u32CurrentValue==1);
    // Repeated runtime menu requests must never replace the original B/absent snapshot.
    CHECK(apply(1,stored,changed)==NVAPI_OK && stored==1);
    CHECK(apply(3,stored,changed,false,true)==NVAPI_OK);
    CHECK(db.size()==1 && db[ids[1]].u32CurrentValue==2);
    // Failed persistence leaves driver values and original backup intact.
    fail_save=true; CHECK(apply(3,stored,changed)!=NVAPI_OK);
    CHECK(db.size()==1 && db[ids[1]].u32CurrentValue==2); fail_save=false;
    wcscpy_s(target_exe,L"Other.exe");
    CHECK(apply(3,stored,changed)!=NVAPI_OK);
    wcscpy_s(target_exe,L"PBZ.exe");
    CHECK(apply(3,stored,changed,false,true)==NVAPI_OK);
    DeleteFileW(backup_path);
    // Inherited/predefined values must be restored by removing our override.
    b.isCurrentPredefined=1; db[ids[1]]=b;
    CHECK(apply(3,stored,changed)==NVAPI_OK);
    CHECK(apply(3,stored,changed,false,true)==NVAPI_OK && db.empty());
    DeleteFileW(backup_path);
    return test::finish("fg_profile_test");
}

