#include "host_dx12/native11/coarse11.hpp"

#include "host_dx12/diag/event_log.hpp"

#include <windows.h>
#include <wrl/client.h>
#include <nvapi.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace na::native11::coarse {
namespace {

using QueryFn = void*(__cdecl*)(unsigned int);
using InitFn = NvAPI_Status(__cdecl*)();
using CapsFn = NvAPI_Status(__cdecl*)(IUnknown*, NvU32, NV_D3D1x_GRAPHICS_CAPS*);
using CreateSrrvFn = NvAPI_Status(__cdecl*)(ID3D11Device*, ID3D11Resource*, NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC*, ID3D11NvShadingRateResourceView**);
using SetSrrvFn = NvAPI_Status(__cdecl*)(IUnknown*, ID3D11NvShadingRateResourceView*);
using SetRatesFn = NvAPI_Status(__cdecl*)(IUnknown*, NV_D3D11_VIEWPORTS_SHADING_RATE_DESC*);

std::mutex g_m;
bool g_loaded = false;
CapsFn g_caps = nullptr;
CreateSrrvFn g_create = nullptr;
SetSrrvFn g_set_view = nullptr;
SetRatesFn g_set_rates = nullptr;
ID3D11Device* g_checked_device = nullptr;
bool g_available = false;
const char* g_why = "coarse shading has not been checked yet";
// The rate image: one byte per 16x16 tile, every tile index 0 (the table's entry 0 carries the rate).
ComPtr<ID3D11Texture2D> g_surface;
ComPtr<ID3D11NvShadingRateResourceView> g_view;
uint32_t g_tiles_w = 0, g_tiles_h = 0;

void load_locked()
{
    if (g_loaded) return;
    g_loaded = true;
    HMODULE nv = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto query = nv ? reinterpret_cast<QueryFn>(GetProcAddress(nv, "nvapi_QueryInterface")) : nullptr;
    auto init = query ? reinterpret_cast<InitFn>(query(0x0150e828)) : nullptr;
    if (!init || init() != NVAPI_OK) return;
    g_caps = reinterpret_cast<CapsFn>(query(0x52b1499a));
    g_create = reinterpret_cast<CreateSrrvFn>(query(0x99ca2dff));
    g_set_view = reinterpret_cast<SetSrrvFn>(query(0x1b0c2f83));
    g_set_rates = reinterpret_cast<SetRatesFn>(query(0x34f7938f));
}

bool view_locked(ID3D11Device* device, uint32_t w, uint32_t h)
{
    const uint32_t tw = (w + NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
    const uint32_t th = (h + NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;
    if (g_view && tw <= g_tiles_w && th <= g_tiles_h) return true;
    g_view.Reset();
    g_surface.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = tw; d.Height = th; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8_UINT; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> zeros(size_t(tw) * th, 0);
    const D3D11_SUBRESOURCE_DATA init{zeros.data(), tw, 0};
    if (FAILED(device->CreateTexture2D(&d, &init, &g_surface))) return false;
    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = DXGI_FORMAT_R8_UINT;
    vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipSlice = 0;
    if (g_create(device, g_surface.Get(), &vd, &g_view) != NVAPI_OK || !g_view) { g_surface.Reset(); g_view.Reset(); return false; }
    g_tiles_w = tw; g_tiles_h = th;
    return true;
}

} // namespace

bool Available(ID3D11Device* device, const char** why)
{
    std::lock_guard<std::mutex> lk(g_m);
    if (device && device != g_checked_device) {
        g_checked_device = device;
        load_locked();
        g_available = false;
        if (!g_caps || !g_create || !g_set_view || !g_set_rates) {
            g_why = "NVIDIA's driver interface for DirectX 11 coarse shading is not available on this PC";
        } else {
            NV_D3D1x_GRAPHICS_CAPS caps{};
            const NvAPI_Status s = g_caps(device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
            g_available = s == NVAPI_OK && caps.bVariablePixelRateShadingSupported;
            g_why = g_available ? "" : "this graphics card or driver has no coarse shading for DirectX 11 (NVIDIA variable rate shading)";
            na::eventlog::Emit("fold11", "coarse shading for DirectX 11 (NVAPI): status=%d supported=%d sm=%u.%u", int(s),
                               caps.bVariablePixelRateShadingSupported ? 1 : 0, caps.majorSMVersion, caps.minorSMVersion);
        }
    }
    if (why) *why = g_available ? "" : g_why;
    return g_available;
}

bool Apply(ID3D11DeviceContext* c, ID3D11Device* device, uint32_t rate, uint32_t viewports, uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> lk(g_m);
    if (!c || !g_available || !g_set_rates || !g_set_view) return false;
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC vp[NV_MAX_NUM_VIEWPORTS]{};
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC d{};
    d.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    d.pViewports = vp;
    if (!rate) {
        d.numViewports = 0; // every viewport back to one shading per pixel
        return g_set_rates(c, &d) == NVAPI_OK;
    }
    const NV_PIXEL_SHADING_RATE r = rate == 5 ? NV_PIXEL_X1_PER_2X2_RASTER_PIXELS : NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
    if (!device || !view_locked(device, w, h)) return false;
    const uint32_t n = viewports == 0 ? 1u : viewports > NV_MAX_NUM_VIEWPORTS ? NV_MAX_NUM_VIEWPORTS : viewports;
    for (uint32_t i = 0; i < n; ++i) {
        vp[i].enableVariablePixelShadingRate = true;
        for (auto& t : vp[i].shadingRateTable) t = r;
    }
    d.numViewports = n;
    return g_set_view(c, g_view.Get()) == NVAPI_OK && g_set_rates(c, &d) == NVAPI_OK;
}

void Release()
{
    std::lock_guard<std::mutex> lk(g_m);
    g_view.Reset();
    g_surface.Reset();
    g_tiles_w = g_tiles_h = 0;
    g_checked_device = nullptr;
    g_available = false;
}

} // namespace na::native11::coarse
