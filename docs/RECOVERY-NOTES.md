# Recovery notes (2026-10-23)

The original repo (`C:\Users\cortf\Documents\Codex\2026-09-21\we-re-starting-a-reusable-dx11\`, route-a worktree) was lost in the October 2026 crash. This folder rebuilds it from what survived.

## Sources used

1. `Downloads\neural_alias_codebase.txt`, a single-file dump saved 2026-10-15 at 8:59 PM. It held **all 74 `src/` + `shaders/` files** (the newest state, through task A78). They're copied byte-for-byte.
2. Nine documents Cort uploaded on 2026-10-23: README, AGENTS, HANDOFF, WORKLOG, WORKLOG-ROUTE-A, COLLABORATION, CMakeLists.txt, build.ps1, Feasibility-and-Plan. **These are older snapshots** (around 2026-09-23, through task A23 / B-002). The code is newer than the docs.

## Rebuilt in this pass

- `third_party/`: MinHook v1.3.4, Dear ImGui v1.92.9b, and Streamline v1.1.1 headers, re-vendored at the original pins (see `third_party/README.md`).
- `src/runtime/dxgi_proxy.def`: reconstructed from the exports in `dxgi_proxy.cpp`.
- `.gitignore`: rebuilt from the rules in COLLABORATION.md.

## Known gaps

### The code is ahead of the docs
- **CMakeLists.txt is stale.** It doesn't build these 17 newer source files: `core/motion_arbiter*.cpp`, `adapters/tlou1/*`, `fg/*`, `hooking/variant_registry.cpp`, `host_boot/host_boot.cpp`, `latency/*`, `motion/live_flow.cpp`, `nr/nr_pass.cpp`, `nvof/*`, `overlay/freeze_watchdog.cpp`, `overlay/presenter.cpp`. Targets for these need to be added. First job for whoever owns the build.
- WORKLOG-ROUTE-A stops at A23. Tasks A24 to A78 and B-003 to B-007 (variant registry, host boot / game adapter contract, frame gen, NR pass, Reflex, NVOF) are known only from code comments.
- COLLABORATION.md lists only B-001 and B-002. B-003 to B-007 are referenced in code but their agreements are lost.

### Missing files (referenced but not recovered)
- **Tests (17):** `tests/{hooking,host_observe_abi,host_observe_framing,capture,gpu_timing,ngx_observe,ngx_params,ngx_optimal,overlay,proxy,sl_proxy,frame_contract,size_confirm,temporal_jitter,frame_history,upload_cache}_test.cpp`, `tests/dx12_fixture.cpp`, `tests/ngx_stub.cpp`, `tests/sl_stub.cpp`
- **Tools/scripts:** `tools/nr_replay.cpp`, `scripts/generate-sl-proxy-def.ps1`, `scripts/deploy-sl-proxy.ps1`
- **Docs:** `docs/{TLOU-PART-I,UI-STYLE,NR-INSERTION-DESIGN,NR-TLOU-I-REPLAY,ANNO-1800,PROVENANCE}.md`, `docs/source-provenance.json`, `src/host_dx12/OBSERVATION-API.md`
- **Route B (Astra):** everything under `src/providers/discovery/` (HOST-OBSERVATIONS.md, B-001-ACK.md, discovery code). Astra's Codex history may still have these.
- **Local only (never in git anyway):** `local/` deployment manifests, TLOU session logs, captures.

### Not verified
Nothing has been compiled yet. This rebuild ran on Linux without MSVC. The first Windows `build.ps1` run will show what breaks, which will be the stale CMake first.
