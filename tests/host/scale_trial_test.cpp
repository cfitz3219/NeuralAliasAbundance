// Scale witness, the writing trial, end to end with no game: a pretend game keeps its render scale in a
// settings object on the heap (reached through a pointer in the program) and sizes its "3D world" from it
// every frame, as Crusader Kings III does. The "player" changes it twice so the finder can confirm the
// number; then NeuralAlias must set it by itself, see the world follow, put it back, tell the real setting
// from a copy the game overwrites, and find a route to it from the program.
#include "host_dx12/scale/scale_witness.hpp"
#include <windows.h>
#include <timeapi.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {
struct Settings { uint64_t tag = 0x5345545453455453ull; int quality = 3; float render_scale = 1.0f; int other = 7; };
struct Renderer { int32_t world_w = 0, world_h = 0; float scale_copy = 1.0f; };
std::atomic<bool> g_run{true};
int g_fail = 0;
void expect(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }

std::string read_all(const std::wstring& path)
{
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return text;
    char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return text;
}
bool wait_for(const std::wstring& path, const char* needle, int seconds, std::string* text)
{
    for (int i = 0; i < seconds * 4; ++i) {
        *text = read_all(path);
        if (text->find(needle) != std::string::npos) return true;
        Sleep(250);
    }
    return false;
}
} // namespace

// In the program's own writable data: the fixed place a route starts from.
Settings* g_fake_settings = nullptr;
Renderer* g_fake_renderer = nullptr;

int main()
{
    using namespace na::scale;
    constexpr uint32_t kOutW = 1920, kOutH = 1080;
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    wchar_t tag[64]; swprintf_s(tag, L"na-scale-trial-%lu", GetCurrentProcessId());
    const std::wstring log = std::wstring(temp) + tag + L".log", verdict = std::wstring(temp) + tag + L".txt";
    DeleteFileW(log.c_str()); DeleteFileW(verdict.c_str());

    g_fake_settings = new Settings;
    g_fake_renderer = new Renderer;
    timeBeginPeriod(1); // as games do
    std::thread game([] {
        while (g_run.load()) {
            float s = g_fake_settings->render_scale; // read every frame: a write takes effect on the next one
            if (!(s >= 0.25f)) s = 0.25f;
            if (s > 2.0f) s = 2.0f;
            g_fake_renderer->world_w = int32_t(std::lround(kOutW * s));
            g_fake_renderer->world_h = int32_t(std::lround(kOutH * s));
            g_fake_renderer->scale_copy = s; // a copy the game overwrites: writing it changes nothing
            witness::OnWorldViewport("TEST", float(g_fake_renderer->world_w), float(g_fake_renderer->world_h));
            Sleep(8);
        }
    });

    witness::NoteOutput(kOutW, kOutH);
    witness::SetTestMode(true);
    witness::SetTrial(true);
    witness::Start(log.c_str(), verdict.c_str());
    expect(witness::Active(), "the witness started");

    std::string text;
    expect(wait_for(verdict, "3D world seen:            yes", 30, &text), "the pretend game's world is seen");
    Sleep(1500);
    g_fake_settings->render_scale = 0.7f; // the player moves the slider
    expect(wait_for(verdict, "Render scale changes:     1", 40, &text), "the first change is noticed");
    Sleep(1500);
    g_fake_settings->render_scale = 0.5f;
    expect(wait_for(verdict, "RESULT: PASS", 40, &text), "the finder confirms a scale number after the second change");

    // Now NeuralAlias on its own: no more changes from the "player".
    const bool finished = wait_for(verdict, "How to reach the working number", 240, &text) ||
                          text.find("HIJACK: FAIL") != std::string::npos || text.find("HIJACK: STOPPED") != std::string::npos;
    Sleep(1000);
    text = read_all(verdict);
    std::printf("---- result file ----\n%s\n---- end ----\n", text.c_str());
    expect(finished, "the writing trial ran to its end");
    expect(text.find("HIJACK: PASS") != std::string::npos, "NeuralAlias moved the world by writing the number itself");
    char real[64];
    std::snprintf(real, sizeof real, "0x%llx: was 0.5", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&g_fake_settings->render_scale)));
    const size_t at = text.find(real);
    expect(at != std::string::npos && text.compare(text.find("->", at), 12, "-> responded") == 0, "the real setting answered");
    std::snprintf(real, sizeof real, "0x%llx: was 0.5", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&g_fake_renderer->scale_copy)));
    const size_t copy_at = text.find(real);
    expect(copy_at == std::string::npos || text.compare(text.find("->", copy_at), 10, "-> ignored") == 0,
           "the copy the game overwrites did not count as the setting");
    expect(std::fabs(g_fake_settings->render_scale - 0.5f) < 1e-6f, "the game's own value was put back");
    expect(text.find("scale_trial_test.exe+0x") != std::string::npos && text.find("routes)") != std::string::npos,
           "a route from the program to the setting was found");

    g_run.store(false);
    game.join();
    timeEndPeriod(1);
    DeleteFileW(log.c_str()); DeleteFileW(verdict.c_str());
    std::printf(g_fail ? "scale_trial_test: %d failure(s)\n" : "scale_trial_test: ok\n", g_fail);
    std::fflush(stdout);
    ExitProcess(g_fail ? 1u : 0u); // the witness's threads are detached
}
