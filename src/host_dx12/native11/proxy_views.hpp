// DX11 proxy swap chains (on12/d3d11_on12.cpp, [present] dx11_proxy): the game's back buffer is a
// texture shared with our D3D12 presentation queue. A real DX11 swap chain buffer of an 8-bit UNORM
// chain also accepts sRGB views; a plain texture accepts both only when it is TYPELESS. So 8-bit
// proxies are created TYPELESS, and the game must never notice:
//  - a view the game creates on a proxy with no description, or with DXGI_FORMAT_UNKNOWN, gets the
//    chain's own format (render-target, shader-resource and unordered-access views);
//  - ID3D11Texture2D::GetDesc of a proxy reports the chain's format and no sharing flags.
// Every other resource passes through untouched (one pointer comparison per call).
#pragma once

#include <d3d11.h>

namespace na::native11::proxy_views {

// A proxy back buffer and the format the game asked its swap chain for. At most four at once.
void Register(ID3D11Texture2D* proxy, DXGI_FORMAT game_format);
void Unregister(ID3D11Texture2D* proxy);
// True when `r` is a registered proxy; `game_format` receives the chain's format.
bool Lookup(ID3D11Resource* r, DXGI_FORMAT* game_format);

// Description fix-ups (exposed for the shader-resource-view detour in native11/textures.cpp, which
// owns that vtable slot). Return the description to pass on: `in` itself when nothing changes.
const D3D11_SHADER_RESOURCE_VIEW_DESC* FixSrv(ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* in, D3D11_SHADER_RESOURCE_VIEW_DESC& scratch);
const D3D11_RENDER_TARGET_VIEW_DESC* FixRtv(ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* in, D3D11_RENDER_TARGET_VIEW_DESC& scratch);
const D3D11_UNORDERED_ACCESS_VIEW_DESC* FixUav(ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* in, D3D11_UNORDERED_ACCESS_VIEW_DESC& scratch);

// Hooks render-target and unordered-access view creation and Texture2D::GetDesc (shader-resource
// views go through native11/textures.cpp's detour, installed here too when it is not yet).
// `proxy` is any texture of the class to hook. False + nothing changed when hooking fails.
bool Install(ID3D11Device* device, ID3D11Texture2D* proxy);

} // namespace na::native11::proxy_views
