# Ghost overlay ACCESS_DENIED repair — 2026-10-31

## Failure and eliminated attempts

User's Ghost sessions03:09/03:20 opened Home, then froze/crashed. Preserved logs and dumps under local/ghost-overlay-device-error-20261031. The retry dump shows Ghost deliberately writing the fatal HRESULT0x887A002B to address zero in its error handler, not an overlay CPU access violation. Status has menu submitted48/completed40, two direct queues, native overlay path, hits0/device_queue0/misses1108. NRoff/evals0; qualityrequest0/triggers0.

Opus80dfbcb adds the useful proxy Present fallback for runs where native Present is bypassed. It was NOT engaged in these crashes (overlay_source=native). Opus19dd3ee asks GetDevice for a queue but receives none and still uses the last direct queue; the retry proves that did not correct the failure. Retained boundary visibility work; removed unsupported GetDevice queue recovery and unsafe last-submit fallback.

## Reproduction and repair

The production overlay test now creates the swapchain BEFORE overlay attachment, submits the game backbuffer on queue A, then unrelated work on queue B, opens Home and presents. Unmodified19dd3ee reproduces exactly0x887A002B on the RTX5070Ti; evidence repro.txt. This identifies the invalid operation: overlay submits writes to the native backbuffer on queue B rather than its owning presentation queue A. A fence between arbitrary queues cannot grant permission to write a DXGI buffer on the wrong queue.

When creation/ResizeBuffers1 ownership is known, keep using it. Otherwise watch the actual GetBuffer resources and reuse existing command-list barrier/reset observation. After a real direct-queue submission with an observed write-to-PRESENT transition for a watched buffer, store that queue as private data on the buffer. Overlay retrieves that exact buffer association, retaining the queue for its draw. Until observed, skip injection and continue observing; never guess the last graphics queue. No GPU readback/wait, memory scan, fixed game address or second tracker framework. Entries are recording-local, reset/discard observations cannot publish, resource private data follows buffer lifetime and does not pin swapchain buffers. A late ResizeBuffers1 now also creates missing per-buffer bindings; final chain release erases stale mapping.

This recovers from legacy transition submissions used by the regression. Unobserved/incompatible command-list variants or enhanced-only transitions may withhold recovery; creation/ResizeBuffers1 and explicit SL queues still work. No claim that unsupported observations become permission. User's live Ghost success remains untested.

## Evidence

Full Release build.63 CPU/host/DX12 testsPASS,1 optional actual-model probeSKIP,64selected19.78s. WARP and RTX late-attachment/two-queue cases draw successfully, retain the correct queue, survive resize/queue reassignment and device remains S_OK. Existing ordinary overlay/input/resize, NR/FG, Streamline, live-quality tests pass. Additional discarded/reset, split and read-only recordings do not establish queue ownership. Automated windows only; no game launch, control, native DLL replacement or installation.

Primary references: Microsoft Swap Chains https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains (present queue is assigned at creation, buffers stable until resize); ExecuteCommandLists https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists (submission ordering, runtime validation); DXGI errors https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-error (ACCESS_DENIED); NVIDIA Streamline DLSS-G https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md (proxy/asynchronous presentation).

## 2026-10-31 — correction after failed live acceptance of8b3aae6

Cort's next run still does not show Home. Game reports overlay start failed while native Present counts rise; no menu GPU initialization. This is not successful live acceptance of the queue repair. The old installer enables hooks sequentially. HookVTableSlot refuses while another worker owns an installation transaction; Start short-circuits and skips factory/command-list tracker installation, leaving earlier Present hooks live. With unsafe queue guessing removed, those hooks cannot initialize the menu. Earlier automated tests assumed a fully successful Start and missed this path.

Added an automated competing-installer test: another thread holds an installation transaction while Overlay::Start runs. Oldcodefails Start; repairedcode waits for install ownership, prepares the core DXGI/queue hooks in one existing transaction and commits the batch. No new hook framework. Exact-slot/module/PID failure diagnostics and pre-initialization queue counters are now logged. Existing hook-owner conflicts get a useful error instead of an empty reason. This proves a startup contention defect; original live logs did not record the first failed slot, so it is not proof that contention was the only possible cause in Ghost.

FullReleasePASS;65automatedCPU/host/DX12PASS1optionalmodelSKIP66selected21.48s, including hook transactions, startup contention, WARP/RTX latequeue and retained NR/quality coverage. No game launch/control/deployment. Live acceptance remains with Cort; do not call8b3aae6 a working Ghost overlay.

## 2026-10-31 - Integrated review repair (supersedes the fallback-only route)

