# Ownership repair, reviewed first stage

This candidate implements only the first stage approved in PLAN-REVIEW72d3aaf: O4 core/menu readiness, O1 presentation ownership, cursor ownership/counting, and Opus cd012b1's version-based FG status. The controlling plan is OWNERSHIP-REPAIR-PLAN-20261031.md revision2. No NR algorithm/route scoping or settings/session ownership changes are included.

Core attachment now owns the existing swapchain/queue/submission/reset hooks and discovery worker independently of optional input/ImGui readiness. Both bootstrap entry points use the split. Menu creation failure releases partial backend/window/resources, keeps core work running, and retries on a later frame. Eligible engine controls and NGX logging no longer depend on successful menu setup. Core and menu readiness are reported separately. The early factory gate stays bounded; expiry is recorded as missed early evidence, with late queue recovery retained.

Streamline attachment retries unfinished groups after install acquisition contention. The attach worker and existing install mutex serialize publication. Completed groups/originals are preserved; preparation/commit failures are terminal so dead MinHook targets are never re-enabled as fresh hooks. Deferred proxy/factory acquisition and thread-creation failures release their request latch. First-group deferral and partial-commit failure receive separate regressions.

Streamline's creation callback marks proxy/base ownership before the application can acquire buffers. Once marked, no new NeuralAlias substitute buffers are issued. An existing substitute retains its Present delivery until a successful resize; a rejected resize retains the exact old images and virtual index. An incomplete drain refuses native resize. During handover NeuralAlias does not generate competing intermediate frames or rebind the externally owned queue. Final release clears ownership. Existing chain/queue/RecordingLease machinery is retained.

Lock order: route-table locks end before COM, presenter or consumer work. Cached GetBuffer uses a separate brief buffer-publication mutex, independent of the presenter state mutex held around NR callbacks. Index/active-buffer queries use published atomics. The concurrency fixture asks for an already-acquired buffer from another thread while the presenter frame callback is active; the request must return without waiting for that callback.

Cursor correction: the old ShowCursor loop omitted the final increment, leaving Windows' visibility count one higher after every Home cycle. Count and restore every increment. Intercept competing SetCursor calls while Home is open; while closed, forward the application's cursor request. The fixture checks the actual cursor handle and before/after Windows visibility count. This is code/fixture evidence; Cort still confirms the particular frozen icon in-game.

FG status: merged cb36419/cd012b1. The status leads with the request forwarded to NVIDIA, distinguishes it from the game's original request, and reads the loaded interposer version. Ghost Streamline2.4's one-generated-frame interface is shown as 2x-only; absent/zero capability alone is not used to invent an old version. This stage does not add a multiplier dispatch/confirmation redesign.

## Automated evidence

Final full Release build PASS; CTest selected136:131 PASS,5 SKIP,0 FAIL in81.02s. Skips: optional real-NR runtime probe, three unsupported16xMSAA fixtures, and the optional upscaler-jitter integration. No missing test is counted as a pass. Installer fixtures use Windows PowerShell5.1 and fake game folders: additive install/rollback, immutable payload checks, preference restoration, native DLL preservation, wrapper validation/upgrade ordering/repeat-install and rollback-before-upgrade. No installed game was launched, controlled, installed into or changed.

New presenter fixture uses real WARP textures, production boundary routing, GetBuffer/Index/Present/resize, test-only GPU readback, failed native resize, and a concurrent buffer request. It verifies the acquired red image reached the native display buffer, not merely a status flag. Menu-input and partial-ImGui creation failures leave queue/submission/device discovery alive; later menu retry draws successfully. Existing WARP/RTX, SL, NGX, NR, session and live-control fixtures are retained.

Initial engineering failures were fixed before packaging: core/menu split required the bootstrap test to wait for menu readiness separately from the early queue gate; an added presenter query lock reentered during native DXGI work, so hot queries were changed to atomics and cached GetBuffer received an independent short publication lock. These candidates were never deployed. Full source/fixture logs remain local/ownership-stage1-20261031.

## Delivery and remaining scope

The first-stage packages are separate candidates. Existing Documents updater commands, installed receipts, native game DLSS/Streamline/FG DLLs and b71ba0b package stay unchanged. Ghost's actual known-working baseline is b71ba0b. PBZ's physical installation is still63395c7 and has no new live qualification. Candidate repair uses those actual previous receipts rather than assuming the latest launcher was installed. Existing additive repair may reset NeuralAlias preferences; this is not fixed by this stage.

Candidate rollback uses the existing additive deployment receipt. To return to the user-confirmed Ghost runtime after rollback, use the retained NA-Tsushima-package-b71ba0b installer; it is preserved independently. No automatic deployment or new review round is performed.

O3 is next and separate: try the review's rendering-qualified per-process marker before more elaborate ownership machinery. O2 remains conditional on written R1 frame/view-association evidence. R1/R2 follow-up is still absent in COMMS. Today's native source1/Streamline source2 NR behavior is retained; no universal NR ownership or game visual-quality pass is claimed.

Cort's short live check, when desired: Ghost Home open/close and cursor; existing model/mode/NR controls; native FG, resize and quality changes; then a separate PBZ session. Game testing remains his.
