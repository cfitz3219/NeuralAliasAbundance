# Render-only resizing across display modes

Cort assigned fullscreen/borderless repair to Astra on2026-11-04 Pacific. Branch astra/fullscreen-sizing starts at Opus0951118. Opus retains motion, selection and HUD integration. This change extends the existing render-small controller/presenter; it introduces no monitor-mode virtualization, executable addresses, alternative renderer or game installation.

## Corrected faults

The posted render-size request was converted into WM_SIZE with SIZE_MAXIMIZED whenever the physical window was maximized. Those describe different operations. A request to render fewer pixels should not tell the engine to maximize to the monitor. Offline inspection of the installed Sniper5 executable confirms that its WM_SIZE handler takes different paths: the maximized notification follows monitor/display geometry, while the ordinary size notification reads the client extent and updates renderer dimensions. The inspected addresses are evidence only and are absent from runtime code.

Synthetic render-only requests now use SIZE_RESTORED. The real HWND is neither restored nor resized: WM_SIZE is a notification, not an OS window-placement command. Genuine OS maximize/minimize notifications keep their original meaning. The posted-message token, window-thread callback, advertised/pending sizes, input mapping and actual-allocation confirmation remain in use.

The failed-request latch also ignored display-mode changes unless the output dimensions changed. A rejected choice in one mode could permanently suppress the same choice in another. A changed mode could recreate the native scene while the controller continued to regard the old reduced configuration as applied.

The Present path now observes the real swapchain Windowed state, window popup/frame/maximize style and monitor identity. A new display-mode generation clears the old refusal and revalidates the desired render size once, including at identical dimensions. It waits for existing physical resize work, supersedes an old logical request if needed, and requires fresh scene observations. Existing contradiction, timeout and native-DLAA behavior remain. An old timeout completing during the same tick cannot re-block the new display-mode request. Stable modes do not create a repeated resize loop.

The observation reuses the swapchain descriptor already needed for colour-format handling. It adds only window-style/monitor checks on Present, not a per-draw scan or GPU readback. No SetFullscreenState, ChangeDisplaySettings, monitor geometry rewrite or physical-window command was added.

## Automated evidence

The new hidden WS_POPUP|WS_MAXIMIZE regression fails against original0951118: the simulated engine receives SIZE_MAXIMIZED and ignores the render-size request.16existing cases pass; the new case fails. After correction, Quality, Performance and native-size requests all complete with byte-identical physical window bounds/style and IsZoomed still true.

Additional helper tests cover genuine maximize/minimize messages and same-output windowed/borderless/exclusive-state observations. A controller test reasserts an already-applied size after a mode change, requiring three new matching scene observations; UNKNOWN does not count and a contradictory native sample clears partial confirmation. No forced physical exclusive transition occurs in these tests.

The new host_fullscreen_resize_test uses actual DX12 swapchains and proxy buffers behind hidden popup and maximized-borderless HWNDs. It dispatches the posted request through a fixture game WndProc, calls the production hooked ResizeBuffers/GetBuffer path, and checks actual game-buffer dimensions, actual native-buffer dimensions, controller allocation/confirmation and unchanged HWND style/bounds over Quality->Performance->native-size transitions. This test passes on RTX5070Ti. Its upscale callback is never executed: it qualifies resizing/allocation, not DLSS quality or final scene delivery.

Focused helper/controller suites and the new DX12 allocation test pass. Full Release build passes; the expanded full suite is running when this source increment is committed. Final counts will be appended to WORKLOG/COMMS after completion. Local receipts: local/se5-fullscreen-20261104/{before-test,after-tests,dx12-test,full-tests}.log. The initial standalone CPU-only configuration omitted the host-hooking-dependent test target; it was replaced with the root configuration, not counted as a test pass. A test-only Device.Create typo was corrected to the existing CreateDevice helper before the DX12 run.

## Integration and remaining live qualification

