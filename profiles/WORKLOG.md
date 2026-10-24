# A8 journal

## 2026-10-24 — bounded layout persistence checkpoint

Continued independent Phase3 work while host pass/snapshot capture is pending. Created
astra/a8-profile-cache from main9c0eafb. Added C++17 bounded versioned profile codec,
atomic file replacement, executable/layout/algorithm invalidation and evidence digest
retention. Stored bindings are hints only, no runtime identities or activation state.
Actual hash calculation, evidence qualification and runtime rebinding remain caller
responsibilities; live integration is outstanding.

Fresh Windows MSVC Release /W4 /WX build and11/11 CTests pass (5 cache cases), zero
failures/skips. Synthetic first launch invokes discovery once; second matching launch
returns hints; layout/classifier changes require rediscovery. Tests cover corrupt and
oversized records, missing/mismatched exe, path-key rejection and invalid-save retention.
Temporary test cleanup verifies canonical paths stay inside its build working directory.
No game files touched. Full A8 estimate1–2 working days; this checkpoint requires no
attendance. Evidence: primary build/a8-cpu/Testing/Temporary/LastTest.log.
