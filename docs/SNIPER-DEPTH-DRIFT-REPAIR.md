# Sniper depth-state and camera-drift repair — 2026-10-28

## Observed failed deployment

SE5 installer and deployed DLL agree on 8cbc62dbc73071502962d1933309bb930d4eaf18, SHA256 44B8B618A8274AC313AACE55EB6E66ED990C6B9DF4234CE1B8C613A3A7502F34. It was not a stale shortcut. The archived/current log records 9786 scene frames, 1916 executions, 7870 misses, 12816 depth-state refusal attempts, 144 camera refusal attempts and zero NGX evaluation failures. Cort reports continued upward drift and worse engagement. Failed attempts are not unique missed frames.

## Reproduced defects and corrections

1. The jitter rebasing correction retained only the immediately previous sample. Rotating upload-buffer ranges can retain older injected matrices, which then get treated as native and jittered again. The regression compares that exact single-sample algorithm to a per-resource/per-offset write ledger over 480 frames. Peak old displacement error for rings of 2/3/4/6/8/16 ranges is 13.5000/0.8889/20.0556/4.7778/22.9445/11.2778 pixels. The corrected path equals native camera plus exactly one sample at every step. Game rewrites win; failed writes are not recorded. The ledger retains exact original/written 64-byte ranges, keeps identities separate, and never evicts live patches to reinterpret them as native later. It normalizes the CPU working copy before producing the next replacement; it does not write old originals into GPU-live buffers at Present.

2. The carried-state store excluded ALL placed resources. A placed depth texture transitioned in a prepass therefore had no carried state for a later read-only scene list. Reproduced on the RTX 5070 Ti: restoring the old eligibility predicate fails KnownSubStateNow with registered=0; the fix returns exact combined depth-read/shader-read state 0xe0. Placed depth textures are now tracked, with named alias invalidation and whole-store invalidation for wildcard aliases. Reserved and simultaneous-access textures stay outside this path; ordinary placed colour eligibility is unchanged. A three-way lookup distinguishes absent evidence from pending/invalid evidence, preventing fallback to an older state through an alias or split barrier.

The existing live logs do not include the depth allocation kind or the precise failure of the state query. Accordingly the placed-depth defect is proven in the runtime regression, but NOT yet established as the explanation for every live SE5 refusal. New j2_depth_evidence records allocation kind, list coverage/completeness, registration, exact carried state, CPU submission marks and observer-disable conditions if depth fails. These marks concern overlapping CPU submission callbacks, not GPU fence completion; earlier descriptions conflating them were incorrect.

## Validation

Final Release DLL build passed. 12/12 focused tests passed in 34.90 seconds: jitter_writes; depth_carried_state; observe_sample; J2 live, array/shape, plane barriers, split scene, inline, inline copy, inline record-first, auto inline and auto between. Baseline depth eligibility deliberately reproduced the refusal (exit 1), then corrected source was restored and rebuilt. Initial test-development failures: the three-range test alone did not exceed one pixel, so the test reports all six ring lengths rather than claiming that every rotation accumulates equally; the depth fixture initially used a creation recording rather than a fully observed Reset recording, corrected before baseline comparison. Tests contain GPU waits only in the test utility.

No game launch or deployment was performed. Cort authorized automated regressions and explicitly retains game testing. This patch does not close unrelated placement/camera-acquisition findings or establish live flicker/drift elimination. The unfinished broader changes are parked in the named git stash, not included in this build.

## Guidance checked

- NVIDIA Streamline DLSS guide: required depth/motion/colour inputs, matching frame constants and jitter: https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md . This project uses NGX directly; Streamline-specific matrix conventions are not substituted for NGX conventions.
- AMD FSR integration, Camera jitter and Placement in the frame: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/ . The raster sample must match the supplied temporal input; the fix preserves the existing sequence and changes ownership/rebasing rather than adding an arbitrary camera offset.
- Microsoft resource barriers: https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12 . Depth textures cannot implicitly promote; read flags may combine; named/wildcard aliasing and split transitions require explicit treatment. No generic assumed DEPTH_READ state was introduced.
