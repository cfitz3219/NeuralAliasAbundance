// Reconstructed 2026-10 (C3). Mirrors WORKLOG-ROUTE-A A3/A8/A17/A71 for the
// sl.interposer forwarding proxy (deployed beside a Streamline game as
// sl.interposer.dll, with the original renamed sl_interposer_real.dll):
//   1. the PE export table carries every generated forwarder plus the typed
//      wrappers and the identity/diagnostic exports;
//   2. without NeuralAliasSL.ini the proxy is a pure forwarder: zero hooks;
//   3. forwarded calls (slIsFeatureSupported, slGetNewFrameToken) reach the
//      real interposer with arguments and out-params intact;
//   4. typed wrappers (slInit, slSetFeatureConstants, slGetFeatureSettings,
//      slSetConstants, slEvaluateFeature) forward untouched;
//   5. a forwarder whose target is missing in the real DLL (slSetTag) fails to
//      resolve cleanly instead of crashing: per-export lazy resolution.
// Usage: sl_proxy_test <sl.interposer.dll> <sl_interposer_real.dll>
#include "test_support.hpp"
#include "host_dx12/fg/camera.hpp"

#include <cstddef>
#include <cstring>

#include <string>
#include <vector>

namespace {
struct StubState { uint32_t inits, set_consts, get_settings, set_constants, evaluates; uint32_t last_feature, last_frame, last_id; const void* last_ptr; uint64_t wide[5]; };

// A camera fg::ValidCamera accepts: inverse matrix pairs, finite values, 0/1 flags.
na::fg::Camera ValidTestCamera(float seed)
{
    na::fg::Camera c{};
    const float s = 2.0f + seed;
    float v2c[16] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float c2v[16] = {1 / s, 0, 0, 0, 0, 1 / s, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(c.view_to_clip, v2c, sizeof(v2c));
    memcpy(c.clip_to_view, c2v, sizeof(c2v));
    memcpy(c.clip_to_lens, id, sizeof(id));
    memcpy(c.clip_to_previous, id, sizeof(id));
    memcpy(c.previous_to_clip, id, sizeof(id));
    c.jitter[0] = 0.25f; c.jitter[1] = -0.125f;
    c.motion_scale[0] = 1.f; c.motion_scale[1] = 1.f;
    c.position[0] = seed; c.forward[2] = 1.f; c.up[1] = 1.f; c.right[0] = 1.f;
    c.near_plane = 0.1f; c.far_plane = 1000.f; c.fov = 1.0f; c.aspect = 1.777f; c.invalid_motion = 0.f;
    c.inverted = 1; c.reset = 0; c.jittered = 1;
    return c;
}

std::vector<std::string> ExportNames(HMODULE m)
{
    std::vector<std::string> names;
    auto base = reinterpret_cast<const uint8_t*>(m);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return names;
    auto exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto rvas = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) names.emplace_back(reinterpret_cast<const char*>(base + rvas[i]));
    return names;
}
bool Has(const std::vector<std::string>& v, const char* n) { for (auto& s : v) if (s == n) return true; return false; }
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { std::printf("usage: sl_proxy_test <proxy dll> <stub dll>\n"); return 2; }

    // Stage both DLLs in a private folder under their deployment names.
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"na_sl_proxy_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring proxy_path = dir + L"\\sl.interposer.dll";
    const std::wstring real_path = dir + L"\\sl_interposer_real.dll";
    REQUIRE(CopyFileW(argv[1], proxy_path.c_str(), FALSE));
    REQUIRE(CopyFileW(argv[2], real_path.c_str(), FALSE));
    REQUIRE(GetFileAttributesW((dir + L"\\NeuralAliasSL.ini").c_str()) == INVALID_FILE_ATTRIBUTES);
    SetDllDirectoryW(dir.c_str()); // forwarders resolve sl_interposer_real.dll from here

    HMODULE proxy = LoadLibraryW(proxy_path.c_str());
    REQUIRE(proxy != nullptr);

