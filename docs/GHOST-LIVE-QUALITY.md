# Ghost of Tsushima: live DLSS quality Apply

2026-10-31, Astra. Cort requires live mode changes from NeuralAlias, with no restart or trip to the game menu. This implements a focused engine adapter, not a universal per-game claim. No game was launched, controlled or modified during this work. Live acceptance remains Cort's.

## Evidence from the installed executable

GhostOfTsushima.exe SHA256: ed0931b6f6d3ca3874baf39ebc16d95ff0e93296891182f96c3a790da7109f63. Read-only PE/unwind/disassembly evidence retained under root local/ghost-live-quality-20261031. No executable bytes or proprietary code are committed.

- Settings schema registration names UpscaleMethod and UpscaleQuality at RVAs4612ee/46134f, records offsets b0/b4 in a148-byte POD.
- Menu quality setter491210 stores b4 only when upscaling is enabled and dynamic resolution4c is disabled. UI descriptor at29300 points to this setter.
- Resolution query4683e0 reads the same graphics settings. Method1 selects Streamline DLSS; method0 with AA88=4 selects DLAA. Mode conversion468ce0 maps quality0/1/2/3 to Ultra Performance/Performance/Balanced/Quality.
- Native request421c00 validates via renderer vtable140, copies all148 bytes to pending settings8f0 and sets the pending flag a3a for a forced deferred request. It also sets7a3 so the engine refreshes dependent state.
- Normal per-frame update4257b0 calls dispatcher422760 through vtable130, return42596f. Dispatcher tests pending via4255d0, invokes the game's callbacks and event synchronization, then clears the flag and persists the result. Render-side427150 applies pending settings through421ed0. We reuse this machinery rather than issuing waits or resizing resources ourselves.

## Implementation

Adapter activates only for that exact executable hash and verified hook/call-site bytes. A user Apply queues a choice, not a renderer call on the overlay thread. At the verified normal frame-loop dispatcher entry, the adapter preserves all settings and changes only fixed-mode fields: dynamic resolution off, method/quality or native DLAA AA mode. Existing pending game changes run first. It submits through the native deferred-request function, then forwards the original dispatcher. It never calls a game function while holding our mutex; native renderer synchronization remains the game's responsibility.

The existing NGX confirmation monitor requires30 matching coherent frames at the requested dimensions. Dispatch completion alone is not success. Settings changes cancel outstanding requests; unsupported upscaler selection is reported. The status log includes ghost_apply plus existing size_confirm evidence. Defaults still follow game resolution; disable that toggle to choose a mode in our overlay. Fixed mode intentionally disables the game's dynamic-resolution target, stated in the UI.

The standard NGX resolution answer also now updates Scale and SuperSampling.ScaleFactor alongside dimensions and min/max bounds, including when native dimensions already match. This corrects an inconsistency found by comparison with OptiScaler; it is not claimed to be the demonstrated cause of Ghost's missing live Apply.

## Verification and limits

Release build succeeded. CPU/host/DX12 selection54PASS,1opt-in real-modelSKIP (55 selected),11.97s. New adapter fixture uses the actual dispatch/request code with a simulated engine and covers all5 modes, preservation of unrelated fields, native deferred delivery, wrong caller, already-pending game changes, duplicate requests, cancellation, rejection of another upscaler, original result forwarding and absence of false success. Optimal-settings tests verify scale aliases and pass-through. Final static-byte check caught a missing redundant REX prefix in the initial entry signature; corrected before packaging and rebuilt/tested.

This is implementation plus static/automated evidence. A live Ghost run must establish that the hook is admitted, the normal loop is reached in gameplay and real input dimensions converge without visual/FG regressions. Other Ghost executable versions require re-verifying the adapter. This does not replace the standard NGX/Streamline and existing Unreal/TLOU mechanisms for other games.

Sources used for interface comparison:
- https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md
- https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler/NVNGX_Parameter.cpp
