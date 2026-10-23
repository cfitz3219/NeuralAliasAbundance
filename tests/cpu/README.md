# A1 reconstructed CPU regressions

These tests were **reconstructed 2026-10** from surviving contracts and source.
They are new tests, not recovered copies of the lost suite. They invoke production
functions directly; checks remain active under Release/NDEBUG. No GPU, game,
NVIDIA SDK or network is required.

## Build and run

```sh
cmake -S tests/cpu -B build/cpu -DCMAKE_BUILD_TYPE=Release
cmake --build build/cpu --config Release --parallel 4
ctest --test-dir build/cpu -C Release --output-on-failure
```

Windows uses the MSVC x64 toolchain (`-A x64` with the Visual Studio generator).
The same directory supports `add_subdirectory(tests/cpu)` from Claude's root
build; that build must call `enable_testing()`. All targets start `na_cpu_` and
CTest names start `na.cpu.` to avoid collisions with historical targets.

| Suite | Coverage |
|---|---|
| frame_contract | Evidence-based provider selection, stale/invalid inputs, backend gates, frame continuity and reset |
| temporal_jitter | Independently calculated sample values, wrapping, valid ranges, actual projected pixel displacement and Z/W preservation |
| frame_history | Frame/time gaps, repeats, layout changes, disabled/missing inputs, explicit invalidation and generation recovery |
| upload_cache | Partial initialization, copy-before-unmap, discard, bounds/overflow, identity reuse, capacity and concurrent independent writers |
| size_confirm | Scalar poison/NaN/infinity, signed MV scales, subrect overflow, feature identity, restoration boundary, streak breaks and deadlines |
| motion_arbiter | Numerical camera-motion truth, jitter, raw reverse-Z semantics, engine priority, invalid masks, residual restrictions, snap/clamp and provenance |
| host_observe_abi | Frozen 0.1 sizes/alignment/offsets and explicit wire array offsets; no host runtime claim |
| host_observe_framing | **Windows/MSVC CPU-only:** actual unchanged host transport, callback framing, loss counters, failed resync, delivery-before-validity, malformed records, status capacity, restart, reentrancy and final callback |

The Linux ABI header test uses a local empty spelling for `__cdecl`; it does not
simulate Windows, prove MSVC layout, or substitute a fake observation transport.
The host framing runtime suite is registered only on Windows/MSVC, links the
existing `host_observe` target if available, and otherwise compiles the unchanged
host source directly. Its absence on Linux is explicitly reported at configure
time. It has label `windows`, **not** `gpu`, so the Windows CPU CI job runs it.

Recovered production headers are SYSTEM/external includes to the test targets;
strict warnings remain enabled for test sources. The portable production `.cpp`
files are compiled with strict warnings. No production file or root build file
is edited by A1.

Current local result: seven portable CTest executables (32 named cases) pass in
GCC 14.2 Release. Windows/MSVC compile and the six host runtime cases await
Windows CI; no Windows/GPU/game test session has been performed. Formal peer
review remains at Phase 1's review gate.
