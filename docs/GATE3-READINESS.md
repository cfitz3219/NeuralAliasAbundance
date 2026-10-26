# Gate 3 readiness: Phase 3 (signal discovery)

Process (MASTER-PLAN): cross-review → fixes/regressions → **Argon audit** → resolve/recheck → **test session 3** (classifier output in Anno 1800 + one more DLSS-less game) → close gate.

## Status at PR27 e631083 + PR37

| Step | State |
|---|---|
| Astra → Claude review (C8/C9/C9b) | **Closed.** C9B-R1..R6, REVIEW-2 #1–4, REVIEW-3 #1–2, REVIEW-4 all fixed (PRs 31–36). Producer qualifications (R3 interleaving, signal-failure quarantine, plane count) now have deterministic tests (PR37). |
| Claude → Astra review (ASTRA-P3-REVIEW-1) | R2–R6 **fixed and verified** (e631083). **R1 open** (production-obtainable evidence). Minor follow-up: one implausible MV (e.g. fp16-max sky sentinel) still contradicts the whole buffer; consider counting it as an outlier. |
| CI | All 3 jobs green on e631083. PR37 pending. |
| RTX | 50/50 on c15881d; Astra's expanded run 53 pass / 1 runtime-missing skip / 0 fail. PR37 run pending. |
| A5 acceptance (≥95% on fixture v2) | **Outstanding** (Astra). |
| A9 Anno diagnostics report | **Outstanding** (Astra), depends on R1. |
| Argon audit | Not started. |
| Test session 3 | Blocked on R1 + A9. |

## What blocks closing Gate 3

1. **R1 (Astra):** corroboration paths a real game can satisfy: temporal depth/camera reprojection, H2 HUD-boundary evidence, depth-derived visibility for motion. Without these, every Anno candidate stays Unavailable.
2. **A5 acceptance + A9 (Astra):** measured fixture accuracy, then a live diagnostics report.
3. **Live wiring:** a consumer that runs on the host's capture worker (decoder → validity → classifier → sample requests) behind an explicit INI switch, off by default. Needed for test session 3. It stays read-only: discovery has no effect on rendering in Phase 3.

## Recommended order

1. Merge PR37 into PR27, then PR27 into main as the Phase 3 plumbing baseline. Argon can audit the plumbing immediately (brief below) in parallel with R1/A5/A9.
2. Astra: R1 → A5 measurement → live consumer → A9 report.
3. Claude: review R1/A9 PRs as they land; host side of the live consumer (worker thread, INI switch, sample-request scheduling within the 8-lease cap).
4. Argon recheck of R1/A9 deltas → test session 3 → close.

## Argon audit brief (plumbing, available now)

**Scope:**
- **Host producers:** `src/host_dx12/observe/producers.{hpp,cpp}`, `host_observe_samples.h`, ABI 0.2 in `host_observe.h`.
- **Consumers:** `src/providers/discovery/*`, `classifier/*`, `camera/*`, `profiles/layout_cache.hpp`.

**Focus:**
1. Hook safety: vtable/MinHook install and rollback; re-entrancy (`t_host_work`); behaviour on device loss.
2. Object identity and retirement: canonical keys, retire sentinel, generation cache.
3. C9b sample lifetime:
   - pre-submit source acquisition;
   - per-queue submission lock while samples are armed;
   - fence/quarantine;
   - immortal worker and teardown wait;
   - scope at every operation.
4. Wire decoding of untrusted batches: bounds, overflow, epoch/session.
5. Conservative semantics: no consumer may return a "trusted" input from metadata alone.

**Evidence:**
- CI runs on PR27;
- RTX logs posted on PRs 33–37;
- `docs/C9B-REVIEW.md`;
- `src/providers/discovery/CAPTURE-WORKLOG.md`;
- COMMS 2026-10-24.

**Known, declared limits:** R1/A5/A9 above; captures are app-sync-unknown (no GPU-write proof for CPU-visible buffers); there is no live enablement.
