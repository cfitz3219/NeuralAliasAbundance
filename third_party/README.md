# Third-party code (vendored)

Re-vendored 2026-10-23 after the crash, at the same pins the original project recorded.

| Library | Version | Commit | License | Contents |
|---|---|---|---|---|
| MinHook | v1.3.4 | c3fcafdc10146beb5919319d0683e44e3c30d537 | BSD-2-Clause | full `include/` + `src/` |
| Dear ImGui | v1.92.9b | f1cc2ae15e53a861a874c3034aae6798fde194ab | MIT | core + Win32/DX12 backends |
| Streamline | v1.1.1 | 5bac43f464f53bc0583bab8df506b788d8d14c3c | MIT | `include/` headers only |
| NVAPI | 2026-10 main | 70d337db9186e968eab622f7e786de7e437faf3d | MIT | headers only (`nvapi.h`, `NvApiDriverSettings.h`, …); the runtime is the driver's `nvapi64.dll`, loaded dynamically |
| FidelityFX SDK (ffx-api) | main 2026-06-24 | 60f4ea81909200d8542eca14dccb2628b763a9a3 | MIT | 3 headers only (`ffx_api.h`, `ffx_api_types.h`, `ffx_upscale.h`), SDK layout kept so relative includes work; the runtime is the game's own `amd_fidelityfx_*_dx12.dll` (C7) |
| NVIDIA Image Scaling (NIS) | v1.0.3 | 35e13ba316c98eeecf16f37eae70ce88019911f6 | MIT | `NIS_Scaler.h`, `NIS_Config.h`, `NIS_Main.hlsl` (reference), `licence.txt`; `nis_scaler_embed.hpp` is generated from `NIS_Scaler.h` by `make_embed.py` (J2 Sharpness 101–200 %: sharpen-only, compiled at run time) |

Streamline rule (unchanged): TLOU ships interposer 1.4.7; no public 1.4.7 tag exists. Wrappers may forward
without interpreting SL structs; interpreting any field requires runtime validation evidence first.

## NOT vendored (local only, never committed, per COLLABORATION.md rule 5)

Their licenses forbid redistribution in a public repo. Each developer keeps them in one folder
(`NA_SDK_ROOT`; default `Documents\NeuralAliasAbundance-SDKs`, override with `-DNA_SDK_ROOT=` or the
`NA_SDK_ROOT` environment variable). CMake auto-detects each one and prints a summary line; a missing
SDK compiles that feature out and the build stays green:

| SDK | Expected at | CMake define when found | Without it |
|---|---|---|---|
| NVIDIA DLSS / NGX SDK | `<root>/nvidia-dlss-sdk/include/nvsdk_ngx.h` (+ `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib` for tools) | `NA_HAVE_NGX_SDK` | parameter reading, NR, FG inert (`valid=0`) |
| NVIDIA Optical Flow SDK 5 | `<root>/optical-flow/nvOpticalFlowD3D12.h` | `NA_HAVE_NVOF` | `nvof_flow_stub.cpp`: QueryCaps reports unavailable |
| Reflex PCL stats | `<root>/pclstats/pclstats.h` | `NA_HAVE_PCL_STATS` | PCL stats inert |
| DLSS runtime `nvngx_dlss.dll` | `<root>/nvidia-dlss-sdk/lib/Windows_x86_64/rel/` | (tests only) | DLSS GPU tests skipped |
