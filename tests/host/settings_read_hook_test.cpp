// P4-4 read-time render-scale substitution through real KernelBase hooks: a settings file read
// through CreateFileW/ReadFile gets the configured value in memory; the file on disk keeps the
// original; a save by the "game" of our value writes the original back; other files and the
// hook-off case are untouched.
#include "test_support.hpp"
#include "host_dx12/scale/settings_read_hook.hpp"

#include <windows.h>
#include <string>

namespace {
std::wstring temp_path(const wchar_t* name)
{
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + name;
}
bool write_all(const std::wstring& p, const std::string& s)
{
    const HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = WriteFile(f, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
    CloseHandle(f);
    return ok && w == s.size();
}
std::string read_all(const std::wstring& p)
{
    const HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(4096, '\0');
    DWORD r = 0;
    ReadFile(f, s.data(), static_cast<DWORD>(s.size()), &r, nullptr);
    CloseHandle(f);
    s.resize(r);
    return s;
}
} // namespace

int main()
{
    const std::string ini = "[Display Settings]\r\n; RenderScale = 0.5 to 2.0\r\nRenderScale = 1.000000\r\nResolution_Width = 3440\r\n";
    const std::wstring opt = temp_path(L"na_scale_test_GraphicsOptions.ini");
    const std::wstring other = temp_path(L"na_scale_test_data.bin");
    const std::wstring log = temp_path(L"na_scale_test.log");
    DeleteFileW(log.c_str());
    REQUIRE(write_all(opt, ini) && write_all(other, ini));
    REQUIRE(na::scale::InstallSettingsReadHook());

    // Off: untouched.
    na::scale::Configure(0, nullptr);
    CHECK(read_all(opt) == ini);

    na::scale::Configure(na::scale::FractionForMode(1), log.c_str());
    const std::string seen = read_all(opt);
    std::printf("game reads: %s\n", seen.c_str());
    CHECK(seen.size() == ini.size() && seen.find("RenderScale = 0.666667\r\n") != std::string::npos);
    CHECK(read_all(other) == ini); // not a settings-file extension
    const auto st = na::scale::GetReadHookStats();
    std::printf("stats: opened %llu patched %llu note \"%s\" file \"%s\"\n", (unsigned long long)st.files_opened,
                (unsigned long long)st.reads_patched, st.note, st.file);
    CHECK(st.reads_patched >= 1 && st.original == 1.0 && std::string(st.key) == "RenderScale");

    // The "game" saves what it read: the disk keeps the user's original value.
    CHECK(write_all(opt, seen));
    CHECK(na::scale::GetReadHookStats().writes_restored >= 1);
    na::scale::Configure(0, nullptr);
    CHECK(read_all(opt) == ini);
    // A value the user changed in the game's menu is saved as it is.
    std::string user = ini;
    user.replace(user.find("1.000000"), 8, "0.800000");
    na::scale::Configure(0.5, log.c_str());
    CHECK(write_all(opt, user));
    na::scale::Configure(0, nullptr);
    CHECK(read_all(opt) == user);

    const std::string lg = read_all(log);
    std::printf("log:\n%s", lg.c_str());
    CHECK(lg.find("patched read of") != std::string::npos && lg.find("kept the original") != std::string::npos);
    DeleteFileW(opt.c_str());
    DeleteFileW(other.c_str());
    DeleteFileW(log.c_str());
    return test::finish("settings_read_hook_test");
}
