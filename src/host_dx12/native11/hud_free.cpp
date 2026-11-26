#include "host_dx12/native11/hud_free.hpp"
#include "host_dx12/native11/input_provider.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "core/live_frame_gen.hpp"
#include "host_dx12/native_d3d11/shared_resources.hpp"

#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace na::native11::hud_free {
namespace {

struct Out { uint64_t id; uint32_t w, h; };
struct Op {
    bool compute = false, transfer = false, blend = false, depth = false;
    std::vector<Out> out;
    uint64_t list = 0, recording = 0;
};

DXGI_FORMAT typeless_of(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}

struct State {
    std::mutex m;
    std::atomic<bool> wanted{false};
    // Learning
    bool recording = false;            // this frame's draws are recorded
    std::vector<Op> ops;
    uint64_t frame = 0, learn_after = 0;
    uint32_t learned = 0;
    na::fglive::HudVariant variant = na::fglive::HudVariant::None;
    uint64_t sheet = 0;
    uint32_t snap_k = 0;               // hudfree/snapshot-before-interface: draws into the shown picture before the first interface draw
    uint32_t final_k = 0;              // T2: draws into the shown picture before the final draw
    // Per frame
    uint32_t shown_draws = 0;
    bool pre_taken = false, taken = false, fresh = false;
    uint32_t frames_without = 0;
    // GPU objects (immediate context's device)
    ComPtr<ID3D11Texture2D> picture, pre, save;
    ComPtr<ID3D12Device> effects;                                  // set: the picture is shared with D3D12
    std::shared_ptr<na::native_d3d11::SharedTexture> shared_picture;
    std::vector<std::shared_ptr<na::native_d3d11::SharedTexture>> retired; // replaced shared pictures (kept for a while)
    ComPtr<ID3D11Resource> sheet_res; // hudfree/sheet-redraw: the learned sheet, from its first draw this frame
    ComPtr<ID3D11RenderTargetView> picture_rtv;
    DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN;
    // Stats
    uint64_t learn_attempts = 0, pictures = 0, skipped = 0, relearns = 0;
    char verdict[200] = "The HUD-free picture has not been looked for yet (it is learned when it is first wanted).";
    char method[128] = "none yet";
    char why[112] = "The HUD-free picture is not ready yet.";
    // F8 capture (CaptureFrames)
    std::atomic<bool> capturing{false};
    std::wstring cap_dir;
    uint32_t cap_frames = 0, cap_draws = 0;
    struct Shot { ComPtr<ID3D11Texture2D> staging; uint64_t frame = 0; int kind = 0; uint32_t w = 0, h = 0; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; };
    std::vector<Shot> shots;
    uint64_t cap_written = 0, cap_bytes = 0;
} g;

constexpr uint32_t kCaptureDraws = 4;

// A staging copy of `src` (immediate context, in order). Caller holds g.m.
void capture_copy(ID3D11DeviceContext* c, ID3D11Resource* src, int kind)
{
    ComPtr<ID3D11Texture2D> t;
    if (!src || FAILED(src->QueryInterface(IID_PPV_ARGS(&t)))) return;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return;
    ComPtr<ID3D11Device> dev;
    c->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Format = typeless_of(d.Format);
    sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    State::Shot shot;
    if (!dev || FAILED(dev->CreateTexture2D(&sd, nullptr, &shot.staging))) return;
    na::native11::OwnWorkScope own;
    c->CopySubresourceRegion(shot.staging.Get(), 0, 0, 0, 0, t.Get(), 0, nullptr);
    shot.frame = g.frame; shot.kind = kind; shot.w = d.Width; shot.h = d.Height; shot.format = d.Format;
    g.shots.push_back(std::move(shot));
}

// Writes the staging copies the GPU has finished (never waits). Caller holds g.m.
void capture_write(ID3D11DeviceContext* c)
{
    for (size_t i = 0; i < g.shots.size();) {
        auto& s = g.shots[i];
        D3D11_MAPPED_SUBRESOURCE m{};
        if (c->Map(s.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == DXGI_ERROR_WAS_STILL_DRAWING) { ++i; continue; }
        if (m.pData) {
            const uint32_t bpp = s.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
            wchar_t name[96];
            if (s.kind < 0) swprintf_s(name, L"\\pre-interface-%05llu-hudfree.raw", static_cast<unsigned long long>(s.frame));
            else swprintf_s(name, L"\\pre-interface-%05llu-before-draw%d.raw", static_cast<unsigned long long>(s.frame), s.kind);
            const std::wstring path = g.cap_dir + name;
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                for (uint32_t y = 0; y < s.h; ++y) {
                    DWORD wr = 0;
                    WriteFile(f, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, s.w * bpp, &wr, nullptr);
                    g.cap_bytes += wr;
                }
                CloseHandle(f);
            }
            c->Unmap(s.staging.Get(), 0);
            char line[400];
            const int n = snprintf(line, sizeof line, "{\"frame\": %llu, \"kind\": \"%s%d\", \"width\": %u, \"height\": %u, \"format\": %u, \"method\": \"%s\"}\r\n",
                                   static_cast<unsigned long long>(s.frame), s.kind < 0 ? "hudfree" : "before-draw", s.kind < 0 ? 0 : s.kind, s.w, s.h,
                                   static_cast<unsigned>(s.format), g.method);
            const std::wstring jpath = g.cap_dir + L"\\pre-interface.json";
            HANDLE j = CreateFileW(jpath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (j != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(j, line, DWORD(n > 0 ? n : 0), &wr, nullptr); CloseHandle(j); }
            ++g.cap_written;
        }
        g.shots.erase(g.shots.begin() + std::ptrdiff_t(i));
    }
}

void skip(const char* why) // caller holds g.m
{
    ++g.skipped;
    if (std::strcmp(g.why, why)) {
        strncpy_s(g.why, why, _TRUNCATE);
        na::eventlog::Emit("hud_free11", "HUD-free picture skipped: %s", why);
    }
}

// A texture like `like` (size, format family), render-target capable, on its device.
bool ensure(ComPtr<ID3D11Texture2D>& t, ID3D11Texture2D* like, bool rt)
{
    D3D11_TEXTURE2D_DESC d{};
    like->GetDesc(&d);
    if (t) {
        D3D11_TEXTURE2D_DESC c{};
        t->GetDesc(&c);
        if (c.Width == d.Width && c.Height == d.Height && typeless_of(c.Format) == typeless_of(d.Format)) return true;
        t.Reset();
    }
    ComPtr<ID3D11Device> dev;
    like->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC n{};
    n.Width = d.Width; n.Height = d.Height; n.MipLevels = 1; n.ArraySize = 1;
    n.Format = typeless_of(d.Format);
    n.SampleDesc = {1, 0};
    n.Usage = D3D11_USAGE_DEFAULT;
    n.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0u);
    return dev && SUCCEEDED(dev->CreateTexture2D(&n, nullptr, &t));
}

// Our picture, like `like`: an image shared with the presenter's D3D12 device when one is set.
bool ensure_picture(ID3D11Texture2D* like) // caller holds g.m
{
    if (!g.effects) return ensure(g.picture, like, true);
    D3D11_TEXTURE2D_DESC d{};
    like->GetDesc(&d);
    if (g.shared_picture) {
        const auto& c = g.shared_picture->Description();
        if (c.width == d.Width && c.height == d.Height && c.format == typeless_of(d.Format)) return true;
        g.retired.push_back(g.shared_picture); // the presenter may still read it this frame
        if (g.retired.size() > 4) g.retired.erase(g.retired.begin());
        g.shared_picture.reset(); g.picture.Reset(); g.picture_rtv.Reset(); g.rtv_format = DXGI_FORMAT_UNKNOWN;
    }
    ComPtr<ID3D11Device> dev;
    like->GetDevice(&dev);
    auto t = std::make_shared<na::native_d3d11::SharedTexture>();
    na::native_d3d11::ImageDesc id{d.Width, d.Height, typeless_of(d.Format), false};
    if (!dev || FAILED(t->Create(dev.Get(), g.effects.Get(), id))) return false;
    g.shared_picture = t;
    g.picture = t->Native();
    return true;
}

ComPtr<ID3D11Resource> bound_rt0(ID3D11DeviceContext* c, DXGI_FORMAT* view_format = nullptr)
{
    ComPtr<ID3D11RenderTargetView> v;
    c->OMGetRenderTargets(1, &v, nullptr);
    ComPtr<ID3D11Resource> r;
    if (v) {
        v->GetResource(&r);
        if (view_format) { D3D11_RENDER_TARGET_VIEW_DESC d{}; v->GetDesc(&d); *view_format = d.Format; }
    }
    return r;
}

bool blend_enabled(ID3D11DeviceContext* c)
{
    ComPtr<ID3D11BlendState> b;
    FLOAT f[4];
    UINT mask = 0;
    c->OMGetBlendState(&b, f, &mask);
    if (!b) return false;
    D3D11_BLEND_DESC d{};
    b->GetDesc(&d);
    return d.RenderTarget[0].BlendEnable != FALSE;
}

// hudfree/sheet-redraw's second draw: the sheet's contents swapped for the pre-HUD copy, the same draw into our picture.
void redraw(ID3D11DeviceContext* c, const draw_replay::DrawArgs& a, IssueDraw issue, void* user) // caller holds g.m
{
    if (a.indirect) { skip("the game's final draw takes its arguments from the graphics card (indirect), so it cannot be run again"); return; }
    if (!g.pre_taken) { skip("the HUD sheet was not copied before its first HUD draw this frame"); return; }
    ComPtr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11DepthStencilView> dsv;
    ID3D11RenderTargetView* raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw, dsv.GetAddressOf());
    UINT count = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) { rtvs[i].Attach(raw[i]); if (raw[i]) count = i + 1; }
    if (count != 1 || !raw[0]) { skip("the final draw writes more than one picture"); return; }
    ComPtr<ID3D11Resource> shown;
    raw[0]->GetResource(&shown);
    D3D11_RENDER_TARGET_VIEW_DESC vd{};
    raw[0]->GetDesc(&vd);
    ComPtr<ID3D11Texture2D> shown_t;
    if (FAILED(shown.As(&shown_t))) { skip("the shown picture is not a 2D texture"); return; }
    ComPtr<ID3D11Device> dev;
    c->GetDevice(&dev);
    if (!ensure_picture(shown_t.Get())) { skip("the HUD-free picture could not be made"); return; }
    if (!g.picture_rtv || g.rtv_format != vd.Format) {
        g.picture_rtv.Reset();
        D3D11_RENDER_TARGET_VIEW_DESC pv{};
        pv.Format = vd.Format;
        pv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(g.picture.Get(), &pv, &g.picture_rtv))) { skip("no render-target view for the HUD-free picture"); return; }
        g.rtv_format = vd.Format;
    }
    if (!g.sheet_res || !g.save) { skip("the HUD sheet has gone"); return; }
    na::native11::OwnWorkScope own;
    c->CopyResource(g.save.Get(), g.sheet_res.Get());        // the sheet as the HUD left it
    c->CopyResource(g.sheet_res.Get(), g.pre.Get());         // the sheet before any HUD draw
    if (blend_enabled(c)) c->CopyResource(g.picture.Get(), shown.Get()); // the same background under a blended draw
    ID3D11RenderTargetView* ours = g.picture_rtv.Get();
    c->OMSetRenderTargets(1, &ours, dsv.Get());
    issue(c, a, user);
    c->OMSetRenderTargets(count, raw, dsv.Get());                 // the game's own targets back
    c->CopyResource(g.sheet_res.Get(), g.save.Get());        // and its sheet, before its own draw reads it
    g.taken = true;
}

} // namespace

