#include "host_dx12/settings.hpp"
#include "test_support.hpp"
#include <filesystem>
int main() {
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH,temp);
    wchar_t ini[MAX_PATH]{}; swprintf_s(ini,L"%sNA-settings-test-%lu.ini",temp,GetCurrentProcessId());
    wchar_t scope[64]{}; swprintf_s(scope,L"PBZ-test-%lu",GetCurrentProcessId());
    WritePrivateProfileStringW(L"settings",L"scope",scope,ini);
    WritePrivateProfileStringW(L"dlss",L"mode",L"4",ini);
    WritePrivateProfileStringW(L"dlss",L"follow_game_resolution",L"1",ini);
    WritePrivateProfileStringW(L"fg",L"preset",L"3",ini);
    WritePrivateProfileStringW(L"fg",L"profile_override",L"1",ini);
    na::settings::SetIniPath(ini); na::settings::Load();
    auto& s=na::settings::Get();
    CHECK(s.dlss_mode==4 && s.fg_preset==3 && s.fg_profile_override==1);
    CHECK(s.follow_game_resolution==1);
    CHECK(wcsstr(na::settings::UserIniPath(),scope)!=nullptr);
    CHECK(na::settings::Save());
    s.fg_preset=1; s.dlss_mode=0; s.follow_game_resolution=0; na::settings::Load();
    CHECK(s.follow_game_resolution==1);
    CHECK(s.fg_preset==3 && s.dlss_mode==4);
    WritePrivateProfileStringW(L"fg",L"preset",L"99",na::settings::UserIniPath());
    na::settings::Load(); CHECK(s.fg_preset==1);
    wchar_t marker[MAX_PATH]{};swprintf_s(marker,L"%s.session",ini);
    na::settings::BeginSession(marker);CHECK(!na::settings::SafeStart());
    na::settings::EndSession(0);CHECK(GetFileAttributesW(marker)==INVALID_FILE_ATTRIBUTES);
    na::settings::BeginSession(marker);na::settings::MarkSessionFailed("freeze");na::settings::EndSession(0);
    s.enabled=1;s.nr_enabled=1;s.motion_enabled=1;s.fg_enabled=1;s.reflex_mode=1;s.j2_mip_bias_x100=-75;
    CHECK(na::settings::Save());na::settings::BeginSession(marker);
    CHECK(na::settings::SafeStart() && !strcmp(na::settings::SafeStartReason(),"freeze"));
    CHECK(!s.enabled && !s.nr_enabled && !s.motion_enabled && !s.fg_enabled && !s.reflex_mode && !s.j2_mip_bias_x100);
    CHECK(na::settings::Save());
    CHECK(GetPrivateProfileIntW(L"nr",L"enabled",0,na::settings::UserIniPath())==1);
    CHECK(GetPrivateProfileIntW(L"dlss",L"enabled",0,na::settings::UserIniPath())==1);
    wchar_t bias[32]{};GetPrivateProfileStringW(L"j2",L"mip_bias",L"",bias,32,na::settings::UserIniPath());CHECK(!wcscmp(bias,L"-0.75"));
    na::settings::SetNrEnabled(true,na::settings::NrWriteSource::Overlay);CHECK(na::settings::Save());
    na::settings::SetNrEnabled(false,na::settings::NrWriteSource::Overlay);CHECK(na::settings::Save());
    CHECK(GetPrivateProfileIntW(L"nr",L"enabled",1,na::settings::UserIniPath())==0);
    na::settings::EndSession(0);CHECK(GetFileAttributesW(marker)==INVALID_FILE_ATTRIBUTES);
    na::settings::BeginSession(marker);na::settings::EndSession(1);na::settings::BeginSession(marker);
    CHECK(na::settings::SafeStart() && !strcmp(na::settings::SafeStartReason(),"unclean"));na::settings::EndSession(0);
    DeleteFileW(na::settings::UserIniPath());
    std::filesystem::remove(std::filesystem::path(na::settings::UserIniPath()).parent_path());
    DeleteFileW(ini);
    return test::finish("pbz_settings_test");
}

