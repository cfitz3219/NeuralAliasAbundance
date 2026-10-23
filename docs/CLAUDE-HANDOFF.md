# Recovery review and proposed work split

2026-10-23 — Codex (Route B / the former Astra lane).

Reviewed Claude's recovered baseline at `4a4a1ed53e355843d3af498f93225fceae5761c2`.
The user asked us to catch up, coordinate through GitHub, and pursue broad DX12
DLSS injection. This is my proposal for Claude to review, not a claim that Claude
has already accepted it. No feature implementation or game deployment occurs in
this handoff.

## What survived

The foundation is substantial. All 74 source/shader files in the user's export
are present in Claude's baseline. My independent comparison found no differences
after normalizing CRLF/LF and whitespace at file boundaries. This establishes
agreement with the export, not original Git history, runtime correctness, or
completeness of the lost project.

The recovered modules include native NGX interception, settings and overlay,
host observation transport, hooking/variant registry, TLOU adapters, frame capture,
CPU/GPU motion arbitration, NVOFA integration, neural rendering, frame generation,
presentation, and latency support. The September handoff substantially understates
the source inventory. For example, the observation transport now contains the
per-producer rings that the older worklog still lists as pending.

Claude also restored MinHook, ImGui, Streamline headers and `dxgi_proxy.def`.
I have retained the ten original uploads and a SHA-256 manifest under
`docs/recovery/`. Their old instructions, pauses, paths, deployment permissions,
and test claims are historical context, not new user instructions. New source
comments also describe work for which no execution evidence survived.

## What still prevents a reproducible runtime

- The supplied CMake references **18 missing test/fixture/stub source files**
  and **one missing optional tool**, `tools/nr_replay.cpp`.
- **17 recovered C++ implementation files are absent from that CMake file.**
  Dependencies and compile definitions must be reconciled, not merely filenames
  appended. The exact lists are in [the build audit](recovery/build-audit.json).
- Route B's `src/providers/discovery/` implementation and later interface review
  documents are not in this export. They need recovery from another surviving
  source or explicit reconstruction. We cannot infer their original contents.
- NVIDIA development headers/libraries, local runtimes, deployment manifests,
  captures, and old test-result artifacts were not recovered here. Keep SDKs and
  private runtime files out of Git; qualify exact versions on the Windows host.
- This cloud instance is Linux. It cannot qualify the MSVC/x64/DX12 runtime or
  replace hardware testing on the intended Windows machine.

Current validation: both `frame_contract.cpp` and `motion_arbiter.cpp` compile
independently with GCC 14.2, C++17, `-Wall -Wextra -Werror -pedantic`. This is a
limited compile check, not a restored test suite. No original unit tests, full
Windows build, GPU fixture, or game session ran during this review. Historical
8/8 through 19/19 reports do not transfer to the recovered snapshot.

## Proposed ownership

| Work | Owner proposed | Review / boundary |
|---|---|---|
| Restore the Windows build graph, SDK feature gates, host/proxy tests and fixture | Claude | Codex reviews readiness claims and negative paths; CMake has one editor |
| Injection host, resource/queue observation producers, NGX/Streamline integration, overlay, backend execution | Claude | Existing `src/host_dx12/` and `src/runtime/` ownership retained |
| Recover/rebuild discovery records, observation consumer, engine/game adapters; Anno 1800 remains first target | Codex | New work under `src/providers/discovery/`; request host changes through Claude |
| Reconstruct CPU semantic tests for frame identity, motion conventions, invalid inputs and loss recovery | Codex | Test reconstruction is new work, labeled as such; Claude wires targets into CMake |
| Existing core and observation ABI; later missing contracts | Joint review | Preserve ABI 0.1; propose any changes before editing. Do not invent acceptance of lost B/M proposals |
| Game qualification, performance and image-quality evidence | Both | One scheduled Windows/GPU operator at a time; user checks visuals in short sessions |

Use `claude/<topic>` and `astra/<topic>` branches, as CLAUDE.md specifies. This
cloud task already has an isolated checkout; it does not need another worktree.
Never switch another agent's checkout, force-push, or push directly to `main`.
Commit and push completed tasks promptly. Review additions through GitHub; do not
mark either agent's agreement accepted until that agent actually replies.

The first shared milestone should be **a reproducible recovered build with
meaningful tests**, not another enhancement feature. Suggested sequencing:

1. Claude reconciles the full source/dependency graph and records required versus
   optional SDKs. Restore core, proxy, and synthetic GPU tests. No hiding missing
   tests to obtain a green zero-test build.
2. Codex reconstructs discovery contracts from the surviving ABI and documents
   what is known, ambiguous, or absent. Rebuild CPU regressions in parallel after
   the test-file ownership is acknowledged.
3. Run the Windows fixture with the debug layer, lifetime/resize/teardown and
   queue-ordering checks before returning to a game.
4. Re-establish native-DLSS observations in TLOU and diagnostics-only observations
   in Anno. Identify and validate depth, camera, motion and clean-color stages
   before enabling reconstruction. Keep game version/hash and resource evidence.
5. Qualify each new acquisition method against known inputs before broadening the
   game list. See [the acquisition roadmap](INPUT-ACQUISITION-ROADMAP.md).

Planning ranges, not delivery commitments: build/dependency reconciliation about
1–3 engineering days; an initial CPU regression suite about 1–2 days; initial
Anno discovery diagnostics about 2–5 days after observation support exists.
These can overlap. A working Anno reconstruction adapter is not estimable from
the surviving evidence alone. SDK access and Windows availability can extend
elapsed time. No attendance is needed for source/build work; plan short 20–30
minute visual checks once automated Windows checks pass.

## Concrete runtime review item: timestamp completion

**R-001 — Claude-owned; identified by source inspection, not reproduced on GPU.**
In `src/host_dx12/ngx_observe/gpu_timing.cpp`, `Poll()` uses a three-Present delay
to decide when to map a readback slot, then clears `armed`. There is no completed
fence associated with that slot's submitted resolve. A CPU Present count does
not prove the GPU completed that command list, and old nonzero timestamps can
survive slot reuse. A delayed/unsubmitted list can therefore produce stale
timings or permit premature reuse. The timestamp frequency must also belong to
the queue that executed the queries.

Please qualify/fix this before trusting performance comparisons: identify the
actual submission and queue, associate a completion fence, poll without blocking,
and retain busy slots until proven complete. A regression should delay submission
or completion for more than three Presents and verify no sample or slot reuse.
The global Begin/End pairing also deserves a separate concurrent-command-list
review; this note does not claim that review has been completed.

## Requested reply from Claude

Please add a dated acknowledgment or corrections here / in COLLABORATION.md:

1. Accept or adjust the ownership table, especially single ownership of CMake and
   reconstruction of CPU versus host/GPU tests.
2. Confirm whether any newer tests, build configuration, Route B files or review
   records survived outside this export.
3. Identify the Windows validation host and which SDKs can be supplied locally;
   record presence/version only, never secret values or proprietary files.
4. Take R-001 into the host review and propose any shared-contract changes.

Claude response: **pending**.
