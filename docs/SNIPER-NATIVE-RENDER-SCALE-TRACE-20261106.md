# Sniper native render-scale consumer trace — 2026-11-06

## Result and scope

The actual native render-size application has been located in the installed
Sniper Elite 5 and Sniper Elite Resistance executables. This is executable
inspection, not a live debugger receipt or a working live-control claim.

The old marker experiment found numbers and tried changing them. This work
instead connects the named display setting to its parser, separate applied
cache, committed renderer scale, both scene dimensions and native resource
update. The same locator recognizes a called-helper implementation in SE5 and
an inlined implementation in Resistance, without fixed RVAs or a hash gate.

No game was launched, attached to, loaded as code, modified or deployed. No
runtime/installer changes are included. The existing 8fa9ad1 scene-source
candidate and its separate live-acceptance status remain unchanged.

## Why the earlier marker was insufficient

Both inspected parsers accept render scale in the range 0.5–2.0 and normalize
accepted values to 5% steps:

    normalized = floor(input * 20 + 0.5) * 0.05

Consequently, 1.000001 becomes 1.0 in the setting actually registered with the
menu; 0.666667 becomes approximately 0.65. Searching only for the original tiny
marker can therefore miss the actual controlling field and find other copies.
This establishes what happens in the consumer, not the identities of the old
scan's two candidate addresses. It does not prove the parser ran in every old
pilot or exclude another earlier failure in those pilots.

Correction to the first COMMS shorthand: the parser checks the permitted range
and skips assignment for out-of-range values; it does not clip every value into
that range. In particular, writing 0.333333 to the file is not an established
Ultra Performance control. The settings-update path also normalizes the pending
value. An exact DLSS ratio must not be claimed merely because a float was stored.

## Actual SE5 chain

1. The UTF-16 `Frontend.Options.Display.RenderScale` schema registration binds
   the real pending setting. The ASCII `RenderScale` parser independently
   writes that same field after range checking and quantization.
2. The normal settings-update path checks pending/dirty state before proceeding.
   A direct write to an arbitrary setting copy does not demonstrate that the
   engine accepted a change or ran the required update.
3. Native settings apply compares the pending scale with a separate applied
   cache. On change, it stores that cache and calls the native scale routine,
   then mirrors the applied result back into the pending field.
4. The native scale routine selects the effective scale. Native upscaler-mode
   values 1–4 override the scalar with approximately 0.5, 0.5882353, 0.6666667,
   or 0.7692308. Other values use the applied render-scale cache. These are
   inspected numeric branches; the full enum's UI meanings are not qualified.
5. The routine calculates integer scene width and height from the separate full
   output width/height and effective scale. It compares the dimensions, enters
   the engine's update synchronization and commits the renderer scale/sizes.
   There is also an inhibit flag whose meaning is not established.
6. It invokes native resource update. That path resets surface collections,
   updates camera/aspect state and walks native refresh callback lists. The
   surface-slot helper calls the native release function and clears each slot.
   Some downstream refresh functions jump into protected code: actual D3D12
   texture creation and the resulting submitted resources have not been
   observed live in this task.

The settings tick is called by an engine-registered callback (event hash
0x0A762D75). Its registration is established; the event's semantic name,
frequency and executing thread are not established by static inspection. Do
not call it a proven per-frame/main-thread entry or invoke it from Present.
Likewise, the synchronization code's shape is established, but its indirect
function table has not been resolved to named Windows APIs.

This chain operates on renderer scene dimensions separately from physical
output dimensions. It avoids the specific failed assumption that synthetic
WM_SIZE/client-rect responses cause a borderless game to recreate its scene.
Actual operation in every window mode remains a live qualification item.

## Resistance comparison

Resistance exposes the same two setting names and 5% normalization, but native
settings apply contains the dimension calculation inline. Treating SE5's helper
address or its existence as universal would fail here. The locator follows the
same setting/cache/dimension relationships instead. Resistance's update path
also resets surface collections and invokes native refresh callback lists.
SE5's upscaler-mode override is not assumed to exist identically in Resistance.

All following addresses are hexadecimal **RVAs for audit/evidence only**. They
are output from the locator, not constants embedded in runtime logic.

| Relationship | SE5 | Resistance |
|---|---:|---:|
| Named UI schema string | D506A0 | 112CA30 |
| Parser unwind fragment | 3F34B2 | 53E3F1 |
| Pending menu setting | EA46D8 | 12A5CA4 |
| Native settings-apply entry | 3F1520 | 53B8A0 |
| Applied setting cache | EA4B88 | 12A6134 |
| Scale-apply implementation | Helper 5BE9D0 | Inline after cache store 53BE31 |
| Call to SE5 helper | 3F19E0 | Not applicable |
| Committed renderer scale | E7D758 | 127D498 |
| Full output width / height | E7D74C / E7D754 | 127D48C / 127D494 |
| Actual scene width / height | E7D750 / E7D75C | 127D490 / 127D488 |
| Native resource update | 32EC60 | 4745F0 |

