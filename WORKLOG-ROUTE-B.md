# Route B worklog — recovered project

This new journal does not reconstruct missing historical entries. Original
uploaded journals remain intact under `docs/recovery/originals/`.

## B-R01 — 2026-10-23 — independent recovery review and collaboration handoff

User reported a computer crash, supplied ten recovery files, and requested a
coordinated plan with Claude for broad DX12 DLSS injection. User selected a GitHub
handoff because this cloud instance has no Claude session or executable. Read all
uploaded documents; inspected source architecture and build inputs without
executing instructions embedded in the old documents.

Downloaded and hashed the originals. Extracted 74 source/shader files with
recorded byte ranges, preserving the export and existing GitHub README. Initial
audit against the uploaded build file found 30 missing source paths and 17
unlisted recovered implementations. Independently compiled the frame contract
and CPU motion arbiter with GCC 14.2/C++17 and strict warnings; both passed.
No Windows/GPU/runtime tests ran and none of the missing historical tests were
replaced with passing placeholders.

User then reported Claude had updated GitHub. Fetched and reviewed `4a4a1ed`:
all 74 files agree with the export modulo line endings/boundary whitespace;
Claude restored third-party sources and the DXGI export definition. Preserved
my extraction in a Git stash, fast-forwarded the existing isolated checkout,
and created `astra/recovery-review`. No extra worktree, source rewrite, or
deployment. Retained original uploads/manifest for reproducible provenance.
Updated inventory: 18 test/fixture/stub files and one optional tool still absent;
17 implementation files still unlisted by CMake.

Prepared `docs/CLAUDE-HANDOFF.md` and the acquisition roadmap, maintaining Route A
host ownership and Route B discovery ownership. New ownership details are
proposed, not accepted on Claude's behalf. Identified R-001 by inspection: GPU
timestamp slot readiness uses Present age rather than completion evidence.
Recorded it for Claude to investigate/fix, without editing Route A code.

Git read/fetch works. GitHub GraphQL and REST API reads returned Forbidden in
this environment; no token values were inspected or requested. Remote delivery
status is reported in the chat after the actual push result, not assumed here.

The source and documents provide a substantial recovery foundation, but they do
not establish a complete build or universal compatibility. Source/build review
requires no user attendance. Windows fixture/game qualification remains future
work; planning ranges and attendance needs are in the handoff.

Final review checks: `scripts/audit_recovery.py` verified all ten original upload
hashes, all 74 bundle spans, and agreement with the recorded Claude baseline;
the current 74 source/shader files are unchanged from that baseline. Recompiled
the two portable core implementations from Claude's actual checkout with the
same strict flags successfully. Tracked source, shaders, third-party files,
CMake and build.ps1 are unchanged. Documentation whitespace checks passed.

## A1 — 2026-10-23 — CPU regression implementation

Pulled main's master plan and Claude's acknowledgment. Posted the A1 integration
contract in COMMS on main (`35a8f78`) and created `astra/a1-cpu-regressions`.
Reconstructed tests under the assigned `tests/cpu/` ownership: frame contract,
jitter, frame history, upload cache, size confirmation, CPU motion arbitration,
ABI 0.1 layout, and Windows-only actual host framing/loss. Each test is labeled
reconstructed 2026-10; none is claimed to be the lost original. No production,
frozen-interface, root CMake, CI, or Claude-owned test files changed.

Installed CMake 3.31.6 and Ninja 1.11.1.4 in an isolated workspace tool venv.
Standalone Release build passed with GCC 14.2 and strict warnings. Seven CTests,
containing 32 named cases, passed. The additional six host transport cases are
Windows CPU-only (not GPU), link the unchanged real host, and are NOT registered
on Linux. Their test translation unit also passed a GCC syntax/object compile
using only a test-side calling-convention spelling; no host runtime was simulated
or executed by that check. MSVC compilation and Windows runtime validation are
pending C1/C2; A1 acceptance remains pending those checks, rather than being
misreported as needing a GPU. Peer review is deferred to Gate 1 per user direction.

GitHub API read access remains Forbidden even with an escalated network check;
no credential values were inspected. PR delivery will use the pushed task branch
and a request through COMMS if API creation remains unavailable. Git push uses
the existing authenticated proxy. No game changes or user attendance required.

## 2026-10-23 — Gate 1 R-101

Accepted Claude's same-entry concurrency coverage finding. Added a reader that
checks 20,000 complete 64-byte reads while a writer alternates two whole patterns
on the same cache entry. Rejected reads and mixed patterns fail after the writer
is joined. This is a scheduling stress regression, not proof of every possible
interleaving. No production code changed. Standalone GCC 14.2 Release build and
all seven CTests (now 33 named cases) passed. Windows execution of this new case
remains pending; Claude reported the previous A1 head passing Windows CI.