The user requested the entire Ghost overlay path, not another isolated patch. Reviewed Opus's review-2026-10-31.md against source, preserved dump analysis and NVIDIA's Streamline DLSS-G guide. Coordinated sole implementation/package ownership in root COMMS; no competing Opus changes were present at the final check.

- Installation: shared bounded worker acquisition now covers NGX, FSR, XeSS, Streamline, engine adapters, variant discovery and overlay/input workers. Same-thread nesting fails immediately; timeout records caller/owner and remains visible in status even after another installation succeeds. Ghost apply no longer returns an empty reason on acquisition/preparation failure. This is worker coordination, not a new render-thread wait.
- Startup: the existing bounded early-factory gate now covers Route A with overlay enabled even when discovery producers are off. Already-loaded Streamline is installed on the host init worker before that gate opens. Engine image scans run after capture hooks are ready. Recovery startup retains the observation needed by Home while leaving NR and saved FG requests disabled.
- Presentation: a qualified game-facing proxy/queue is preferred immediately, even when native Presents also run. Its exact creation/ResizeBuffers1 queue has precedence; the current buffer and queue must belong to the same live device. Native duplicates are suppressed only for the associated chain, including asynchronous generated Presents. Other chains retain their own native route. No three-missed-frame heuristic or process-wide routing latch remains.
- Lifecycle: resize retains route ownership, releases buffer references and uses updated per-buffer queues. A late ResizeBuffers1 can create a missing SL binding. A changed base chain behind the same SL proxy refreshes the association/stream epoch; final release removes it. Temporarily missing metadata cannot send an already-selected proxy back to the native path. Native late-attachment write evidence remains a fallback, not a prerequisite for SL overlay drawing.
- Diagnostics: Route A status, startup, NGX observation and SL boundary logs use a PID suffix before .log. Existing old logs are retained. Helper and game processes no longer interleave or truncate those receipts. The configured basename remains unchanged in the ini.

### Review corrections and teardown disposition

The review correctly identifies that legacy-barrier inference alone cannot guarantee queue recovery. An intermediate compute/copy producer does not grant an arbitrary queue ownership of a native DXGI swapchain buffer. The repair captures the application's queue at its creation/resize boundary instead of inventing a broader last-writer heuristic.

The secondary dump in local/ghost-overlay-freeze-19dd3ee-20261031/analysis/GhostOfTsushima.exe(1).50992.dmp.scan.txt faults reading null+0x48 inside nvngx_dlssg+0x16c50, after the original ACCESS_DENIED. ReleaseFeatureImpl forwards that game-owned native FG handle exactly once and only retires our observation after successful native release. It is not our synthesized FG handle. The evidence does not establish a duplicated release or justify swallowing the call and reporting success after device removal. That suggested blanket guard was not applied. Existing release-failure/retry and successful-retirement tests remain passing. The independent NVIDIA-internal failure is recorded, not claimed repaired; the reproduced wrong-queue submission which triggered the loss is removed.

### Automated evidence and limits

Full Release build passed. Broad selection:72 PASS and1 optional actual-model SKIP (73 selected,29.52s), including CPU, NGX, native-FG/NR, SL, live controls, FSR/XeSS, loader and overlay paths. The subsequent recovery-start change was checked with the final overlay/bootstrap/SL/session selection. PowerShell5.1 additive PBZ/Ghost installation, rollback and Ghost upgrade fixtures passed; fixture output is not a real game installation.

New production-overlay WARP and RTX tests use a game-facing wrapper exposing queue-A buffers and a separate native presentation chain on queue B. One real boundary call is followed by multiple real native Presents and TEST Presents: the menu draws once, on A. No legacy barrier evidence is supplied. Home/input, resize, replacement queue, missing metadata and final release are exercised. The Route A bootstrap test has discovery producers disabled and verifies the queue is captured before any Execute/barrier, then opens Home and submits a real GPU draw. Existing late-queue ACCESS_DENIED reproduction, busy draw-slot and competing-install tests stay passing. Pure association tests cover unrelated chains, changed bases and address reuse; the real SL-hook fixture checks factory and late-resize queue capture plus changed base identity.

These establish implementation behavior with real D3D12 submissions and synthetic SL/model boundaries. They do not run NVIDIA frame interpolation or prove Ghost's live visual quality. Cort retains game installation/testing; no game was launched, controlled or modified. The menu is now part of the game-facing image before FG, so actual interpolated menu appearance remains a live acceptance check.

Primary reference: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md (application/proxy/asynchronous native presentation, HUD composition, resize lifecycle). Microsoft queue references above remain applicable.

Final post-adjustment selection:13/13 PASS21.20s (overlay WARP/RTX, bootstrap normal/recovery, SL boundary and session exit). No test failures outstanding.
