# Onboarding for a fresh Claude (Opus 5.5) instance

You are picking up Cort's **NeuralAliasAbundance** project mid-stream. Read this
top to bottom, then read the files it points you to. Do not start editing before
you have read the coordination files — this is a two-agent project and work is
coordinated, not improvised.

## 1. Connect to GitHub first

- Source of truth: **`cfitz3219/NeuralAliasAbundance`** (GitHub). Local copies in
  the session container are disposable.
- This repo may not be attached to a fresh session. Attach it with the
  `add_repo` tool (owner `cfitz3219`, repo `NeuralAliasAbundance`), run the clone
  command it returns, then call `register_repo_root` so CLAUDE.md, skills and
  plugins load. Do **not** pre-check the repo with curl/`gh` first — a private
  repo returns 404 unauthenticated and will mislead you.
- Cort's identity for attribution: `cortfitzer@gmail.com`.
- Commit trailer to end every commit message with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: <your own session URL>
  ```
- PR descriptions end with the Claude Code generated-with line plus your session URL.

## 2. Read these, in this order

1. **`CLAUDE.md`** (repo root) — the rules. Branch model, never push to `main`,
   installer-is-additive-only, file ownership. This overrides your defaults.
2. **`MASTER-PLAN.md`** — what is being built, by whom, in what order.
3. **`COMMS.md`** — the Claude ↔ Astra message board. **Read the last ~15
   entries before doing anything.** This is where the live state is. Astra
   (Codex) is the other agent; coordinate here, don't collide.
4. **`AGENTS.md`, `COLLABORATION.md`, `HANDOFF.md`, `docs/RECOVERY-NOTES.md`** —
   working agreements and recovery history.
5. **`docs/PBZ-LIVE-QUALITY-ASSESSMENT.md`** and **`docs/GHOST-LIVE-QUALITY.md`** —
   the two test games and what "working" means for each.

## 3. What the project is (one paragraph)

A `dxgi.dll` proxy that adds DLSS control, Neural Rendering (NR) and Frame
Generation (FG) features to DX12 games, sitting below Streamline. Two test
games: **Phantom Blade Zero** (PBZ, Unreal Engine) and **Ghost of Tsushima**
(Nixxes, Streamline). Installers are **additive only** — never modify, replace
or delete game files, and never replace a native game DLL.

## 4. Ground rules Cort has set (do not violate)

- Cort owns all game testing. **Never launch, deploy, or install** on his PC.
- Additive installer only; if it failed last time, everything defaults off (safe start).
- Work on your own branch `claude/<task-id>-<topic>`; PR into the production
  integration branch (currently `astra/pbz-rebuild-queues`). Coordination files
  (MASTER-PLAN, COMMS, COLLABORATION, HANDOFF) are edited directly on `main`
  (pull --rebase first).
- Don't apply anything you suspect is wrong.
- File ownership: `overlay/overlay.cpp`, `nr/**` and the `ngx_observe` lifetime
  sections are **Astra's**. Check COMMS for current per-task ownership before
  touching a shared file.
- Preferences: be warm and direct; if you disagree, say it once then move on.

## 5. Current state as of 2026-10-31 ~02:30 PT (read COMMS for the latest)

- Production integration branch: **`astra/pbz-rebuild-queues`**, tip around
  `f46adb7` (Astra's Ghost startup-crash fix + rollback-safety follow-up).
- **Open problem #1 — Ghost overlay regression (Astra owns):** after the startup
  crash fix (`742ff30`/`f46adb7`), Ghost launches but the overlay no longer
  draws. Evidence is in COMMS (the `02:26` and later entries): in the game
  process our real-swapchain Present detour never runs (status `fps=0.0`,
  `presenter real/game_presents=0`, no `menu_gpu` line), while Streamline's proxy
  Present advances thousands of frames — so frames reach the screen by a path
  that bypasses the DXGI vtable patch the overlay draws from. Suggested starting
  point is in COMMS. Bisect across the packaged builds `76d9a9f`, `63395c7`,
  `f46adb7` to isolate it fast. Cort reports the overlay worked on **every**
  earlier Ghost build, so this is a regression, not a never-worked.
- **Open problem #2 — PBZ Unreal exact-build restriction (handoff b48ab83):**
  the Unreal adapter admits only the one exact PBZ executable. The task to make
  it recognize the UE console-variable ABI more generally was handed to Opus and
  then returned unfinished — see COMMS. Reassign per Cort/Astra.
- Launchers/packages on Cort's PC live in `~/Documents` (`NA-*` scripts and
  `NA-*-package-<commit>` folders). Ghost launchers currently target
  `NA-Tsushima-package-f46adb7`. Never run these yourself.

## 6. How to help without breaking things

- Most useful safe work: read Cort's logs after a test (staged from
  `%LOCALAPPDATA%\NeuralAlias\<Game>\status.log`, `startup.log`,
  `sl_boundary.log`), review Astra's PRs, check CI is green before anything
  reaches his PC, and keep launchers/packages pointed at the tested commit.
- Always update COMMS when you finish a task or stop one.
- Diagnostic logs on the PC: `C:\Users\Cort\NeuralAliasAbundance\local\<topic>-<date>`.

---
*Written by Claude Opus 5.5 as a handoff. The live state is always COMMS.md on
`main`; trust it over this file if they disagree.*
