// DX9 transport (DX9 plan step D4, docs/DX9-PLAN.md). Owner: Claude.
//
// At the DLSS point the hooks found, on the game's thread: D3D9On12 hands us
// the real D3D12 resources behind the scene colour and depth
// (UnwrapUnderlyingResource, on our queue), the D3D12 game bridge copies them
// into the textures shared with the 64-bit helper, the helper runs DLSS, the
// result is copied back into the scene colour, and the resources go back to
// DX9 with our fence (ReturnUnderlyingResource), so the game's next call (the
// tonemap read, or the HUD) sees the result.
#include "host_dx9/dx9_transport.hpp"

#include "bridge/dx12_game_bridge.hpp"
#include "host_dx9/dx9_hooks.hpp"
#include "host_dx9/vtable_hook.hpp"

#if __has_include(<d3d9on12.h>)
#include <d3d9on12.h>
#define NA_HAVE_9ON12 1
#endif
#include <wrl/client.h>

#include <atomic>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace na::dx9 {
namespace {

std::mutex g_mu;
TransportConfig g_cfg;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
na::bridge::Dx12GameBridge g_bridge;
TransportStats g_stats;
bool g_failed_logged = false;
uint32_t g_logged = 0;
uint64_t g_last_frame = 0;

#ifdef NA_HAVE_9ON12
bool EnsureQueue(IDirect3DDevice9On12* on12)
{
    if (g_queue) return true;
    // 9On12 made its own D3D12 device: a queue of ours on it.
    ComPtr<ID3D12Device> dev;
    if (FAILED(on12->GetD3D12Device(IID_PPV_ARGS(&dev))) || !dev) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) return false;
    g_device = dev;
    return true;
}

void Hand(IDirect3DDevice9On12* on12, IDirect3DResource9* r, ID3D12Fence* fence, uint64_t value)
{
    if (value && fence) {
        UINT64 v = value;
        ID3D12Fence* fences[1] = {fence};
        on12->ReturnUnderlyingResource(r, 1, &v, fences);
    } else {
        on12->ReturnUnderlyingResource(r, 0, nullptr, nullptr);
    }
}
#endif