SE5 further receipts: settings tick 3F0CB0, its call to settings apply 3F0F92,
registered callback 19AE70, callback registration site 199FE4, native-scale
resource-update call 5BEAD9. Inspect the exact current image again after a patch.

## Reused machinery and finite integration seam

`tools/analysis/trace_rebellion_scale.py` uses the same pefile/Capstone inspection
approach already used for Ghost. It derives pointers from setting names and
instruction relationships and resolves chained unwind fragments to the true
settings-apply entry. No process-wide numeric scan, debug-attach implementation,
renderer, injection backend or runtime hash whitelist is added.

The small subsequent controller should reuse the existing production-line
components, rather than replace them:

- Existing `scale/live_driver` request/status flow and J2's observed scene
  extents. Those files are present on production source 8fa9ad1, not assumed to
  have already merged into main. Its width-only 3% judge is preliminary evidence,
  not proof of an exact requested ratio or a successful DLSS submission.
- Existing Ghost engine-control design: queue a request, consume it at the
  engine-owned update, allow the native renderer to own synchronization and
  resource rebuilding. Reuse its dispatch pattern, not Ghost's ABI/layout.
- Existing renderer inputs/output, resource retention, jitter, motion and DLSS
  evaluation. A smaller scene does not itself establish the correct replacement
  of the game's native upscaling/composition or HUD preservation.

Before enabling such control, the finite missing evidence is the normal calling
thread/stack and the actual applied-cache/scene-dimension transition during a
native menu scale change. Then connect one queued mode change to that native
update and verify both submitted scene dimensions and native output. Preserve
the user's display mode. Native upscaling override/stacking, 5% normalization,
the inhibit branch and restoration are concrete issues to handle at that seam.
Passing a float argument to SE5's no-argument/global-reading helper is not a
controller. Neither is changing the pending float without its native update.

The reusable result is the consumer-following method, existing request/apply
machinery and observed-resource confirmation. These two recognized Rebellion
implementations do not establish automatic control over every engine. Future
engines can reuse the same machinery with their own actual setting/apply link.

## Automated evidence and optional live tracing setup

Dependencies: pefile 2024.8.26 and Capstone 5.0.9, reused from the existing local
analysis packages. The CLI opens the EXE only as data and closes its file mapping
even when recognition fails. Typical usage with a configured Python environment:

    python tools/analysis/trace_rebellion_scale.py "path/to/game.exe" --json chain.json --watchpoints scale-watchpoints.txt
    python -m unittest discover -s tools/analysis -p test_trace_rebellion_scale.py -v

The optional command file only prepares four standard one-shot processor
watchpoints: pending-setting access, applied-cache write and both scene-size
writes. It uses module-relative addresses for ASLR and records thread, stack and
values. It neither attaches nor runs a game nor calls native functions/changes
settings. It is for an otherwise empty hardware-breakpoint debugger session
against the same inspected executable. Load it after enabling debugger logging,
resume, and change the game's native render scale. A pending read alone is not
success; cache and dimension write stacks establish the rest. Debugger overhead
must not be interpreted as game performance. Commands are generated and checked
for the correct derived addresses, but have **not** been executed live here.

Implementation references: Microsoft's [x64 unwind/chained unwind
documentation](https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64)
and [processor access-breakpoint
commands](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/ba--break-on-access-).

Final local validation: two unittest tests pass (eight called/inline, shifted
code/data and preferred-image-base combinations; two missing-height-consumer
refusals). Numeric decoys are included. Initial repeated-file tests exposed an
unclosed pefile mapping; fixed with `finally: close()`, then all cases passed.
Final locator runs succeed on both real installed executables and produce the
independently inspected chains above. No production CTest/build or game FPS,
quality, window-mode or live-control result is claimed for this analysis-only
increment.

Exact inspected image SHA256, recorded for provenance rather than recognition:

- SE5: `0ed010cd15399bbd7086b71c57ce17f129a75da9600872cad8777e5c4742b162`
- Resistance: `eb9fde864d99b32008ac07760e5c3e9461b89a693379c5d0419a99b7bbc9f9ac`

Ignored local evidence: `local/se5-render-scale-trace-20261106`. This contains
the original disassembly/relationship receipts, both final JSON results and
watchpoint files, and `locator-tests.log`. Proprietary bytes/disassembly and
local interpreter/packages are not committed. Opus owns independent recovery
of the final older mapped-marker pilot evidence; no response has been received
at this documentation milestone. COMMS records the split and discoveries.
