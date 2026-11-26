#include "host_dx12/native11/proxy_views.hpp"
#include "host_dx12/native11/textures.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <mutex>

namespace na::native11::proxy_views {
namespace {

enum : uint32_t { kCreateUnorderedAccessView = 8, kCreateRenderTargetView = 9 }; // ID3D11Device
enum : uint32_t { kTexture2DGetDesc = 10 };                                      // ID3D11Texture2D

struct Entry { std::atomic<ID3D11Resource*> res{nullptr}; std::atomic<DXGI_FORMAT> format{DXGI_FORMAT_UNKNOWN}; };
Entry g_entries[4];
std::atomic<uint32_t> g_count{0};
std::mutex g_install_m;
bool g_installed = false;

using CreateRtvFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**);
using CreateUavFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**);
using GetDescFn = void(STDMETHODCALLTYPE*)(ID3D11Texture2D*, D3D11_TEXTURE2D_DESC*);
CreateRtvFn g_rtv = nullptr;
CreateUavFn g_uav = nullptr;
GetDescFn g_get_desc = nullptr;

HRESULT STDMETHODCALLTYPE CreateRtv_d(ID3D11Device* d, ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** out)
{
    D3D11_RENDER_TARGET_VIEW_DESC scratch{};
    return g_rtv(d, r, FixRtv(r, desc, scratch), out);
}
HRESULT STDMETHODCALLTYPE CreateUav_d(ID3D11Device* d, ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc, ID3D11UnorderedAccessView** out)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC scratch{};
    return g_uav(d, r, FixUav(r, desc, scratch), out);
}
// The D3D11 runtime itself (view validation) must keep seeing the real TYPELESS description.
bool runtime_caller(const void* address)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &m) || !m)
        return false;
    static const HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll"), dxgi = GetModuleHandleW(L"dxgi.dll");
    return m == d3d11 || m == dxgi;
}
void STDMETHODCALLTYPE GetDesc_d(ID3D11Texture2D* t, D3D11_TEXTURE2D_DESC* desc)
{
    const void* caller = _ReturnAddress();
    g_get_desc(t, desc);
    DXGI_FORMAT f;
    if (desc && Lookup(t, &f) && !runtime_caller(caller)) {
        desc->Format = f;
        desc->MiscFlags &= ~static_cast<UINT>(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
    }
}

} // namespace

void Register(ID3D11Texture2D* proxy, DXGI_FORMAT game_format)
{
    if (!proxy) return;
    ID3D11Resource* r = proxy;
    std::lock_guard<std::mutex> lk(g_install_m); // writers serialised; readers only load
    for (auto& e : g_entries) if (e.res.load() == r) { e.format.store(game_format); return; }
    for (auto& e : g_entries)
        if (!e.res.load()) { // free slot: the format first, then publish
            e.format.store(game_format);
            e.res.store(r, std::memory_order_release);
            g_count.fetch_add(1);
            return;
        }
}

void Unregister(ID3D11Texture2D* proxy)
{
    ID3D11Resource* r = proxy;
    if (!r) return;
    std::lock_guard<std::mutex> lk(g_install_m);
    for (auto& e : g_entries)
        if (e.res.load() == r) { e.res.store(nullptr); g_count.fetch_sub(1); }
}

bool Lookup(ID3D11Resource* r, DXGI_FORMAT* game_format)
{
    if (!r || !g_count.load(std::memory_order_relaxed)) return false;
    for (const auto& e : g_entries)
        if (e.res.load(std::memory_order_acquire) == r) { if (game_format) *game_format = e.format.load(); return true; }
    return false;
}

const D3D11_SHADER_RESOURCE_VIEW_DESC* FixSrv(ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* in, D3D11_SHADER_RESOURCE_VIEW_DESC& s)
{
    DXGI_FORMAT f;
    if ((in && in->Format != DXGI_FORMAT_UNKNOWN) || !Lookup(r, &f)) return in;
    if (in) s = *in;
    else { s = {}; s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; s.Texture2D.MipLevels = 1; }
    s.Format = f;
    return &s;
}
const D3D11_RENDER_TARGET_VIEW_DESC* FixRtv(ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* in, D3D11_RENDER_TARGET_VIEW_DESC& s)
{
    DXGI_FORMAT f;
    if ((in && in->Format != DXGI_FORMAT_UNKNOWN) || !Lookup(r, &f)) return in;
    if (in) s = *in;
    else { s = {}; s.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; }
    s.Format = f;
    return &s;
}
const D3D11_UNORDERED_ACCESS_VIEW_DESC* FixUav(ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* in, D3D11_UNORDERED_ACCESS_VIEW_DESC& s)
{
    DXGI_FORMAT f;
    if ((in && in->Format != DXGI_FORMAT_UNKNOWN) || !Lookup(r, &f)) return in;
    if (in) s = *in;
    else { s = {}; s.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; }
    s.Format = f;
    return &s;
}

bool Install(ID3D11Device* device, ID3D11Texture2D* proxy)
{
    std::lock_guard<std::mutex> lk(g_install_m);
    if (g_installed) return true;
    if (!device || !proxy) return false;
    // Shader-resource views: native11/textures.cpp owns that slot and calls FixSrv.
    if (!na::native11::InstallTextures(device)) return false;
    if (!na::hooking::Initialize()) return false;
    bool ok = na::hooking::BeginInstallWait("native11_proxy_views");
    ok = ok && na::hooking::PrepareVTableSlot(device, kCreateRenderTargetView, reinterpret_cast<void*>(&CreateRtv_d), reinterpret_cast<void**>(&g_rtv));
    ok = ok && na::hooking::PrepareVTableSlot(device, kCreateUnorderedAccessView, reinterpret_cast<void*>(&CreateUav_d), reinterpret_cast<void**>(&g_uav));
    ok = ok && na::hooking::PrepareVTableSlot(proxy, kTexture2DGetDesc, reinterpret_cast<void*>(&GetDesc_d), reinterpret_cast<void**>(&g_get_desc));
    if (ok) ok = na::hooking::CommitInstall();
    else na::hooking::AbandonInstall();
    g_installed = ok;
    return ok;
}

} // namespace na::native11::proxy_views
