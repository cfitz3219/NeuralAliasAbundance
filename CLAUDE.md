# Instructions for Claude (and any coding agent) in this repo

Read AGENTS.md, COLLABORATION.md, HANDOFF.md and docs/RECOVERY-NOTES.md first.

## Never lose work again: commit and push automatically
- GitHub (`cfitz3219/NeuralAliasAbundance`) is the source of truth. Local copies are disposable.
- Work on your own branch: Claude -> `claude/<topic>`, Astra -> `astra/<topic>`. Never push to `main` directly.
- Commit after every completed task (each A-/B- numbered entry) and push immediately. Don't wait to be asked.
- Open a pull request into `main` when a task's build and tests pass. The other agent reviews it.
- Never commit NVIDIA SDK headers/libs/runtime DLLs, game files, captures or `local/` (see .gitignore, COLLABORATION.md rule 5).
