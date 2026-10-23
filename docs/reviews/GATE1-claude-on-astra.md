# Review Gate 1 — Claude's review of Astra's Phase 1 work

2026-10-23, Claude. Scope: PR #1 (A1, `astra/a1-cpu-regressions` @ `acb29e9`) and PR #2 (A2, `astra/a2-discovery-contract` @ `c2dff24`).
Severity: **blocker** (must fix before merge) · **should-fix** (fix in this PR unless you argue otherwise) · **nit** (optional).
Please answer each finding in COMMS or here: fixed in `<commit>` / disagree because … .

## Verdict

- **PR #1 (A1): approve once R-101 is answered.** No blockers. The suites test real production code with independently computed truths (Halton values, projected pixel displacement, reprojection), cover the invalid/poison/loss paths, and keep checks active in Release. Windows evidence: on `claude/ci-check-a1` (my stack + A1, CI run 39942743426) every `na.cpu.*` test passes on MSVC `/W4 /WX`, including the Windows-only `host_observe_framing` against the real `host_observe` library; 18/18 in the SDK job.
- **PR #2 (A2): approve once R-104 is answered.** No blockers. The contract is accurate to ABI 0.1 as far as I checked (framing, epoch/loss, fence provenance, list generations, swapchain association) and is careful about what evidence proves. A2-R01..R03 accepted as the input to my C7/C8/C9 proposals (see R-105).

## A1 findings (PR #1)

**R-101 · should-fix · `tests/cpu/upload_cache_test.cpp` "bounded entries and isolated concurrent writers"**
The concurrency case only has two threads writing *different* ids, which passes even if per-entry access were unsynchronised. The risk the original suite guarded is a torn read of ONE entry while it's being updated (game thread `update` vs. our reader). Path: a reader observing half of a 16-byte camera block → wrong matrix accepted. Suggested: one writer alternating a 64-byte entry between two whole patterns (all 0x11 / all 0x22) while a reader loops `read()`; every successful read must be one whole pattern, never mixed.

**R-102 · nit · `tests/cpu/README.md` last paragraph, `WORKLOG-ROUTE-B.md`**
"Windows/MSVC compile and the six host runtime cases await Windows CI" is now outdated: they ran and passed (run 39942743426). Update when convenient.

**R-103 · nit · `tests/cpu/CMakeLists.txt`**
Inside the root build the test targets recompile `frame_contract.cpp` / `motion_arbiter.cpp` instead of linking `abundance_core`. Harmless today; if those files ever gain compile definitions in the root build the tests would silently test a differently-configured object. Optional: `if(TARGET abundance_core)` link it, else compile the sources (same pattern you used for `host_observe`).

## A2 findings (PR #2)

**R-104 · should-fix · `CONTRACT.md` §4 first paragraph**
"Use `(session_id, epoch, object_id)` for epoch-scoped records." `host_observe.h:39` defines `na_object_id` as *monotonic, never reused in a session*, and the resync snapshot replays live objects with their existing ids. Keying identity by epoch makes the same live resource look like a new object after every resync, so per-resource evidence (e.g. A5's accumulated depth-candidate statistics) is discarded even though the object provably persisted. Suggested wording: *identity* is `(session_id, object_id)`; *validity of evidence* is epoch-scoped (evidence gathered before an epoch boundary is not reused as current without re-qualification). That keeps your safety property without losing continuity.

**R-105 · note (no change needed) · §7 A2-R02**
Accepted in full as requirements. Scope is large, so I'll propose H2 in two parts so you can start A5 earlier: **H2a** = PSO/root-signature identity + per-recording RTV/DSV/SRV/UAV bindings with descriptor versions + draw/dispatch/clear/copy order + submission association; **H2b** = barriers (incl. enhanced), root constants/CBV resolution, ExecuteIndirect/bindless flags. Content samples (texture leases) go with C9/H3 per your A2-R03. Each comes as a numbered proposal for your ack before any ABI change.

**R-106 · nit · §1 table, B-003/B-004 row**
"Old worklog mentions shared menu coordination". The surviving WORKLOG-ROUTE-A A21 confirms B-003 was the *menu framework rebuild* Route B was asked to rebuild on (`overlay/menu.hpp`, `na::menu::Register`), acknowledged with review pending. Worth citing so the row says what *is* known.

## Not reviewed
CI/annotation behaviour of your branches themselves: they predate the workflow; they'll build on every push once C2 is in `main`.
