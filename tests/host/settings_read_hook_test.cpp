// P4-4 read-time render-scale substitution through real KernelBase hooks: a settings file read
// through CreateFileW/ReadFile gets the configured value in memory; the file on disk keeps the
// original; a save by the "game" of our value writes the original back; other files and the
// hook-off case are untouched.
#include "test_support.hpp"
#include "host_dx12/scale/settings_read_hook.hpp"

#include <windows.h>
#include <cstdlib>
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
    // Whole file: the log outgrows any fixed buffer (CI's longer temp path cut the last line).
    std::string s;
    char buf[4096];
    DWORD r = 0;
    while (ReadFile(f, buf, sizeof buf, &r, nullptr) && r) s.append(buf, r);
    CloseHandle(f);
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
    CHECK(seen.size() == ini.size() && seen.find("RenderScale = 0.666668\r\n") != std::string::npos); // tagged for live switching
    CHECK(read_all(other) == ini); // not a settings-file extension
    const auto st = na::scale::GetReadHookStats();
    std::printf("stats: opened %llu patched %llu note \"%s\" file \"%s\"\n", (unsigned long long)st.files_opened,
                (unsigned long long)st.reads_patched, st.note, st.file);
    CHECK(st.reads_patched >= 1 && st.original == 1.0 && std::string(st.key) == "RenderScale");

    // Live switch: the "game" parsed the marked value into its heap twice: a menu copy (float) and
    // the control the renderer reads (double). Only the control moves the picture (J2 stand-in:
    // "extent changed" = the control holds the request). Volatile so the stores really happen.
    static float* volatile game_f = nullptr;
    static double* volatile game_d = nullptr;
    {
        const size_t at = seen.find("\nRenderScale = ") + 15; // not the comment line above it
        game_f = new float(0);
        game_d = new double(0);
        *game_f = static_cast<float>(std::atof(seen.c_str() + at));
        *game_d = std::atof(seen.c_str() + at);
        const float marker_f = *game_f;
        using na::scale::LiveResult;
        CHECK(na::scale::SetLiveFraction(0.5) == LiveResult::Scanning); // never scans on the caller's thread
        CHECK(na::scale::WaitLiveScan(60000));
        auto ls = na::scale::GetLiveStats();
        std::printf("live scan: %u candidates (float %u double %u), %llu KB in %llu ms, regions %u writable %u stacks %u, copies at %p %p, \"%s\"\n",
                    ls.candidates, ls.float_hits, ls.double_hits, (unsigned long long)ls.scanned_kb, (unsigned long long)ls.scan_ms, ls.regions,
                    ls.writable_regions, ls.stack_allocations_skipped, static_cast<void*>(game_f), static_cast<void*>(game_d), ls.note);
        CHECK(ls.tagged && ls.candidates == 2 && ls.float_hits == 1 && ls.double_hits == 1); // nothing of ours
        // One candidate at a time; a candidate that did not move the picture is put back.
        int tries = 0;
        for (LiveResult r = ls.result; r == LiveResult::Trying && tries < 8; ++tries) {
            const int written = (*game_f == 0.5f) + (*game_d == 0.5);
            CHECK(written == 1 || (written == 0 && ls.candidates > 2)); // never two at once
            const bool moved = *game_d == 0.5;
            na::scale::ReportLiveOutcome(moved);
            if (moved) break;
            CHECK(*game_f == marker_f); // the menu copy is back exactly as it was
            r = na::scale::SetLiveFraction(0.5);
        }
        ls = na::scale::GetLiveStats();
        std::printf("live confirm: control %d after %d tr%s, restores %u, \"%s\"\n", ls.control, tries + 1, tries ? "ies" : "y", ls.restores, ls.note);
        CHECK(ls.control >= 0 && *game_d == 0.5 && *game_f == marker_f);
        // The confirmed control alone is written from now on.
        CHECK(na::scale::SetLiveFraction(1.0 / 1.72) == LiveResult::Applied && *game_d == 1.0 / 1.72 && *game_f == marker_f);
        // The user moved the game's slider: the control is no longer ours and is left alone.
        *game_d = 0.75;
        CHECK(na::scale::SetLiveFraction(0.5) == LiveResult::NoLongerOurs && *game_d == 0.75);
        *game_d = 1.0 / 1.72; // (the game's own value again, for the save below)
    }

    // The "game" saves what it read: the disk keeps the user's original value.
    CHECK(write_all(opt, seen));
    CHECK(na::scale::GetReadHookStats().writes_restored >= 1);
    na::scale::Configure(0, nullptr);
    CHECK(read_all(opt) == ini);
    // A live value we wrote, saved in the game's own formatting, is put back too.
    {
        std::string live = ini;
        live.replace(live.find("1.000000"), 8, "0.581395"); // 1/1.72, six decimals
        na::scale::Configure(0.5, log.c_str());
        CHECK(write_all(opt, live));
        na::scale::Configure(0, nullptr);
        CHECK(read_all(opt) == ini);
    }
    // An asynchronous save is forwarded untouched (its buffer must outlive the call) and logged.
    {
        na::scale::Configure(0.5, log.c_str());
        const HANDLE f = CreateFileW(opt.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, nullptr);
        CHECK(f != INVALID_HANDLE_VALUE);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD w = 0;
        if (!WriteFile(f, seen.data(), static_cast<DWORD>(seen.size()), nullptr, &ov) && GetLastError() == ERROR_IO_PENDING)
            GetOverlappedResult(f, &ov, &w, TRUE);
        CloseHandle(ov.hEvent);
        CloseHandle(f);
        na::scale::Configure(0, nullptr);
        CHECK(read_all(opt) == seen);
        CHECK(write_all(opt, ini));
    }
    // A same-named file in another folder is a different file: not restored.
    {
        const std::wstring dir = temp_path(L"na_scale_test_dir");
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring twin = dir + L"\\na_scale_test_GraphicsOptions.ini";
        na::scale::Configure(0.5, log.c_str());
        CHECK(write_all(twin, seen));
        na::scale::Configure(0, nullptr);
        CHECK(read_all(twin) == seen);
        DeleteFileW(twin.c_str());
        RemoveDirectoryW(dir.c_str());
    }
    // A value the user changed in the game's menu is saved as it is.
    std::string user = ini;
    user.replace(user.find("1.000000"), 8, "0.800000");
    na::scale::Configure(0.5, log.c_str());
    CHECK(write_all(opt, user));
    na::scale::Configure(0, nullptr);
    CHECK(read_all(opt) == user);

    // A field too short to carry a marker: startup substitution works, live switching says so.
    {
        CHECK(write_all(opt, "RenderScale=1.0\r\n"));
        na::scale::Configure(0.5, log.c_str());
        CHECK(read_all(opt) == "RenderScale=0.5\r\n");
        CHECK(!na::scale::GetLiveStats().tagged);
        CHECK(na::scale::SetLiveFraction(1.0 / 1.5) == na::scale::LiveResult::NoTag);
        na::scale::Configure(0, nullptr);
    }
    // A read through a handle whose open we never saw (SE5 2026-11-02: GraphicsOptions.ini opened
    // through CreateFileW, read elsewhere): recognised by its bytes and path, adopted, patched.
    {
        CHECK(write_all(opt, ini));
        na::scale::Configure(na::scale::FractionForMode(1), log.c_str());
        const HANDLE f = CreateFileW(opt.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE dup = nullptr;
        CHECK(f != INVALID_HANDLE_VALUE &&
              DuplicateHandle(GetCurrentProcess(), f, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS));
        CloseHandle(f);
        std::string s(4096, '\0');
        DWORD r = 0;
        CHECK(dup && ReadFile(dup, s.data(), static_cast<DWORD>(s.size()), &r, nullptr));
        if (dup) CloseHandle(dup);
        s.resize(r);
        std::printf("read through an unseen handle: %s\n", s.c_str());
        CHECK(s.find("\nRenderScale = 0.666668\r\n") != std::string::npos);
        CHECK(na::scale::GetReadHookStats().handles_adopted >= 1);
        CHECK(read_all(other) == ini); // a non-settings file with the same bytes is still left alone
        na::scale::Configure(0, nullptr);
    }
    // Mapped read (SE5 2026-11-02 maps GraphicsOptions.ini): the view shows our value in private
    // copy-on-write pages; the file on disk keeps its own.
    {
        CHECK(write_all(opt, ini));
        na::scale::Configure(na::scale::FractionForMode(1), log.c_str());
        const HANDLE f = CreateFileW(opt.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(f != INVALID_HANDLE_VALUE);
        const HANDLE m = f != INVALID_HANDLE_VALUE ? CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
        CHECK(m != nullptr);
        const char* v = m ? static_cast<const char*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0)) : nullptr;
        CHECK(v != nullptr);
        if (v) {
            const std::string seen_map(v, ini.size());
            std::printf("mapped view: %s\n", seen_map.c_str());
            CHECK(seen_map.find("\nRenderScale = 0.666668\r\n") != std::string::npos);
            UnmapViewOfFile(v);
        }
        if (m) CloseHandle(m);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        na::scale::Configure(0, nullptr);
        CHECK(read_all(opt) == ini); // never written back to the file
    }
    delete game_f;
    delete game_d;

    const std::string lg = read_all(log);
    std::printf("log:\n%s", lg.c_str());
    CHECK(lg.find("patched read of") != std::string::npos && lg.find("kept the original") != std::string::npos);
    CHECK(lg.find("asynchronously") != std::string::npos && lg.find("is the control") != std::string::npos);
    CHECK(lg.find("through a handle we did not see opened") != std::string::npos);
    CHECK(lg.find("private copy-on-write pages") != std::string::npos);
    DeleteFileW(opt.c_str());
    DeleteFileW(other.c_str());
    DeleteFileW(log.c_str());
    return test::finish("settings_read_hook_test");
}
