// Streamline DLSS mode layer (sl_dlss_mode): this executable plays
// sl.interposer + sl.dlss. It exports slGetFeatureFunction, which hands out
// fake slDLSSGetOptimalSettings / slDLSSSetOptions; the production boundary
// installer runs against this image, and the game-side calls go through the
// pointers the game would have resolved. Proves:
//   - no request: the game's own options pointer and answer pass through;
//   - a mode request rewrites only the mode in a COPY (game struct untouched),
//     and pins the returned dynamic-resolution bounds to the optimal size;
//   - DLSS off in the game's call is never turned on;
//   - SetOptions: presets are written whenever requested; the mode label is
//     rewritten only after the game took a rewritten size this generation;
//   - an unknown struct version passes through untouched.
#include "test_support.hpp"
#include "host_dx12/sl_boundary/sl_boundary.hpp"
#include "host_dx12/sl_boundary/sl_dlss_mode.hpp"

namespace {

volatile int g_sink = 0;
const void* g_seen_options = nullptr;
uint8_t g_seen[96] = {};
int g_optimal_calls = 0, g_set_calls = 0;

const uint8_t kOptionsGuid[16] = {0xe4, 0x26, 0xc8, 0x6a, 0x61, 0x4c, 0x01, 0x41, 0xa9, 0x2d, 0x63, 0x8d, 0x42, 0x10, 0x57, 0xb8};
const uint8_t kOptimalGuid[16] = {0x57, 0x09, 0x1d, 0xef, 0x58, 0xfd, 0xf7, 0x4d, 0xb5, 0x04, 0x8b, 0x69, 0xd8, 0xaa, 0x6b, 0x76};

template <class T> void put(uint8_t* p, size_t off, T v) { std::memcpy(p + off, &v, sizeof(T)); }
template <class T> T get(const uint8_t* p, size_t off) { T v{}; std::memcpy(&v, p + off, sizeof(T)); return v; }

// NVIDIA's ratios per sl::DLSSMode (MaxPerformance 50%, Balanced 58%, MaxQuality 66.7%, UltraPerformance 33.3%, DLAA 100%).
float ratio(uint32_t mode)
{
    switch (mode) {
    case 1: return 0.5f;
    case 2: return 0.58f;
    case 3: return 0.667f;
    case 4: return 0.333f;
    case 6: return 1.0f;
    default: return 0.f;
    }
}

} // namespace

