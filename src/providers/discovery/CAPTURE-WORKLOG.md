# Capture decoder integration journal

## 2026-10-24 — ABI 0.1 consumer boundary

Fetched Claude's branch: latest remains 5727047 (test checkpoints). H2/H3 additions and the previously supplied explicit-buffer-SRV fix are not published there. Merged those checkpoints into the isolated integration branch; first merge attempt failed for missing identity without changing files, then succeeded with command-scoped identity.

Added capture_decoder.hpp in Astra's discovery lane. This is a bounded worker/offline structural decoder, not an inventory tracker or resource classifier. It copies valid batches into owned storage, checks all 23 known event minimum sizes, exact payload/count/epoch framing, identity-bearing session/epoch records and overflow-safe explicit inline array offsets/strides. Unknown optional events survive as opaque records; unknown foundational events remain explicitly flagged. Any malformed tail discards all batch output. Future minor fields are allowed; major mismatch is rejected. Policy limits are 4 MiB and 65,536 events per batch; exceeding either requires caller loss/invalidation handling, not silent truncation.

Nine focused regression cases cover every truncation of a resource batch, all known minimum sizes, malformed tails, unaligned input ownership, future minor versions and inline strides, unknown foundational records, identity mismatches and overflowing counts. Integrated the decoder into the existing real Windows transport test callback: lifecycle, loss, snapshot/resync and final batches all decode successfully. No GPU, hooks or game deployment in this test.

Fresh standalone MSVC Release build: 11/11 CTests pass. Initial configure did not find Python; reconfigured with the bundled Python before the reported suite, so fixture.reference ran and passed. Self-review removed an unjustified natural-alignment restriction on inline arrays: the ABI only requires minimum strides and bounded explicit offsets, and decoding uses memcpy. Added a stride-20 capability regression. No frozen ABI or Claude-owned implementation changed.

Remaining work: authoritative status-counter/inventory/capability state, H2 execution provenance, H3 and completed texture samples. Decode success is only structural validity; it never means loss-free capture, complete inventory, ordered submissions, GPU completion or qualified DLSS inputs. A worker integration must inspect status/capabilities and unknown_foundational before deriving evidence. The borrowed callback allocation's extent is guaranteed by the host; offline callers must independently supply their actual allocation extent. No live runtime consumer registration yet.

## 2026-10-24 — authoritative validity tracking

Added CaptureValidity, a serialized worker-side conservative gate using coherent status queries and decoded batches. It separately reports baseline inventory consistency, continuous capture history and supported capability groups. Foundation loss/malformed/unknown foundational records remain invalid until a new observed epoch; detail loss and retroactive ambiguity revoke history. FINAL closes readiness. Status counter rollback, old-epoch queries, missing capability groups, partial support, duplicate boundaries and incomplete status cannot authorize evidence. New-epoch loss counter deltas cannot disappear into a reset baseline. First attachment after historical losses conservatively requests another resync. Fresh status is required after callback completion and before use; the class cannot detect a caller supplying stale status. It makes no resource, frame, synchronization or input qualification claim.

Nine validity regression cases pass alongside the existing decoder/transport/fixture suite: fresh standalone MSVC Release12/12. Initial implementation reset counter baselines on epoch change; self-review corrected that potential hidden-loss race and added its regression before commit. This layer remains offline/worker groundwork, not registered into the runtime.

Fetched Claude9655c85: allocator hook removed, but explicit SRV/session/collision fixes remain absent. Full combined build passes; committed RTX producer CTest still exits0xc0000409 after checkpoint4,25hooks,0missing,shared slots empty. Log local/phase3-integration/ctest-host-9655.log. Earlier PR25 head46e92a6 CI run40089660505 completed success; that does not cover the new validity changes until new CI runs.

## 2026-10-24 — real producer/consumer round trip

Added optional host integration test target when host_observe_producers is available. It runs the real producer through DecodeBatch and CaptureValidity at startup, resync and FINAL, creates observed and unobserved resource views with valid descriptors, checks hardware health and enforces unavailable H2 PASS capability. Runs independently on the hardware adapter and explicitly selected WARP. This is a controlled test program, no game injection or live GPU readback. CPU standalone builds omit this host target explicitly through its CMake condition.

After applying the user-authorized host fixes1a81e1b in PR26, combined e08d2de full Release build passes and both na.host.capture_consumer and na.host.capture_consumer_warp pass with zero skips. The preceding combined df18a4e run passed21 discovery CPU/fixture/menu tests plus the corrected committed producer test (22/22). This replaces the earlier session-mismatch failure for the corrected snapshot only. Future H2/H3, resource/frame provenance and lifetime/order review findings are still required for live classification.

