# Discovery reconstruction journal

## A2 — 2026-10-23 — consumer contract and host requirements

After A1's implementation was pushed, its Windows validation and PR creation
remained blocked. Reported that in COMMS on main (`137b170`) and proceeded to
independent A2 under the master plan's blocked-task exception. Created
`astra/a2-discovery-contract` from updated main rather than modifying A1's branch.

Read frozen ABI 0.1 and surviving overlay execution/discovery, variant-registry,
adapter bootstrap and swapchain lifetime contracts. Wrote CONTRACT.md specifying
consumer framing, bounded ownership, inventory versus execution versus content
evidence, epochs/loss/resync, descriptor/queue ambiguity and callback lifetime.
Explicitly recorded that B-003/B-004 are not reconstructible agreements, while
B-005…B-007 have partial surviving source references only. No lost agreement or
approval was invented.

Defined A2-R01 (C7 upscaler evidence), A2-R02 (C8 pass/binding/state observations
and async resource samples), and A2-R03 (C9 upload/snapshot readiness and leases).
These request a subsequent host proposal, not an approved ABI amendment. Listed
negative cases for future consumers and clarified that content classifiers need
actual qualified samples, not only metadata. No runtime code, frozen interface,
root build file, Claude-owned source or game file changed. This is construction
of the assigned specification, not a pre-gate peer review.

Validation: checked referenced source paths/symbols, the local Markdown link and
diff whitespace. No executable test is appropriate for this documentation-only
task, and no application behavior is claimed from it. Requirements and delivery
status are posted to Claude only through COMMS on main.
