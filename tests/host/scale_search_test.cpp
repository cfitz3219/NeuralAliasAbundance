// The Search, end to end with no game and NO slider moves: a pretend game keeps its render scale at 100% in
// a settings object that carries the setting's name; hundreds of look-alike objects carry other "scale"
// names and the same value, and the renderer keeps a copy beside the 3D picture's size. NeuralAlias must
// find the one number the game obeys by itself (starting value, names, a 5-point nudge in halving groups),
// put every value back, and go on to the writing trial.
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
struct Settings { uint64_t tag = 0x5345545453455453ull; char name[16] = "render_scale"; int quality = 3; float render_scale = 1.0f; int other = 7; };
struct Decoy { char name[24] = "ui_scale_factor"; float value = 1.0f; int other = 0; };
// "--copy": the game also keeps what is applied now, under a render-scale name of its own, and acts only when
// the setting differs from it (moving both together cancels out).
struct Applied { char name[24] = "render_scale_applied"; float value = 1.0f; int other = 0; };
constexpr int kDecoys = 300;
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
Decoy* g_fake_decoys = nullptr;
Applied* g_fake_applied = nullptr;
bool g_copy_mode = false;

int main(int argc, char** argv)
{
    g_copy_mode = argc > 1 && std::strcmp(argv[1], "--copy") == 0;
    std::printf("pretend game: %s\n", g_copy_mode ? "acts only when the setting differs from its applied copy" : "reads the setting every frame");
    using namespace na::scale;
    constexpr uint32_t kOutW = 1920, kOutH = 1080;
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    wchar_t tag[64]; swprintf_s(tag, L"na-scale-search-%lu", GetCurrentProcessId());
    const std::wstring log = std::wstring(temp) + tag + L".log", verdict = std::wstring(temp) + tag + L".txt";
    DeleteFileW(log.c_str()); DeleteFileW(verdict.c_str());

    g_fake_decoys = new Decoy[kDecoys];
    g_fake_settings = new Settings; // after the look-alikes: not the first candidate
    g_fake_renderer = new Renderer;
    Decoy* more = new Decoy[kDecoys];
    for (int i = 0; i < kDecoys; ++i) strcpy_s(more[i].name, "ui_resolution_scale"); // precise look-alikes
    g_fake_applied = new Applied;
    timeBeginPeriod(1);
    std::thread game([] {
        while (g_run.load()) {
            static float in_use = 1.0f;
            float s = g_fake_settings->render_scale;
            if (g_copy_mode) {
                if (s != g_fake_applied->value) { in_use = s; g_fake_applied->value = s; } // the apply step
                s = in_use;
            }
            if (!(s >= 0.25f)) s = 0.25f;
            if (s > 2.0f) s = 2.0f;
            g_fake_renderer->world_w = int32_t(std::lround(kOutW * s));
            g_fake_renderer->world_h = int32_t(std::lround(kOutH * s));
            g_fake_renderer->scale_copy = s;
            witness::OnWorldViewport("TEST", float(g_fake_renderer->world_w), float(g_fake_renderer->world_h));
            Sleep(8);
        }
    });

    witness::NoteOutput(kOutW, kOutH);
    witness::SetTestMode(true);
    witness::SetSearch(true);
    witness::Start(log.c_str(), verdict.c_str());
    expect(witness::Active(), "the witness started");

    std::string text;
    const uint64_t began = GetTickCount64();
    // The "player" never touches anything.
    const bool finished = wait_for(verdict, "How to reach the working number", 240, &text) || text.find("SEARCH: NOT FOUND") != std::string::npos ||
                          text.find("SEARCH: STOPPED") != std::string::npos || text.find("HIJACK: FAIL") != std::string::npos;
    std::printf("the run took %.1f s from start to result\n", double(GetTickCount64() - began) / 1000.0);
    Sleep(1000);
    text = read_all(verdict);
    std::printf("---- result file ----\n%s\n---- end ----\n", text.c_str());
    std::printf("---- log, search lines ----\n");
    {
        const std::string all = read_all(log);
        size_t at = 0;
        while ((at = all.find("search:", at)) != std::string::npos) {
            const size_t end = all.find('\n', at);
            std::printf("%s\n", all.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str());
            if (end == std::string::npos) break;
            at = end;
        }
    }
    std::printf("---- end ----\n");
    expect(finished, "the Search ran to its end");
    expect(text.find("Render scale changes:     0") != std::string::npos, "no slider move was made");
    char real[80];
    std::snprintf(real, sizeof real, "at game memory 0x%llx is the game's render scale",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&g_fake_settings->render_scale)));
    expect(text.find("SEARCH: FOUND") != std::string::npos && text.find(real) != std::string::npos, "the Search found the real setting by itself");
    expect(text.find("RESULT: PASS") != std::string::npos, "the result is PASS");
    expect(text.find("NEXT STAGE: THE CONNECTION") != std::string::npos, "the next stage is The Connection");
    expect(text.find("HIJACK: PASS") != std::string::npos, "the writing trial then moved the world with that number");
    expect(std::fabs(g_fake_settings->render_scale - 1.0f) < 1e-6f, "the game's own value was put back");
    bool untouched = true;
    for (int i = 0; i < kDecoys; ++i) untouched = untouched && g_fake_decoys[i].value == 1.0f && more[i].value == 1.0f;
    expect(untouched, "every look-alike number was put back");
    expect(g_fake_renderer->world_w == int32_t(kOutW), "the 3D world is back at its own size");

    // The handle the Connection will drive: ask for the DLSS levels' sizes and give the game's own back.
    expect(witness::ControlReady(), "the render-scale handle is ready after the Search");
    expect(!witness::RequestScale(0.1), "a size outside 25%..100% is refused");
    auto world_becomes = [&](int32_t want) {
        for (int i = 0; i < 100; ++i) { if (g_fake_renderer->world_w == want) return true; Sleep(10); }
        return false;
    };
    expect(witness::RequestScale(2.0 / 3.0) && world_becomes(1280), "asked 66.7% (Quality): the 3D world became 1280 wide");
    expect(witness::RequestScale(0.58) && world_becomes(1114), "asked 58% (Balanced): the 3D world became 1114 wide");
    expect(witness::RequestScale(0.5) && world_becomes(960), "asked 50% (Performance): the 3D world became 960 wide");
    if (!g_copy_mode) {
        g_fake_settings->render_scale = 1.0f; // the game puts its own value back
        Sleep(100);
        expect(world_becomes(960), "the game's own write was corrected: still 960 wide");
    }
    expect(witness::RequestScale(0.0) && world_becomes(int32_t(kOutW)), "given back: the 3D world is at the game's own size again");
    Sleep(200);
    expect(std::fabs(g_fake_settings->render_scale - 1.0f) < 1e-6f, "the game's own value is in place");

    g_run.store(false);
    game.join();
    timeEndPeriod(1);
    DeleteFileW(log.c_str()); DeleteFileW(verdict.c_str());
    std::printf(g_fail ? "scale_search_test: %d failure(s)\n" : "scale_search_test: ok\n", g_fail);
    std::fflush(stdout);
    ExitProcess(g_fail ? 1u : 0u); // the witness's threads are detached
}