void FireLocked(IDirect3DDevice9* dev, int where, const FireInfo& info)
{
#ifndef NA_HAVE_9ON12
    (void)dev; (void)where; (void)info;
    if (!g_failed_logged) { Log("DLSS transport: built without d3d9on12.h"); g_failed_logged = true; }
#else
    ComPtr<IDirect3DDevice9On12> on12;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&on12))) || !on12) {
        if (!g_failed_logged) { Log("DLSS transport: this device is not on D3D9On12 (fallback d3d9), no DLSS"); g_failed_logged = true; }
        ++g_stats.skipped;
        SetDlssLive(false);
        return;
    }
    if (!EnsureQueue(on12.Get())) {
        if (!g_failed_logged) { Log("DLSS transport: no D3D12 queue on 9On12's device"); g_failed_logged = true; }
        ++g_stats.skipped;
        SetDlssLive(false);
        return;
    }
    // (Re)start the bridge for this size (resize, resolution change).
    const na::bridge::SessionLimits& l = g_bridge.Limits();
    if (!g_bridge.Started() || l.output_w != info.width || l.output_h != info.height) {
        na::bridge::Dx12Config c;
        c.limits = {info.width, info.height, info.width, info.height};
        c.helper_path = g_cfg.helper_path;
        c.copy_test = g_cfg.copy_test;
        if (const char* why = g_bridge.Start(g_device.Get(), g_queue.Get(), c)) {
            if (!g_failed_logged) { Log("DLSS transport could not start: %s", why); g_failed_logged = true; }
            ++g_stats.skipped;
            return;
        }
        Log("DLSS transport started for %ux%u%s", info.width, info.height, g_cfg.copy_test ? " (COPY TEST: no DLSS, red/blue swapped)" : "");
        g_stats.restarts++;
        g_last_frame = 0;
    }

    ComPtr<ID3D12Resource> color, depth;
    HRESULT hc = on12->UnwrapUnderlyingResource(info.color, g_queue.Get(), IID_PPV_ARGS(&color));
    HRESULT hd = SUCCEEDED(hc) ? on12->UnwrapUnderlyingResource(info.depth, g_queue.Get(), IID_PPV_ARGS(&depth)) : E_FAIL;
    if (FAILED(hc) || FAILED(hd)) {
        if (SUCCEEDED(hc)) Hand(on12.Get(), info.color, nullptr, 0);
        if (g_logged < 5) { Log("DLSS transport: unwrap failed (colour 0x%08lx, depth 0x%08lx)", static_cast<unsigned long>(hc), static_cast<unsigned long>(hd)); ++g_logged; }
        ++g_stats.skipped;
        SetDlssLive(false);
        return;
    }

    na::bridge::FrameRecord f;
    f.source_frame = info.frame;
    f.render_w = f.output_w = info.width;
    f.render_h = f.output_h = info.height;
    f.mode = static_cast<uint32_t>(na::bridge::Mode::DLAA);
    f.preset = g_cfg.preset;
    f.flags = na::bridge::kAutoExposure;
    f.jitter_x = info.jitter_x;
    f.jitter_y = info.jitter_y;
    // Float scene colour (A16B16G16R16F, A32B32G32R32F, R16F..G32R32F) is HDR.
    if (info.format >= static_cast<uint32_t>(D3DFMT_R16F) && info.format <= static_cast<uint32_t>(D3DFMT_A32B32G32R32F)) f.flags |= na::bridge::kHdr;
    if (!g_last_frame || info.frame != g_last_frame + 1) f.flags |= na::bridge::kReset; // first frame, or frames skipped
    g_last_frame = info.frame;

    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;
    const na::bridge::Result r = g_bridge.Process({color.Get(), depth.Get(), color.Get()}, f, &fence, &value);
    Hand(on12.Get(), info.color, fence, value);
    Hand(on12.Get(), info.depth, fence, value);

    // Jitter (mode 1) runs only while DLSS delivers; the copy test has no DLSS.
    if (r == na::bridge::Result::Ok) SetDlssLive(!g_cfg.copy_test);
    else if (r == na::bridge::Result::Failed) SetDlssLive(false);
    switch (r) {
    case na::bridge::Result::Ok: ++g_stats.ok; break;
    case na::bridge::Result::Repeated: ++g_stats.repeated; break;
    case na::bridge::Result::NotReady: ++g_stats.not_ready; break;
    case na::bridge::Result::Failed: ++g_stats.failed; break;
    }
    if (where == 1) ++g_stats.at_read;
    if (where == 2) ++g_stats.at_hud;
    if (where == 3) ++g_stats.at_present;
    g_stats.wait_ms = g_bridge.LastWaitMs();
    if (r == na::bridge::Result::Failed && !g_failed_logged) {
        Log("DLSS transport stopped: %s", g_bridge.Reason());
        g_failed_logged = true;
    } else if (g_logged < 5 && r != na::bridge::Result::Ok) {
        Log("DLSS transport frame %llu: %s%s%s", static_cast<unsigned long long>(info.frame),
            r == na::bridge::Result::Repeated ? "late, last good result shown" : "not ready", g_bridge.Reason()[0] ? ": " : "", g_bridge.Reason());
        ++g_logged;
    }
    if (g_stats.ok == 1 && r == na::bridge::Result::Ok)
        Log("DLSS transport: first result delivered (%s)", where == 1 ? "before the scene read" : where == 2 ? "before the HUD" : "at Present");
    if ((g_stats.ok + g_stats.repeated) % 600 == 0 && r == na::bridge::Result::Ok)
        Log("DLSS transport: %u results, %u late, %u not ready, wait %.2f ms", g_stats.ok, g_stats.repeated, g_stats.not_ready, g_stats.wait_ms);
#endif
}

void Fire(IDirect3DDevice9* dev, int where, const FireInfo& info)
{
    std::lock_guard<std::mutex> l(g_mu);
    FireLocked(dev, where, info);
}

} // namespace

void StartTransport(const TransportConfig& cfg, ID3D12Device* device, ID3D12CommandQueue* queue)
{
    std::lock_guard<std::mutex> l(g_mu);
    g_cfg = cfg;
    g_device = device;
    g_queue = queue;
    g_bridge.Stop();
    SetDlssLive(false);
    g_failed_logged = false;
    g_logged = 0;
    SetFire(cfg.enabled ? &Fire : nullptr);
}

TransportStats GetTransportStats()
{
    std::lock_guard<std::mutex> l(g_mu);
    return g_stats;
}

} // namespace na::dx9
