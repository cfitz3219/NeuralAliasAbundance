# Descriptor contention: independent diagnosis and complementary repair

## Finding

Opus identified a real major problem: our global descriptor bookkeeping lock serializes work that Direct3D permits on independent recording threads. His copy-stripe implementation is useful and its current reader coverage is complete. It is not sufficient alone: descriptor creation still takes the same global lock exclusively, twice per view when replay snapshots are enabled. In a controlled mixed workload that remaining writer contention erases the copy improvement. This repair finishes that part without removing tracking or changing rendering.

The evidence supports lock contention as the leading measured CPU problem. It does not establish a precise share of whole-game frame time: the old ETW trace has instruction samples without matched call stacks, and the later hook log sums elapsed time across threads. We have not run either game for this work. Cort owns game testing; Opus owns combining the source and updating the established additive CMD.

## Actual game evidence checked independently

- `Documents/NA-se5-results-20261105-000734/status-28000.log`, build66840fe, profiling enabled: a representative final window reports566506copies/s,19.38us/copy including15.20us waiting. Writes92606/s,2.49us including2.32us measured wait. Nested timings must not be added as independent CPU costs.
- That same final report has33.1fps,~30.2ms/frame, no presenter backlog and0.27ms Present time. The reported0.144ms presentation copy is NOT a timing for all DLSS/flow/render work. AMD flow reports~1.55ms; it cannot account for the large CPU deficit on its own. The global overlay's `dlss_gpu_ms=0` is not a valid measurement of J2's separate DLSS backend.
- `Documents/na-cpu2.etl` is an EARLIER c1b3ed2 capture, process37736,~38.6seconds; not the66840fe run. It has no matched sampled call stacks. The parsed exports in `Documents/NA-cpu2-etw` were read, and recorded consumer/worker/kernel addresses were independently resolved from those exports, without launching or attaching to a game. The supplied reconstructed PDB differs in47text bytes, so symbol attribution retains that qualification.
- The three sampled worker threads in the exported W group spend~67% of their resolved instruction samples in exclusive-lock contention/backoff/kernel lock-owner wait. That corroborates serious contention, but an instruction in the lock implementation alone does not identify which application lock or reconstruct the frame's submission chain. The consumer also spends time in allocation, gather and Consume. That is another cost, not proof that the consumer limits frame rate.

The older summary's `dxgi loaded:False` is a module-name resolution limitation; the independently supplied PDB/image bases resolve our functions. Do not reinterpret that string as proof our DLL was absent. Likewise, do not turn the parser's tiny reported `wait_us` totals into reliable scheduler latency: its CSwitch field parsing and lack of matched stacks do not establish those waits.

## Code audit and repair

Base7184611 includes Opus's fc3562d copy stripes,327c37c sampler variants and bba65a6 boundary rescue. This branch adds63905ca (opt-in benchmark) and c2b1a82 (runtime plus concurrency regression).

All `info` and `versions` accesses were checked, not just the sites named in COMMS: view_ref_locked; root-table versions/SRV/CBV reads; before_shader_use; native colour/depth capture; graphics SRV enumeration; RestoreListState; NativeDescriptor; both operation-key reads; NativeOperation snapshot. Shared-registry readers hold the covering slot stripe, and copied shared_ptrs are retained before releasing it. Copy locks remain globally sorted, duplicate source/destination stripes merge with exclusive winning, and a reader holds at most one stripe. Large-copy fallback still takes the registry exclusively and excludes every striped writer/reader. Registry ownership retains heap metadata; no borrowed heap pointer survives it. Cache revision is captured before scanning, so a concurrent publication invalidates the next cache lookup.

The missing write half now uses registry SHARED plus the destination stripe EXCLUSIVE. Resource identity and heap lookup happen while registry lifetime is protected. Compact slot data, replay creation arguments, resource/counter identity and version are published together. The old second exclusive `remember_view` pass is removed. CBV address resolution shares the same registry read. Descriptor events/slot versions and unknown-resource semantics are preserved; fewer internal cache revision bumps do not remove any publication. Sampler/view functions still call the game's original D3D implementation. No per-game data, skipped descriptors, disabled effects, callback GPU waits or discovery removals.

The required concurrency test was added to the existing actual-DX12 scoped bindings regression: four writers update/copy independent descriptor slots2000times each while snapshots are read concurrently. Resource identity, returned resource, SRV format, monotonic versions and exact final versions are checked. On the original split-publication runtime this produced1655 bad/incomplete snapshots (39105successful snapshots), and the test failed. The repair passes with zero bad snapshots. That reproduces a correctness benefit as well as reduced serialization. Initial affected scoped-bindings/observer tests:2/2PASS4.58s. Full final receipt follows below.

## Controlled timing

