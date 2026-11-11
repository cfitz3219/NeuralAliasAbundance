# Neural controls and audit — v0.11.1

Open ReShade with **Home**, keep **DLAA** checked, and enable **Neural rendering
(experimental)**. Open **Edit global preprocessor definitions** to adjust these
settings. Use **Reload** after editing if ReShade has not reloaded automatically.
Only the neural helper restarts; no game restart is required. The existing DLAA
status panel also reports neural status. Uncheck neural to compare with DLAA alone.

| Definition | Default | Meaning |
|---|---:|---|
| `DX11_NR_INTENSITY` | 1.0 | Overall strength, 0–2. Zero exactly returns input RGB in replay tests, but still costs processing time. Uncheck neural to remove that cost. |
| `DX11_NR_STRUCTURE` | 1.0 | Fine detail enhancement, 0–2. Stronger is not necessarily better. |
| `DX11_NR_TONE` | 0.0 | Broader lighting/color adjustment, 0–2. Zero preserves original tone. Try 0.25 for a modest comparison; this is not a proven best setting. |
| `DX11_NR_SKIN` | 1.0 | Skin detail, -1–2. -1 follows structure; zero reduces that contribution. |
| `DX11_NR_AUTO_SKIN` | 1 | Automatic skin detection: 0 off, 1 on. Does NOT protect HUD text. |
| `DX11_NR_STYLE` | 0 | Profiles 0/1/2, all producing different replay results. Community names standard/natural/cinematic are not verified NVIDIA names. |
| `DX11_NR_MODEL` | 0 | Runtime choice 0, requested hints 1/2/3. All four produced identical output here. Actual different model selection is unverified; keep 0. |

Change one setting at a time in the same scene. Defaults reproduce the earlier
picture. Restore the table values to undo tuning. ReShade saves your selections.
The separate `DX11_DLAA_PRESET` remains Auto 0, J 10, K 11, L 12, M 13; these are
reconstruction presets, not neural styles or neural model letters.

NVIDIA describes DLSS 5 as generating enhanced final appearance from rendered
color, engine motion and temporal state, with artistic controls. It complements
the renderer; it does not turn this game into a newly ray-traced renderer.
Sources: [NVIDIA research](https://research.nvidia.com/labs/adlr/DLSS5/) and
[NVIDIA artistic controls](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/).

The audit added creation-time settings, validation, session settings logs and
helper recreation on changes. Eight automated tests pass. Actual ReShade tests
verify all seven settings and hot reload. Two-frame model replay confirms style,
intensity, structure, tone, skin and automatic skin masking alter output. All
variant outputs are finite and preserve alpha. Model hints remain unverified
after testing both signed and unsigned parameter types. This is not a gameplay
benchmark or proof of long-term temporal quality.

The current final SDR stage includes HUD/text. There is no separate UI texture or
mask. The unused UI-correction hint was removed; automatic skin masking serves a
different purpose. HUD exclusion, material-specific masks and reliable camera-cut
detection need further game integration. Final color versus earlier motion/depth
alignment also needs live testing. A quality setting cannot guarantee those issues
are solved. No live v0.11.1 gameplay result has been reported yet.

Start at defaults, compare tone 0 versus 0.25 and styles 0/1/2, and watch moving
details, text, dark scenes and FPS. Maximum values are not a verified best preset.
This remains an unofficial, experimental integration, not a release-ready mod.

Interface facts were checked against pinned primary community source:
[OptiScaler contract](https://github.com/Dagherbou/OptiScaler_DLSSNR/blob/973761621353b99bee3dc7d4bb27b117fef2644f/OptiScaler/shaders/dlssnr/DlssNr_Common.h),
[OptiScaler controls](https://github.com/Dagherbou/OptiScaler_DLSSNR/blob/973761621353b99bee3dc7d4bb27b117fef2644f/OptiScaler/dlssnr/DlssNr_Menu.cpp),
and [AIO integration](https://github.com/kibblerz/DLSS5-Reshade-AIO/tree/09301f5528e619e8b9ec17c257d167e2985f53b0).
No third-party implementation was copied. These are community interface findings,
not an official NVIDIA SDK guarantee for this DLL.

Evidence: `docs/evidence/2026-09-21-neural-control-audit.json`,
`2026-09-21-neural-controls-smoke.log`, `2026-09-21-v0.11.1-tests.log`.
Private session `settings.txt` records exact parameters beside `runtime.txt`.
See WORKLOG entries 127–128 for actions, failures, deployment and limitations.
