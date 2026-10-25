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

## Follow-up and exit correction (supersedes normal-exit interpretation above)

Windows Application event 1000 subsequently revealed `0xc0000409` in the diagnostic DXGI DLL at shutdown of both earlier deployed processes, including the rendered-session PID39576. Thus “exited normally” above describes the UI action, not a clean runtime teardown. A standalone transport process-exit regression reproduced the identical fail-fast before the correction. The cause was destruction of a joinable transport `std::thread` when the application had not called `na_observe_stop`.

The correction gives transport state process lifetime and registers an exit guard when observation starts. During EXE CRT teardown it stops/drains a still-running worker before other globals are destroyed. During DLL process-detach, if Windows has already terminated that worker, it avoids touching potentially abandoned locks and leaves the retained state to process reclamation. Explicit stop/restart still joins normally. The reproducer now passes, as does a separate active-diagnostic-DLL `ExitProcess` test. This correction was tested outside Anno after rollback; another clean in-game shutdown has not been demonstrated.

A short follow-up at approximately20:00–20:01 used the inventory-only acquisition path and live-readable log. It emitted35 distinct eligible texture inventory records (bounded24 per report), all confidence **Unavailable**, rather than calling creation flags semantic evidence. Examples: format19 textures at2752×1152 and3072×3072, format28/29 textures at1376×576, format56 at1376×576, and format10 at2048×2048. Format19 is R32G8X24_TYPELESS; this diagnostic scalar decoder does not qualify that packed/multiplane format. These are resource characteristics, not main-depth/scene classifications.

Eight emitted sample results failed while armed (`host_status=4`, reason5: timeout), with queue/ticket0 and no bytes. No completed copy or known source occurrence was reported. Final summary:49 submissions,370 resource records,9 snapshot events,0 malformed/drops, incomplete capture. The process ended before a rendered follow-up scene could be verified; its cause is not established by this log. This does not replace the earlier rendered-session evidence. Both deployment manifests show verified rollback; no diagnostic DLL/INI remains installed.

Follow-up local evidence: `local/gate3-anno-followup/{manifest.json,discovery.jsonl,deploy.ps1}`. Capture SHA256: `9CEB52D5FFF45B0DA724107355CF809552A04580C07FA5ABA908003615F12151`. The renderer is recovered from the original black-screen problem; live input qualification and reliable in-game diagnostic lifecycle remain review/compatibility findings, not passed acceptance claims.

## 2026-10-24 continuation — startup cause fixed, actual clean gameplay exit

Source30b4256 (following descriptor-slot correction442d7d5). The startup failure is now established: a real debugger stack showed a driver/DXGI queue Wait re-entering ExecuteBegin on the submitting thread while its nonrecursive sample-order mutex was held. A regression reproduced std::system_error/resource deadlock before the correction. Same-thread nested occurrences now preserve the outer lock/reference owner and mark both occurrences order-ambiguous; they do not authorize sampling. Eight related tests passed after a full Release rebuild. A separate overlay route-tag stack-lifetime bug was also corrected.

Restored baseline PID15644 reached the menu and normal UI exit returned0. Corrected diagnostic PID38424 reached coastal boat/lighthouse gameplay with HUD. Overlay showed A9 READ ONLY, the game's own output,0 DLSS modules/features/passes and unavailable picture controls. Displayed rate was approximately150–165fps at the inspected moments; no controlled performance comparison is claimed. The earlier black startup ultimately progressed; a debugger attachment attempt did not obtain thread stacks and detached. That startup delay is not classified as a proven deadlock. No full test suite ran concurrently with this game session.

Normal Save and Exit from gameplay returned process exit code0 at2026-10-25T03:30:14.997Z. This verifies the corrected build's normal in-game exit for this session. No new Application1000 event appeared in the checked window. Manifest rollback verified the original installation afterward.

