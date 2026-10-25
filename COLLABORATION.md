# Collaboration agreement — NeuralAlias / NeuralAliasAbundance

## 2026-10-25 — Cort removes the Gate 3 audit recheck

Cort explicitly directed: "No need for Argon to recheck. I don't want to get caught up in endless reviews. One check is enough." For Gate 3, the completed audit satisfies the independent-review requirement. Finish known fixes, verify those corrections with targeted peer checks/regressions, then perform the agreed in-game acceptance tests and close the gate on passing evidence. No additional Argon review packet, delta audit or approval is a prerequisite. This overrides the older recheck requirements below for Gate 3; preserve their historical record. Do not substitute another broad review cycle for the waived recheck.

## 2026-10-24 — Argon independent gate audit (Cort's direction)

This supplements the accepted Astra/Claude ownership split. **Gemini Argon is the
independent third auditor at each gate, starting with Gate 2.** Gate 1's completed
reviews/tests remain historical; this does not claim Argon reviewed them.

### Sequence and responsibilities

1. Astra and Claude finish the phase and cross-review each other's PRs.
2. Authors address findings; the other agent checks the fixes. Run relevant
   regressions and collect CI evidence. Record any unresolved questions explicitly.
3. Astra assembles the Argon handoff; Claude checks that his work and qualifications
   are included. Give Argon the entire repository context and the complete gate
   change set, not just excerpts selected by the authors.
4. Argon audits the exact snapshot independently for architectural and functional
   defects, synchronization/lifetime errors, input assumptions, test gaps, and
   consistency with the universal DX12 acquisition/synthesis goal. Ask for new
   failure paths as well as scrutiny of our fixes; do not treat our prior approval
   as evidence that a component is correct.
5. Authors answer every finding with a fix commit and regression evidence, or a
   reasoned disagreement supported by source/API/runtime evidence. The peer checks
   fixes; send affected changes and responses back to Argon. Unresolved blocking
   findings or disagreements go to Cort rather than silently disappearing.
6. Record Argon's verdict against the final audited commit(s), then run the
   scheduled Windows/GPU/game validation and close the gate. Fixes prompted by that
   session receive peer review and an Argon delta audit when they materially change
   the audited implementation. Ordinary development tests/CI are never postponed
   to wait for a gate audit.

Argon is an auditor, not a new code owner. Existing branches, ownership and frozen
interface rules remain in effect. An audit recommendation is evaluated as a
technical finding; it does not by itself authorize unrelated implementation,
interface changes or game deployment. Additional review improves defect detection;
passing it is not proof of universal compatibility or freedom from bugs.

### Access and handoff

No Gemini tool/session is connected to Astra's workspace at adoption time. Cort
can give the packet to his Gemini/Argon session using repository access or a source
bundle. Do not claim access was granted or the audit ran until confirmed. Prefer
read access; no write credential is needed for an audit. Never put credentials,
private SDKs, runtime DLLs, game files or private captures in the handoff. If direct
Gemini access becomes available, retain the same packet and verdict records.

Post packet links, delivery status, Argon's returned findings and author responses
through **COMMS.md**. Preserve an imported audit's attribution; distinguish its
claims from verified findings and Cort's instructions. At a gate, record audit
status explicitly: `not ready`, `ready for Argon`, `delivered`, `findings open`, or
`audit complete`. Silence and elapsed time are not an audit verdict.

### Required packet contents

- Gate number, universal-DX12 objective, scope and acceptance criteria.
- Base commit, candidate/integration commit, and every included PR with exact head
  SHA. Link full trees and diffs. If PRs are unmerged, provide each branch snapshot
  and the intended combined snapshot/merge order; main alone may omit the work.
- Task/owner inventory, shared interfaces, architecture, and known limitations.
- Both peer reviews, finding dispositions, fix commits and remaining disagreements.
- Reproducible build/test commands, CI run URLs and tested SHAs; distinguish mocks,
  software adapters and real RTX/game runs. Include failures, skips and unrun checks.
- Concrete request: audit the whole gate and relevant surrounding code; report
  each issue with `ARGON-G<gate>-<number>`, severity, file/line at the audited SHA,
  reproducible trigger, impact, proposed correction and a regression that detects it.
- Final verdict: `clear`, `changes required`, or `insufficient evidence`, with the
  exact audited snapshot. Later commits are not covered by an earlier verdict.

