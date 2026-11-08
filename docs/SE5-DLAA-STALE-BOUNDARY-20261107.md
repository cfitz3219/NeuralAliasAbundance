# Sniper DLAA's repeating three-on / two-off failure

## User report and saved evidence

Cort reports unbearable DLAA flicker, with NVIDIA's registry-enabled diagnostic
overlay disappearing and returning. Switching camera jitter, TOX2 2, optical flow
and XeGTAO off/on does not eliminate it. The game was closed and Results saved
before this investigation. This is separate from the right/bottom viewport fringe
repair f1868f6 / PR123 handed to Opus for installation.

Evidence is from runtime **693d960**, process **8092**, not the viewport-edge candidate:

- `C:\Users\Cort\Documents\NA-se5-results-20261107-160640\status-8092.log`
- Same Results folder: `presents-luma-presents-20261107-160217-sniper5_dx12-p8092.txt`
- `C:\Users\Cort\AppData\Local\NeuralAlias\captures\presents-20261107-160217-sniper5_dx12-p8092`
- Evaluation captures `j2-20261107-160100-sniper5_dx12-p8092`, `160109`, `160139`, `160236` under that captures folder.
- Analysis receipt/contact sheet: `C:\Users\Cort\NeuralAliasAbundance\local\se5-flicker-20261107\`.

The consecutive-present capture is decisive: frames 0–2 receive DLAA, 3–4 do
not, 5–7 receive it, 8–9 do not, and this repeats through all 40 saved frames.
All 16 missing frames have zero new evaluations. The NVIDIA text is visibly
absent on the missing frames. Home remains submitted and visible throughout.
Mean backbuffer brightness jumps approximately 9% at each dropout. The status
`active` bit lags the first missed frame: this is why counting that bit alone
reports eight rather than sixteen missing frames. Use actual per-present
evaluation deltas and final pixels.

The last five status intervals show 143, 141, 139, 135 and 135 missed scene frames.
In the first of these intervals, 215 evaluations produce 215 later-write
contradictions, with 216 cached-boundary rescue attempts and zero camera, state
or NGX failures. The following intervals retain the same pattern (with a few
additional camera acquisition failures). At the end of the run: 13,684 late-write
frames and 9,115 route changes. The newest late-write records identify the
boundary fallback, followed by four scene-depth draws in the same recording.
This is not evidence of an NVIDIA backend failure or of a settings toggle.

## Actual failure chain

1. `FindBoundary` can recover an entry marked intermediate by using one of eight
   previously delivered scene-ending shapes. This was added to handle legitimate
   alternating scene endings.
2. A formerly terminal shape becomes intermediate when additional scene drawing
   follows it. The actual entry profile records that fact, but the remembered
   boundary remains eligible indefinitely. Successful history never had a
   corresponding withdrawal path.
3. That memory keeps placing DLAA before unfinished scene work. The route
   detector sees three contradictions, withdraws the route, and spends two
   frames relearning the same inline arrangement.
4. The obsolete remembered shape immediately wins again. The cycle repeats,
   resets temporal history, and visibly alternates processed/unprocessed images.

The fix withdraws **every cached alias matching the actual submitted insertion
prefix** after subsequent observed scene writes disprove it. The snapshot travels
with the pending recording and is published only when that recording submits.
The just-completed frame still teaches its true final boundary; other successful
alternate endings remain available. Native Quality deliveries clear this
scene-boundary snapshot rather than reuse a previous DLAA record's data.

No route error is hidden, contradiction threshold weakened, shader identifier
whitelisted, or new scanning system introduced. The added work is a bounded
scan of at most eight items after a contradicted delivery, under the existing
lock. A `retired` counter on `j2_entry` makes withdrawal observable.

## Reproduction and verification

The existing actual-DX12 post-entry fixture now has
`host_j2_post_entry_boundary_retire_test`. It first delivers and remembers two
different scene endings. Both are then used before a new final scene draw.
The test reads the real downstream texture, so an early fake-NGX call cannot
pass by incrementing an evaluation counter. Automatic routing remains enabled.

- Previous runtime: **38 failed assertions**, 18 late-write frames, **0/25**
  settled downstream images enhanced; automatic route repeatedly relearns.
- Repaired runtime: **25/25** settled downstream images enhanced; only the first
  disproval of each formerly valid ending (two in total), one initial route
  activation, no ongoing drop/relearn loop. No camera/state failures.
- The first test draft alternated the boundary during cold automatic discovery,
  so no remembered endings were learned. That was not a reproduction. The
  corrected fixture learns each initial ending before changing the layout;
  its failing pixel result above is the applicable negative control.
- Full Release build passed after retrying one MSVC compiler process access
  violation at lower build parallelism; no source change was needed for retry.

Further affected-suite and package results are appended below after completion.
The fixture uses a fake model to test scheduling and actual image delivery.
It is not a claim that the repaired runtime has already passed Cort's gameplay.
No game launch, installation, or replacement of public CMDs occurred in this
investigation. Opus retains installation coordination.

## Completed automated receipt and handoff

Runtime commit **abf70fdc9432d30dd46907e1e9c8844deb407fdd**, PR124 targets
`claude/se5-tox2-gates` at **304f1c9**, which already contains the separate edge
fix. Read-only merge-tree check is clean. Opus can merge PR124 or cherry-pick
abf70fd while retaining his TOX2 gate counters; do not replace that newer lane
with this branch's baseline.

The rebuilt affected suite completed **86 cases: 83 passed, 3 unsupported
16x-MSAA skips, 0 failed**, 229.94 seconds. This includes automatic/forced
placement, alternating/retired boundaries, late-bound native Quality producers,
HUD, viewport jitter, draw replay, XeGTAO and five tests using real NVIDIA DLSS.
Log: `C:\Users\Cort\NeuralAliasAbundance\local\se5-stale-boundary-regressions.log`.
GitHub CI was still running at the handoff; no CI-success claim is made here.

Prepared standalone repair candidate `C:\Users\Cort\Documents\NA-se5-motion-abf70fd`:

- DLL SHA256: `470E9418603DA075A032226D453EF43C5B68BA2EFE22CC9F4F25CB665B6FABBC`.
- All 20 payload hashes verified; all five isolated additive installation,
  preferences preservation, corrupt package rejection, rollback and foreign-DLL
  preservation checks passed.
- Dedicated `NA-se5-dlaa-flicker-test.cmd` exists for this precise candidate;
  public update/results/rollback launchers were not repointed.
- Opus should build his combined lane after integrating the source fix, retaining
  288d54b's TOX2 counter additions. The standalone candidate does not contain
  those independent newer counter changes.

No game install or game testing was performed for abf70fd. The actual saved-run
diagnosis and the automated repair verification are distinct from post-repair
gameplay acceptance.
