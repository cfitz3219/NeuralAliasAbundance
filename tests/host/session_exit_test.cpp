#include "test_support.hpp"
#include "host_dx12/host_boot/session.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include <filesystem>
#include <string>
int wmain(int argc,wchar_t** argv){
    if(argc==3){
        na::settings::SetIniPath(argv[2]);na::settings::Load();
        if(!na::hooking::Initialize())return 9;
        na::host::StartSession();
        if(!wcscmp(argv[1],L"freeze"))na::settings::MarkSessionFailed("freeze");
        if(!wcscmp(argv[1],L"kill"))TerminateProcess(GetCurrentProcess(),5);
        ExitProcess(0);
    }
    wchar_t temp[MAX_PATH]{},exe[MAX_PATH]{},ini[MAX_PATH]{},scope[80]{};
    GetTempPathW(MAX_PATH,temp);GetModuleFileNameW(nullptr,exe,MAX_PATH);
    swprintf_s(ini,L"%sNA-session-%lu.ini",temp,GetCurrentProcessId());
    swprintf_s(scope,L"NA-session-%lu",GetCurrentProcessId());
    WritePrivateProfileStringW(L"settings",L"scope",scope,ini);
    na::settings::SetIniPath(ini);na::settings::Load();
    const std::wstring marker=std::wstring(na::settings::UserIniPath())+L".session";
    for(const auto mode:{L"clean",L"kill",L"clean",L"freeze",L"clean"}){
        std::wstring command=L"\""+std::wstring(exe)+L"\" "+mode+L" \""+ini+L"\"";
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        REQUIRE(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi));
        const auto wait=WaitForSingleObject(pi.hProcess,10000);
        if(wait!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,9);
        DWORD code=0;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        CHECK(wait==WAIT_OBJECT_0);CHECK(code==(!wcscmp(mode,L"kill")?5u:0u));
        const bool exists=GetFileAttributesW(marker.c_str())!=INVALID_FILE_ATTRIBUTES;
        CHECK(exists==(wcscmp(mode,L"clean")!=0));
    }
    DeleteFileW(marker.c_str());DeleteFileW(ini);DeleteFileW(na::settings::UserIniPath());
    std::filesystem::remove(std::filesystem::path(na::settings::UserIniPath()).parent_path());
    return test::finish("session_exit_test");
}
