// DX11 games through the DX12 program (Cort, 2026-11-08: "a way to bridge that
// program to a DX11 program").
//
// Windows ships a DX11-on-DX12 translation layer (D3D11On12). When [dx11on12]
// enabled=1, the game's D3D11CreateDevice / D3D11CreateDeviceAndSwapChain are
// answered with an 11on12 device built on OUR D3D12 device and direct queue.
// Every draw the game makes then reaches the GPU as D3D12 command lists on that
// queue, so the unchanged DX12 host (J2 scene/depth/camera discovery, DLAA,
// overlay menu, logs) sees a DX12 game. Nothing here knows any game.
//
// Fail-quiet: software/WARP/debug devices, feature-level probes, and any
// failure (no D3D12, no 11on12, an 11on12 device that cannot own a swap chain)
// fall back to the game's original call; the reason is in the status line.
#pragma once

#include <cstddef>

struct IUnknown;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12Fence;
struct IDXGISwapChain;

namespace na::on12 {

using LogFn = void (*)(const char*);

// Reads [dx11on12] enabled from `ini`; when on, hooks the system d3d11.dll
// exports. Call from the host init thread (never under the loader lock),
// before the game creates its device.
void Start(const wchar_t* ini, LogFn log);

// The D3D12 direct queue behind an 11on12 device we created, for any interface
// of that device (ID3D11Device, IDXGIDevice, ...). Null when not ours. The
// returned pointer is AddRef'ed.
ID3D12CommandQueue* QueueFor(IUnknown* device);

// The D3D12 device behind an 11on12 device we created (AddRef'ed), or null.
ID3D12Device* DeviceFor(IUnknown* device);

// The menu draws on the back buffer as a D3D12 resource, but an 11on12 swap
// chain only hands out D3D11 textures. Borrow the D3D12 resource underneath
// (ID3D11On12Device2::UnwrapUnderlyingResource) for one frame, and give it
// back with the fence that ends our draw (or none when nothing was submitted).
// Unwrap returns false for any swap chain that is not on our 11on12 device.
struct Borrowed {
    void* token = nullptr; // opaque: the D3D11 back buffer + its 11on12 device
    Borrowed() = default;
    Borrowed(const Borrowed&) = delete;
    Borrowed& operator=(const Borrowed&) = delete;
    ~Borrowed();
    void Submitted(ID3D12Fence* fence, unsigned long long value);
    ID3D12Fence* fence = nullptr;
    unsigned long long value = 0;
};
bool UnwrapBackBuffer(IDXGISwapChain* sc, unsigned index, ID3D12CommandQueue* queue, ID3D12Resource** out, Borrowed& borrowed);

// [dx11on12] menu_buffer: 1/2 draw normally; 3 = submit nothing, 4 = submit an empty
// list, 5 = submit only the back buffer barriers (diagnostics for the menu-open stall).
int BorrowMode();

// One status line (no newline) for the 5-second status log.
int FormatStatus(char* out, size_t capacity);

} // namespace na::on12
