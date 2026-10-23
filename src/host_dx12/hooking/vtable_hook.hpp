// Shared hooking core for the DX12 observation host and the SL/NGX
// interception paths (OBSERVATION-API.md). Wraps MinHook with:
//  - one-time library init/deinit with refcounting,
//  - vtable-slot hooking for COM objects: the hook targets the concrete
//    function found in an object's vtable, so every object sharing that
//    vtable (i.e. same implementation) is observed at once,
//  - idempotent per-function registration (hooking the same target twice
//    returns the existing trampoline instead of failing),
//  - ordered removal that restores original bytes before MinHook shutdown,
//  - A45: an installation TRANSACTION (prepare all, publish every original,
//    then enable in one step) so a detour can never run before its original
//    pointer is visible; a per-thread detour depth so an observer can tell an
//    outermost call from a forwarded inner one; OriginalFor() lookup.
//
// Threading (A45 restatement, proposed to Route B in COLLABORATION.md B-005;
// A46 adds the owner/partial-failure rules below):
//  - Patching is safe against concurrent callers: MinHook suspends the other
//    threads while writing and relocates instruction pointers that sit inside
//    the patched bytes. What was NOT safe before A45 was publication: a hook
//    was enabled before its trampoline was written to the caller's variable.
//    Every install path now writes the trampoline first (HookFunction /
//    HookVTableSlot) or before the single enable step (PrepareHook +
//    CommitInstall), and a failed commit leaves the originals null and the
//    targets unpatched.
//  - Detours that read state published by the installer (module records,
//    slot tables) must be able to see that state before enable: prepare the
//    hooks, publish the state, then commit.
//  - Never install or remove under the Windows loader lock, never from inside
//    a detour or a render callback (installation suspends threads and takes
//    the install mutex): queue the work to an install/watch thread.
//  - Removal (Unhook/Shutdown) requires quiescence: no thread may be inside
//    the detour or its trampoline. In a live game hooks therefore persist for
//    the process lifetime; observers stop by flag.
#pragma once

#include <cstdint>

namespace na::hooking {

// Begin/end a hooking session. Reference-counted; every Initialize must be
// paired with a Shutdown. Returns false on MinHook failure.
bool Initialize();
void Shutdown();

// Hooks `slot_index` of `object`'s vtable (0-based). On success writes the
// trampoline (original function) to *out_original BEFORE enabling the hook.
// If that concrete function is already hooked, returns the existing
// trampoline and does not double-patch. `object` is a COM interface pointer;
// only its vtable is read, no reference is kept.
bool HookVTableSlot(void* object, uint32_t slot_index, void* detour,
                    void** out_original);

// Hooks a free function (e.g. a resolved export). Same idempotence rule and
// the same publish-before-enable order.
bool HookFunction(void* target, void* detour, void** out_original);

// ---- A45 installation transaction ----
// BeginInstall opens a transaction (one at a time, per process). PrepareHook
// creates the hook DISABLED and writes its trampoline to *out_original at
// once; an already-hooked target returns its existing trampoline and is not
// part of the transaction. CommitInstall enables every prepared hook in one
// MinHook apply; if that fails, every prepared hook is removed and its
// *out_original reset to null, so callers never see a half-installed set.
// AbandonInstall drops the prepared hooks the same way without enabling.
// A46 (per A45-B005-B006-REVIEW): the transaction has ONE owner, the thread
// that called BeginInstall; Prepare/Commit/Abandon from any other thread
// are refused, and Unhook is refused while a transaction is open (Shutdown
// from a non-owner thread is refused too). Every publication of a pending
// target's original (a second PrepareHook of the same target into another
// variable) is tracked, so Abandon nulls all of them. A commit is a BATCH,
// not an all-or-nothing guarantee: hooks are enabled one by one; on a
// failure the never-enabled ones are removed and nulled (nothing can be
// inside them), the already-enabled ones are disabled again but their
// records and trampolines are KEPT (a detour may be mid-flight with its
// original in hand), marked dead and counted in DeadHookCount(); the caller
// gets false and LastError(). Dead targets cannot be re-hooked in this
// session.
bool BeginInstall();
bool PrepareHook(void* target, void* detour, void** out_original);
bool PrepareVTableSlot(void* object, uint32_t slot_index, void* detour, void** out_original);
bool CommitInstall();
void AbandonInstall();
// A47: a synchronised COPY of the most recent failure text (empty when none);
// returns its length. Safe from any thread.
uint32_t LastError(char* out, uint32_t capacity);
uint32_t DeadHookCount();    // hooks retained after a failed enable/disable/removal (dead, never reused)
// A47 cleanup contract: every MinHook disable/remove result is honoured on
// every path (immediate-enable failure, transaction rollback/abandon,
// Unhook, Shutdown). A hook whose cleanup fails keeps its record and
// trampoline (dead, counted), its published originals stay valid, the
// operation returns false/LastError, and Shutdown then keeps the session
// alive (refcount held) instead of uninitialising MinHook under live code.
// Test injection: the `index`-th enable (0-based, in preparation order) of
// the NEXT CommitInstall is treated as failed (-1 clears); the next
// removal / disable is treated as failed and NOT performed.
void FailNextCommitEnableForTest(int index);
void FailNextRemoveForTest();
void FailNextDisableForTest();

// The trampoline for an installed (or prepared) target, or null. Takes the
// install mutex: for setup-time use, not for hot paths (detours keep their
// own original pointers).
void* OriginalFor(void* target);

// Per-thread detour nesting. A detour that wants to distinguish the
// outermost call from a forwarded inner one (a runtime implementation that
// calls a driver implementation we also hook) constructs a DetourScope and
// observes only when `depth == 0`. Forwarding to the original is unaffected.
int DetourDepth();
struct DetourScope {
    int depth; // nesting depth on entry: 0 = outermost
    DetourScope();
    ~DetourScope();
    DetourScope(const DetourScope&) = delete;
    DetourScope& operator=(const DetourScope&) = delete;
};

// Removes one hook by its original target function pointer. Safe to call for
// a target that was never hooked (returns false). Requires quiescence.
bool Unhook(void* target_function);

// Number of currently installed hooks (diagnostics/tests).
uint32_t InstalledCount();

} // namespace na::hooking