Existing opt-in `observe_hotpath_bench` was extended, not added to ordinary CTest. Persistent four recording workers perform6000loop iterations per simulated frame. Copy-only has18000single-descriptor copies. Mixed adds6000SRV creations and6000indexed draws/table binds. This deliberately exercises both creation and copying; it is not an exact model of every game draw. Source descriptors are non-shader-visible per [Microsoft's copy contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/copying-descriptors); worker destinations are disjoint and stripe-aligned. No model evaluation, queue submission, GPU wait or GPU readback occurs in timed sections. Separate executables use the same benchmark code and before/after runtime. No concurrent benchmark processes or builds in the reported trials; no Sniper/Anno process was running when checked. Other desktop scheduling still contributes variation.

Before-runtime bba65a6 needed the unrelated7184611 local variable rename to compile with /WX. That build-only correction is disclosed; descriptor code is the actual pre-fc3562d source. The three executables are frozen in ignored local evidence. Third trial reverses variant order. Median of three observer_full phases, profiling OFF:

| Runtime | Copy-only wall ms | Copy-only process CPU ms | Mixed wall ms | Mixed process CPU ms |
|---|---:|---:|---:|---:|
| Before copy stripes |2.9363|11.0677|9.1244|35.5114|
| Opus copy stripes |2.0077|8.0381|10.2587|38.2612|
| Copy + write stripes |2.1795|7.9070|5.4896|21.4489|

Copy-only improves~32% with Opus's change. Adding the write repair reduces mixed wall time~46.5% relative to copy-only stripes (~39.8% relative to the old global-lock runtime). Mixed process CPU drops~43.9% relative to copy-only stripes. Trials vary; copy-only results after the unrelated write repair should not be sold as a further improvement. These are CPU recording measurements, NOT game FPS or GPU/image-quality results.

Separate true bare-D3D control for mixed workload:0.2485ms wall/0.8545ms process CPU. The HostWorkScope bypass phase is NOT that control: device descriptor hooks intentionally continue shadowing descriptors there. Substantial tracking cost remains even after this repair. A separate same-source profiling comparison was5.7028ms OFF vs5.7895ms ON for observer_full (single comparison, not a robust percentage); profiling is not enough to explain the measured mixed global-lock penalty, but production FPS comparisons should use profile_hooks=0.

Evidence: ignored `local/se5-cpu-critical-20261105/` has all trial logs, benchmark-medians.json, baseline reproduction, final build/tests and independently resolved recorded-thread-functions.txt. No ETL, PDB, private SDK or game DLL is committed. No installed file or installer pointer was changed.

## Integration and next measurement

Opus should pick63905ca (tool), then c2b1a82 (runtime/test), onto the newest HUD/CPU branch; the later receipt doc commit is optional. There is no overlap with HUD source, and the copy-stripe prerequisites are already on his branch. Keep the completed HUD picks, jitter repair, AF/mip work and saved settings. He builds/tests the combination and updates the existing reversible additive CMD; Cort performs the live test. No extra review cycle is requested.

The useful next live comparison is the same scene, quality mode and effects with profiling disabled, logging frames and copy/write timings only in a separate diagnostic run. If a limit remains, inspect remaining per-draw registry/list locks, global invalidation and event transport/consumer work against actual submission delay. Do not remove HUD, motion correctness or lifetime ownership merely to lower aggregate CPU. This increment fixes a reproduced missing serialization path; it does not claim all of the user's reported130FPS loss has been recovered.

## Final receipt: ready for Opus's combined package

Frozen runtime c2b1a82: complete Release build PASS; full200case CTest **195 passed, five skipped, zero failed**,301.31seconds. Skips are optional NR runtime, three unsupported MSAA16 cases and optional upscaler jitter. No runtime edits followed this full run. The new concurrent descriptor regression passes alongside all existing observer, scoped binding, actual jitter/AMD, HUD, NGX, presenter and fullscreen/resize regressions. No extra repetition or review cycle was added. Final logs `local/se5-cpu-critical-20261105/{final-build,final-tests}.log`.

HUD is separately engineering-ready and already acknowledged/picked by Opus as73435ad/36b82c2/f30d97e. This CPU suite is on the7184611-based CPU branch, not a claim that his latest combined package was tested by this run. He owns that combined build/test and CMD update. No game installation, launch, control or native DLL replacement occurred. User retains visual/FPS testing.

Combined-source confirmation: aa33cbd cleanly merges Opus's a664dfd (including his picked HUD and optional usage diagnostics) with the descriptor repair. Built the production DLL and all eight affected test targets; eight regressions PASS16.10s. Initial build command also named nonexistent j2_flow_real_test target; corrected to its actual j2_flow_test target before running tests. This was a command selection error, not a runtime or test failure. Full200receipt above remains at c2b1a82; no second200run claimed after the merge. PR99 now carries the combined source for Opus to package; no extra source integration task should be necessary.
