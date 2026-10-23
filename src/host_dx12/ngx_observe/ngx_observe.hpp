// NGX-layer observation for Route A (OBSERVATION-API.md).
//
// Hooks the NVSDK_NGX_D3D12_* exports of the NGX CORE modules (nvngx.dll /
// _nvngx.dll; never feature DLLs, A43) to observe the DLSS calls a Streamline title
// actually makes: init identity, feature creation, per-frame evaluation
// cadence and result codes. First increment records call metadata only â€”
// no NVSDK_NGX_Parameter contents are interpreted (the Parameter object is a
// C++ interface whose layout we only trust from the locally pinned NVIDIA
// SDK, used in a local-only build config later).
//
// Function typedefs below mirror the pinned SDK's signatures
// (nvsdk_ngx.h, lines noted in WORKLOG-ROUTE-A A7). Pointer-typed parameters
// are opaque here on purpose.
//
// Hot-path rules: detours touch atomics and a preallocated ring only, then
// forward. No allocation, I/O, locks or GPU work in a detour.
#pragma once

#include <windows.h>

#include <cstdint>

namespace na::ngx {

enum class CallKind : uint32_t {
    Init = 1,          // any NVSDK_NGX_D3D12_Init* flavor
    CreateFeature = 2,
    EvaluateFeature = 3,
    ReleaseFeature = 4,
    Shutdown = 5,
};

// DLSS parameter snapshot read from NVSDK_NGX_Parameter at Create/Evaluate.
// Populated only when the module is built against the locally pinned NVIDIA
// SDK (NA_HAVE_NGX_SDK); otherwise `valid` stays 0. Values the runtime did
// not provide keep their poison defaults â€” never invented.
struct EvalParams {
    uint32_t valid;         // 1 = fields below were read from the params object
    uint32_t reset;         // history reset flag; 0xFFFFFFFF unknown
    uint32_t in_width, in_height;    // render size; 0xFFFFFFFF unknown
    uint32_t out_width, out_height;  // output size; 0xFFFFFFFF unknown
    float jitter_x, jitter_y;        // NaN-free poison: 1e38f unknown
    float mv_scale_x, mv_scale_y;
    uint64_t color, depth, motion, output, exposure; // opaque resource ptrs
    // A13 disambiguation: the authoritative render size per evaluate.
    uint32_t subrect_width, subrect_height; // DLSS.Render.Subrect.Dimensions.*
    uint32_t perf_quality;   // PerfQualityValue at create; 0xFFFFFFFF unknown
    uint32_t create_flags;   // DLSS.Feature.Create.Flags; 0xFFFFFFFF unknown
    uint32_t overridden;     // 1 = host overrides were applied to this call
    // A67 (Astra R4): subrect origins per input; 0xFFFFFFFF unknown. Coverage
    // of a resource is base + size, not size alone.
    uint32_t color_base_x, color_base_y, depth_base_x, depth_base_y;
    uint32_t mv_base_x, mv_base_y, out_base_x, out_base_y;
};

// Takeover writes, all driven by na::settings (ini + overlay):
//  - ApplyCreateOverrides: render-preset hints written INTO the game's
//    parameter object at CreateFeature, before forwarding.
//  - Optimal-settings interception: after NVSDK_NGX_D3D12_GetCapabilityParameters
//    returns, the DLSSOptimalSettingsCallback pointer inside the capability
//    parameters is replaced with ours, which calls the original and then, per
//    settings.resolution_mode, rewrites OutWidth/OutHeight and the dynamic
//    min/max so the ENGINE renders at native (DLAA) or a chosen ratio.
bool ApplyCreateOverrides(void* ngx_parameter);
// The replacement callback itself (exposed for tests): `original` is the
// runtime's callback (may be null in tests), `ngx_parameter` the params.
int32_t OptimalSettingsCallback(void* ngx_parameter);
// Test hook: installs `original_callback` as the forwarded callback.
void SetOriginalOptimalCallbackForTest(void* original_callback);

struct OptimalStats {
    uint64_t calls;             // times our callback ran
    uint32_t last_user_w, last_user_h;   // what the game asked about
    uint32_t last_out_w, last_out_h;     // what we answered
    uint32_t last_overridden;   // 1 if we rewrote the answer
};
OptimalStats GetOptimalStats();

struct CallRecord {
    uint64_t qpc_ticks;     // QueryPerformanceCounter at detour entry
    uint32_t kind;          // CallKind
    uint32_t thread_id;
    int32_t  ngx_result;    // NVSDK_NGX_Result returned by the runtime
    uint32_t feature_id;    // CreateFeature only; else 0
    uint64_t app_id;        // Init only; else 0
    uint64_t handle;        // feature handle value (opaque identity); 0 if n/a
    uint64_t cmdlist;       // opaque ID3D12GraphicsCommandList* value; 0 if n/a
    EvalParams params;      // Create/Evaluate only; zeroed otherwise
};

// Reads the DLSS-relevant keys from an NVSDK_NGX_Parameter object. Returns
// false (out untouched apart from valid=0) when built without the SDK or on
// null input. Exposed for tests.
bool ReadEvalParams(const void* ngx_parameter, EvalParams* out);

struct Stats {
    uint64_t inits;
    uint64_t creates;
    uint64_t evaluates;
    uint64_t releases;
    uint64_t shutdowns;
    uint64_t records_dropped;   // ring overflow (drop-newest)
    uint32_t hooked_functions;  // how many exports were found and hooked
    uint32_t module_attached;   // 0/1
    uint32_t core_preloaded;    // 0/1 PreloadAndHookNgxCore succeeded
    uint32_t rr_seen;           // 0/1 the game created a Ray Reconstruction feature
    uint32_t fg_seen;           // 0/1 the game created a Frame Generation feature (id 11)
    // A30 menu evidence: successful DLSS super-resolution feature creations
    // and the sizes the LAST one was created with (0xFFFFFFFF = not read).
    uint64_t sr_creates;
    uint32_t last_sr_in_w, last_sr_in_h, last_sr_out_w, last_sr_out_h;
    // A42: creation failures and the fallback that keeps DLSS running.
    uint64_t create_failures;   // creations the runtime refused (after fallback)
    uint64_t create_recovered;  // creations rescued by dropping our hints
    uint32_t preset_rejected;   // 0/1 the runtime refused our preset hint; preset reset to Automatic
    uint32_t quality_rejected;  // 0/1 the runtime refused our quality value; game's value restored
    // A46 "Apply now": in-place re-creations of the game's DLSS feature.
    uint64_t reapplies;         // successful re-creations
    uint64_t reapply_failures;  // re-creations the runtime refused (old feature kept)
    uint32_t reapply_pending;   // 0/1 a request waits for the next evaluate
    // A66: game-driven DLSS creations recorded (CreateRecord ring), total.
    uint64_t create_records;
};

// A66/A67 (Astra's A65/A66 reviews): request-scoped render-size confirmation.
// Arm pins the requested size BEFORE the window is touched, so a creation the
// game performs from inside the resize is observed; MarkSizeConfirmationRestored
// sets the boundary after which evaluations count. The evaluate detour then
// counts CONSECUTIVE successful evaluations of the eligible feature (known
// non-zero handle created since arming), each at that size with coherent
// inputs (structural checks only: colour/depth/motion/output resources cover
// base + size for their subrect, motion at render or output size per the
// creation's MVLowRes flag, known non-zero output size, finite sub-pixel
// jitter, finite non-zero motion scale; anything unknown counts as NOT
// coherent). Failed or unreadable evaluations of that feature and evaluations
// with no handle break the streak. Deadlines: `create_timeout_ms` from the
// restoration for the creation, `confirm_timeout_ms` from the creation for
// the streak, `overall_timeout_ms` from arming for everything; enforced
// before every event and on every GetSizeConfirmation() read, so no menu
// page needs to stay open. One request runs; a new one supersedes. Returns
// the request id.
uint64_t ArmSizeConfirmation(uint32_t w, uint32_t h, uint32_t need, uint32_t create_timeout_ms, uint32_t confirm_timeout_ms,
                             uint32_t overall_timeout_ms);
// The window is back; evaluations from now on count (no-op for another id).
void MarkSizeConfirmationRestored(uint64_t id);
// Withdraws the running request (e.g. the user edited the mode meanwhile).
void CancelSizeConfirmation();
struct SizeConfirmation {
    uint64_t id;
    uint32_t state, reason;        // confirm::State / confirm::Reason (size_confirm.hpp)
    uint32_t req_w, req_h, need, streak;
    uint64_t evals, matching, mismatched, incoherent, failed, resets, unknown_handle, other_feature, before_restore, creations;
    uint32_t created_w, created_h, last_w, last_h;
    uint64_t armed_tick, restored_tick, created_tick, done_tick;
};
SizeConfirmation GetSizeConfirmation();

// A66 (R3): one bounded record per game-driven DLSS creation seen by the
// detour (the host's own re-creations bypass it): sequence, time, thread,
// feature, result, sizes, the size-confirmation request active at the time
// (0 = none: a baseline or game-menu creation), and the raw return addresses
// captured IN the detour. Modules are resolved when the record is READ (log
// thread), never in the detour. `game` marks frames inside the process's
// main executable; the rest are labelled by module and kept, so the chain is
// complete, not just its game part.
struct CreateFrame {
    uint64_t address;    // raw return address
    uint64_t rva;        // offset from the containing module's base (0 if unresolved)
    uint32_t game;       // 1 = main executable, 0 = other module, 2 = unresolved
    char module[40];     // file name of the containing module ("" if unresolved)
};
struct CreateRecord {
    uint64_t seq;                 // 1-based, increasing
    uint64_t tick;                // GetTickCount64 at the creation
    uint32_t thread_id;
    uint32_t feature_id;
    int32_t result;               // NVSDK_NGX_Result returned (1 = success)
    uint32_t in_w, in_h, out_w, out_h; // requested by the game's parameters (0xFFFFFFFF = unread)
    uint64_t confirm_id;          // ArmSizeConfirmation id active at the time (0 = none)
    uint32_t frame_count;
    CreateFrame frames[24];       // A71: 24 raw return addresses (was 10)
};
// Copies records with seq > `after_seq`, oldest first, up to `capacity`;
// returns the count. Keeps the last 16.
uint32_t GetCreateRecords(uint64_t after_seq, CreateRecord* out, uint32_t capacity);

// A71 (Astra A70 response: "follow the job producer path"): the game's calls
// at the Streamline boundary that carry its DLSS settings â€” the proxy's typed
// wrappers for slSetFeatureConstants (kind 1) and slGetFeatureSettings
// (kind 2) record them here. These run on the thread that applies the
// game's video settings (synchronously, in the game's own code), so their
// caller chains name the settings-apply path that the asynchronous creation
// job cannot show. The first three 32-bit words of the constants block are
// kept RAW and labelled raw: the pinned SL 1.1.1 header's layout for the
// game's SL 1.4.7 is unverified, so nothing is interpreted from them.
struct SlCallRecord {
    uint64_t seq, tick;
    uint32_t thread_id, kind, feature;
    uint32_t raw[3];              // first three dwords of the constants block (0xFFFFFFFF = none)
    uint32_t repeats;             // A73: identical calls (kind, feature, raw, first six callers) folded into this record
    uint32_t frame_count;
    CreateFrame frames[24];
};
void RecordSlCall(uint32_t kind, uint32_t feature, const void* consts, void* const* frames, uint32_t frame_count);
uint32_t GetSlCallRecords(uint64_t after_seq, SlCallRecord* out, uint32_t capacity);

// A46: ask the evaluate detour to re-create the game's current DLSS feature
// with the current preset choice at the next evaluate (same sizes, same
// quality value; the game keeps using its handle, forwarded to ours). No-op
// until the game has created a DLSS feature.
void RequestReapply();

// A42 fallback helpers (exposed for tests): clear all six preset hint keys
// to Default; write back a quality value.
bool StripPresetHints(void* ngx_parameter);
bool RestoreQualityValue(void* ngx_parameter, uint32_t perf_quality);

// Per-evaluate parameter reads + log records are diagnostics; off by default
// in play sessions to keep the hot path minimal (counters always run).
void SetDetailLogging(bool enabled);

// Deterministic attach (A17): loads the driver's NGX core (_nvngx.dll from
// HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath) BEFORE Streamline
// does and hooks it immediately, so Init/GetCapabilityParameters are
// observed from the very first call. The module reference is kept for the
// process lifetime. Idempotent; returns true when the core is hooked. Must
// not be called under the loader lock.
bool PreloadAndHookNgxCore();

// A30: the ORIGINAL runtime entry points (bypassing our detours) plus the
// parameter allocators, for the live Neural Rendering pass. Returns false
// until a module has been hooked.
bool GetRuntimeEntryPoints(void** create, void** evaluate, void** release, void** allocate_params,
                           void** destroy_params, void** get_capability_params);

// Resolves and hooks whichever observed exports `ngx_module` provides.
// Requires na::hooking::Initialize() to have succeeded. Returns the number
// of functions hooked (0 = nothing matched, or the module is not an NGX
// core). Must NOT be called under the Windows loader lock. Idempotent per
// module. A43: each hooked module keeps its OWN original entry points (the
// driver's two cores may both be hooked); feature DLLs such as
// nvngx_dlss.dll / nvngx_dlssnr.dll export the same names and are refused â€”
// hooking them forwarded the game's DLSS calls into the wrong DLL (session 6).
uint32_t InstallForModule(HMODULE ngx_module);

// True for the NGX core module names (_nvngx.dll, nvngx.dll) and the test
// stub (nvngx_stub.dll); `name` may be a bare name or a full path. False for
// every feature DLL. Exposed for tests.
bool IsNgxCoreModuleName(const wchar_t* name, size_t len);

// Snapshot of counters (atomic reads; internally consistent enough for
// diagnostics, not a coherent seqlock snapshot).
Stats GetStats();

// Drains up to `capacity` records into `out`, oldest first; returns count.
// Single-consumer; intended for the logger thread or tests.
uint32_t DrainRecords(CallRecord* out, uint32_t capacity);

// Starts a watcher that installs hooks automatically when an NGX core module
// (IsNgxCoreModuleName; never a feature DLL, A43) is loaded. Uses
// LdrRegisterDllNotification; the notification only queues the module and
// signals a worker thread, which installs OUTSIDE the loader lock. Also
// scans modules already loaded at call time. Returns false on failure.
// `sync_loader_hook` additionally patches kernelbase LoadLibraryExW so nvngx
// modules are hooked synchronously inside the load call (closes the Init
// race). DEFAULT OFF: correlated with early-startup crashes in TLOU
// (windows.storage/ucrtbase faults, WORKLOG A12); re-enable only for a
// dedicated diagnosis session.
bool StartModuleWatch(bool sync_loader_hook = false);
void StopModuleWatch();   // unregisters, joins the worker; hooks stay until
                          // na::hooking teardown

// Starts/stops a background thread that appends drained records as text
// lines to `log_path` (UTF-16 path, UTF-8 content) every ~250 ms. Never
// called from detours. One logger at a time.
bool StartLogger(const wchar_t* log_path);
void StopLogger();

} // namespace na::ngx
