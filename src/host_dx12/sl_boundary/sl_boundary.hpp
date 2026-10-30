// Game-facing Streamline boundary (Route A, games that ship Streamline 2.x).
//
// WHY: our dxgi.dll hooks sit BENEATH Streamline: SL's swap-chain proxy calls
// into the real chain after DLSS-G has already taken its copies. This module
// observes the game's calls INTO Streamline instead:
//   - slSetTag / slSetTagForFrame (exports of the loaded sl.interposer.dll),
//     with each tag's resource, declared state, lifecycle, extent, frame
//     token, viewport and command buffer, reported BEFORE the original runs
//     (so an eOnlyValidNow tag can be acted on before SL snapshots it);
//   - the game's Present / Present1 on SL's IDXGISwapChain proxy, reported
//     BEFORE and AFTER the original (so work recorded at that point precedes
//     everything Streamline and DLSS-G do with the frame).
//
// Discovery covers every path SL can create its swap-chain proxy by: the
// proxy class's vtable is located from MSVC RTTI in sl.interposer.dll
// (".?AUDXGISwapChain@interposer@sl@@"), which is shared by proxies made via
// slUpgradeInterface, SL's DXGI factory proxy, or SL's hooks on the real
// factory (automatic proxy creation, dxgi.cpp setupSwapchainProxy). If RTTI
// is absent, swap chains and factories returned by slUpgradeInterface and the
// interposer's CreateDXGIFactory* exports are inspected as a fallback; the
// status line names which path found it.
//
// WHAT THIS MODULE DOES NOT CLAIM:
//   - Same CPU thread is not GPU ordering. A tag's `state` is the game's
//     declaration for the point of `command_list` in GPU order; which queue
//     executes that list, and when, is not known at the call.
//   - Pointers in events are BORROWED for the callback's duration. An AddRef
//     keeps an object alive, not its contents: eOnlyValidNow resources may be
//     overwritten as soon as the original tag call returns.
//   - Callbacks run whenever the boundary is installed; whether an effect is
//     enabled is the consumer's decision (FG suspension does not stop the
//     metadata stream).
//
// Callback rules: invoked on the game's thread inside SL entry points. Do not
// block, do not call slSetTag*/Present re-entrantly, do not install hooks.
#pragma once

#include <cstddef>
#include <cstdint>

struct IDXGISwapChain;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;
struct DXGI_PRESENT_PARAMETERS;

