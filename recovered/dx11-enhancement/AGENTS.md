# Working instructions

This is a reusable DX11 enhancement project with Steam Alien: Isolation as its
first adapter. Read `HANDOFF.md`, then `WORKLOG.md`, before continuing.

## Explanations and record keeping

The user is new to programming. Define new terms and explain the purpose and
result of each step in plain language. Do not replace the graphics roadmap with
a documentation exercise: logging accompanies the existing work.

Maintain `WORKLOG.md` as an append-only action journal. For every assistant action
and every user action/report visible in the conversation, record:

- Date/time when known, actor, request or action, and why it was taken.
- Files inspected or changed, commands/tool operations, dependency versions and
  external sources when applicable. Include failed attempts and corrections.
- Observed result and evidence path, plus what remains unknown.
- Any deployment, backup, rollback, game launch, configuration change and test.
- The next step that follows from the result.

Record decisions and brief technical reasons, not private internal reasoning.
Do not claim to observe user actions that were merely requested. Quote or
paraphrase user reports as reports; separate them from automated measurements.
For earlier reconstructed history, preserve the reconstruction label. Do not
invent timestamps or missing events. Redact secrets and unnecessary personal
data from command outputs before storing reviewable evidence.

Group tightly related read-only inspections in a single entry only if each
operation and result is still identifiable. Keep substantive edits, tests,
deployments and failures individually identifiable. Append corrections instead
of erasing historical results. Update `HANDOFF.md` with the current state so a
reviewer can catch up quickly, including incomplete work and test limitations.

Preserve relevant runtime-log excerpts in `docs/evidence/` before launching the
game again, since ReShade.log is overwritten. Full machine-specific logs,
deployment records, builds and backups belong in ignored `local/` or `build/`.
Do not imply that a journal excerpt contains the full original conversation.

## Existing constraints

- Keep shared machinery in `src/core` and game-specific knowledge in
  `src/adapters`. DLSS in this 32-bit game remains an unproven later integration.
- Do not overwrite unrelated game mods or presets. Existing deployment scripts
  verify the prior diagnostic hash and back it up before updating it.
- Describe visual validation, benchmark results and automated tests accurately.
  User-observed lack of lag is not a measured latency benchmark.
- Preserve upstream licenses and credits. Never invent authorship or verification.
