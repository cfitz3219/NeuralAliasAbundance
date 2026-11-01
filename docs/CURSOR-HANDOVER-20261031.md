# Home cursor handover correction

Cort confirms the startup-corrected build works in Ghost, but reports the busy cursor remains and the game cursor is invisible after closing Home.

## Reproduced defects

The prior close path restored only the ShowCursor increments and clipping rectangle, not the game's cursor handle. A blocked game SetCursor request was lost. Separately, ImGui caches its last requested shape: a later competing cursor write can survive indefinitely while ImGui still thinks its arrow is current. The regression against the previous runtime failed both cursor-restoration and competing-busy-cursor assertions. Evidence is local/ownership-stage2-20261031/cursor-before.log, not committed.

ImGui's Win32 backend also calls SetCursor(NULL) when drawing a software cursor. Our Present code runs with an overlay-input exemption, allowing that write on the render thread. No subsequent closed-frame backend call restored it. This is a confirmed code path; the automated reproduction above specifically demonstrated the missing handle restoration and cached shape rather than proving which API call produced Cort's invisible cursor.

## Correction

The game window thread now owns acquiring, refreshing and releasing the menu cursor. Home restores immediately even without another Present; mouse-close requests a window message. Per-frame refresh is coalesced to at most one pending message and reasserts the menu arrow independently of ImGui's cache. Queued messages reconcile current state rather than replaying stale open/close actions. Repeated refreshes never repeat the ShowCursor acquisition.

The latest game cursor request from the window owner is retained while we temporarily show the arrow. Closing restores that handle, including a deliberate NULL for games drawing their own pointer, and balances only our visibility increments. Worker/render-thread cursor requests cannot replace the window owner's restore handle. Closed game calls pass through. ImGui retains software drawing as a visibility fallback, with NoMouseCursorChange preventing its backend from writing OS cursor state. Stop restores ownership before removing the window subclass; an unresponsive window leaves cleanup retryable rather than losing the restoration message.

The periodic status log adds menu_cursor: open/owned/window_thread/pending plus observed global cursor visibility/shape and the saved game shape. Global state is observational, not proof of the game's rendering of a software cursor.

## Verification and limits

The focused cursor regression uses an actual WARP swap chain/window, a distinct Present thread, forced hidden-cursor fallback, competing direct cursor writes bypassing our hook, custom and NULL game cursors, close without Present, rapid toggles with queued refreshes, visibility-count balance, and Stop while open. Existing overlay WARP/hardware, native-boundary, optional-input, failed cleanup, presenter and session tests are run alongside it. These tests do not replace Cort's Ghost check. No game was launched, controlled or deployed by Astra; no native game DLL or NVIDIA profile changes were made.

References: existing vendored third_party/imgui/backends/imgui_impl_win32.cpp, UpdateMouseCursor and its cached LastMouseCursor; Microsoft's [ShowCursor contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showcursor), [cursor ownership and display](https://learn.microsoft.com/en-us/windows/win32/menurc/about-cursors).
