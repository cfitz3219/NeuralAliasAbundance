// SL boundary (claude/sl-boundary): the game-facing Streamline seam.
//
// No game and no Streamline: this executable plays sl.interposer.dll. It
// exports slSetTag/slSetTagForFrame and defines sl::interposer::DXGISwapChain
// (same RTTI name and vtable slot order as Streamline's proxy), then runs the
// production installer against itself. Proves:
//   1. RTTI discovery finds the proxy vtable and hooks Present/Present1/ResizeBuffers*;
//   2. tag calls are decoded (resource, declared state, lifecycle, extent,
//      frame token, viewport, command buffer) and reported BEFORE the original,
//      which still runs once with the game's own arguments;
//   3. an unrecognised tag layout is reported undecoded and still forwarded;
//   5. native DLSS-G control: slGetFeatureFunction lookups get the game's
//      slDLSSGSetOptions/GetState hooked; a request rewrites only mode/count in
//      a copy (all other fields kept); out-of-range or unsupported requests
//      pass the game's own options through untouched.
//   4. Present: Real advances the serial, TEST makes no callback, a
//      WAS_STILL_DRAWING result makes the next Present a Retry of the same
//      serial, a new proxy and a successful ResizeBuffers advance the epoch,
//      and the base chain comes from StreamlineRetrieveBaseInterface.
#include "test_support.hpp"
#include "host_dx12/sl_boundary/sl_boundary.hpp"
#include "host_dx12/sl_boundary/sl_tags.hpp"
#include "host_dx12/sl_proxy/sl_abi.hpp"

#include <vector>

namespace {

constexpr uint32_t kAllShader = static_cast<uint32_t>(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
constexpr uint32_t kNonPixel = static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
constexpr uint32_t kHdr10 = static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM);

volatile int g_sink = 0;
int g_tag_frame_calls = 0, g_tag_calls = 0;
uint64_t g_order = 0, g_original_order = 0, g_callback_order = 0;
const void* g_orig_seen_tags = nullptr;
uint32_t g_orig_seen_n = 0;
void* g_orig_seen_cmd = nullptr;
HRESULT g_next_present_hr = S_OK;
int g_present_calls = 0, g_present1_calls = 0, g_resize_calls = 0;

} // namespace

