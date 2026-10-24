# A8 layout cache — persistence checkpoint

layout_cache.hpp stores bounded versioned layout hints under the executable's
SHA-256 filename. Load also requires an exact current structural-layout SHA-256
and classifier-version match; any mismatch/corruption returns no hints. A profile
contains role/format/extent/binding/offset and shader digest plus an evidence digest.
It never stores process pointers, resource IDs, leases or an effect-enable bit.

Caller contract: calculate executable and structural digests from actual inputs,
record the evidence digest of a confirmed layout, and use hints only to narrow
rediscovery. This module validates digest syntax and equality; it does not compute
hashes, authenticate evidence or prove camera/content semantics. A8 live integration
must obtain stable shader/layout signatures from C8 and revalidate current resource
versions/coverage before use. A matching cache cannot bypass A5/A6/A7 validation.

Writes use a random temporary sibling followed by atomic replacement. Failed writes
remove only their temporary and retain the previous profile. On Windows replacement
uses MoveFileExW with replace-existing/write-through; POSIX uses rename. This avoids
partial-reader visibility; no universal power-loss durability claim is made. I/O
belongs on a control/worker thread, never in game callbacks. Root is a caller-owned
cache directory; serialized data cannot supply file paths. Lowercase64-hex executable
keys prevent path traversal. Maximum record size8192 bytes, at most5 unique roles.

Five CPU tests cover round-trip, bounds/schema/duplicate/truncation rejection,
synthetic second-launch reuse, layout/algorithm/executable invalidation, invalid-save
preservation and corrupt-file handling. Windows Release suite11/11 passed. The
second-launch test models discovery invocation counts with a deterministic confirmed
fixture layout; it is not an instrumented game launch or proof of live cache speed.
A8 remains in progress until wired to actual observed/confirmed layouts.
