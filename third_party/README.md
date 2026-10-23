# Third-party code (vendored)

Re-vendored 2026-10-23 after the crash, at the same pins the original project recorded.

| Library | Version | Commit | License | Contents |
|---|---|---|---|---|
| MinHook | v1.3.4 | c3fcafdc10146beb5919319d0683e44e3c30d537 | BSD-2-Clause | full `include/` + `src/` |
| Dear ImGui | v1.92.9b | f1cc2ae15e53a861a874c3034aae6798fde194ab | MIT | core + Win32/DX12 backends |
| Streamline | v1.1.1 | 5bac43f464f53bc0583bab8df506b788d8d14c3c | MIT | `include/` headers only |

Streamline rule (unchanged): TLOU ships interposer 1.4.7; no public 1.4.7 tag exists. Wrappers may forward
without interpreting SL structs; interpreting any field requires runtime validation evidence first.

## NOT vendored (local only, never committed, per COLLABORATION.md rule 5)

Each developer supplies these locally:
- NVIDIA DLSS / NGX SDK (`nvsdk_ngx*.h`, `nvsdk_ngx_defs_dlssg.h`, `nvsdk_ngx_s.lib`). CMake auto-detects via `NA_NGX_SDK_INCLUDE`.
- NVAPI (`nvapi.h`)
- NVIDIA Optical Flow SDK (`nvOpticalFlowCommon.h`, `nvOpticalFlowD3D12.h`)
- PCL Stats / Reflex (`pclstats.h`)
- Any NVIDIA runtime DLLs (nvngx_dlss*.dll, NR runtime)