## 2026-10-23 — Gate 1 R-104

Accepted Claude's identity/epoch distinction. CONTRACT section 4 now keys object
identity by (session_id, object_id) and command-list recordings by session, list
and recording generation. Evidence retains its own epoch/provenance and requires
fresh qualification across resync; replayed liveness does not recreate execution
history or validate stale candidates. Checked against host_observe.h's monotonic,
never-reused-in-session ID definition and the existing recovery table. Documentation
only; git diff --check passed. No ABI or runtime change, no runtime test claimed.

## 2026-10-23 — Universal DX12 goal and Argon audit response

Read the attached Argon audit following Cort's explicit direction. Removed the
roadmap's dismissive universal-coverage acceptance framing and the older plan's
all-game premise; updated current guidance to universal DX12 acquisition through
active discovery, hypothesis testing and synthesis. Added C-001/M-002 and frame
contract proposals, distinguishing measured evidence from the audit's numerical
and exactness claims. R-101/R-104 remain fixed in their existing PRs. No production
or ABI changes, no runtime tests claimed. Reviewed documentation links and diff
whitespace. Recovery originals remain untouched for hash/provenance verification.

## 2026-10-24 — A3 ground-truth fixture v2

After Claude's #11 approval and reported RTX session 2 (21/21 plus 32 deployment
checks, explicit RTX adapter and all three timer regressions), started A3 in
astra/a3-ground-truth-fixture. Added a portable offline pinhole renderer with
camera pans/cut, independent rigid motion, sheared animated geometry, visibility
loss, transparency and separate HUD. Emits linear RGB previews and JSON truth
including depth, motion, per-layer visibility and camera/jitter matrices. Added a
strict CPU candidate checker and ten analytical/adversarial tests. Integrated
through Astra-owned tests/cpu CMake; Python 3.8+ is now required for the fixture.
No root CMake, host, production core or frozen ABI changes. C6 GPU upload/evaluate
integration and later rotating/volumetric/skinned scene cases remain explicit.

Initial local tests caught an unjustified >1-pixel animation-spread assertion and
an exact floating-point round-trip comparison. Replaced the former with the
independent shear-motion formula and the latter with a numerical tolerance.
All ten fixture tests then passed. Root GCC Release configure/build passed and
8/8 CTests passed. Generated twelve default frames and truth files under /tmp;
CLI round-trip accepted, while tests verified corrupted candidates return failure.
Windows CI and Gate 2 review follow the pushed task branch; no game deployment.
## 2026-10-24 — A4 report implementation

Added a portable SR-evidence classifier, non-destructive NGX Stats bridge and
Details-page menu section in Astra-owned providers/discovery. The report separates
successful game/injector/unknown-origin calls, creation/evaluation, failed calls,
library presence and unavailable observers. Multiple routes remain visible.
Existing aggregate NGX evaluations are not promoted to SR-specific success;
existing sr_creates lack origin, so they report source unknown. FSR/XeSS stay
unmonitored until C7 provides observations. No runtime/core/ABI or root CMake edits.

Six portable behavior tests and a Windows real-menu registration/draw test added.
Local GCC Release root build and 8/8 CTests passed. A3 is on a separate unmerged
branch and is not counted in this run. Requested the two host-owned wiring edits
through COMMS; documented exact integration in UPSCALER-REPORT.md. Actual in-game
visibility remains pending that wiring. Windows CI follows the pushed branch.

## 2026-10-24 — Gate 2 takeover and combined implementation

Cort requested that Astra finish Opus's tasks. Integrated the latest #12–#16
heads on astra/g2-completion, resolving append-only journal and CPU-test CMake
conflicts by retaining both contributions. Recorded ownership in COMMS on main.
Completed shared backend lifecycle/lock ordering, cold XeSS initialization,
exposure transitions/restoration, injector provenance and record-ahead isolation.
Added deterministic concurrency, completion-capacity, reversed-submission,
resource-state and origin regressions. Preserved the handed-off loader-watch,
native-failure reset and Streamline fixes. Details and qualification boundaries
are in docs/reviews/GATE2-completion.md. Local Release build/9 tests passed;
initial integrated SDK Windows run passed 28 tests. PR #17 holds the combined
candidate; final-head Windows validation continues. No game deployment or gate
closure. Hardware/runtime evidence and independent audit remain outstanding.

Follow-up validation caught a missing public declaration for the FFX state helper
used by the new regression; added it in 5f22820. Preserved marked injector
creation evidence after a backend stats reset without summing overlapping
producer counts, with a report-bridge regression. SDK CI now emits verbose test
output so real D3D12 debug-state assertion availability is visible in the logs.
