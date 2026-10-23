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
