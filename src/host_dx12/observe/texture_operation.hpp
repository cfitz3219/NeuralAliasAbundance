#pragma once
#include "shader_operation.hpp"
#include <d3d12.h>

namespace na::observe {
enum class TextureKind { ClearColor, ClearDepth, Copy, CopyRegion, Resolve };
// Borrowed native call arguments; valid only through Before/After. A replacement
// records its complete private operation under HostWorkScope. Otherwise the
// original API is forwarded exactly once. No guessed descriptor contents.
struct TextureCall {
    TextureKind kind = TextureKind::Copy;
    uint64_t target_id = 0;
    ID3D12Resource* source = nullptr;
    ID3D12Resource* destination = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    float color[4]{}, depth = 0;
    UINT8 stencil = 0;
    D3D12_CLEAR_FLAGS flags{};
    UINT rect_count = 0;
    const D3D12_RECT* rects = nullptr;
    D3D12_TEXTURE_COPY_LOCATION source_location{}, destination_location{};
    const D3D12_BOX* box = nullptr;
    UINT x = 0, y = 0, z = 0, source_sub = 0, destination_sub = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
class TextureOperationHook {
public:
    virtual ~TextureOperationHook() = default;
    virtual ShaderScope Before(ID3D12GraphicsCommandList*, const TextureCall&) noexcept = 0;
    virtual void After(ID3D12GraphicsCommandList*, const TextureCall&, ShaderScope&) noexcept = 0;
};
void SetTextureOperationHook(std::shared_ptr<TextureOperationHook>);
// Independent owners must not replace each other's clear/copy handling. The
// compatibility overload above owns Effects; scene bindings use Bindings.
// Enter in lane order and unwind in reverse, retaining the exact snapshot
// throughout the native call, just like ShaderLane.
enum class TextureLane { Bindings = 0, Effects = 1 };
void SetTextureOperationHook(TextureLane, std::shared_ptr<TextureOperationHook>);
// Observation only: called on the recording thread for each game ClearDepthStencilView that
// clears depth, before it is forwarded, with the view's resource id. Records nothing.
using DepthClearListener = void (*)(ID3D12GraphicsCommandList*, uint64_t target_id);
void SetDepthClearListener(DepthClearListener);
// Depth reads (j2/depth_reads.cpp). The game's shader reads of `game_depth`'s depth plane are made
// on `twin` (same description): every view of it the game creates from now on, and, through
// RedirectExistingDepthReads, the views it made before. Null, null = none. The twin must outlive
// every descriptor that may name it, so the owner keeps it at least as long as the game's depth.
void SetDepthReadTwin(ID3D12Resource* game_depth, ID3D12Resource* twin);
// Rewrites the existing views (and recorded copies of them) of `game_depth`'s depth plane to
// `target`: the twin, or the game's depth itself to undo. Returns how many. Call at the game's
// Present with the game's queue idle, once per change.
uint32_t RedirectExistingDepthReads(ID3D12Resource* game_depth, ID3D12Resource* target);
// Called (as host work) after the game's barrier whose last word on the twinned depth's depth plane
// is a transition, on the recording that holds it. before/after = the game's states;
// all_planes = the barrier named every subresource.
using DepthReadableListener = void (*)(ID3D12GraphicsCommandList* list, ID3D12Resource* game_depth, uint32_t state_before, uint32_t state_after, bool all_planes);
void SetDepthReadableListener(DepthReadableListener);
// A recording marked with SetDepthReadPending (on the recording thread) gets this call, as host
// work, before each of its following draws and dispatches until the mark is taken off (null).
using DepthPendingListener = void (*)(ID3D12GraphicsCommandList* list);
void SetDepthPendingListener(DepthPendingListener);
void SetDepthReadPending(ID3D12GraphicsCommandList* list);
// created: views made on the twin at creation; swept: existing views rewritten; readable: barriers
// that made the depth shader-readable; other: barriers that left it not shader-readable;
// unknown_view: existing views left alone because their exact description was not recorded;
// raced: slots the game wrote during a sweep (given back what the game wrote).
struct DepthReadStats { uint64_t created, swept, readable, other, unknown_view, raced; };
DepthReadStats GetDepthReadStats();
// What the scene's depth was drawn with, as far as `list`'s recording can know: its own pinned
// scene draws, else another recording's pin for the same depth and the sample of the frame being
// recorded now. False: not nudged, not known, or not one shape.
struct SceneRaster { uint64_t sample = 0; bool expanded = false; bool own = false; };
bool SceneRasterFor(ID3D12CommandList* list, uint64_t depth, SceneRaster& out);
// A host draw can be recorded here and everything handed back: a DIRECT recording outside passes,
// with targets, viewports, scissors, topology, pipeline, root and heaps all known, no game query
// counting and no predication.
bool ListHostDrawAllowed(ID3D12CommandList* list);
// A host copy can be recorded here: a DIRECT recording outside passes with no predication.
bool ListHostCopyAllowed(ID3D12CommandList* list);
// The game's own viewports were just bound by host work: the next scene draw applies the sample again.
void ViewportJitterDropped(ID3D12CommandList* list);
// Called only for a forwarded game texture operation, with the actual call arguments.
using TextureTraceListener = void (*)(ID3D12GraphicsCommandList*, const TextureCall&) noexcept;
void SetTextureTraceListener(TextureTraceListener);
} // namespace na::observe