    // 1. Export table.
    const auto names = ExportNames(proxy);
    std::printf("proxy exports: %zu\n", names.size());
    CHECK(names.size() >= 260);
    for (const char* n : {"slInit", "slSetFeatureConstants", "slGetFeatureSettings", "slSetConstants", "slEvaluateFeature",
                          "slIsFeatureSupported", "slGetNewFrameToken", "slSetTag", "D3D12CreateDevice",
                          "NeuralAliasSLProxyIdentity", "NeuralAliasSLProxyStats"})
        CHECK(Has(names, n));

    auto identity = reinterpret_cast<const char*(*)()>(GetProcAddress(proxy, "NeuralAliasSLProxyIdentity"));
    REQUIRE(identity != nullptr);
    CHECK(std::strstr(identity(), "NeuralAliasAbundance") != nullptr);

    // 2. Inert without an ini: no hooks installed (give the init thread time to run).
    auto stats = reinterpret_cast<void(*)(uint64_t*, uint64_t*, uint32_t*)>(GetProcAddress(proxy, "NeuralAliasSLProxyStats"));
    REQUIRE(stats != nullptr);
    Sleep(300);
    uint64_t ev = 1, cr = 1; uint32_t hooked = 1;
    stats(&ev, &cr, &hooked);
    CHECK(hooked == 0 && ev == 0 && cr == 0);

    // 3. Forwarded calls reach the real interposer.
    auto supported = reinterpret_cast<int(__cdecl*)(uint32_t, const void*)>(GetProcAddress(proxy, "slIsFeatureSupported"));
    auto token = reinterpret_cast<int(__cdecl*)(void**, const uint32_t*)>(GetProcAddress(proxy, "slGetNewFrameToken"));
    REQUIRE(supported && token);
    CHECK(supported(7, nullptr) == 0x5107);
    void* tok = nullptr; const uint32_t frame = 42;
    CHECK(token(&tok, &frame) == 0);
    CHECK(tok != nullptr && *static_cast<uint32_t*>(tok) == 0xF7A3E);

    HMODULE real = GetModuleHandleW(L"sl_interposer_real.dll");
    REQUIRE(real != nullptr);
    auto st = reinterpret_cast<const StubState*(__cdecl*)()>(GetProcAddress(real, "SlStubState"));
    REQUIRE(st != nullptr);
    CHECK(st()->last_frame == 42);

    // 4. Typed wrappers forward untouched.
    int prefs = 0;
    auto init = reinterpret_cast<bool(__cdecl*)(const void*, int)>(GetProcAddress(proxy, "slInit"));
    auto setfc = reinterpret_cast<bool(__cdecl*)(uint32_t, const void*, uint32_t, uint32_t)>(GetProcAddress(proxy, "slSetFeatureConstants"));
    auto getfs = reinterpret_cast<bool(__cdecl*)(uint32_t, const void*, void*)>(GetProcAddress(proxy, "slGetFeatureSettings"));
    auto setc = reinterpret_cast<bool(__cdecl*)(const void*, uint32_t, uint32_t)>(GetProcAddress(proxy, "slSetConstants"));
    auto evalf = reinterpret_cast<bool(__cdecl*)(void*, uint32_t, uint32_t, uint32_t)>(GetProcAddress(proxy, "slEvaluateFeature"));
    REQUIRE(init && setfc && getfs && setc && evalf);
    CHECK(init(&prefs, 0x1234));
    CHECK(st()->inits == 1 && st()->last_ptr == &prefs && st()->last_id == 0x1234);
    int consts = 0;
    CHECK(setfc(0, &consts, 9, 3));
    CHECK(st()->set_consts == 1 && st()->last_ptr == &consts && st()->last_frame == 9 && st()->last_id == 3);
    uint32_t settings = 0;
    CHECK(getfs(0, &consts, &settings));
    CHECK(st()->get_settings == 1 && settings == 0x5E77);
    na::fg::Camera cam1 = ValidTestCamera(1.f); // a COMPLETE 1.x Constants object (R-307)
    REQUIRE(na::fg::ValidCamera(cam1));
    CHECK(setc(&cam1, 11, 0));
    CHECK(st()->set_constants == 1 && st()->last_frame == 11);
    CHECK(evalf(nullptr, 1, 12, 0));
    CHECK(st()->evaluates == 1 && st()->last_feature == 1 && st()->last_frame == 12);