// ---- the "Streamline" exports ----
extern "C" __declspec(dllexport) __declspec(noinline) int32_t slSetTagForFrame(const void*, const void*, const void* tags, uint32_t n, void* cmd)
{
    ++g_tag_frame_calls;
    g_original_order = ++g_order;
    g_orig_seen_tags = tags;
    g_orig_seen_n = n;
    g_orig_seen_cmd = cmd;
    g_sink = g_sink + 3;
    return 0;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t slSetTag(const void*, const void* tags, uint32_t n, void*)
{
    ++g_tag_calls;
    g_original_order = ++g_order;
    g_orig_seen_tags = tags;
    g_orig_seen_n = n;
    g_sink = g_sink + 5;
    return 0;
}

// ---- the "Streamline" DLSS-G plugin functions ----
namespace {
const void* g_fg_seen_options = nullptr;
uint8_t g_fg_seen_bytes[128] = {};
int g_fg_set_calls = 0, g_fg_get_calls = 0;
uint32_t g_fg_max = 5;
uint8_t g_fg_dynamic = 0;
}
extern "C" __declspec(noinline) int32_t fake_dlssg_set_options(const void*, const void* options)
{
    ++g_fg_set_calls;
    g_fg_seen_options = options;
    std::memcpy(g_fg_seen_bytes, options, sizeof(g_fg_seen_bytes));
    g_sink = g_sink + 17;
    return 0;
}
extern "C" __declspec(noinline) int32_t fake_dlssg_get_state(const void*, void* state, const void*)
{
    ++g_fg_get_calls;
    auto* b = static_cast<uint8_t*>(state);
    uint64_t version = 0;
    std::memcpy(&version, b + 24, 8);
    const uint32_t status = 0, presented = 2;
    std::memcpy(b + 40, &status, 4);
    std::memcpy(b + 48, &presented, 4);
    if (version >= 2) std::memcpy(b + 52, &g_fg_max, 4);
    if (version >= 4) b[80] = g_fg_dynamic;
    g_sink = g_sink + 19;
    return 0;
}
extern "C" __declspec(dllexport) __declspec(noinline) int32_t slGetFeatureFunction(uint32_t feature, const char* name, void** fn)
{
    g_sink = g_sink + 23;
    if (feature != 1000 || !name || !fn) return 1;
    if (std::strcmp(name, "slDLSSGSetOptions") == 0) { *fn = reinterpret_cast<void*>(&fake_dlssg_set_options); return 0; }
    if (std::strcmp(name, "slDLSSGGetState") == 0) { *fn = reinterpret_cast<void*>(&fake_dlssg_get_state); return 0; }
    return 1;
}

// ---- the "Streamline" swap-chain proxy: same RTTI name and slot order ----
namespace sl::interposer {
#define NA_DUMMY(n) virtual int __stdcall Dummy##n() { g_sink = g_sink + n * 11; return n; }
struct DXGISwapChain {
    IUnknown* base = nullptr;
    LONG refs = 1;
    // 0..2 IUnknown
    virtual HRESULT __stdcall QueryInterface(REFIID riid, void** out)
    {
        // StreamlineRetrieveBaseInterface {ADEC44E2-61F0-45C3-AD9F-1B37379284FF}
        static const GUID kBase = {0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
        if (riid == kBase && base) {
            base->AddRef();
            *out = base;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    virtual ULONG __stdcall AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    virtual ULONG __stdcall Release() { return static_cast<ULONG>(InterlockedDecrement(&refs)); }
    NA_DUMMY(3) NA_DUMMY(4) NA_DUMMY(5) NA_DUMMY(6) NA_DUMMY(7)
    // 8 Present
    virtual HRESULT __stdcall Present(UINT sync, UINT flags)
    {
        ++g_present_calls;
        g_sink = g_sink + static_cast<int>(sync + flags) + 101;
        return g_next_present_hr;
    }
    NA_DUMMY(9) NA_DUMMY(10) NA_DUMMY(11) NA_DUMMY(12)
    // 13 ResizeBuffers
    virtual HRESULT __stdcall ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
    {
        ++g_resize_calls;
        g_sink = g_sink + static_cast<int>(n + w + h + flags + static_cast<UINT>(f)) + 131;
        return S_OK;
    }
    NA_DUMMY(14) NA_DUMMY(15) NA_DUMMY(16) NA_DUMMY(17) NA_DUMMY(18) NA_DUMMY(19) NA_DUMMY(20) NA_DUMMY(21)
    // 22 Present1
    virtual HRESULT __stdcall Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*)
    {
        ++g_present1_calls;
        g_sink = g_sink + static_cast<int>(sync + flags) + 221;
        return g_next_present_hr;
    }
    NA_DUMMY(23) NA_DUMMY(24) NA_DUMMY(25) NA_DUMMY(26) NA_DUMMY(27) NA_DUMMY(28) NA_DUMMY(29) NA_DUMMY(30)
    NA_DUMMY(31) NA_DUMMY(32) NA_DUMMY(33) NA_DUMMY(34) NA_DUMMY(35) NA_DUMMY(36) NA_DUMMY(37) NA_DUMMY(38)
    // 39 ResizeBuffers1
    virtual HRESULT __stdcall ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT*, IUnknown* const*)
    {
        ++g_resize_calls;
        g_sink = g_sink + static_cast<int>(n + w + h + flags + static_cast<UINT>(f)) + 391;
        return S_OK;
    }
};
#undef NA_DUMMY
} // namespace sl::interposer

namespace {

// A COM object to stand for the base chain.
struct FakeBase : IUnknown {
    LONG refs = 1;
    HRESULT __stdcall QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG __stdcall AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG __stdcall Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs)); }
};

