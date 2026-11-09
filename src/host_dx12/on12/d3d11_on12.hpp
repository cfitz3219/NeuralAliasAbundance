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

#include <dxgi1_2.h>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12Fence;

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

// Recover the exact queue for an already-created translated swap chain.
// Native D3D11 and unknown translation devices return null.
ID3D12CommandQueue* QueueForSwapchain(IDXGISwapChain* chain);

// The D3D12 device behind this translated swap chain (AddRef'ed), or null.
ID3D12Device* DeviceForSwapchain(IDXGISwapChain* chain);

// The menu draws through D3D12, but an 11on12 swap chain only hands out D3D11
// textures. For one frame, hand the menu a D3D12 render target
// (ID3D11On12Device2::UnwrapUnderlyingResource) and take it back with the fence
// that ends the menu draw. By default that target is a canvas of ours (clear it
// first: `clear` is set), which is then blended onto the back buffer by the game's
// D3D11 context; a D3D12 draw straight into the presented buffer stalls the
// D3D11 present for ~2 s. Returns false for any swap chain not on our device.
struct Borrowed {
    void* token = nullptr; // opaque
    bool clear = false;    // the target is the canvas: clear it to transparent before drawing
    Borrowed() = default;
    Borrowed(const Borrowed&) = delete;
    Borrowed& operator=(const Borrowed&) = delete;
    ~Borrowed();
    void Submitted(ID3D12Fence* fence, unsigned long long value);
    ID3D12Fence* fence = nullptr;
    unsigned long long value = 0;
};
bool UnwrapBackBuffer(IDXGISwapChain* sc, unsigned index, ID3D12CommandQueue* queue, unsigned format, ID3D12Resource** out,
                      Borrowed& borrowed);

// ---- Translated presentation (2026-11-09) --------------------------------
// Every DX12 part of the program (overlay queue, menu target, J2 scene
// consumer / present lineage, captures, NR, resize events) reads the game's
// swap chain as a D3D12 swap chain: GetBuffer(ID3D12Resource), its creation
// queue. An 11on12 swap chain answers none of that, so each part failed in
// turn (queue misses, ~2 s menu stalls, "the scene's consumer is not visible").
// Instead, the game's swap chain is created as a REAL D3D12 flip-model chain
// on our queue. The game keeps D3D11 semantics: GetBuffer(0) hands it one
// virtual back buffer (a D3D12 texture of ours wrapped once for D3D11), and
// at Present that buffer is copied into the chain's current D3D12 buffer by a
// D3D12 list submitted through the normal (observed) queue path, exactly like
// a DX12 game's final copy. The program then sees a DX12 game, unchanged.
bool Enabled();

// Factory CreateSwapChain / CreateSwapChainForHwnd: when `device` is one of
// our 11on12 devices and the description can be presented by D3D12, returns
// true with the queue to create on (AddRef'ed) and the translated description.
// False: create the chain as asked (native D3D11, or the old 11on12 chain).
bool TranslateSwapChain(IUnknown* device, const DXGI_SWAP_CHAIN_DESC& game, DXGI_SWAP_CHAIN_DESC& real, ID3D12CommandQueue** queue);
bool TranslateSwapChain1(IUnknown* device, const DXGI_SWAP_CHAIN_DESC1& game, DXGI_SWAP_CHAIN_DESC1& real, ID3D12CommandQueue** queue);
// After the translated chain was created: build the game's virtual back buffer. False = the
// chain is unusable for the game (nothing kept); the caller releases it and creates the chain
// the game asked for on the game's device instead.
bool AttachSwapChain(IDXGISwapChain* chain, IUnknown* device, DXGI_FORMAT game_format, UINT game_count, DXGI_SWAP_EFFECT game_effect,
                     DXGI_SAMPLE_DESC game_samples, DXGI_USAGE game_usage);
bool IsTranslated(IDXGISwapChain* chain);

// GetBuffer for anything but a D3D12 interface on a translated chain: the
// virtual back buffer (the game's D3D11 view of its swap chain).
bool HandleGetBuffer(IDXGISwapChain* chain, unsigned index, const GUID& riid, void** out, long* hr);
// GetDevice that the D3D12 chain refused (e.g. ID3D11Device): the game's device.
bool HandleGetDevice(IDXGISwapChain* chain, const GUID& riid, void** out);
// Game descriptor queries: the game sees the description it asked for.
void RewriteDesc(IDXGISwapChain* chain, DXGI_SWAP_CHAIN_DESC* desc);
void RewriteDesc1(IDXGISwapChain* chain, DXGI_SWAP_CHAIN_DESC1* desc);

// First thing in the game's Present / Present1: hand the virtual buffer's
// picture to the chain's current D3D12 buffer (D3D11 work flushed first).
void BeforePresent(IDXGISwapChain* chain, unsigned flags);
// Right after the game's Present / Present1 returned: how long DXGI took (CPU us), for the
// status line's per-present split (handoff / copy / reacquire / DXGI).
void AfterPresent(IDXGISwapChain* chain, unsigned flags, unsigned long long present_us);
// ResizeBuffers / ResizeBuffers1: waits for our copies, hands the virtual buffer back from
// D3D11 (kept, with the advertised description, until DXGI has decided) and translates the
// count/format/flags. AfterResize: success builds the new virtual buffer and adopts the
// requested description; failure reacquires the exact old buffer, nothing advertised changes.
// False: our copies on the old buffers did not complete in time; the caller must refuse the
// resize without touching the chain (nothing of the game's was released or replaced).
bool BeforeResize(IDXGISwapChain* chain, unsigned* count, DXGI_FORMAT* format, unsigned* flags);
void AfterResize(IDXGISwapChain* chain, long result);
// The game is about to drop its last reference: wait for our GPU copies.
void BeforeFinalRelease(IDXGISwapChain* chain);
void ForgetSwapChain(IDXGISwapChain* chain);

// One status line (no newline) for the 5-second status log.
int FormatStatus(char* out, size_t capacity);

} // namespace na::on12
