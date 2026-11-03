# Anno camera-movement stutter: results and reproduced corrections

Astra, 2 November 2026 Pacific. Cort asked for analysis of Opus's current work,
latest build/results, and practical stutter repairs. No game was launched,
controlled or deployed. Opus's worktree and its build artifacts are untouched.
Write-watch remains his lane; no new presentation or route-selection work starts.

## What actually ran

Documents/NA-anno-results-20261102-183829 identifies **fab5928**, DLL
D28C914DEC18E53AF6A7BFC5FC00E3CDA1862EC9071C01A83F8411BD0CB77B01.
The existing update CMD still selected that package at inspection. Newer
**8a0ee44** was built in Opus's worktree but was not the tested package.

The latest status13380 run ends with 3,680 windowed frames and 598 repeated old
pictures (16.25%); 219 repeats are already more than four Presents beyond the
last fresh evaluation. There are zero recorded DLSS evaluation, busy-slot or
depth-state failures. Camera work reports about1.277ms CPU per frame. The prior
37228 run has1,394repeats/8,165windowed frames (17.07%),536stale repeats. These
are different runs, not controlled performance comparisons.

The old-picture repeat prevents an incorrect clipped/zoomed frame reaching the
screen, but cannot make camera movement smooth. Present FPS counts those repeats.
Removing repeats without correcting their cause would restore the earlier picture
pops; this repair retains coherent fallback and the no-drop-to-DLAA behavior.

## What the latest proposed scan changes do

8a0ee44 raises the other-block ceiling to4,096 and probes64bytes at one learned
offset before reading an unknown block. Broader collection addresses the old
budget's millions of dropped bindings. However, one probe cannot exclude a known
camera component at another position or a standalone projection/inverse. Also,
camera knowledge is resource-wide: a large upload resource can contain thousands
of unrelated objects, so being a known camera resource does not mean every block
is a camera or that the probe saves work there.

## Three defects reproduced and corrected

1. **Short aliases hide the real camera.** Both patching and camera acquisition
   deduplicate by resource/address and keep the first view size. Binding256bytes
   then768at the same address makes the camera at336 disappear from the scan.
   The DX12 regression fails old code with zero evaluations. Both collections
   now retain the largest observed bound range, still capped by the existing
   1KiB read limit. No speculative read beyond an observed view is introduced.

2. **Separate camera components are skipped.** After the cheap probe, the inner
   patcher also requires the main VP to appear in a block before patching anything
   else. A separate exact projection copy at+16 is never patched. The DX12
   regression, with the alias case disabled, independently reproduces that
   failure. The corrected path reads each bounded block once and compares known
   camera/component bytes across the supported16-byte-aligned positions. Exact
   projection, inverse and previous-VP copies can be patched using the existing
   write ledger without requiring a current VP beside them. It does not run a
   general matrix search over unrelated object data. Both regressions pass.

3. **Recognized patched VP loses its sample provenance.** A camera block can
   contain the continuous current VP without its neighbouring inverse. The
   existing accepted fallback computes the inverse, but unconditionally sets
   g_injected=false. In window mode that falsely labels the current picture
   unpacked and copies an older output over it. Carry holds_jittered's exact
   known-sample result through that same fallback. Camera acceptance/continuity
   checks do not change. A new production-path DX12 test warms up the window,
   moves the camera, then supplies the VP without the inverse: before this fix,
   10of15windowed frames repeat; afterward,15fresh frames,0unpacked,0repeated.

The tests use actual DX12 resources, descriptors, command submission, GPU work
and the production camera/patcher path, with fake NGX. They prove these machinery
defects and corrections; they do not measure neural quality or the percentage
of the user's real-game stutter attributable to each one.

## Relationship to write-watch

Write-watch is a useful complement: it can reduce which upload pages need to be
examined and prevent budget-related omissions. It does not fix a longer view
being discarded, a found component being skipped, or a patched current camera
being falsely treated as unpatched. Opus should use this corrected patcher in
that work. GPU/write/reset ownership qualifications remain in the earlier
research report and COMMS; no second tracking framework is added here.

The bounded-read change's actual game CPU cost has not been measured. Do not
claim it meets the0.5ms target or that Quality's true fresh-frame FPS gain is
qualified from the earlier display counters. Keep those measurements with
Opus's write-watch integration and Cort's matched in-game comparison.

## Verification and handoff

Local receipts are under local/anno-stutter-20261102: alias-before/after,
projection-copy-before/after, window-before/after, configuration/build and full
test logs. Full rebuilt-suite results and final source pin are recorded below
when complete. The new window-pan case is registered in CTest. Existing core,
backend, acquired-camera validators, ledger, lifetimes and repeat fallback are
retained. No fixture result is called a successful game run.

## Final receipt and newer user run

During this work Cort installed **8a0ee44** and reported it worse. Actual
status41592 ends at1,203windowed frames,187repeats(15.54%),115stale repeats,
zero dropped/capped camera blocks and about1.364ms camera work/frame. Thus the
broader scan did remove budget omissions but did not eliminate stutter. This
does not establish how many of those187frames each correction above resolves;
the accepted VP-only fallback was used only once in that log. Keep reproduction
evidence separate from attribution of every real-game repeated frame.

Runtime **2ffc23d54321aa7d7519c1b3160577ef86c7b4a9**, PR83 against
claude/p44-projection-window: full Release build and182registered tests,
178passed/4skipped/0failed in212.42s. Reproduced old failures and corrected
window15fresh/0repeats are retained in local receipts. No game CPU target or
image-quality pass is inferred from those tests.

Prepared Documents/NA-Anno-layer2-package-2ffc23d, DLL SHA256
FDFC79380BECE2DDA3E7214952ADCD656E68B28A2F31F1BC40D306DFEEBD3B17.
Existing Anno layer2 update/results/rollback CMDs now select it. Previous CMDs
are preserved in Documents/NA-anno-layer2-launchers-before-2ffc23d-20261102-185542.
Isolated WindowsPowerShell old->candidate->old->original install/rollback tests
pass both with and without a pre-existing native DLSS library. Original files
and final inventory are verified; payload hashes verified before publication.
Actual Anno remains8a0ee44/7D7C0B39..., unchanged by Astra. Cort runs the game.

The candidate includes the narrow patcher corrections, not write-watch. Opus
has the source and ownership split in COMMS so it can be integrated without
duplicating his memory-tracking work. No additional review gate is requested.