    {
        auto layout = reinterpret_cast<uint32_t(*)(uint64_t*)>(GetProcAddress(proxy, "NeuralAliasSLProxyLayout"));
        REQUIRE(layout != nullptr);
        uint64_t blocks = 0;
        CHECK(layout(&blocks) == 1 && blocks == 1);
        auto find = reinterpret_cast<bool(*)(uint64_t, uint32_t, void*)>(GetProcAddress(proxy, "NeuralAliasSLProxyFindCamera"));
        REQUIRE(find != nullptr);
        na::fg::Camera got{};
        CHECK(find(12, 0, &got)); // stored under engine frame 11 + 1, viewport 0
        CHECK(got.position[0] == 1.f && got.view_to_clip[0] == 3.f && got.near_plane == 0.1f && got.inverted == 1 && got.jittered == 1);
        CHECK(!find(12, 1, &got)); // another viewport: nothing

        // Truncated input: an object that ends at an inaccessible page is
        // rejected (guarded copy fails), never partially decoded.
        SYSTEM_INFO si{}; GetSystemInfo(&si);
        uint8_t* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, si.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        REQUIRE(pages != nullptr);
        DWORD old_protect = 0;
        REQUIRE(VirtualProtect(pages + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &old_protect));
        uint8_t* tail = pages + si.dwPageSize - 100; // 100 readable bytes, a 1.x block needs 432
        memcpy(tail, &cam1, 100);
        CHECK(setc(tail, 13, 0));
        CHECK(layout(&blocks) == 1 && blocks == 1); // not counted
        CHECK(!find(14, 0, &got));
        // A 2.x header (Constants GUID) whose body runs into the inaccessible page.
        uint8_t* tail2 = pages + si.dwPageSize - 64;
        memset(tail2, 0, 64);
        const uint8_t cguid_t[16] = {0xd7, 0x5a, 0xd3, 0xdc, 0x4a, 0x4e, 0xad, 0x4b, 0xa9, 0x0c, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe};
        memcpy(tail2 + 8, cguid_t, 16);
        CHECK(setc(tail2, 15, 0));
        CHECK(layout(&blocks) == 1 && blocks == 1);
        CHECK(!find(16, 0, &got));
        VirtualFree(pages, 0, MEM_RELEASE);
    }