Integrate the focused delta from this branch onto Opus's current I1 branch. It touches native_present size_spoof/size_controller APIs, two narrow overlay integration locations (small_follow_mode and the existing descriptor query in presenter_present), existing sizing fixtures and a new host test. Preserve Opus's newer motion/capture/HUD hunks. Include in the agreed combined package; Astra did not publish a competing installer or change the game.

Cort retains game testing. The code repairs two reproduced shared sizing defects. Actual Sniper fullscreen/borderless acceptance, physical exclusive-mode transitions, another engine that ignores ordinary WM_SIZE entirely, and final DLSS image quality are not established by the hidden-window fixtures. The offline WndProc also has an early application-state branch; its live condition is not established here. Do not call all exclusive-fullscreen engines universally qualified or substitute allocation success for an actual scene-size/delivery result. The existing controller will continue to report an ignored request if an engine uses another resolution-setting interface.

Microsoft references: [WM_SIZE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-size), [ResizeBuffers](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers), [DXGI best practices](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/dxgi-best-practices). Physical output dimensions and game render-buffer dimensions remain separate, and the game owns its resource recreation in response to the notification.

## ResizeTarget completion and notification ordering

First increment3b85e16 completed the full197-case Release suite:192PASS,5SKIP,0FAIL. Skips: NR runtime probe, three unavailable16xMSAA cases and optional upscaler-jitter test. All existing real-NGX tests passed. That receipt covers3b85e16 before the following addition.

Some engines call ResizeTarget while rebuilding. Microsoft documents that [ResizeTarget](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizetarget) changes window size in windowed mode and display mode in fullscreen. Forwarding our logical size there could change the physical target.

Only render-small now installs a ResizeTarget hook. It rewrites Width/Height to physical output only for a registered game caller on the attached HWND, with a delivered render-only request whose dimensions exactly match. Other calls, natural window resizes, non-size fields (refresh, format, scanline ordering and scaling) and original HRESULTs remain unchanged. The descriptor query has a short NativeScope; the original ResizeTarget call does not, since it may call back into the game. No new physical resize or mode command is initiated by NeuralAlias.

A redundant real WM_SIZE reporting the same physical output could cancel a pending logical quality change: BeginWindowResize used the old committed scale to replace the requested size. The helper now retains the logical request when physical output did not change. A real different-size output still supersedes it. New regression fails before this correction (20PASS/1FAIL) and passes afterward.

The actual-DX12 fixture now calls ResizeTarget through a separately registered engine-like DLL, then production ResizeBuffers/GetBuffer. It processes natural WM_SIZE through BeginWindowResize before logical rewriting, mirroring the host ordering. Popup and hidden maximized-borderless transitions preserve physical HWND/native-buffer dimensions and allocate requested reduced game proxies. Focused helper/controller/DX12 tests3/3PASS after rebuilding; final full results follow separately.

First extended host-test run failed before main with0xc0000135 because its foreign-caller DLL was not copied beside the host executable. Cort saw the Windows loader error. CMake now copies it to the host test directory. This was a corrected test-layout failure, not a runtime/game deadlock. No installation or game testing occurred.

## Final automated receipt
Runtime6310cce (3b85e16 plus ResizeTarget/notification completion) passes full Release build and197-case suite:192PASS/5SKIP/0FAIL,276.44s. Same five skips listed above; existing real-NGX tests pass. Full results local/se5-fullscreen-20261104/complete-tests.log, build complete-build.log. No further source changes followed the test build. Main merge8290934 changes coordination, journal and preexisting design/reference artifacts only; src/tests/CMake are identical to6310cce.

PR91: https://github.com/cfitz3219/NeuralAliasAbundance/pull/91 . Focused runtime commits are3b85e16 and6310cce, not all stacked branch ancestry. Read-only merge-tree check against Opus c6d0f52 succeeds, including overlay.cpp; this is merge compatibility, not testing a combined binary. Main journal append conflict was resolved retaining both histories. Include these two runtime commits in the agreed combined package. No independent installer was published, no deployment/game control occurred, and exclusive-mode/live picture quality remain Cort's qualification.
