// Gate 2 R-306: when the real Streamline export a wrapper needs is missing,
// neither a 1.x caller (bool) nor a 2.x caller (sl::Result, 0 = ok) may see
// success for a call that never ran; a real nonzero 2.x error passes through.
// argv[1] = path of the proxy (sl.interposer.dll). The minimal stand-in
// sl_interposer_real.dll sits next to this executable.
#include "test_support.hpp"
#include "host_dx12/fg/camera.hpp"
#include <cstddef>
#include <cstring>

int main(int argc, char** argv)
{
    REQUIRE(argc > 1);
    HMODULE proxy = LoadLibraryA(argv[1]);
    REQUIRE(proxy != nullptr);
    // 1.x view of slInit / slEvaluateFeature: bool -> must read false.
    auto init1 = reinterpret_cast<bool(__cdecl*)(const void*, int)>(GetProcAddress(proxy, "slInit"));
    auto eval1 = reinterpret_cast<bool(__cdecl*)(void*, uint32_t, uint32_t, uint32_t)>(GetProcAddress(proxy, "slEvaluateFeature"));
    // 2.x view: sl::Result -> must be nonzero (an error).
    auto init2 = reinterpret_cast<uint32_t(__cdecl*)(const void*, uint64_t)>(GetProcAddress(proxy, "slInit"));
    auto eval2 = reinterpret_cast<uint32_t(__cdecl*)(uint32_t, const void*, const void**, uint32_t, void*)>(GetProcAddress(proxy, "slEvaluateFeature"));
    auto setc2 = reinterpret_cast<uint32_t(__cdecl*)(const void*, const void*, const void*)>(GetProcAddress(proxy, "slSetConstants"));
    auto getfs1 = reinterpret_cast<bool(__cdecl*)(uint32_t, const void*, void*)>(GetProcAddress(proxy, "slGetFeatureSettings"));
    REQUIRE(init1 && eval1 && init2 && eval2 && setc2 && getfs1);
    int prefs = 0;
    CHECK(init1(&prefs, 1) == false);
    HMODULE real = GetModuleHandleW(L"sl_interposer_real.dll");
    REQUIRE(real != nullptr && GetProcAddress(real, "SlStubMinMarker") != nullptr); // the minimal stand-in was loaded
    CHECK(init2(&prefs, 0x0002000E0010C0DEull) != 0);
    CHECK(eval1(nullptr, 1, 2, 0) == false);
    CHECK(eval2(1, nullptr, nullptr, 0, nullptr) != 0);
    uint32_t settings = 0;
    CHECK(getfs1(0, &prefs, &settings) == false && settings == 0);
    // A real 2.x error from the runtime is passed back unchanged, and nothing
    // is recorded. R-310: the argument is a COMPLETE, valid 2.x Constants
    // object (plus FrameToken and ViewportHandle), so the proxy may read all
    // of it; recording is refused only because the call failed.
    struct Header { void* next; uint8_t guid[16]; size_t version; };
    struct Constants2 { Header h; float body[103]; uint8_t flags[7]; uint8_t pad; float sep; } c2{};
    static_assert(offsetof(Constants2, body) == 32 && offsetof(Constants2, flags) == 444, "2.x layout");
    const uint8_t cguid[16] = {0xd7, 0x5a, 0xd3, 0xdc, 0x4a, 0x4e, 0xad, 0x4b, 0xa9, 0x0c, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe};
    memcpy(c2.h.guid, cguid, 16);
    c2.h.version = 2;
    {
        na::fg::Camera cam{};
        float v2c[16] = {3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float c2v[16] = {1.f / 3, 0, 0, 0, 0, 1.f / 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(cam.view_to_clip, v2c, sizeof(v2c));
        memcpy(cam.clip_to_view, c2v, sizeof(c2v));
        memcpy(cam.clip_to_lens, id, sizeof(id));
        memcpy(cam.clip_to_previous, id, sizeof(id));
        memcpy(cam.previous_to_clip, id, sizeof(id));
        cam.jitter[0] = 0.25f; cam.jitter[1] = -0.125f;
        cam.motion_scale[0] = 1.f; cam.motion_scale[1] = 1.f;
        cam.position[0] = 1.f; cam.forward[2] = 1.f; cam.up[1] = 1.f; cam.right[0] = 1.f;
        cam.near_plane = 0.1f; cam.far_plane = 1000.f; cam.fov = 1.0f; cam.aspect = 1.777f;
        cam.inverted = 1; cam.jittered = 1;
        REQUIRE(na::fg::ValidCamera(cam)); // recording would happen if the call had succeeded
        memcpy(c2.body, &cam, sizeof(c2.body));
        c2.flags[0] = 1; c2.flags[6] = 1;
    }
    struct Token { const void* const* vtbl; Header h; } frame_token{};
    using TokenOp = uint32_t(__cdecl*)(const void*);
    static const TokenOp token_vtbl[1] = {[](const void*) -> uint32_t { return 41; }};
    frame_token.vtbl = reinterpret_cast<const void* const*>(token_vtbl);
    const uint8_t tguid[16] = {0x35, 0x0f, 0x0a, 0x83, 0x84, 0xdb, 0x71, 0x41, 0xa8, 0x04, 0x59, 0xb2, 0x06, 0x49, 0x9b, 0x18};
    memcpy(frame_token.h.guid, tguid, 16);
    struct Viewport { Header h; uint32_t value; } vp{};
    const uint8_t vguid[16] = {0x35, 0x64, 0x1b, 0x17, 0x3c, 0x9b, 0xc8, 0x4f, 0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4};
    memcpy(vp.h.guid, vguid, 16);
    CHECK(setc2(&c2, &frame_token, &vp) == 5);
    auto find = reinterpret_cast<bool(*)(uint64_t, uint32_t, void*)>(GetProcAddress(proxy, "NeuralAliasSLProxyFindCamera"));
    REQUIRE(find != nullptr);
    na::fg::Camera got{};
    CHECK(!find(42, 0, &got)); // FrameToken 41 + 1, viewport 0: the failed call stored nothing
    FreeLibrary(proxy);
    return test::finish("sl_proxy_missing_test");
}