    // 4b. C7b: Streamline 2.x signatures pass through at full width and its
    // Constants / FrameToken / ViewportHandle are recognised by type GUID.
    {
        auto init2 = reinterpret_cast<uint64_t(__cdecl*)(const void*, uint64_t)>(GetProcAddress(proxy, "slInit"));
        const uint64_t sdk_version = 0x0002000E0010C0DEull; // 2.x kSDKVersion-shaped: high bits set
        init2(&prefs, sdk_version);
        CHECK(st()->wide[1] == sdk_version); // 1.x-typed wrappers truncated this to 32 bits

        auto eval2 = reinterpret_cast<uint64_t(__cdecl*)(uint32_t, const void*, const void**, uint32_t, void*)>(GetProcAddress(proxy, "slEvaluateFeature"));
        const void* inputs[1] = {&consts};
        void* cmd = reinterpret_cast<void*>(0x00007FF612345678ull);
        eval2(1, reinterpret_cast<const void*>(0x00007FF6AABBCCDDull), inputs, 1, cmd);
        CHECK(st()->wide[1] == 0x00007FF6AABBCCDDull && st()->wide[2] == reinterpret_cast<uintptr_t>(inputs) && st()->wide[4] == reinterpret_cast<uintptr_t>(cmd));

        // A 2.x-shaped Constants block, FrameToken (vptr first) and ViewportHandle.
        struct Header { void* next; uint8_t guid[16]; size_t version; };
        struct Constants2 { Header h; float body[103]; uint8_t flags[7]; uint8_t pad; float sep; } c2{};
        static_assert(offsetof(Constants2, body) == 32 && offsetof(Constants2, flags) == 444, "2.x layout");
        const uint8_t cguid[16] = {0xd7, 0x5a, 0xd3, 0xdc, 0x4a, 0x4e, 0xad, 0x4b, 0xa9, 0x0c, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe};
        memcpy(c2.h.guid, cguid, 16);
        c2.h.version = 2;
        {   // a valid camera in the 2.x body: same fields, 7 flag bytes, no notRenderingGameFrames
            const na::fg::Camera cam2 = ValidTestCamera(2.f);
            memcpy(c2.body, &cam2, sizeof(c2.body));
            c2.flags[0] = 1; c2.flags[1] = 0; c2.flags[2] = 0; c2.flags[3] = 0; c2.flags[4] = 0; c2.flags[5] = 0; c2.flags[6] = 1;
        }
        // FrameToken: vtable pointer first (operator uint32_t is entry 0), then BaseStructure.
        struct Token { const void* const* vtbl; Header h; } frame_token{};
        using TokenOp = uint32_t(__cdecl*)(const void*);
        static const TokenOp token_vtbl[1] = {[](const void*) -> uint32_t { return 77; }};
        frame_token.vtbl = reinterpret_cast<const void* const*>(token_vtbl);
        const uint8_t tguid[16] = {0x35, 0x0f, 0x0a, 0x83, 0x84, 0xdb, 0x71, 0x41, 0xa8, 0x04, 0x59, 0xb2, 0x06, 0x49, 0x9b, 0x18};
        memcpy(frame_token.h.guid, tguid, 16);
        struct Viewport { Header h; uint32_t value; } vp{};
        const uint8_t vguid[16] = {0x35, 0x64, 0x1b, 0x17, 0x3c, 0x9b, 0xc8, 0x4f, 0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4};
        memcpy(vp.h.guid, vguid, 16);
        vp.value = 3;
        auto stub_ret = reinterpret_cast<void(__cdecl*)(uint64_t)>(GetProcAddress(real, "SlStubSetConstantsReturn"));
        REQUIRE(stub_ret != nullptr);
        stub_ret(0); // sl::Result::eOk
        auto setc2 = reinterpret_cast<uint64_t(__cdecl*)(const void*, const void*, const void*)>(GetProcAddress(proxy, "slSetConstants"));
        CHECK(setc2(&c2, &frame_token, &vp) == 0);
        CHECK(st()->wide[1] == reinterpret_cast<uintptr_t>(&frame_token) && st()->wide[2] == reinterpret_cast<uintptr_t>(&vp));
        auto layout = reinterpret_cast<uint32_t(*)(uint64_t*)>(GetProcAddress(proxy, "NeuralAliasSLProxyLayout"));
        uint64_t blocks = 0;
        CHECK(layout(&blocks) == 2 && blocks == 2);
        auto find2 = reinterpret_cast<bool(*)(uint64_t, uint32_t, void*)>(GetProcAddress(proxy, "NeuralAliasSLProxyFindCamera"));
        na::fg::Camera got2{};
        CHECK(find2(78, 3, &got2)); // FrameToken 77 + 1, ViewportHandle value 3
        CHECK(got2.position[0] == 2.f && got2.view_to_clip[0] == 4.f && got2.inverted == 1 && got2.jittered == 1 && got2.not_game == 0);
        // A nonzero 2.x runtime error is passed back untouched and nothing is recorded.
        stub_ret(7);
        CHECK(setc2(&c2, &frame_token, &vp) == 7);
        CHECK(layout(&blocks) == 2 && blocks == 2);
        stub_ret(1);
    }

    // 5. Forwarder with a missing target: resolution fails, nothing crashes.
    CHECK(GetProcAddress(proxy, "slSetTag") == nullptr);

    stats(&ev, &cr, &hooked);
    CHECK(hooked == 0); // still a pure forwarder

    // The proxy pins nothing without the ini, so it can unload.
    CHECK(FreeLibrary(proxy) != 0);
    SetDllDirectoryW(nullptr);
    return test::finish("sl_proxy_test");
}
