# C8 capture correction journal

## 2026-10-24 — user-authorized fix takeover

Cort authorized Astra to fix both reported capture defects and coordinate with Claude. Posted ownership in main COMMS before editing, based on Claude1154c0c (latest trace instrumentation). No duplicated fixes were present on that head.

The unobserved4096-byte buffer now uses explicit R32_UINT/BUFFER SRV with1024 elements and default component mapping. The resource remains unobserved, preserving the original unknown-resource assertion. Test checks device health immediately after the view call. A local RAII session guard stops/drains observation on REQUIRE returns, avoiding the joinable-worker termination that masked the original failure.

Producer snapshot queries coherent host status and puts its nonzero session into the capability report. Invalid/missing session returns snapshot failure instead of emitting an unscoped report. No wire layout changed. Regression checks capability session against enclosing batch on the worker, using an atomic error count read after stop. The test now resyncs and requires two reports with matching session and increasing epoch.

Combined df18a4e full MSVC Release build passes;22/22 selected CTests pass:21 discovery CPU/fixture/menu tests plus committed host_observe_producers_test on RTX5070Ti. Logs local/phase3-integration/build-capture-fixes.log and ctest-capture-fixes.log. This closes the two reproduced defects locally, not the remaining shared-target/lifetime/order review concerns or Phase3 live signal qualification. No game deployment.