extern "C" __declspec(noinline) int32_t fake_optimal(const void* options, void* settings)
{
    ++g_optimal_calls;
    g_seen_options = options;
    std::memcpy(g_seen, options, 88);
    const auto* o = static_cast<const uint8_t*>(options);
    auto* s = static_cast<uint8_t*>(settings);
    const uint32_t mode = get<uint32_t>(o, 32), ow = get<uint32_t>(o, 36), oh = get<uint32_t>(o, 40);
    const uint32_t w = static_cast<uint32_t>(ow * ratio(mode) + 0.5f), h = static_cast<uint32_t>(oh * ratio(mode) + 0.5f);
    put<uint32_t>(s, 32, w);
    put<uint32_t>(s, 36, h);
    put<uint32_t>(s, 44, ow / 3); // dynamic-resolution range NVIDIA would report
    put<uint32_t>(s, 48, oh / 3);
    put<uint32_t>(s, 52, ow);
    put<uint32_t>(s, 56, oh);
    g_sink = g_sink + 31;
    return 0;
}
extern "C" __declspec(noinline) int32_t fake_set_options(const void*, const void* options)
{
    ++g_set_calls;
    g_seen_options = options;
    std::memcpy(g_seen, options, 88);
    g_sink = g_sink + 37;
    return 0;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t slGetFeatureFunction(uint32_t feature, const char* name, void** fn)
{
    g_sink = g_sink + 41;
    if (feature != 0 || !name || !fn) return 1;
    if (std::strcmp(name, "slDLSSGetOptimalSettings") == 0) { *fn = reinterpret_cast<void*>(&fake_optimal); return 0; }
    if (std::strcmp(name, "slDLSSSetOptions") == 0) { *fn = reinterpret_cast<void*>(&fake_set_options); return 0; }
    return 1;
}

namespace {

struct alignas(8) Options { uint8_t b[88] = {}; };
struct alignas(8) Optimal { uint8_t b[60] = {}; };

Options make_options(uint32_t mode, uint32_t ow, uint32_t oh, uint64_t version = 3, uint32_t preset = 0)
{
    Options o;
    std::memcpy(o.b + 8, kOptionsGuid, 16);
    put<uint64_t>(o.b, 24, version);
    put<uint32_t>(o.b, 32, mode);
    put<uint32_t>(o.b, 36, ow);
    put<uint32_t>(o.b, 40, oh);
    put<float>(o.b, 48, 1.f);
    put<float>(o.b, 52, 1.f);
    for (int i = 0; i < 6; ++i) put<uint32_t>(o.b, 60 + 4 * i, preset);
    return o;
}

Optimal make_optimal()
{
    Optimal s;
    std::memcpy(s.b + 8, kOptimalGuid, 16);
    put<uint64_t>(s.b, 24, 1);
    return s;
}

} // namespace

int main()
{
    using namespace na::slb::dlss;
    CHECK(SlModeForDlssMode(0) == kSlDlaa && SlModeForDlssMode(1) == kSlMaxQuality && SlModeForDlssMode(2) == kSlBalanced);
    CHECK(SlModeForDlssMode(3) == kSlMaxPerformance && SlModeForDlssMode(4) == kSlUltraPerformance && SlModeForDlssMode(9) == kSlOff);

    REQUIRE(na::slb::InstallForModuleForTests(GetModuleHandleW(nullptr)));
    using GffFn = int32_t(__cdecl*)(uint32_t, const char*, void**);
    using OptimalFn = int32_t(__cdecl*)(const void*, void*);
    using SetFn = int32_t(__cdecl*)(const void*, const void*);
    auto gff = reinterpret_cast<GffFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "slGetFeatureFunction")));
    REQUIRE(gff);
    void* optimal_fn = nullptr;
    void* set_fn = nullptr;
    CHECK(gff(0, "slDLSSGetOptimalSettings", &optimal_fn) == 0 && optimal_fn);
    CHECK(gff(0, "slDLSSSetOptions", &set_fn) == 0 && set_fn);
    REQUIRE(WaitInstalledForTests(3000));
    auto optimal = reinterpret_cast<OptimalFn>(optimal_fn);
    auto set = reinterpret_cast<SetFn>(set_fn);
    const uint8_t viewport[36] = {};

    // No request: pass-through.
    Options q = make_options(kSlMaxQuality, 3440, 1440);
    Optimal a = make_optimal();
    CHECK(optimal(q.b, a.b) == 0);
    CHECK(g_seen_options == q.b);
    CHECK(get<uint32_t>(a.b, 32) == 2294 && get<uint32_t>(a.b, 44) == 3440 / 3); // NVIDIA's answer and range untouched

    // Request Balanced while the game asks for Quality.
    ModeRequest req;
    req.own_mode = true;
    req.sl_mode = kSlBalanced;
    SetModeRequest(req);
    // SetOptions before the game took our size: the label is NOT rewritten.
    CHECK(set(viewport, q.b) == 0);
    CHECK(g_seen_options == q.b);
    a = make_optimal();
    CHECK(optimal(q.b, a.b) == 0);
    CHECK(g_seen_options != q.b && get<uint32_t>(g_seen, 32) == kSlBalanced);
    CHECK(get<uint32_t>(g_seen, 36) == 3440 && get<uint32_t>(g_seen, 40) == 1440); // rest of the copy kept
    CHECK(get<uint32_t>(q.b, 32) == kSlMaxQuality);                                  // game's struct untouched
    const uint32_t bw = static_cast<uint32_t>(3440 * 0.58f + 0.5f), bh = static_cast<uint32_t>(1440 * 0.58f + 0.5f);
    CHECK(get<uint32_t>(a.b, 32) == bw && get<uint32_t>(a.b, 36) == bh);
    CHECK(get<uint32_t>(a.b, 44) == bw && get<uint32_t>(a.b, 48) == bh && get<uint32_t>(a.b, 52) == bw && get<uint32_t>(a.b, 56) == bh); // pinned
    ModeState st = GetModeState();
    CHECK(st.size_followed && st.game_mode == kSlMaxQuality && st.sent_mode == kSlBalanced && st.rewritten_this_generation == 1);
    CHECK(st.optimal_w == bw && st.min_w == bw && st.max_w == bw);
    // Now the label follows the size.
    CHECK(set(viewport, q.b) == 0);
    CHECK(g_seen_options != q.b && get<uint32_t>(g_seen, 32) == kSlBalanced);
    CHECK(GetModeState().set_sent_mode == kSlBalanced);

    // DLSS off in the game's call stays off.
    Options off = make_options(kSlOff, 3440, 1440);
    a = make_optimal();
    CHECK(optimal(off.b, a.b) == 0);
    CHECK(g_seen_options == off.b);

    // Presets: written into all six per-mode fields, mode untouched without a mode request.
    ModeRequest preset;
    preset.own_preset = true;
    preset.preset = 13; // M
    SetModeRequest(preset);
    CHECK(set(viewport, q.b) == 0);
    CHECK(g_seen_options != q.b && get<uint32_t>(g_seen, 32) == kSlMaxQuality);
    for (int i = 0; i < 6; ++i) CHECK(get<uint32_t>(g_seen, 60 + 4 * i) == 13);
    CHECK(get<uint32_t>(q.b, 60) == 0); // game's struct untouched

    // Unknown (newer) struct version: untouched even with a request.
    SetModeRequest(req);
    Options future = make_options(kSlMaxQuality, 3440, 1440, 4);
    a = make_optimal();
    CHECK(optimal(future.b, a.b) == 0);
    CHECK(g_seen_options == future.b);
    CHECK(set(viewport, future.b) == 0);
    CHECK(g_seen_options == future.b);

    // Request withdrawn: pass-through again, new generation.
    const uint64_t gen = GetModeState().generation;
    SetModeRequest({});
    CHECK(GetModeState().generation == gen + 1 && !GetModeState().size_followed);
    a = make_optimal();
    CHECK(optimal(q.b, a.b) == 0);
    CHECK(g_seen_options == q.b);
    return test::finish("sl_dlss_mode_test");
}
