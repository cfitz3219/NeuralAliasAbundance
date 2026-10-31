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