void SetWanted(bool on) { g.wanted.store(on, std::memory_order_release); }
bool Wanted() { return g.wanted.load(std::memory_order_acquire) || g.capturing.load(std::memory_order_acquire); }

void CaptureFrames(const wchar_t* dir, uint32_t frames)
{
    if (!dir || !*dir || !frames) return;
    std::lock_guard<std::mutex> lock(g.m);
    g.cap_dir = dir;
    g.cap_frames = frames > 8 ? 8 : frames;
    g.cap_draws = 0;
    g.capturing.store(true, std::memory_order_release);
    na::eventlog::Emit("hud_free11", "capture: the shown picture before its first %u draws and the HUD-free picture, for %u frames, to the capture folder",
                       kCaptureDraws, g.cap_frames);
}

void BeforeDraw(ID3D11DeviceContext* c, const DrawFacts& f, const draw_replay::DrawArgs& a, IssueDraw issue, void* user)
{
    if (!Wanted() || !c || !f.rt0) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (g.capturing.load(std::memory_order_relaxed) && g.cap_frames && f.rt0 == f.shown && g.cap_draws < kCaptureDraws) {
        ComPtr<ID3D11Resource> shown = bound_rt0(c);
        capture_copy(c, shown.Get(), int(g.cap_draws));
        ++g.cap_draws;
    }
    if (g.recording) {
        Op op;
        op.blend = blend_enabled(c);
        op.depth = f.depth;
        op.out.push_back({f.rt0, f.rt_w, f.rt_h});
        op.recording = g.frame;
        g.ops.push_back(std::move(op));
        if (g.ops.size() > 200000) { g.recording = false; g.ops.clear(); } // a runaway frame is not learned
    }
    if (!g.learned) return;
    if (g.variant == na::fglive::HudVariant::Snapshot) {
        if (f.rt0 != f.shown) return;
        if (g.shown_draws++ != g.snap_k || g.taken) return;
        ComPtr<ID3D11Resource> shown = bound_rt0(c);
        ComPtr<ID3D11Texture2D> t;
        if (!shown || FAILED(shown.As(&t))) { skip("the shown picture is not a 2D texture"); return; }
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        if (d.SampleDesc.Count != 1) { skip("the shown picture is multisampled"); return; }
        if (!ensure_picture(t.Get())) { skip("the HUD-free picture could not be made"); return; }
        na::native11::OwnWorkScope own;
        c->CopyResource(g.picture.Get(), shown.Get());
        g.taken = true;
        return;
    }
    if (g.variant == na::fglive::HudVariant::SheetRedraw) {
        if (f.rt0 == g.sheet && !g.pre_taken) { // first draw into the sheet this frame: keep it before any HUD draw
            ComPtr<ID3D11Resource> sheet = bound_rt0(c);
            ComPtr<ID3D11Texture2D> t;
            if (!sheet || FAILED(sheet.As(&t)) || !ensure(g.pre, t.Get(), false) || !ensure(g.save, t.Get(), false)) {
                skip("the HUD sheet's copies could not be made"); return;
            }
            g.sheet_res = sheet;
            na::native11::OwnWorkScope own;
            c->CopyResource(g.pre.Get(), sheet.Get());
            g.pre_taken = true;
            return;
        }
        if (f.rt0 != f.shown) return;
        if (g.shown_draws++ != g.final_k || g.taken) return;
        if (f.depth) { skip("the final draw uses a depth buffer"); return; }
        redraw(c, a, issue, user);
    }
}

