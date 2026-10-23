# Instructions for Claude (and any coding agent) in this repo

**Start here: MASTER-PLAN.md (what to build, who, in what order) and COMMS.md (the Claude ↔ Astra message board).**
Then AGENTS.md, COLLABORATION.md, HANDOFF.md and docs/RECOVERY-NOTES.md.

## Never lose work again: commit and push automatically
- GitHub (`cfitz3219/NeuralAliasAbundance`) is the source of truth. Local copies are disposable.
- Work on your own branch: Claude -> `claude/<task-id>-<topic>`, Astra -> `astra/<task-id>-<topic>`. Never push code to `main` directly.
- Exception: the coordination files MASTER-PLAN.md, COMMS.md, COLLABORATION.md, HANDOFF.md are edited directly on `main` (pull --rebase first, small commits, never force-push).
- Commit after every completed task (each A-/B- numbered entry) and push immediately. Don't wait to be asked.
- Open a pull request into `main` when a task is done. Current mode (Cort): build now; the other agent reviews PRs at each phase's review gate, before testing.
- Never commit NVIDIA SDK headers/libs/runtime DLLs, game files, captures or `local/` (see .gitignore, COLLABORATION.md rule 5).
