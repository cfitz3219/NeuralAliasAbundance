# A9 — Anno 1800 DX12 diagnostic session

2026-10-24, approximately 19:54–19:57 America/Los_Angeles. This is a real-game diagnostics report, not a claim that Anno's rendering inputs are qualified for reconstruction.

## Build and procedure

Anno 1800 Steam v18.4.1412158.165273, NVIDIA GeForce RTX 5070 Ti. Game log confirms a D3D12 feature-level 12.0 device. Executable SHA256: `C323FD6BE31BEF2CFD30A80CC6585818BA01E99799DC48E47862AB5A150DA1E7`.

The consumer was built from PR27 `cd3d29c` plus the optional startup-log change in `host_boot.cpp`. The exact deployed DLL/INI hashes and original-file absence are recorded in the local manifest. Later reader-sharing/PID-log corrections were tested locally; they were not in this game process. No NVIDIA runtime DLL was copied into the game.

Explicit switches enabled the host, observation producers and read-only discovery adapter. Route A, FSR/XeSS interception/replacement and the replacement presenter were disabled. Loaded-module inspection confirmed the local forwarding DXGI DLL and the system DXGI/D3D12 libraries. Continued the existing campaign into the coastal lighthouse/boat scene, opened the NeuralAlias overlay, inspected Picture and Details, then exited normally. No building/quest actions were performed.

## Results

Final periodic report, session `14620485787834971611`, epoch 1:

| Observation | Result |
|---|---:|
| Periodic reports | 73 |
| Transport batches | 14,054,278 |
| Events consumed | 56,493,581 |
| Queue submissions | 139,414 |
| Live resource records | 21,664 |
| Snapshot events (all statuses) | 3,315,759 |
| Malformed records | 0 |
| Foundational/detail drops reported by transport | 0 / 0 |
| Inventory consistent at final report | Yes |
| Complete contiguous capture | No |
| Present events in discovery transport | 0 |
| Emitted write-role candidates | 0 |
| Completed texture samples | 0 |
| Matrix hypotheses emitted | 0 |
| Confirmed inputs | 0 |

Snapshot-event count is not a count of readable or qualified matrices. No texture sample result was emitted; this is absence of completed acquisition, not a successful content check. This bounded logger did not retain per-operation/binding rejection reasons, so the cause of the missing candidate chain cannot be localized from this session. Do not infer that Anno has no depth/color/motion resources. Presentation was visible in the overlay, but its count is not an observed transport frame association.

Depth, pre-HUD color, camera, motion, jitter and frame association therefore remain **Unavailable** for effect use. Reasons include incomplete execution coverage, no completed source samples, no qualified execution-time camera association and no preserved pre-HUD composition lineage. The production confirmation APIs are conditional on those observations; this session did not satisfy them. No diagnostic result promoted an effect input.

The overlay rendered in gameplay and explicitly reported that picture data still needed checking and picture controls were unavailable. Technical details showed zero DLSS modules/features/passes and the game's own screen-output path, with Frame Generation unavailable. The route-label text was not readable/reliable in this capture and is not used as evidence.

Sampled overlay performance was roughly 135–155 frames/s. This is not a benchmark: camera/overlay state varied and a local build/test run overlapped part of the session. The game also resized near exit. No speedup or diagnostic-overhead claim is made. Water, terrain, boat and HUD rendered coherently in sampled images. Bright/clipped highlights were also present in the repaired baseline; Windows showed Auto HDR enabled. Temporal ghosting and HDR fidelity were not qualified by these screenshots.

## Failures and recovery

Before deployment, ordinary DX12 launches displayed a black screen. The game log showed successful device creation followed by shader-cache reads failing with error 362, cloud provider not running. With Anno closed, the cache was renamed to `shaders.backup-20261024` and an empty `shaders` directory created. The next unmodified DX12 launch rendered its title/menu. This preserves the original cache; it does not repair unavailable cloud save files. No driver, OneDrive, security or privacy settings changed.

The first deployed session loaded the proxy but produced no consumer log. Optional startup breadcrumbs were added; the subsequent run reported overlay/adapter started and captured the results above. The change adds observability, not a proven fix for intermittent bootstrap behavior. Startup lines in that session lacked process IDs; the final source adds IDs to distinguish launcher/game processes.

The original CRT file-open mode prevented reading the capture while active. The final source uses a share mode that allows readers and refuses competing writers; the activation test now verifies live reading. The session log was inspected after exit. A final drain marker was not recorded during process teardown; metrics above are the last flushed periodic report.

The game log also contains missing asset/source-shader messages and one CreateShaderResourceView failure. Missing-file messages also existed before deployment; no causal attribution to the consumer is established.

## Restoration and evidence

Rollback verified removal of the newly added `dxgi.dll` and `NeuralAliasHost.ini`; neither existed beforehand. The game executable hash is unchanged. Window configuration remains 2752×1152 with the same fullscreen mode, and DX12 remains selected. No diagnostic deployment remains active. The regenerated cache is retained to preserve the black-screen repair; the previous cache remains separately backed up.

Local-only evidence: `local/gate3-anno-20261024/{manifest.json,manifest-session1.json,discovery.jsonl,metrics.json,startup.log,game-session.log,deploy.ps1}`. No game files, SDKs or captures are committed. Capture SHA256: `2D30A28C45ED6B5F7BC7E37B2C34FA50B4890E679A73448DF552D4A375CC2A07`.

A9's requested session/report is delivered. It establishes that the read-only transport consumer runs in Anno, and documents its lack of qualified inputs. It does **not** close live acquisition/compatibility gaps or Gate 3. Review must assess those limits alongside the measured fixture acceptance in `A5-ACCEPTANCE.md`.
