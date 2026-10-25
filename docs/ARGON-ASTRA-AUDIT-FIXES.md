# Astra audit fixes after the ownership change

2026-10-25. Cort assigned extraction algorithms to Opus and restricted Astra to
the remaining errors in Astra's code. This records that limited scope; it does
not close Gate 3 or claim successful Anno discovery.

| Finding | Disposition |
| --- | --- |
| G3-01: ordinary observations invalidate the whole capture | Fixed in 00591f6 and ed19e7e. Known creation metadata and provisional snapshots do not imply lost inventory/events. Actual loss, malformed packets, ambiguity and relevant truncation remain explicit. Snapshot regression expectation corrected in 5f3eaab. |
| G3-08: missing 16-bit scalar decoding | Fixed in 00591f6. Explicit formats 54, 55 and 56 decode with padded-row bounds. Typeless format 53 remains uninterpreted without an explicit view format. |
| G3-09: permanent sample-attempt blacklist and stale retirement state | ed19e7e replaced the lifetime cap with retries/backoff and retirement cleanup. This follow-up cancels all outstanding leases for a retired resource and stops retrying explicitly unsupported samples. Timeout/rearm and multi-range retirement regressions pass. |
| G3-04 and G3-05: render/camera extraction | Owned by Opus under Cort's revised instruction. Existing earlier implementation is available for him to adapt or replace. No extraction algorithm was changed in this follow-up. |

## Integration notes for Opus

Preserve the validity and scalar-decoding corrections when replacing discovery
code. In the adapter's retirement handler, call `acquisition.Retire(e.object)`
and remove its retry entries. This releases consumer leases without waiting;
the host retains responsibility for fence-owned GPU cleanup. A late completion
can no longer recreate retry state for that retired resource.

Timeouts remain retryable. `NA_SAMPLE_E_UNSUPPORTED` sets the current identity's
retry deadline to `UINT64_MAX`; retirement/session reset removes that state.
The adapter changes in this follow-up are exactly those two lifecycle lines.
Do not restore the old 32-resource lifetime attempt cap from an earlier example.

## Verification

- Full Windows Release build passed.
- CPU/fixture suite: 28/28 passed.
- Targeted validity, decoder, acquisition, real-host snapshot bindings and RTX
  diagnostic-adapter checks: 5/5 passed.
- The new acquisition cases verify timeout followed by successful retry, all
  pending ranges cancelled on retirement, no read after cancellation, and no
  double release or cancellation of another resource.

Local logs: `local/gate3-astra-audit-fixes-build.log` and
`local/gate3-astra-audit-fixes-cpu.log`. No game deployment or launch. These are
local test results; peer acceptance and the remaining Opus work are separate.