Final retained summary: session15572607776423076350,epoch1,12,839,506 batches,42,437,647 events,136,355 submissions,21,578 resource records,2,766,204 snapshot records.12 completed texture samples and7 arm timeouts were logged. Completed samples carried actual queue6/tickets, formats28/29/10/54 and131,072–33,554,432 bytes. Draw bindings now resolve: logged candidates reached694,547 depth writes and334,627 color writes. These establish observed use and completed copies, not semantic input qualification.

Health counters:21,672 captured snapshots,2,521,659 unmapped,29,984 GPU-written;0 truncated snapshots. Sampling refusals recorded7,736 absent/unsuitable transitions,0 missing-recording/coverage/state-overflow refusals.0 malformed events and0 foundational drops;9,467 detail drops occurred by the final summary. Inventory remained consistent, capture contiguous remained false.0 transport Presents and0 matrix hypotheses in this particular build are addressed by the next follow-up; do not substitute the overlay Present counter for transport evidence. No camera execution-time association, preserved pre-HUD lineage or motion semantic confirmation was promoted.

Evidence: local/gate3-anno-reentry-fixed/{discovery.jsonl,metrics.json,exit.json,manifest.json}. Capture SHA256 B16AF965D7EBDBAA2E6547E8FB656483F384916EE8ED0B6FBA26911D6FAD62B9. Earlier failures remain separately preserved in binding-check/exit-probe/debug directories with rolled-back manifests. The failure cause supersedes the earlier unknown-cause qualification only for the reproduced queue-reentry path.

## 2026-10-24 final planned verification — Present and scan wiring verified

Tested source9e1b140a517bcfe9d97675e16668f1f138c6de72 in one fresh launch after the user resumed. No restart was performed. PID37488 reached the same coastal gameplay/HUD scene. The overlay displayed A9 READ ONLY, game-owned output and0 DLSS modules/features/passes; picture controls remained unavailable. The inspected displayed rate was approximately165fps, not a controlled benchmark. No new visual defect was apparent in the short static coastal observation; the known HDR screenshot brightness limits visual comparison. Normal Save and Exit returned0 at2026-10-25T04:03:07.160Z. Rollback verified all original installation files; no new Application1000 event was found in the checked ten-minute window.

Session2943275297063217984,epoch1: final logged summary12,166,904 batches,43,855,000 events,105,711 submissions,16,419 actual transport Presents,21,283 resource records and2,463,954 snapshots.19 sample outcomes:12 completed,7 arm timeouts.16 bounded matrix_scan records explicitly show captured512-byte buffers being scanned with execution_qualified=false; no matrix hypotheses or confirmed inputs were reported. Captured-status stride selection is active, but it does not manufacture a camera from unrelated constants.

Final health:4,924,404 draws,750,618 target binds,315,916 unresolved views;16,421 captured snapshots,2,255,105 unmapped,34,382 GPU-written and0 truncated. Sample refusals:7,488 missing/unsuitable transitions,0 missing-recording/partial-coverage/partial-state refusals.0 malformed events,0 foundational drops,4,592 detail drops. InventoryConsistent and CaptureContiguous remain false: the newly observed preexisting swapchain deliberately carries METHOD_UNOBSERVED for its creation queue/resize history/color space, which conservatively invalidates complete-inventory evidence. This is incomplete provenance, not proof that a Present was lost or a reason to remove the safeguard.

Present logging, captured-buffer scan wiring, actual read-only gameplay and normal exit are verified. Remaining qualification gaps are execution-time camera provenance, packed-depth acquisition, preserved pre-HUD lineage, motion corroboration and complete capture coverage. Repeating this same capture does not resolve them; code/evidence acquisition work and review are required. No additional Anno restart is scheduled for these checks.

Evidence: local/gate3-anno-final-verification/{discovery.jsonl,metrics.json,exit.json,manifest.json}. Capture SHA25686DED476C2886DBB9427E96AAC74979C7FE7CF54B5A9952C71E9F8153225680A. Deployed DLL SHA256B61D739497E0B50E00506178D5BDC2B7FAC677FD2E6050F057BC012B2820496D. Source9e1b140 passed all3 CI jobs in run40158744541; local full Release/default61/61 tests passed, plus the extended mixed-status adapter regression. Tests were completed before the game launch.