A reusable prompt for the handoff:

> You are Argon, the independent third auditor for NeuralAliasAbundance Gate N.
> Astra and Claude have completed the attached cross-reviews and responses.
> Audit the full pinned repository/change set and relevant surrounding architecture.
> Pursue the universal DX12 goal through concrete engineering; distinguish current
> implementation gaps from feasibility claims. Independently test our assumptions
> and the adequacy of our tests. Separate demonstrated bugs, plausible risks and
> questions; support findings with precise source/API evidence and a failure path.
> Return numbered findings and a verdict for the exact snapshot. State what you
> could not access or execute. Do not infer success from our reports alone.

---

## 2026-10-23 recovery coordination — proposed, awaiting Claude

The September agreement below is preserved as history; its machine paths,
status board, build results and worktree arrangement do not describe this cloud
checkout. Claude's `4a4a1ed` recovery has been independently compared with the
74-file export. Read [Codex's review and proposed split](docs/CLAUDE-HANDOFF.md)
and [the acquisition roadmap](docs/INPUT-ACQUISITION-ROADMAP.md).

Retain Claude's host/native-upscaler lane and Codex's discovery/Anno lane. Proposed
next split: Claude restores CMake/SDK gates and host/GPU tests; Codex reconstructs
discovery contracts and CPU regressions after test ownership acknowledgment.
Existing core/ABI changes still need joint review. No new interface is approved
by this note. **Claude response (2026-10-23): ACCEPTED with one tweak. See COMMS.md. Work order now lives in MASTER-PLAN.md.**

Use `claude/<topic>` / `astra/<topic>` branches and prompt commit/push per
CLAUDE.md; never push directly to main. The cloud task is already isolated and
uses its existing checkout without a new worktree. Windows/GPU qualification is
still outstanding. No game deployment is authorized by historical records alone.

---

Written 2026-09-23 by Claude (Fable 5), taking over per user's direction. Audience: GPT-6 Astra, Claude, and the user. Both assistants work on the same repository at `Documents\Codex\2026-09-21\we-re-starting-a-reusable-dx11`, in parallel. This file is the ground truth for who owns what. Update it, don't contradict it silently.

## Division of labor

- **Claude — Route A: DX12 games that already ship working DLSS Super Resolution.** Intercept the game's existing NGX/Streamline calls to obtain labeled color/depth/motion/jitter/exposure, then add DLAA/SR control and neural rendering. First target: **The Last of Us Part I**. First step is the 2–4 h inspection from the feasibility report (confirm actual NGX/Streamline usage, map the interception route) before writing code.
- **Astra — Route B: DX12 games without DLSS.** Buffer/camera discovery, jitter injection, TAA replacement — the adapter-heavy path (24–60 h per game per the report). Astra picks its own target title, preferably one with an established modding community.
- **Shared, frozen-by-default:** everything in `outputs/NeuralAliasAbundance/src/core/` (frame contract, temporal jitter, frame history, upload cache) and its tests, plus the existing evaluator/bridge logic in `outputs/dx11-enhancement/src/`. Either party may READ and reuse; neither changes these interfaces without first proposing the change in this file's "Interface change proposals" section and getting the other's (or the user's) acknowledgment.
- **The installed Alien: Isolation mod is a working deliverable. Do not modify its deployed files, configs, or local/ records as a side effect of either route.**

## Repository rules

1. **Git.** DONE 2026-09-23 (Claude): repo initialized at the project root on branch `main`, baseline commit of the current source/docs state (687 files, ~21 MB; build outputs, `local/`, captures, binaries, archives and third-party clones excluded via root `.gitignore`). The empty commit-less `.git` dirs found in `outputs/dx11-enhancement` were renamed to `.git-empty-disabled` (no history existed); `work/gametism-dlaa`'s empty repo is gitignored as a third-party clone. Each route works on its own branch: `route-a-dlss-intercept` (Claude), `route-b-discovery` (Astra). Merge to `main` only with builds/tests passing. Never rewrite the other's history; never force-push.
   **Worktrees (Astra's proposal — accepted):** the root checkout stays on `main` and is treated as read-mostly shared state; each agent edits code only in its own worktree: `.worktrees/route-a` (branch `route-a-dlss-intercept`) and `.worktrees/route-b` (branch `route-b-discovery`), both created by Claude and gitignored. Never `git checkout`/`switch` the root checkout or the other agent's worktree. Cross-cutting files like this one may be edited in the root `main` checkout and committed there; pull `main` into your branch to pick them up.
2. **Directory ownership.** Route A code lives under `outputs/NeuralAliasAbundance/src/providers/ngx_intercept/` and a new DX12 host under `outputs/NeuralAliasAbundance/src/host_dx12/`. Route B gets sibling directories (e.g. `src/providers/discovery/`). Don't edit files under the other route's directories.
3. **Logging discipline (unchanged from existing practice).** Every action, failure, and user report goes in the owning project's WORKLOG.md; current status goes at the TOP of HANDOFF.md, newest-first, with honest validation limits. Never claim in-game success from offline tests; never claim performance wins from uncontrolled comparisons. Route-specific worklogs may be split (e.g. `WORKLOG-ROUTE-A.md` / `-ROUTE-B.md`) to avoid merge conflicts on one file — do this at first conflict.
4. **Machine state.** Only one of us drives the GPU/game at a time; the user schedules test sessions. Record any installed/deployed file with hashes and rollback info in `local/`, as before. Don't uninstall or overwrite anything the deployment records in `local/` say the other route (or the Alien mod) owns.
5. **Licenses and redistribution.** Same constraints as the feasibility report: no redistribution of NVIDIA runtimes/SDKs, game files, or private captures; preserve third-party notices; GPL sources (OptiScaler, Special K) are architecture references, not copy sources, until a license decision is recorded here.

## Current facts both parties should assume

- Alien: Isolation mod (ReShade host, x86 addon + x64 DX12 helper): DLAA, DLSS Q/B/P with real resolution split, and combined NR are implemented, installed, and passing; NR costs ~10 ms GPU; neural-noise image-quality complaint remains open. See `dx11-enhancement/HANDOFF.md`.
- `outputs/NeuralAliasAbundance/` includes core modules/tests, a native Windows settings preview, hardware-status tool, controlled DX12 GPU fixture, and limited explicitly loaded DXGI forwarding prototype (`src/runtime/`). Last Release build and all eight CTests passed before the collaboration pause. No completed game-injection host, live input provider or connected enhancement backend exists. The forwarding DLL is not deployment-ready. This corrects the earlier core-only inventory; see its HANDOFF.md and README.md.
- Hardware/toolchain: RTX 5070 Ti, driver 616.92-era; VS 2026 (MSVC 19.51), SDK 10.0.26100, CMake, C++17, `/W4 /WX`, CTest.
- Estimates from the feasibility report: Route A first playable prototype ~20–50 h; Route B per-game adapter 24–60 h; full standalone framework 132–312 h.

## Interface change proposals

### B-001 — Astra — proposed DX12 host observations — status: SCOPE ACCEPTED by Claude 2026-09-23

Requirements: `NeuralAliasAbundance/src/providers/discovery/HOST-OBSERVATIONS.md` (branch `route-b-discovery`, commit 81607b5). Claude's full response: `NeuralAliasAbundance/src/host_dx12/OBSERVATION-API.md` (branch `route-a-dlss-intercept`). Summary of the agreement:

- Hooking is MinHook-style vtable hooking, not COM wrappers; unobserved newer interfaces stay functional and are reported `unknown` in the capability report. Destroy/retire via private-data sentinel (reports final release, not last GPU use).
- Identity/ordering as B-001 requires: opaque never-reused 64-bit IDs, command-list `(id, recording_generation)`, atomic global submission sequence taken in the ExecuteCommandLists hook, Signal/Wait relations; CPU callback order is never execution order.
- H1 (Inventory) provides in full: session/capabilities, resource lifetime, command-list lifecycle, queue execution (bundle inheritance NOT modeled — captures with bundles flagged incomplete), swapchain boundaries. Partial: descriptor writes/copies with record-time values only. H2 (Armed capture): PSO identity (hashes at creation), filtered pass bindings, resource use/order, root constants by value. H3: upload-content snapshots (submission-time copy, validity labeled `app-sync-unknown`) and leased async resource snapshots.
- Delivery: preallocated per-thread lock-free rings, worker-thread batching, no allocation/waits/IO on hot paths. Detail channel drop-newest with per-group loss counters; foundational-channel overflow sets session-wide `inventory_invalid` instead of blocking. Astra's budgets adopted as configurable defaults. Host tags and filters its own GPU work.
- Host makes no semantic judgments (depth/motion/camera/HUD/exposure/title hashes) — adapter's job.
- Next step: after Astra acks this scope, Claude drafts the `host_observe.h` event-struct ABI as proposal **B-002**; no consumer coding against structs before B-002 is recorded here.

**B-002 FROZEN at ABI 0.1 (2026-09-23):** Astra accepted rev3 (B-002-REV3-ACK.md, route-b 73d2980). Header marked frozen at route-a commit 03d16fd; further layout/semantics changes require a MAJOR bump proposed here. H1 implementation started framing/loss-first per Astra's sequencing: framing/epoch/status core + contract tests covering the R2/R4 acceptance criteria landed (13/13 CTests). Interim transport is mutex-guarded (semantics contractual; lock-free rings required before in-game host use). Separately: user authorized TLOU proxy deployment; observation proxy installed with manifest/rollback in route-a `local/`; first in-game run pending user session.

**B-002 rev3 (2026-09-23):** Astra's rev2 review (B-002-REV2-REVIEW.md, 1c0fbff) accepted R1–R4 and correctly showed R5's single-observed-signal wait correlation was unsound (threshold waits + unobserved CPU signals can fabricate causality). Claude accepted the minimal fix in full (commit 6bb6022): H1 wait events carry provenance UNKNOWN/AMBIGUOUS with zero source ids absent a documented closed-world guarantee (host-owned fences only; no game fence qualifies); no cross-queue edges from game fence waits in H1; signal hresult proves submission, not completion. Implementation notes also applied (list initial open/closed semantics, swapchain buffer association only via observed GetBuffer, foundational ambiguity-range events). Status: header at rev3 — Astra may confirm and ABI 0.1 freezes.

**B-002 rev2 (2026-09-23):** Astra's review (B-002-ACK.md, 5c4ec5a) requested R1–R5 before freeze; Claude accepted all five and rewrote the header (commit 626d071 on route-a): complete H1 payload set with offset/stride arrays and unknown-value markers (R1); full framing/packing/status-capacity/sink-safety contract (R2); exit-time overlap detection + retroactive NA_EV_ORDER_AMBIGUITY_RANGE (R3); NA_EV_INVENTORY_COMPLETE validity boundary, random session_id, inventory-vs-history distinction (R4); sync hresults, PROV_AMBIGUOUS, provable single-signal source correlation, CPU-signal reserved-not-emitted (R5). Static-assert ABI test added; 11/11 CTests. Status: awaiting Astra's rev2 acceptance, then ABI 0.1 freezes.

### B-002 — Claude — `host_observe.h` event-struct ABI draft — status: PROPOSED 2026-09-23, awaiting Astra review

Astra acked B-001 scope in `src/providers/discovery/B-001-ACK.md` (b2d7c44). Draft header: `NeuralAliasAbundance/src/host_dx12/host_observe.h` on `route-a-dlss-intercept`. How it answers B-001-ACK's five explicit points:

1. **Tickets vs order:** `queue_ticket` is an occurrence identity; it proves same-queue submission order only when `NA_EVF_ORDER_AMBIGUOUS` is clear. The host sets that flag whenever another call on the same queue was concurrently inside its hook window (per-queue in-flight counter), so hook-entry ticketing is never silently promoted to proven order.
2. **Failed Reset / bundles / unknown methods:** `recording_generation` increments only on successful Reset; failed Reset repeats the old generation with `NA_LISTF_RESET_FAILED`. `contains_bundles` on queue-execute events and `NA_EVF_METHOD_UNOBSERVED` are machine-readable.
3. **Batch ownership:** batches delivered serially on one host worker thread, memory valid only during the callback (consumers copy); `abi_major/minor` + per-struct `size_bytes` for append-only evolution; `stop()` delivers a `NA_BATCHF_FINAL` batch and guarantees no sink call after return. H1 events contain no pointers at all; leases are an H3 object with explicit acquire/release.
4. **Loss visibility / recovery:** `na_observe_query_status()` reads host-owned always-allocated atomic storage, readable even with all rings full. `inventory_invalid` is cleared only by `na_observe_resync()`, which bumps the epoch and emits `NA_EV_EPOCH_BEGIN` + fresh snapshot; absence of further loss never clears it. Every event carries its epoch.
5. **Fence provenance:** signals/waits carry `na_fence_provenance`; waits on never-observed values are `PROV_UNKNOWN`, no synthetic signals are emitted, and unknown provenance must not become an inferred completed frame.

Also recorded: third-party deps pinned in `NeuralAliasAbundance/third_party/` — MinHook v1.3.4 (BSD-2) vendored in full, Streamline **v1.1.1** headers (MIT). No public v1.4.7 tag exists (TLOU's interposer version); v1.1.1 matches its export set but struct ABI is unverified — rule recorded in `third_party/README.md`: wrappers may forward without interpreting structs; interpreting any SL struct field requires runtime validation evidence first.

## Status board

| Route | Owner | Target game | Current step | Last update |
|---|---|---|---|---|
| A (DLSS intercept) | Claude | The Last of Us Part I | Inspection done (`docs/TLOU-PART-I.md`, route-a branch): Streamline 1.4.7 + real NGX confirmed. User removed OptiScaler stack (verified; note: active nvngx_dlss.dll remains upgraded 310.9.1.0, .dlsss backups still on disk). Three TLOU sessions done: observation → full DLSS parameter capture → **write path proven** (preset hints written at CreateFeature, creation still succeeds; 41c9098). Known conventions: HDR color, low-res non-jittered MVs, non-inverted depth, auto-exposure; render sizes Perf 1720x720 / Quality 2296x960. Sync loader hook disabled after a startup-crash regression (A12); Init capture intermittent. H1 host: R1–R3 + completion race fixed, accepted by Astra; lock-free transport still pending. Next: optimal-settings callback interception (DLAA/quality-ratio takeover). | 2026-09-23 |
| B (non-DLSS discovery) | Astra | Anno 1800, DX12 (user-selected) | Baseline/worktree verified; discovery plan, evidence records and B-001 host requirements written (81607b5). B-001 scope accepted by Claude — Astra to ack `OBSERVATION-API.md`, then B-002 defines the event-struct ABI. No hooks or GPU/game tests yet. | 2026-09-23 |

Keep this table current when handing back to the user.

### Route B acknowledgment and checkout coordination

Astra has read this agreement and accepts Route B ownership, frozen core/tests/evaluator interfaces, Route A directory ownership, deployment protection and user-scheduled GPU sessions. Anno 1800 remains the user's already-selected first title. Prior audit found an x64 executable and DX12 configuration; no motion/depth/camera/HUD resources have been validated in-game. Evidence: `NeuralAliasAbundance/docs/ANNO-1800.md`.

At acknowledgment, `git status --short --branch` at the stated root returned "not a git repository". Astra has not initialized Git, switched branches, committed or merged; Claude retains baseline ownership. Once baseline exists, Astra will verify it before working on `route-b-discovery`.

**Checkout coordination proposal:** use separate Git worktrees for the two branches, sharing one repository/history. Two different branches cannot safely remain checked out simultaneously in the same working directory. Do not switch the shared checkout underneath the other agent. Await Claude/user acknowledgment of the worktree arrangement before parallel code edits. If a single checkout is required, branch changes and edits must instead be serialized.

After the user's renewed go-ahead: define Route B discovery records and bounded observation requirements in Route B-owned files; identify required callbacks from Claude's `src/host_dx12/`; coordinate integration here rather than editing Route A code. Frozen-interface changes require a proposal and acknowledgment. CPU-only work can proceed after checkout coordination; GPU/game work waits for the user's scheduled slot. No real-game DLAA/SR/NR success is claimed.

## C8-H2-ACK-1 — 2026-10-24, Astra

Acknowledged Claude's numbered C8-H2 proposal on claude/c8-h2 for additive ABI 0.2 events 24–26 and its op vocabulary, with required semantic amendments recorded in COMMS (all descriptor ranges or explicit incompleteness, truncation/unknown range flags, fingerprints not unique identity, record-time descriptor versions not submission proof). No existing 0.1 layout or meaning may change. Exact emitted layouts still need regression review. This permits Claude's append-only implementation; it is not runtime qualification. A5 consumes no new wire events yet.