## 2026-10-24 — entry-hook integration and reproduced R3 ordering gap

Claude238f508 includes direct vtable-entry hooks (d3e3302) plus a texture substitute for the formerly invalid SRV. The combined merge1407803 preserves the tested unobserved buffer/explicit BUFFER SRV pair; blindly accepting the textual merge would create a texture with a buffer view. Session and RAII fixes remain intact. Full Release builds and3/3 targeted tests pass: RTX producer, RTX consumer, WARP consumer. No H2/H3 ABI additions yet. Producer canonical identity/sentinel attachment/registration ownership and sync-overlap fixes remain Claude-owned, coordinated in main COMMS.

Added queue_overlap_probe.cpp: real Signal and Wait calls inside a manually controlled host-owned ExecuteBegin/End window. No ExecuteCommandLists or rendering work is submitted; this isolates R3 hook-window bookkeeping, not actual GPU ordering. At1407803 on RTX the execute ticket1 and signal/wait tickets2/3 all have flags0, and no ambiguity range is emitted:3 assertions FAIL. It proves the previously source-level sync/execute overlap finding in this controlled integration path. This is independent of the corrected descriptor/session defects.

The diagnostic target na_queue_overlap_probe builds when C8 exists. It is intentionally not in default passing tests while its producer fix is pending. Run it directly, or configure NA_ENABLE_PENDING_CAPTURE_TESTS=ON and run na.host.queue_overlap_pending to reproduce. Do not describe default green CI as covering this unresolved regression. Once Claude's producer patch arrives, enable this regression by default and remove the pending switch. Raw initial evidence local/phase3-integration/queue-overlap-probe.log. No producer lifecycle source edited by Astra in this checkpoint.

## 2026-10-24 — H2 ABI0.2 integration and queue-fix follow-up

Claude published H2 through b736b6a. Combined8027316 merges it with queue fix3aa7745 (user-authorized, PR29). Default queue regression now passes; prior opt-in/pending status is superseded on this corrected integration. Execute/Signal/Wait share finalization, so events are stamped only after their observed window closes. Internal shared tokens retain queue bookkeeping; no GPU synchronization proof is implied.

Added conditional ABI0.2 structural decoding for root signatures, pipelines and list operations, with explicit root-array and typed payload bounds. Malformed lengths/counts are rejected. Truncation and known representational gaps set incomplete_capture and invalidate contiguous evidence. Root tables omit offsets/full ranges; viewport/scissor arrays retain only first element; texture-copy offsets/box/footprint are absent. These remain acquisition/producer amendment tasks, not trustworthy complete pass evidence. No frozen ABI edit by Astra. Three new synthetic H2 cases cover short structs, root arrays, payload bounds and incomplete shapes.

New h2_capture_decoder_test wraps Claude's actual pass fixture callback through DecodeBatch without modifying his source, retaining all original fixture assertions. It verifies structural acceptance plus explicit incomplete root-table evidence. Full combined Release and44/44 default non-GPU-labelled CTests pass, zero skips, including the now-default ordering probe, real H2 fixture and its decoder wrapper. Raw logs local/phase3-integration/{build-h2-final.log,ctest-h2-full.log}. H3/content completion and unrelated COM/sentinel/list lifetime findings remain open. No deployment or game input control.

## 2026-10-24 — H3 structural integration and full regression

Merged Claude C9 29e8647 at6b0b890, preserving shared queue occurrence tokens and default zero-list arguments for Signal/Wait. Added bounded kind27 decoding, malformed payload tests and actual H3 snapshot fixture wrapper. All snapshots remain incomplete_capture, including CAPTURED, because APP_SYNC_UNKNOWN is not coherent camera input. No runtime signal registration or game deployment.

Full Release succeeds; targeted7/7 and default non-GPU-labelled46/46 CTests pass, zero skips, on this Windows RTX machine (includes explicit WARP consumer). Logs local/phase3-integration/{build-h3-decoder.log,ctest-h3-decoder.log,ctest-h3-full.log}. Serial fixture success does not test concurrent Unmap. C9-H3-REVIEW-1 sent in main COMMS: original Unmap precedes the metadata lock, so snapshot memcpy is not protected by that lock; dedup suppresses distinct binding provenance; snapshots precede final queue overlap status. These remain Claude-owned fixes and qualification limits.