// sl::FrameToken: vptr, BaseStructure, value; operator uint32_t is vtable[0].
struct FakeFrameToken {
    virtual uint32_t value() const { return v; }
    uint8_t header[32] = {};
    uint32_t v = 0;
    explicit FakeFrameToken(uint32_t value_in) : v(value_in) { std::memcpy(header + 8, na::sl::kFrameTokenGuid, 16); }
};

struct FakeViewport {
    uint8_t header[32] = {};
    uint32_t value = 0;
    explicit FakeViewport(uint32_t v) : value(v) { std::memcpy(header + 8, na::sl::kViewportGuid, 16); }
};

// sl::Resource (112 bytes) and sl::ResourceTag (64 bytes), byte-exact.
struct alignas(8) FakeResource { uint8_t b[112] = {}; };
struct alignas(8) FakeTag { uint8_t b[64] = {}; };

template <class T> void put(uint8_t* p, size_t off, T v) { std::memcpy(p + off, &v, sizeof(T)); }

FakeResource make_resource(uint64_t native, uint32_t state, uint32_t w, uint32_t h, uint32_t fmt)
{
    FakeResource r;
    std::memcpy(r.b + 8, na::slb::abi::kResourceGuid, 16);
    r.b[32] = 0; // eTex2d
    put<uint64_t>(r.b, 40, native);
    put<uint32_t>(r.b, 64, state);
    put<uint32_t>(r.b, 68, w);
    put<uint32_t>(r.b, 72, h);
    put<uint32_t>(r.b, 76, fmt);
    return r;
}

FakeTag make_tag(const FakeResource* res, uint32_t type, uint32_t life, uint32_t top, uint32_t left, uint32_t w, uint32_t h)
{
    FakeTag t;
    std::memcpy(t.b + 8, na::slb::abi::kResourceTagGuid, 16);
    put<uint64_t>(t.b, 32, reinterpret_cast<uint64_t>(res));
    put<uint32_t>(t.b, 40, type);
    put<uint32_t>(t.b, 44, life);
    put<uint32_t>(t.b, 48, top);
    put<uint32_t>(t.b, 52, left);
    put<uint32_t>(t.b, 56, w);
    put<uint32_t>(t.b, 60, h);
    return t;
}

struct Recorder {
    std::vector<na::slb::TagEvent> tag_events;
    std::vector<std::vector<na::slb::Tag>> tags;
    std::vector<na::slb::PresentEvent> before, after;
    std::vector<long> after_hr;
} g_rec;

void on_tag(const na::slb::TagEvent& e, void*)
{
    g_callback_order = ++g_order;
    g_rec.tag_events.push_back(e);
    g_rec.tags.emplace_back(e.tags, e.tags + e.count);
}
void on_before(const na::slb::PresentEvent& e, void*) { g_rec.before.push_back(e); }
void on_after(const na::slb::PresentEvent& e, long hr, void*)
{
    g_rec.after.push_back(e);
    g_rec.after_hr.push_back(hr);
}

} // namespace