void BeforeCopy(uint64_t dst, uint32_t w, uint32_t h)
{
    if (!Wanted() || !dst) return;
    std::lock_guard<std::mutex> lock(g.m);
    if (!g.recording) return;
    Op op;
    op.transfer = true;
    op.out.push_back({dst, w, h});
    op.recording = g.frame;
    g.ops.push_back(std::move(op));
}

void OnPresent(ID3D11DeviceContext* immediate, uint64_t shown, uint32_t shown_w, uint32_t shown_h)
{
    if (g.capturing.load(std::memory_order_acquire) && immediate) {
        std::lock_guard<std::mutex> lock(g.m);
        if (g.cap_frames) {
            if (g.taken && g.picture) capture_copy(immediate, g.picture.Get(), -1);
            --g.cap_frames;
            g.cap_draws = 0;
        }
        capture_write(immediate);
        if (!g.cap_frames && g.shots.empty()) {
            g.capturing.store(false, std::memory_order_release);
            na::eventlog::Emit("hud_free11", "capture written: %llu pictures, %.1f MB", static_cast<unsigned long long>(g.cap_written), double(g.cap_bytes) / 1048576.0);
            g.cap_written = g.cap_bytes = 0;
        }
    }
    if (!Wanted()) {
        std::lock_guard<std::mutex> lock(g.m);
        g.fresh = false; g.recording = false; g.ops.clear();
        g.shown_draws = 0; g.taken = g.pre_taken = false;
        return;
    }
    std::lock_guard<std::mutex> lock(g.m);
    ++g.frame;
    if (g.recording) { // the recorded frame ends here: learn it
        g.recording = false;
        ++g.learn_attempts;
        na::fglive::Sheet sheet;
        na::fglive::Snapshot snap;
        g.variant = na::fglive::ChooseHudVariant(g.ops, shown, shown_w, shown_h, &sheet, &snap);
        if (g.variant == na::fglive::HudVariant::SheetRedraw) {
            g.sheet = sheet.sheet;
            g.final_k = 0;
            for (size_t i = 0; i < sheet.final_op; ++i)
                if (!g.ops[i].compute && !g.ops[i].transfer && !g.ops[i].out.empty() && g.ops[i].out[0].id == shown) ++g.final_k;
            snprintf(g.verdict, sizeof g.verdict, "%s", na::fglive::SheetWhy(sheet.verdict));
        } else if (g.variant == na::fglive::HudVariant::Snapshot) {
            g.snap_k = snap.draws_before;
            snprintf(g.verdict, sizeof g.verdict, "%s (sheet: %s)", na::fglive::SnapshotWhy(snap.verdict), na::fglive::SheetWhy(sheet.verdict));
        } else {
            snprintf(g.verdict, sizeof g.verdict, "No generated pictures: the HUD could not be separated (sheet: %s; interface on the shown picture: %s)",
                     na::fglive::SheetWhy(sheet.verdict), na::fglive::SnapshotWhy(snap.verdict));
            g.learn_after = g.frame + 600;
        }
        g.learned = g.variant != na::fglive::HudVariant::None ? 1u : 0u;
        snprintf(g.method, sizeof g.method, "%s", na::fglive::HudVariantName(g.variant));
        na::eventlog::Emit("hud_free11", "learn attempt %llu over %zu ops: %s -> %s", static_cast<unsigned long long>(g.learn_attempts), g.ops.size(),
                           g.method, g.verdict);
        g.ops.clear();
        g.frames_without = 0;
    } else if (g.learned) { // a learned frame: was the picture taken?
        g.fresh = g.taken;
        if (g.taken) { ++g.pictures; g.frames_without = 0; }
        else {
            skip(g.variant == na::fglive::HudVariant::SheetRedraw ? "no HUD sheet or final draw was seen this frame (menu, loading or cutscene)"
                                                                 : "the interface's first draw on the shown picture was not seen this frame");
            if (++g.frames_without >= 120) {
                ++g.relearns;
                g.learned = 0;
                g.variant = na::fglive::HudVariant::None;
                g.sheet_res.Reset();
                g.learn_after = g.frame;
                snprintf(g.verdict, sizeof g.verdict, "The HUD is being learned again (the frame's shape changed).");
                na::eventlog::Emit("hud_free11", "relearning (%llu): no HUD-free picture for 120 frames", static_cast<unsigned long long>(g.relearns));
            }
        }
    }
    g.shown_draws = 0;
    g.taken = g.pre_taken = false;
    if (!g.learned && g.frame >= g.learn_after) { g.recording = true; g.ops.clear(); }
}