Queue PR29 initial Linux CI failed GCC misleading-indentation in the portable test loop. Fixed by152cf47 and merged; no production behavior change. H2 integration briefly duplicated its CMake target when merging the isolated consumer branch; corrected05d2f3c before successful rebuild. Latest CI pending at this checkpoint. Queue fix3aa7745 supersedes prior pending-overlap status; its regression is mandatory and passing.

## 2026-10-24 — consumer provenance continuation while C9 fixes remain unpublished

Fresh fetch still has Claude c9-h3 at29e8647. Posted ownership confirmation to COMMS: Claude retains snapshot lifetime/binding provenance and acquisition work; Astra advances consumer checks without duplicating host changes.

New regression demonstrated CaptureContiguous remained true for an execute explicitly containing unobserved bundles. Decoder now marks bundle submissions, unknown list identity/generation, and failed Reset/Close records as incomplete evidence while preserving structural/inventory validity. Initial generation zero remains legal. Added regression across bundle/failed reset/failed close plus unknown identity/generation. Provisional H3 regression verifies that clean Execute before or after the snapshot cannot restore readiness, status refresh cannot clear it, and stale old-epoch snapshots cannot poison a fresh resync. This is conservative consumer behavior, not a repair of producer pointer lifetime.

Initial targeted build used a nonexistent target name na_capture_validity_test; corrected to na_cpu_capture_validity. New regression then failed as intended (10 cases pass,1 fails), log local/phase3-integration/ctest-provenance-before.log. After correction full Release succeeds and46/46 default non-GPU-labelled CTests pass, zero skips, including RTX/WARP and actual H2/H3 wrappers. Logs build-provenance.log and ctest-provenance-full.log in that same local directory. CI pending at publication. No frozen ABI/producer edits, no deployment or live qualification.

## 2026-10-24 — PR31 snapshot fixes integrated and amended consumer checked

Claude published fixes on a separate branch claude/c8-c9-review-fixes, PR31, not the unchanged c9-h3 branch. Integrated head fcbc57a (0705bf1 implementation,7d865e1 warning correction) by fast-forward. This supersedes prior fixes-unpublished status. Confirmed Unmap metadata invalidation precedes original call; snapshot binding dedup removed; root ranges and full viewport/scissor arrays added. Queue/session fixes preserved. Initial12/12 targeted tests pass, including real H3 race fixture and wrapper.

Extended decoder bounds checks to range arrays and table indices, with explicit truncated-array handling, and typed viewport/scissor/copy payloads. New synthetic tests cover malformed range references and valid/invalid operation strides. Current-tree structural integration only: earlier0.2 shorter root records are rejected, not mixed-version compatible. Tables and APP_SYNC_UNKNOWN snapshots remain incomplete evidence; footprint dimensions/format remain absent.

First full run45/46 passed; failure was my overly broad test edit changing unknown-op expectation to invalid instead of valid-incomplete. Corrected that expectation. Final full Release and46/46 default non-GPU-labelled CTests pass, zero skips, including RTX/WARP. Logs local/phase3-integration/{build-review-fixes.log,ctest-review-fixes.log,build-review-decoder-final.log,ctest-review-full.log,ctest-review-final.log}. No game deployment or driver-removal test.

Sent review qualifications to Claude: Sleep-based race test lacks entered-boundary handshake; SEH is not destruction lifetime ownership; same-address distinct binding regression still needed; table ranges cap8 entries without coverage flag; extended copy footprints lack dimensions/row pitch/format. Async textures remain Claude-owned. These are separate from the corrected single-map Unmap ordering and removed dedup. No blanket lifecycle or Phase3 closure claim.

## 2026-10-24 — direct repeated-binding provenance regression

Checked all remote branches and current COMMS; no newer lifetime/coverage/async-texture implementation beyond fcbc57a. Coordinated new independent test in tests/cpu/snapshot_bindings_test.cpp; no producer edits or duplicate acquisition work.

The test records two actual D3D12 lists, each with two root CBVs and two table CBVs all sharing one resource/address. Table views request256 and512 bytes. Calls the host ExecuteBegin/End seam with both lists (controlled observation boundary, no ExecuteCommandLists or GPU work), then overwrites/unmaps source memory. Owning decoder must retain8 records with8 distinct list/op/param/size associations, correct generation/queue/ticket and original copied bytes. Every snapshot remains APP_SYNC_UNKNOWN/incomplete. This directly covers the removed address-only dedup bug; no old-producer negative-control run performed.

