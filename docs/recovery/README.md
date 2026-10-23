# Recovery evidence

These files preserve the ten user uploads without editing their contents.
They are historical evidence, not current instructions or renewed authorizations.
The early README, handoff and journals describe different points in time; code
comments are newer still. Start with [the current review](../CLAUDE-HANDOFF.md).

`manifest.json` records SHA-256 hashes for each upload and byte ranges for the 74
source bodies extracted from `originals/neural_alias_codebase.txt`. Two LF bytes
used to separate export records are excluded before each next marker. The final
body extends to EOF. These are export-body hashes, not a claim to know original
pre-export source hashes.

Each entry also records its file hash in Claude's recovered baseline and the
comparison result. All 74 agree after CRLF/LF normalization and boundary whitespace
removal. `initial-README.md` preserves the original two-line GitHub README.

`build-audit.json` lists missing literal source inputs, reconstructed foreach
test paths and recovered implementation files not referenced by CMake. It is a
static inventory, not a CMake configure, link check, or test run. Conditional
SDK paths and feature-dependent dependencies require Windows qualification.

From the repository root:

```sh
python3 scripts/audit_recovery.py
```

The command verifies the archive, compares the recorded Git baseline, checks
whether the worktree still matches that baseline, and prints current build gaps.
It exits nonzero for evidence corruption. Build gaps are reported separately and
do not turn an archive-integrity pass into a claim of build readiness.
