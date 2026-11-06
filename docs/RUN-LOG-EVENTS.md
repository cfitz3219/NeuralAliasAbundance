# Run log: event lines and full-data blocks

Cort 2026-11-06: every run's log must gather all data we may need, including what the engine exposes.
The 5 s status samples stay. These additions sit next to them in the same per-process status log.

## Event lines (written as they happen)

`event t=<ms since boot> frame=<present number> thread=<id> src=<panel|code> <category> ...`

| category | when | fields |
|---|---|---|
| `dlss` | DLSS/DLAA delivered changes (every on/off edge) | delivered, why (delivered/no_camera/no_depth_state/no_candidate/other), inline, rejects, reject text, later_scene_write |
| `route` | route, state or mode changes | new and previous route/state/mode, reason, change, missing |
| `hud_auto` | HUD method, verification or epoch changes | method, verified, epoch, trial, by (frame/receipt), per-method frames/receipts/passes/failures/pixels |
| `mip`, `af` | a new texture-detail / filtering level is asked for | level, bias, asked value, previous |
| `mip_af` | the asked level becomes live | bias, af, previous, took_ms, objects done/total, static samplers changed |
| `setting` | any persisted setting changes | name=new was=old; src=panel when changed while the panel draws |
| `settings_all` | once at start | every persisted setting (XeGTAO, bloom, DLSS, NR, FG, ...) |
| `swapchain` | every swap chain change | size, format, buffers, swap effect, flags, windowed, client size, refresh |
| `identity` | first frame | API, exe, pid, ms from process start to first frame, adapter, vendor/device id, VRAM |
| `xegtao`, `bloom`, `gpu_services` | reason text or error count changes | reason / errors |
| `camera_cut` | the camera history is reset | why (no_history/gap/translation/rotation/fov/origin/restart_*), value, held, from_j1; `fov` = a zoom such as a scope |
| `virtual_screen` | the screen the game is told about changes (scale, monitor or window origin); every game move/resize of its own window; every ResizeTarget mapped back | game and physical size, scale, monitor real->virtual, origin, primary, device; `window_pos game=... physical=... clamped=`; `resize_target game= physical=` |
| `swapchain set_fullscreen` | the game (or anyone) switches DXGI fullscreen | want, target, hr, state after, caller game/other, advertised size |
| `device_removed` | device removal detected | hr (the DRED file also starts with time and frame) |

## Periodic blocks

- `event_log frame= total= dropped=` every 5 s (dropped > 0 means the queue overflowed).
- `effects xegtao{...} bloom{...} gpu_services{...}` every 5 s.
- `sampler_table` and `sampler_row` lines, at most once a minute and only when the game created new samplers. Each distinct sampler is listed (heap, heap2, root-signature static) with filter, anisotropy, address modes, bias, LOD range, comparison and count.
- `virtual_screen on= scale= monitor=WxH->WxH primary= rewrites{monitor display_modes metrics outputs output_modes window_moves clamped display_change resize_target}` every 5 s in render-small: how many of the game's screen-size answers were scaled (one mechanism for windowed, borderless and fullscreen).
- `size_probe` every 5 s in render-small (and with `[present] size_probe=1`): every window/monitor/display/DXGI-output size call the game made, its callers and the last value it was given.
- `driver_texture` when it changes: read-only. For both the app and the global profile it lists value@location for selector, level, LOD bias (auto/manual), negative LOD clamp, the three filtering optimisations and quality.

Not yet captured: which samplers are bound most by draws (needs per-draw descriptor tracking).