namespace na::slb {

struct Tag {
    uint32_t buffer_type = 0;        // sl::kBufferType* (abi::kBuffer* in sl_tags.hpp)
    uint32_t lifecycle = 0;          // 0 eOnlyValidNow, 1 eValidUntilPresent, 2 eValidUntilEvaluate
    ID3D12Resource* resource = nullptr; // null: the game untagged this buffer type
    uint32_t resource_type = 0;      // sl::ResourceType, 0 = eTex2d
    uint32_t state = 0xFFFFFFFFu;    // game-declared D3D12_RESOURCE_STATES at command_list's point (UINT_MAX: none)
    uint32_t width = 0, height = 0, native_format = 0;
    uint32_t extent_top = 0, extent_left = 0, extent_width = 0, extent_height = 0; // width 0: whole resource
    bool resource_decoded = false;   // false: tag understood but its sl::Resource block was not
};

constexpr uint32_t kMaxTagsPerCall = 32;

struct TagEvent {
    uint64_t sequence = 0;           // per process, every tag call
    uint64_t stream_epoch = 0;       // swap-chain incarnation current at the call (see PresentEvent)
    bool for_frame = false;          // slSetTagForFrame (true) or legacy slSetTag
    bool has_frame = false;
    uint32_t frame_token = 0;        // sl::FrameToken value
    bool has_viewport = false;
    uint32_t viewport = 0;           // sl::ViewportHandle value
    ID3D12GraphicsCommandList* command_list = nullptr; // sl::CommandBuffer*; may be null for non-volatile tags
    uint32_t thread_id = 0;
    bool decoded = false;            // false: tag layout not recognised (tags empty; the original still runs)
    uint32_t raw_count = 0;          // numTags the game passed
    uint32_t count = 0;              // decoded tags (<= kMaxTagsPerCall)
    const Tag* tags = nullptr;
};

enum class PresentKind : uint32_t {
    Real = 0,  // a game frame: real_serial advanced for it
    Retry = 1, // the game re-presenting after DXGI_ERROR_WAS_STILL_DRAWING: same real_serial
    Test = 2,  // DXGI_PRESENT_TEST: no frame
};

struct PresentEvent {
    uint64_t real_serial = 0;        // advances once per accepted real game Present
    uint64_t stream_epoch = 0;       // +1 for a newly seen proxy and after each successful ResizeBuffers*
    PresentKind kind = PresentKind::Real;
    bool present1 = false;
    IDXGISwapChain* game_facing = nullptr; // SL's proxy; GetBuffer/GetCurrentBackBufferIndex on it return what the game drew
    IDXGISwapChain* base = nullptr;  // the chain beneath SL (via SL's StreamlineRetrieveBaseInterface), may be null
    uint32_t sync = 0, flags = 0;
    const DXGI_PRESENT_PARAMETERS* params = nullptr;
    uint32_t thread_id = 0;
    uint64_t last_tag_sequence = 0;  // newest TagEvent::sequence issued before this Present
    bool has_last_frame_token = false;
    uint32_t last_frame_token = 0;   // frame token of the newest decoded tag call
};

using TagCallback = void (*)(const TagEvent& event, void* user);
using BeforePresentCallback = void (*)(const PresentEvent& event, void* user);
using AfterPresentCallback = void (*)(const PresentEvent& event, long hr, void* user);

struct Callbacks {
    TagCallback on_tag = nullptr;
    BeforePresentCallback before_present = nullptr;
    AfterPresentCallback after_present = nullptr;
    void* user = nullptr;
};

// One consumer (NR). Replaces any previous set; pass {} to detach.
void SetCallbacks(const Callbacks& callbacks);

// Watches for sl.interposer.dll and installs once it is loaded. Called from
// host_boot on Route A. `log_path` (optional): evidence file written 30 s and
// 120 s after start (status line + every distinct tag shape seen).
// Returns false if the module watch could not start.
bool Start(const wchar_t* log_path = nullptr);

enum class Discovery : uint32_t { None = 0, Rtti = 1, UpgradeInterface = 2, FactoryExport = 3 };

struct Stats {
    uint32_t module_seen = 0;        // sl.interposer.dll loaded
    uint32_t tag_hooks = 0;          // 0..2 (slSetTag, slSetTagForFrame)
    uint32_t present_hooks = 0;      // 0..2 (Present, Present1 on the proxy)
    Discovery discovery = Discovery::None;
    uint64_t tag_calls = 0, tag_undecoded = 0, tags_seen = 0;
    uint64_t presents_real = 0, presents_retry = 0, presents_test = 0;
    uint64_t stream_epoch = 0;
    uint32_t proxies_seen = 0;
    // Newest decoded tag of the three FG/NR inputs, for the status line.
    uint32_t hudless_lifecycle = 0xFFFFFFFFu, hudless_state = 0xFFFFFFFFu;
    uint32_t depth_lifecycle = 0xFFFFFFFFu, motion_lifecycle = 0xFFFFFFFFu;
    uint32_t backbuffer_tagged = 0, ui_tagged = 0;
    char reason[96] = "";
};
Stats GetStats();

// One status-log line ("sl_boundary ..."), NUL-terminated. Returns length.
int FormatStatus(char* out, size_t cap);

// ---- native frame generation (DLSS-G) control ----
// Wraps the game's own slDLSSGSetOptions / slDLSSGGetState (resolved through
// sl.interposer's slGetFeatureFunction): the game keeps ownership of DLSS-G
// and presentation; a request only rewrites `mode` / `numFramesToGenerate` in
// a copy of the game's options (every other field, the struct version and
// its `next` chain are preserved) on the game's next call. Requests outside
// what the runtime reports (DLSSGState::numFramesToGenerateMax, dynamic MFG
// support) are refused, not clamped silently. Our own FG presenter is never
// involved.
enum class FgMode : uint32_t { Game = 0, Off = 1, On = 2, Dynamic = 3 };
struct FgRequest {
    FgMode mode = FgMode::Game;
    uint32_t frames = 0; // generated frames per real frame (1 = 2x, 3 = 4x); 0 = keep the game's
};
void SetFgRequest(const FgRequest& request);

struct FgState {
    uint32_t set_hooked = 0, get_hooked = 0;
    uint64_t set_calls = 0, get_calls = 0;
    uint32_t game_mode = 0xFFFFFFFFu, game_frames = 0;       // last values the game passed (sl::DLSSGMode)
    uint32_t sent_mode = 0xFFFFFFFFu, sent_frames = 0;       // what reached Streamline
    uint32_t max_frames = 0xFFFFFFFFu;                       // DLSSGState::numFramesToGenerateMax (UINT_MAX unknown)
    int32_t dynamic_supported = -1;                          // DLSSGState::bIsDynamicMFGSupported (-1 unknown)
    uint32_t status = 0xFFFFFFFFu;                           // DLSSGState::status flags
    uint32_t presented = 0;                                  // numFramesActuallyPresented at the last state read
    int32_t last_result = 0;                                 // sl::Result of the last forwarded SetOptions
    FgRequest request;
    char reason[160] = "";
};
FgState GetFgState();

// ---- test seams (no game, no sl.interposer) ----
// Runs the same installation against `module` as if it were sl.interposer.dll
// (RTTI search, tag export hooks). Returns true if anything was hooked.
bool InstallForModuleForTests(void* module);
void ResetCountersForTests();

} // namespace na::slb