int main()
{
    // Instantiating the proxy emits its vtable/RTTI into this image.
    FakeBase base;
    sl::interposer::DXGISwapChain proxy;
    proxy.base = &base;
    sl::interposer::DXGISwapChain proxy2;
    proxy2.base = &base;

    REQUIRE(na::slb::InstallForModuleForTests(GetModuleHandleW(nullptr)));
    na::slb::Stats s = na::slb::GetStats();
    CHECK(s.tag_hooks == 2);
    CHECK(s.present_hooks == 2);
    CHECK(s.discovery == na::slb::Discovery::Rtti);
    na::slb::Callbacks cb;
    cb.on_tag = &on_tag;
    cb.before_present = &on_before;
    cb.after_present = &on_after;
    na::slb::SetCallbacks(cb);

    // ---- 2. tags ----
    using TagFrameFn = int32_t(__cdecl*)(const void*, const void*, const void*, uint32_t, void*);
    using TagFn = int32_t(__cdecl*)(const void*, const void*, uint32_t, void*);
    auto tag_frame = reinterpret_cast<TagFrameFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "slSetTagForFrame")));
    auto tag = reinterpret_cast<TagFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "slSetTag")));
    REQUIRE(tag_frame && tag);
    const FakeResource hud = make_resource(0x1111000, kAllShader, 3440, 1440, kHdr10);
    const FakeResource depth = make_resource(0x2222000, kNonPixel, 1146, 480, static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT));
    FakeTag tags[3] = {
        make_tag(&hud, na::slb::abi::kBufferHudlessColor, na::slb::abi::kOnlyValidNow, 0, 0, 0, 0),
        make_tag(&depth, na::slb::abi::kBufferDepth, na::slb::abi::kValidUntilPresent, 2, 4, 1140, 476),
        make_tag(nullptr, na::slb::abi::kBufferUiColorAndAlpha, na::slb::abi::kValidUntilPresent, 0, 0, 0, 0),
    };
    FakeFrameToken frame(4242);
    FakeViewport viewport(7);
    void* const cmd = reinterpret_cast<void*>(0x3333000);
    CHECK(tag_frame(&frame, &viewport, tags, 3, cmd) == 0);
    CHECK(g_tag_frame_calls == 1);
    CHECK(g_orig_seen_tags == tags && g_orig_seen_n == 3 && g_orig_seen_cmd == cmd); // game's own args
    CHECK(g_callback_order != 0 && g_callback_order < g_original_order);              // callback BEFORE original
    REQUIRE(g_rec.tag_events.size() == 1);
    {
        const na::slb::TagEvent& e = g_rec.tag_events[0];
        CHECK(e.decoded && e.for_frame && e.count == 3 && e.raw_count == 3);
        CHECK(e.has_frame && e.frame_token == 4242);
        CHECK(e.has_viewport && e.viewport == 7);
        CHECK(e.command_list == cmd);
        CHECK(e.sequence == 1);
        const auto& t = g_rec.tags[0];
        CHECK(t[0].buffer_type == na::slb::abi::kBufferHudlessColor && t[0].lifecycle == na::slb::abi::kOnlyValidNow);
        CHECK(reinterpret_cast<uintptr_t>(t[0].resource) == 0x1111000 && t[0].resource_decoded);
        CHECK(t[0].state == kAllShader && t[0].width == 3440 && t[0].height == 1440);
        CHECK(t[0].native_format == kHdr10);
        CHECK(t[1].state == kNonPixel && t[1].lifecycle == na::slb::abi::kValidUntilPresent);
        CHECK(t[1].extent_top == 2 && t[1].extent_left == 4 && t[1].extent_width == 1140 && t[1].extent_height == 476);
        CHECK(t[2].resource == nullptr && !t[2].resource_decoded); // untag
    }
    s = na::slb::GetStats();
    CHECK(s.hudless_lifecycle == na::slb::abi::kOnlyValidNow && s.hudless_state == kAllShader);
    CHECK(s.depth_lifecycle == na::slb::abi::kValidUntilPresent);
    CHECK(s.ui_tagged == 0);

    // ---- 3. unrecognised layout: undecoded, still forwarded ----
    FakeTag bad = tags[0];
    bad.b[8] ^= 0xFF;
    CHECK(tag(&viewport, &bad, 1, nullptr) == 0);
    CHECK(g_tag_calls == 1);
    REQUIRE(g_rec.tag_events.size() == 2);
    CHECK(!g_rec.tag_events[1].decoded && g_rec.tag_events[1].count == 0 && !g_rec.tag_events[1].for_frame);
    CHECK(na::slb::GetStats().tag_undecoded == 1);

    // ---- 4. presents ----
    auto* chain = reinterpret_cast<IDXGISwapChain*>(&proxy);
    CHECK(chain->Present(1, 0) == S_OK);
    CHECK(g_present_calls == 1);
    REQUIRE(g_rec.before.size() == 1 && g_rec.after.size() == 1);
    CHECK(g_rec.before[0].kind == na::slb::PresentKind::Real && g_rec.before[0].real_serial == 1);
    CHECK(g_rec.before[0].stream_epoch == 1);   // first proxy seen
    CHECK(g_rec.before[0].game_facing == chain);
    CHECK(g_rec.before[0].base == reinterpret_cast<IDXGISwapChain*>(static_cast<IUnknown*>(&base)));
    CHECK(g_rec.before[0].last_tag_sequence == 2 && g_rec.before[0].has_last_frame_token && g_rec.before[0].last_frame_token == 4242);
    CHECK(g_rec.after_hr[0] == S_OK);
    CHECK(base.refs == 1); // base borrowed, not leaked

    CHECK(chain->Present(0, DXGI_PRESENT_TEST) == S_OK);
    CHECK(g_present_calls == 2);
    CHECK(g_rec.before.size() == 1 && g_rec.after.size() == 1); // TEST: no callbacks
    CHECK(na::slb::GetStats().presents_test == 1);

    g_next_present_hr = DXGI_ERROR_WAS_STILL_DRAWING;
    CHECK(chain->Present(0, DXGI_PRESENT_DO_NOT_WAIT) == DXGI_ERROR_WAS_STILL_DRAWING);
    g_next_present_hr = S_OK;
    auto* chain1 = reinterpret_cast<IDXGISwapChain1*>(&proxy);
    DXGI_PRESENT_PARAMETERS pp{};
    CHECK(chain1->Present1(0, DXGI_PRESENT_DO_NOT_WAIT, &pp) == S_OK);
    CHECK(g_present1_calls == 1);
    REQUIRE(g_rec.before.size() == 3);
    CHECK(g_rec.before[1].kind == na::slb::PresentKind::Real && g_rec.before[1].real_serial == 2);
    CHECK(g_rec.before[2].kind == na::slb::PresentKind::Retry && g_rec.before[2].real_serial == 2); // same frame
    CHECK(g_rec.before[2].present1 && g_rec.before[2].params == &pp);
    CHECK(chain->Present(1, 0) == S_OK);
    CHECK(g_rec.before[3].kind == na::slb::PresentKind::Real && g_rec.before[3].real_serial == 3);

    CHECK(chain->ResizeBuffers(3, 1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 0) == S_OK);
    CHECK(g_resize_calls == 1);
    CHECK(chain->Present(1, 0) == S_OK);
    CHECK(g_rec.before.back().stream_epoch == 2); // resize = new incarnation

    auto* other = reinterpret_cast<IDXGISwapChain*>(&proxy2);
    CHECK(other->Present(1, 0) == S_OK);
    CHECK(g_rec.before.back().stream_epoch == 3 && g_rec.before.back().game_facing == other); // new proxy
    s = na::slb::GetStats();
    CHECK(s.presents_real == 5 && s.presents_retry == 1 && s.proxies_seen == 2);

    // ---- 5. native DLSS-G control ----
    {
        using GffFn = int32_t(__cdecl*)(uint32_t, const char*, void**);
        using SetFn = int32_t(__cdecl*)(const void*, const void*);
        auto gff = reinterpret_cast<GffFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "slGetFeatureFunction")));
        REQUIRE(gff);
        void* set_fn = nullptr;
        void* get_fn = nullptr;
        CHECK(gff(1000, "slDLSSGSetOptions", &set_fn) == 0 && set_fn == reinterpret_cast<void*>(&fake_dlssg_set_options));
        CHECK(gff(1000, "slDLSSGGetState", &get_fn) == 0 && get_fn);
        for (int i = 0; i < 200 && !(na::slb::GetFgState().set_hooked && na::slb::GetFgState().get_hooked); ++i) Sleep(10);
        REQUIRE(na::slb::GetFgState().set_hooked == 1 && na::slb::GetFgState().get_hooked == 1);
        auto set = reinterpret_cast<SetFn>(set_fn);
        // The game's options: v5 struct, mode eOn, 1 frame, colour size 3440x1440, a chained `next`.
        alignas(8) uint8_t opts[120] = {};
        const uint8_t guid[16] = {0xcb, 0xf1, 0xc5, 0xfa, 0xfd, 0x2d, 0x36, 0x4f, 0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5};
        const uint64_t next = 0xABCDEF00, version = 5;
        const uint32_t on = 1, one = 1, cw = 3440, ch = 1440;
        std::memcpy(opts, &next, 8);
        std::memcpy(opts + 8, guid, 16);
        std::memcpy(opts + 24, &version, 8);
        std::memcpy(opts + 32, &on, 4);
        std::memcpy(opts + 36, &one, 4);
        std::memcpy(opts + 64, &cw, 4);
        std::memcpy(opts + 68, &ch, 4);
        FakeViewport vp(7);
        // Game request: the game's own pointer goes through.
        CHECK(set(&vp, opts) == 0);
        CHECK(g_fg_seen_options == opts);
        // 4x (3 generated): a copy with only the count changed.
        na::slb::SetFgRequest({na::slb::FgMode::On, 3});
        CHECK(set(&vp, opts) == 0);
        CHECK(g_fg_seen_options != opts);
        uint32_t mode = 0, frames = 0, w = 0;
        uint64_t seen_next = 0;
        std::memcpy(&mode, g_fg_seen_bytes + 32, 4);
        std::memcpy(&frames, g_fg_seen_bytes + 36, 4);
        std::memcpy(&w, g_fg_seen_bytes + 64, 4);
        std::memcpy(&seen_next, g_fg_seen_bytes, 8);
        CHECK(mode == 1 && frames == 3 && w == 3440 && seen_next == next);
        CHECK(std::memcmp(g_fg_seen_bytes + 8, guid, 16) == 0);
        uint32_t game_frames = 0;
        std::memcpy(&game_frames, opts + 36, 4);
        CHECK(game_frames == 1); // the game's own struct is untouched
        na::slb::FgState fg = na::slb::GetFgState();
        CHECK(fg.max_frames == 5 && fg.sent_frames == 3 && fg.game_frames == 1);
        // Above the runtime's maximum: refused, game's options pass through.
        na::slb::SetFgRequest({na::slb::FgMode::On, 9});
        CHECK(set(&vp, opts) == 0);
        CHECK(g_fg_seen_options == opts);
        // Dynamic when the runtime did not report support: refused.
        na::slb::SetFgRequest({na::slb::FgMode::Dynamic, 0});
        CHECK(set(&vp, opts) == 0);
        CHECK(g_fg_seen_options == opts);
        // Off: mode rewritten, count kept.
        na::slb::SetFgRequest({na::slb::FgMode::Off, 0});
        CHECK(set(&vp, opts) == 0);
        std::memcpy(&mode, g_fg_seen_bytes + 32, 4);
        CHECK(g_fg_seen_options != opts && mode == 0);
        na::slb::SetFgRequest({});
    }

    char line[1024];
    CHECK(na::slb::FormatStatus(line, sizeof(line)) > 0);
    std::printf("%s\n", line);
    CHECK(std::strstr(line, "discovery=rtti") != nullptr);
    CHECK(std::strstr(line, "hudless=only_valid_now/0x") != nullptr);

    na::slb::SetCallbacks({});
    CHECK(chain->Present(1, 0) == S_OK); // detached: still forwards
    return test::finish("sl_boundary_test");
}
