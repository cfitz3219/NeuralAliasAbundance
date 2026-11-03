# Render-small size and input integration (S1/S2)

Owner: Astra. Presenter/J2/overlay integration: Opus. The projection-window route
is parked. These helpers do not alter cameras, render targets, user settings or
desktop modes. They give the game a coherent client extent and map input to it.

## Integration points

1. On the boot worker, with `[present] render_small=1`, call
   `na::native_present::InstallSizeHooks()` before game swap-chain creation.
   It installs only GetClientRect/GetWindowRect through the existing host hook
   transaction. No competing cursor/raw-input hooks are installed. Helpers are
   transparent until attached. `Detach(hwnd)` disables them for a dying chain.
2. In the game swap-chain create hook, determine the **physical** client/output
   size under `NativeScope`. Compute render size with the presenter's same
   rounding rule. `Attach(hwnd, outputSize, renderSize)` establishes the one
   game window. The ORIGINAL native swap-chain creation must receive native
   dimensions and run under `NativeScope`; do not pass a small requested game
   size into the real native chain and then multiply by scale a second time.
   Zero create/resize dimensions are resolved in that native scope as well.
3. In game-facing GetDesc/GetDesc1 detours, filter the original return address
   through `GameCaller`, then call `RewriteDesc` / `RewriteDesc1`. The latter
   needs the HWND, obtained through the ORIGINAL chain or its recorded identity.
   The presenter reads real descriptors: exclude our own module and use
   `NativeScope` for nested runtime calls. `GetFullscreenDesc` has no width or
   height fields. No display-mode or monitor lies are needed for these helpers.
4. On initial proxy creation and a natural resize, use
   `CommitBuffers(hwnd, output, actualProxySize)` to keep advertised dimensions
   and input tied to the textures actually returned. It does not service a live
   pending request; use its explicit completion below. If allocation decides
   against scaling, publish actual native dimensions rather than a small lie.
5. For a live change, first set the presenter's wanted scale, then
   `RequestRenderSize(newSize)`. This posts a registered message and returns a
   request token. It does not physically resize the user's window. Requests are
   serialized; 0 means unchanged, already pending, detached or post failure.
6. At the END of our WndProc's own input processing, immediately before calling
   the GAME's original WndProc, call `PrepareSizeMessage(hwnd,msg,wp,lp,scratch)`.
   Use local writable copies of msg/wp/lp and a local WINDOWPOS scratch that
   remains alive across that call. Consume stale registered messages. A current
   registered message becomes WM_SIZE with the requested render extent. Only
   then do game geometry queries advertise the requested size. WM_SIZE
   minimization is untouched. WM_WINDOWPOSCHANGED has a pointer to a structure,
   so the helper substitutes scratch, never edits Windows-owned memory.
7. When the game's resize succeeds AND new proxies have actually been allocated,
   call `CompleteResize(token,true,actualProxySize)`. Do not acknowledge just
   because the native chain ResizeBuffers returned S_OK: proxies are allocated
   lazily by GetBuffer. Matching successful completion advances input geometry.
   A failed resize calls `CompleteResize(token,false,{})` and restores the old
   advertised extent. Late/wrong tokens cannot commit. The normal tick calls
   `ExpireResize()` so a game ignoring WM_SIZE cannot strand a pending request.
   Old input mapping stays active throughout the pending period. This changes
   no route and introduces no old-picture repeats.
8. Add `FormatSizeStatus` and `FormatInputStatus` to the existing worker log.
   Those functions format only: no disk I/O or readbacks occur in hooks. These
   counters prove which implemented surfaces a game queried; static EXE imports
   alone do not establish runtime use or successful scene target rebuilding.

Do not invoke game callbacks while holding presenter/helper/overlay locks. The
helpers release their mutex before posting messages and do not call a game
WndProc themselves. Do not run the old A63 physical-window nudge in this mode:
its game-visible window rectangle is intentionally virtual.

## Input: a single coherent screen space

The real client origin stays fixed in screen coordinates. The game's virtual
screen space has that same origin but uses render-size units inside the client.

- After a successful ORIGINAL GetCursorPos, `MapCursorRead(p, caller)` converts
  to virtual screen. Before ORIGINAL SetCursorPos, `MapCursorWrite(x,y,caller)`
  converts back. Pass the GAME's original return address from the detour, not
  `_ReturnAddress()` from inside a nested helper.
- `ScreenToClient` / `ClientToScreen` continue to add/subtract the same origin.
  **Do not scale their result again.** Polling and message-based input then
  agree, as do ClientToScreen -> SetCursorPos and GetCursorPos -> ScreenToClient.
- ClipCursor uses a scratch physical RECT from `MapClipRect`. Null remains null
  (unclip). After ORIGINAL GetClipCursor, `MapClipRead` returns virtual bounds,
  so a read/restore pair does not double-scale the clip.
- `MapGameMouseMessage` runs only on the messages forwarded to the game, after
  the overlay recorded or swallowed its real messages. It distinguishes client
  mouse events from screen-coordinate wheel/pointer messages. It preserves
  signed negative monitor positions. Keyboard/non-coordinate messages remain
  untouched. POINTER_INFO query structs, if a game later uses them, need their
  screen-coordinate fields converted consistently; that API is not hooked here.
- In the existing GetRawInputData/GetRawInputBuffer mouse-record path, invoke
  `MapRawMouseRead(record.data.mouse, gameCaller)`. It maps only ABSOLUTE devices
  using the real primary/virtual desktop selected by RAWMOUSE flags. Relative
  deltas, button fields and wheel data are unchanged. Preserve the existing
  correct buffer traversal/header/size handling. No mapping for query-only reads.
- Keep our overlay's polling/rendering in `NativeScope`, and continue the
  existing overlay-open input suppression. Exempting our DLL's direct callers
  is automatic; the scope also covers nested DXGI/user32 calls from our work.

## Build and evidence

`src/host_dx12/native_present/CMakeLists.txt` defines `host_native_present`.
Opus's root integration adds that directory after `host_hooking` and links the
target to the host. CPU test registration also creates the target if needed, so
the helpers can be built/tested before root integration without editing his files.

`na_cpu_native_present_size` exercises the actual user32 detours on hidden test
windows: native/game exemptions, outer borders, descriptors, pending/committed
resize identity, failure/retry/expiry, scratch message storage, negative monitor
origin, polling vs messages, clipping, absolute vs relative input and concurrent
snapshots. No mouse movement, foreground changes or game process is involved.

Initial result: **12 cases pass**, Release `/W4 /WX`. This proves helper behavior,
not that Anno or SE5 has accepted the smaller output. The first actual game run
belongs to Cort, after Opus integrates C1-C4 and these helpers.

Primary coordinate/resize contracts:

- https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-windowposchanged
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowrect
- https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mousewheel
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers

No game installation, CMD or native file has been changed for this increment.