ID3D11Texture2D* Picture(const char** why)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.fresh && g.picture) { g.fresh = false; return g.picture.Get(); }
    thread_local char t_why[200];
    strncpy_s(t_why, g.learned ? g.why : g.verdict, _TRUNCATE);
    if (why) *why = t_why;
    return nullptr;
}

void SetEffectsDevice(ID3D12Device* device)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.effects.Get() == device) return;
    g.effects = device;
    if (g.shared_picture) g.retired.push_back(g.shared_picture);
    g.shared_picture.reset(); g.picture.Reset(); g.picture_rtv.Reset(); g.rtv_format = DXGI_FORMAT_UNKNOWN;
}

ID3D12Resource* EffectsPicture(const char** why)
{
    std::lock_guard<std::mutex> lock(g.m);
    if (g.fresh && g.shared_picture) { g.fresh = false; return g.shared_picture->Effects(); }
    thread_local char t_why[200];
    strncpy_s(t_why, !g.effects ? "the HUD-free picture is not shared with the presenter (no DX11 proxy chain)" : g.learned ? g.why : g.verdict, _TRUNCATE);
    if (why) *why = t_why;
    return nullptr;
}

Stats GetStats()
{
    Stats s;
    std::lock_guard<std::mutex> lock(g.m);
    s.wanted = Wanted() ? 1u : 0u;
    s.learned = g.learned;
    s.variant = static_cast<uint32_t>(g.variant);
    s.learn_attempts = g.learn_attempts;
    s.pictures = g.pictures;
    s.skipped = g.skipped;
    s.relearns = g.relearns;
    strncpy_s(s.verdict, g.verdict, _TRUNCATE);
    strncpy_s(s.method, g.method, _TRUNCATE);
    strncpy_s(s.last_skip, g.why, _TRUNCATE);
    return s;
}

int FormatStatus(char* out, size_t capacity)
{
    const Stats s = GetStats();
    return std::snprintf(out, capacity, "hud_free11{wanted=%u learned=%u method=\"%s\" pictures=%llu skipped=%llu learn_attempts=%llu relearns=%llu verdict=\"%s\" last_skip=\"%s\"}",
                         s.wanted, s.learned, s.method, static_cast<unsigned long long>(s.pictures), static_cast<unsigned long long>(s.skipped),
                         static_cast<unsigned long long>(s.learn_attempts), static_cast<unsigned long long>(s.relearns), s.verdict, s.last_skip);
}

void Reset()
{
    std::lock_guard<std::mutex> lock(g.m);
    g.recording = false; g.ops.clear(); g.learned = 0; g.variant = na::fglive::HudVariant::None;
    g.picture.Reset(); g.pre.Reset(); g.save.Reset(); g.picture_rtv.Reset(); g.sheet_res.Reset(); g.shared_picture.reset();
    g.fresh = g.taken = g.pre_taken = false; g.shown_draws = 0; g.learn_after = g.frame;
}

} // namespace na::native11::hud_free
