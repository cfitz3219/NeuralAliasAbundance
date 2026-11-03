// O3: same-executable child processes; no game/GPU/profile access.
#include "test_support.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <filesystem>
#include <thread>
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#if defined(NA_HAVE_NGX_SDK)
#include "fake_ngx_params.hpp"
#endif
namespace fs=std::filesystem;
struct Child {
    PROCESS_INFORMATION pi{};HANDLE boot=nullptr,go=nullptr,ready=nullptr,stop=nullptr;std::wstring result;
    ~Child(){if(pi.hProcess){if(WaitForSingleObject(pi.hProcess,0)!=WAIT_OBJECT_0){TerminateProcess(pi.hProcess,9);WaitForSingleObject(pi.hProcess,2000);}CloseHandle(pi.hProcess);}if(pi.hThread)CloseHandle(pi.hThread);for(HANDLE h:{boot,go,ready,stop})if(h)CloseHandle(h);}
    void Start(){SetEvent(go);}bool Ready(){return WaitForSingleObject(ready,10000)==WAIT_OBJECT_0;}
    unsigned Get(const wchar_t* key){return GetPrivateProfileIntW(L"result",key,99,result.c_str());}
    void End(){SetEvent(stop);CHECK(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0);DWORD code=9;GetExitCodeProcess(pi.hProcess,&code);CHECK(code==0);}
    void Kill(){TerminateProcess(pi.hProcess,5);CHECK(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0);}
};
int child(const wchar_t* mode,const wchar_t* ini,const wchar_t* prefix,const wchar_t* result){
    auto event=[&](const wchar_t* suffix){return OpenEventW(EVENT_MODIFY_STATE|SYNCHRONIZE,FALSE,(std::wstring(prefix)+suffix).c_str());};
    HANDLE boot=event(L"-boot"),go=event(L"-go"),ready=event(L"-ready"),stop=event(L"-stop");if(!boot||!go||!ready||!stop)return 8;
    na::settings::SetIniPath(ini);na::settings::Load();if(!na::hooking::Initialize())return 9;
    if(!wcscmp(mode,L"hook-failed"))na::hooking::FailNextCommitEnableForTest(0);
    na::host::StartSession();
#if defined(NA_HAVE_NGX_SDK)
    test::FakeParams params;params.Set(NVSDK_NGX_Parameter_PerfQualityValue,2u);
    const auto keys=params.values.size();
    if(na::ngx::ApplyCreateOverridesForFeature(1,&params) || params.values.size()!=keys)ExitProcess(6);
#endif
    std::thread worker([]{na::host::RunSessionWorker();});
    SetEvent(boot);if(WaitForSingleObject(go,10000)!=WAIT_OBJECT_0)ExitProcess(8);
    const bool helper=!wcscmp(mode,L"helper");
    if(!wcscmp(mode,L"edit-before")){na::settings::Get().dlss_mode=4;na::settings::Save();}
    if(!helper){na::host::RendererObserved();for(unsigned i=0;i<1000&&!na::host::SessionReady();++i)Sleep(5);if(!na::host::SessionReady())ExitProcess(7);}
    if(helper||!wcscmp(mode,L"local-edit")){na::settings::SetNrEnabled(false,na::settings::NrWriteSource::Overlay);na::settings::Get().nr_model=0;}
    if(!wcscmp(mode,L"explicit-off"))na::settings::SetNrEnabled(false,na::settings::NrWriteSource::Overlay);
    na::settings::Save();if(!helper)for(unsigned i=0;i<1000&&na::settings::LastSaveResult()==3;++i)Sleep(5);
    if(!wcscmp(mode,L"freeze")||helper)na::settings::MarkSessionFailed("freeze");
    auto put=[&](const wchar_t* key,unsigned value){wchar_t text[32]{};swprintf_s(text,L"%u",value);WritePrivateProfileStringW(L"result",key,text,result);};
    put(L"primary",na::settings::CanPersist());put(L"safe",na::settings::SafeStart());put(L"effects",na::settings::EffectsReady());
    put(L"nr",na::settings::Get().nr_enabled.load());put(L"model",na::settings::Get().nr_model.load());put(L"mode",na::settings::Get().dlss_mode.load());put(L"saved",na::settings::LastSaveResult());
    SetEvent(ready);WaitForSingleObject(stop,15000);ExitProcess(0);
}
int wmain(int argc,wchar_t** argv){
    if(argc==6&&!wcscmp(argv[1],L"--child"))return child(argv[2],argv[3],argv[4],argv[5]);
    wchar_t temp[MAX_PATH]{},exe[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);GetModuleFileNameW(nullptr,exe,MAX_PATH);
    const auto tag=L"NA-session-O3-"+std::to_wstring(GetCurrentProcessId());const auto root=fs::path(temp)/tag;fs::create_directory(root);const auto ini=root/L"defaults.ini";
    WritePrivateProfileStringW(L"settings",L"scope",tag.c_str(),ini.c_str());WritePrivateProfileStringW(L"nr",L"enabled",L"1",ini.c_str());WritePrivateProfileStringW(L"nr",L"model",L"1",ini.c_str());WritePrivateProfileStringW(L"dlss",L"mode",L"1",ini.c_str());
    na::settings::SetIniPath(ini.c_str());na::settings::Load();const fs::path user=na::settings::UserIniPath();REQUIRE(user.parent_path().filename()==tag);REQUIRE(na::settings::Save());unsigned sequence=0;
    auto spawn=[&](Child& c,const wchar_t* mode){
        const auto prefix=L"Local\\"+tag+L"-"+std::to_wstring(++sequence);auto event=[&](const wchar_t* suffix){return CreateEventW(nullptr,TRUE,FALSE,(prefix+suffix).c_str());};
        c.boot=event(L"-boot");c.go=event(L"-go");c.ready=event(L"-ready");c.stop=event(L"-stop");c.result=(root/(std::to_wstring(sequence)+L".ini")).wstring();
        std::wstring command=L"\""+std::wstring(exe)+L"\" --child "+mode+L" \""+ini.wstring()+L"\" \""+prefix+L"\" \""+c.result+L"\"";
        STARTUPINFOW si{};si.cb=sizeof(si);return CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&c.pi)&&WaitForSingleObject(c.boot,10000)==WAIT_OBJECT_0;
    };
    auto marker=[&](const Child& c){return fs::path(user.wstring()+L".session."+std::to_wstring(c.pi.dwProcessId));};
    auto count=[&](){unsigned n=0;for(const auto& e:fs::directory_iterator(user.parent_path()))if(e.path().filename().wstring().find(L".session.")!=std::wstring::npos)++n;return n;};
    auto stored=[&](const wchar_t* section,const wchar_t* key){return GetPrivateProfileIntW(section,key,99,user.c_str());};
    {Child helper,owner;REQUIRE(spawn(helper,L"helper"));helper.Start();REQUIRE(helper.Ready());CHECK(!helper.Get(L"effects")&&!helper.Get(L"primary")&&count()==0);CHECK(stored(L"nr",L"enabled")==1);
        REQUIRE(spawn(owner,L"owner"));owner.Start();REQUIRE(owner.Ready());CHECK(owner.Get(L"primary")==1&&!owner.Get(L"safe")&&owner.Get(L"nr")==1);helper.End();CHECK(fs::exists(marker(owner)));owner.End();CHECK(count()==0);}
    {Child owner,helper,local;REQUIRE(spawn(owner,L"owner"));owner.Start();REQUIRE(owner.Ready());REQUIRE(spawn(helper,L"helper"));helper.Start();REQUIRE(helper.Ready());
        REQUIRE(spawn(local,L"local-edit"));local.Start();REQUIRE(local.Ready());CHECK(local.Get(L"primary")==0&&local.Get(L"effects")==1&&local.Get(L"saved")==2);CHECK(local.Get(L"nr")==0&&stored(L"nr",L"enabled")==1&&stored(L"nr",L"model")==1);
        local.End();helper.End();CHECK(fs::exists(marker(owner)));owner.End();CHECK(count()==0);}
    {Child a,b;REQUIRE(spawn(a,L"owner"));REQUIRE(spawn(b,L"owner"));a.Start();b.Start();REQUIRE(a.Ready()&&b.Ready());CHECK(a.Get(L"primary")+b.Get(L"primary")==1);CHECK(!a.Get(L"safe")&&!b.Get(L"safe"));a.End();b.End();CHECK(count()==0);}
    {Child a;REQUIRE(spawn(a,L"edit-before"));WritePrivateProfileStringW(L"nr",L"model",L"0",user.c_str());a.Start();REQUIRE(a.Ready());CHECK(a.Get(L"model")==0&&a.Get(L"mode")==4);CHECK(stored(L"nr",L"model")==0&&stored(L"dlss",L"mode")==4);a.End();}
    WritePrivateProfileStringW(L"nr",L"model",L"1",user.c_str());
    {Child dead;REQUIRE(spawn(dead,L"owner"));dead.Start();REQUIRE(dead.Ready());dead.Kill();CHECK(fs::exists(marker(dead)));Child recovered;REQUIRE(spawn(recovered,L"owner"));recovered.Start();REQUIRE(recovered.Ready());CHECK(recovered.Get(L"safe")==1&&recovered.Get(L"nr")==0&&stored(L"nr",L"enabled")==1);recovered.End();CHECK(count()==0);}
    {Child frozen;REQUIRE(spawn(frozen,L"freeze"));frozen.Start();REQUIRE(frozen.Ready());frozen.End();CHECK(fs::exists(marker(frozen)));Child recovered;REQUIRE(spawn(recovered,L"explicit-off"));recovered.Start();REQUIRE(recovered.Ready());CHECK(recovered.Get(L"safe")==1&&stored(L"nr",L"enabled")==0);recovered.End();}
    WritePrivateProfileStringW(L"nr",L"enabled",L"1",user.c_str());
    {Child reused;REQUIRE(spawn(reused,L"owner"));HANDLE f=CreateFileW(marker(reused).c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);REQUIRE(f!=INVALID_HANDLE_VALUE);CloseHandle(f);reused.Start();REQUIRE(reused.Ready());CHECK(reused.Get(L"safe")==1);reused.End();}
    {const auto legacy=user.wstring()+L".session";HANDLE f=CreateFileW(legacy.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);REQUIRE(f!=INVALID_HANDLE_VALUE);CloseHandle(f);Child helper;REQUIRE(spawn(helper,L"helper"));helper.Start();REQUIRE(helper.Ready());helper.End();CHECK(fs::exists(legacy));Child migrated;REQUIRE(spawn(migrated,L"owner"));migrated.Start();REQUIRE(migrated.Ready());CHECK(migrated.Get(L"safe")==1&&!fs::exists(legacy));migrated.End();}
    {REQUIRE(SetFileAttributesW(user.c_str(),FILE_ATTRIBUTE_READONLY));Child ro;REQUIRE(spawn(ro,L"local-edit"));ro.Start();REQUIRE(ro.Ready());CHECK(ro.Get(L"saved")==2&&stored(L"nr",L"enabled")==1);ro.End();SetFileAttributesW(user.c_str(),FILE_ATTRIBUTE_NORMAL);}
    // A journal creation failure remains process-local and cannot save or erase recovery.
    {Child blocked;REQUIRE(spawn(blocked,L"owner"));fs::create_directory(marker(blocked));
        const auto prior=user.wstring()+L".session";HANDLE f=CreateFileW(prior.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);REQUIRE(f!=INVALID_HANDLE_VALUE);CloseHandle(f);
        blocked.Start();REQUIRE(blocked.Ready());
        CHECK(blocked.Get(L"primary")==0 && blocked.Get(L"saved")==2 && blocked.Get(L"effects")==1 && blocked.Get(L"safe")==1 && blocked.Get(L"nr")==0);
        blocked.End();CHECK(fs::exists(prior));fs::remove(marker(blocked));fs::remove(prior);}
    // An unavailable exit observer must not leave a false crash journal.
    {Child missing;REQUIRE(spawn(missing,L"hook-failed"));missing.Start();REQUIRE(missing.Ready());
        CHECK(missing.Get(L"primary")==0 && missing.Get(L"saved")==2 && missing.Get(L"effects")==1);missing.End();CHECK(count()==0);}
    // Clean close/delete and a new election are serialized: no false crash.
    for(unsigned n=0;n<4;++n){Child a,b;REQUIRE(spawn(a,L"owner"));a.Start();REQUIRE(a.Ready());REQUIRE(spawn(b,L"owner"));
        SetEvent(a.stop);b.Start();REQUIRE(b.Ready());CHECK(b.Get(L"safe")==0);a.End();b.End();}
    // One local renderer can remain active while a dead primary is recovered.
    {Child a,local,c;REQUIRE(spawn(a,L"owner"));a.Start();REQUIRE(a.Ready());REQUIRE(spawn(local,L"owner"));local.Start();REQUIRE(local.Ready());
        CHECK(local.Get(L"primary")==0);a.Kill();REQUIRE(spawn(c,L"owner"));c.Start();REQUIRE(c.Ready());
        CHECK(c.Get(L"primary")==1 && c.Get(L"safe")==1);c.End();local.End();}
    CHECK(count()==0);REQUIRE(user.parent_path().filename()==tag&&root.filename()==tag&&fs::equivalent(root.parent_path(),fs::path(temp)));
    fs::remove_all(user.parent_path());fs::remove_all(root);return test::finish("session_ownership_test");
}