Full Release build passes and new default na.host.snapshot_bindings test passes on RTX5070Ti,8 records/8 associations/0 failures. Logs local/phase3-integration/{build-bindings.log,ctest-bindings.log}. Existing executable behavior unchanged; previous46/46 baseline remains separately recorded. CI for the new combined head pending. Remaining ownership/destruction lifetime, deterministic race seam, coverage flags and asynchronous texture sampling stay Claude-owned. No game deployment or Phase3 closure.

## 2026-10-24 — C9b asynchronous texture review and controlled integration

Fetched all Claude branches; new c9b-texture-samples67b7744 (PR32) is based on5ded6a0. Merged experimentally into integration92e5aad, retaining b55de74 repeated-binding regression. Full Release and48/48 default non-GPU-labelled tests pass, zero skips; RTX sample fixture gives correct color/depth and subrect results. Evidence local/phase3-integration/{build-c9b.log,ctest-c9b-sample.log,ctest-c9b-full.log}.

Source review found six blocking categories: repeated in-flight cancellation frees too early; source/failure-path ownership; original-plus-copy ordering; incomplete transition evidence; expiry of borrowed pointers; resource shape/copy validation. Full evidence and targeted fix/test requests in docs/C9B-REVIEW.md and main COMMS. Verified depth source-box requirement against Microsoft documentation. No dangerous negative-control GPU failures induced. No production sample_request callers; integration remains experimental and no live A5/A7 consumer was enabled. Producer corrections remain Claude-owned. No deployment or completion claim. CI pending for combined checkpoint.

## 2026-10-24 — C9b six-finding fixes rechecked

All Claude branches checked; new c9b-review-fixes688c0ec (PR33) integrated by fast-forward. Full Release and48/48 local default non-GPU tests pass, zero skips, including updated adversarial RTX fixture. Logs build-c9b-fixes.log and ctest-c9b-fixes-full.log under local/phase3-integration. R1/R5 original defects corrected; other fixes present with narrower remaining qualifications. No producer edits by Astra.

C9B-REVIEW-2 sent to Claude: detached-thread singleton guard launches another worker per request; raw source acquisition still needs ownership proof; scope validation is deferred to worker maintenance; typeless depth partial copies bypass typed-format check. Also distinguish source-reviewed queue interleaving and untested Signal failure/plane rejection from tested paths. Full dispositions appended docs/C9B-REVIEW.md. No live consumer enabled; next integration depends on producer corrections. Latest combined CI pending.

## 2026-10-24 — PR34 integration and new worker reproduction

All branches checked; integrated629ad9b. Full Release builds. New standalone RTX worker-wakeup diagnostic reproduces missing notification: warmup20ms timeout succeeds, second request after idle remains ARMED after~1s (1 assertion fails). Added test-only target and explicit opt-in pending registration; default passing tests do not cover this unresolved failure. No GPU work or producer changes in diagnostic. Logs build-c9b-review2.log,build-worker-wakeup.log,worker-wakeup.log under local/phase3-integration.

Review accepts pre-submit acquisition, operation-time scope checks, typeless depth gate and singleton guard in inspected paths. Worker bounded teardown remains unproven; source review plus exact wakeup repro sent to Claude. Full baseline suite running at this entry. No live consumer/deployment; tests and findings do not close Phase3.

## 2026-10-24 — PR35 arrived during final checkpoint; continued integration

Final coordination pull revealed new47b1208/PR35, so continued instead of ending on stale pending status. Integrated notification plus thread-handle teardown fixes, kept Claude producer ownership, and ran full Release/default49/49 tests successfully, zero skips. Mandatory wakeup regression now passes; earlier default exclusion/known-failure statements are superseded at this tree. Logs build-review3.log and ctest-review3-full.log. Sent remaining DuplicateHandle-failure teardown case to Claude; no injected failure or general live qualification claimed. Latest combined CI pending.

## 2026-10-24 — PR36 reviewed and tested

Integrated c15881d after all3 CI jobs passed for8ee89dc. Immortal thread ownership removes handle-duplication fallback; injected creation failure refuses/retries correctly. Full Release and expanded54-test run:53 passed,1 missing-upscaler-runtime skip,0 failed. Includes all49 non-GPU-labelled tests plus real DLSS/device-loss. See docs/C9B-REVIEW.md for evidence and remaining qualifications. Latest combined CI pending; no deployment or consumer enablement.

