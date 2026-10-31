#include "host_dx12/hooking/vtable_hook.hpp"

#include <windows.h>

#include <MinHook.h>

#include <map>
#include <mutex>
#include <vector>

namespace na::hooking {
namespace {

std::mutex g_mutex;
int g_init_refs = 0;

struct HookRecord {
    void* detour = nullptr;
    void* trampoline = nullptr;
    bool enabled = false;
    bool dead = false; // A46: was enabled once, could not be cleanly removed; unpatched, trampoline kept
};
// Keyed by the concrete target function. Guarded by g_mutex.
std::map<void*, HookRecord> g_hooks;

// Transaction state (guarded by g_mutex). A46: one owner thread; every
// original-pointer publication of a pending target is tracked so a
// cancellation can null all of them.
struct Pending {
    void* target;
    std::vector<void**> outs;
};
bool g_in_transaction = false;
DWORD g_transaction_owner = 0;
std::vector<Pending> g_pending;
int g_fail_enable_at_for_test = -1; // A46 test injection: the Nth enable in a commit "fails"
bool g_fail_next_remove_for_test = false;  // A47: the next MinHook removal is treated as failed (not performed)
bool g_fail_next_disable_for_test = false; // A47: the next MinHook disable is treated as failed (not performed)
char g_last_error[160] = "";
uint32_t g_dead_count = 0;

thread_local int t_detour_depth = 0;

void set_error(const char* text)
{
    lstrcpynA(g_last_error, text, sizeof(g_last_error));
}

// A47: MinHook disable/remove with their results honoured (and test
// injection). A failure leaves the record owned and marks it dead: the
// target may still be patched and the trampoline must stay callable.
bool disable_checked(void* target)
{
    if (g_fail_next_disable_for_test) { g_fail_next_disable_for_test = false; return false; }
    return MH_DisableHook(target) == MH_OK;
}
bool remove_checked(void* target)
{
    if (g_fail_next_remove_for_test) { g_fail_next_remove_for_test = false; return false; }
    MH_STATUS st = MH_RemoveHook(target);
    return st == MH_OK || st == MH_ERROR_NOT_CREATED;
}
// Marks a record dead after a failed cleanup: kept, never reused, counted.
void retain_dead_locked(void* target, const char* why)
{
    auto it = g_hooks.find(target);
    if (it == g_hooks.end()) return;
    if (!it->second.dead) ++g_dead_count;
    it->second.dead = true;
    set_error(why);
}

Pending* find_pending(void* target)
{
    for (Pending& p : g_pending)
        if (p.target == target) return &p;
    return nullptr;
}

// Creates the hook and publishes the trampoline. Enables it only when
// `enable` is set; otherwise records it as pending for CommitInstall.
bool hook_target_locked(void* target, void* detour, void** out_original, bool enable)
{
    if (!target || !detour || !out_original) return false;
    auto it = g_hooks.find(target);
    if (it != g_hooks.end()) {
        // Idempotent: same target may be reached through many objects sharing
        // one vtable. A second registration with a different detour is a
        // programming error we refuse rather than silently chain.
        if (it->second.detour != detour || it->second.dead) { set_error("target already owned by another detour or retired hook"); return false; }
        *out_original = it->second.trampoline;
        if (!enable) {
            // A46 (A45 review finding 1): a pending target published to a
            // second variable is tracked too, so Abandon nulls both.
            if (Pending* p = find_pending(target)) p->outs.push_back(out_original);
        }
        return true;
    }
    void* trampoline = nullptr;
    if (MH_CreateHook(target, detour, &trampoline) != MH_OK) {
        set_error("MH_CreateHook failed");
        return false;
    }
    // Publish BEFORE enabling (A45): a caller that reaches the detour the
    // instant the patch lands must find its original pointer already set.
    *out_original = trampoline;
    g_hooks[target] = HookRecord{detour, trampoline, false, false};
    if (enable) {
        if (MH_EnableHook(target) != MH_OK) {
            set_error("MH_EnableHook failed");
            // A47: the failed hook is removed only if MinHook confirms it;
            // otherwise the record is retained dead and the original stays valid.
            if (remove_checked(target)) {
                *out_original = nullptr;
                g_hooks.erase(target);
            } else {
                retain_dead_locked(target, "MH_EnableHook failed and the removal failed too; record retained");
            }
            return false;
        }
        g_hooks[target].enabled = true;
    } else {
        g_pending.push_back({target, {out_original}});
    }
    return true;
}

// Removes never-enabled pending hooks (safe: no thread can be inside them)
// and nulls every published original. A47: a removal MinHook refuses keeps
// its record (dead) and its published originals, and is reported.
bool drop_never_enabled_locked()
{
    bool ok = true;
    for (Pending& p : g_pending) {
        auto it = g_hooks.find(p.target);
        if (it != g_hooks.end() && !it->second.enabled && !it->second.dead) {
            if (remove_checked(p.target)) {
                g_hooks.erase(it);
                for (void** o : p.outs) if (o) *o = nullptr;
            } else {
                retain_dead_locked(p.target, "removal of a never-enabled hook failed; record retained");
                ok = false;
            }
        }
    }
    return ok;
}

} // namespace

bool Initialize()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init_refs == 0) {
        if (MH_Initialize() != MH_OK) return false;
    }
    ++g_init_refs;
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init_refs == 0) return;
    if (g_in_transaction) {
        // A46: only the transaction owner may tear down; another thread's
        // Shutdown during a transaction is refused (refcount untouched).
        if (g_transaction_owner != GetCurrentThreadId()) {
            set_error("Shutdown refused: another thread owns an open transaction");
            return;
        }
        drop_never_enabled_locked();
        g_pending.clear();
        g_in_transaction = false;
    }
    if (--g_init_refs == 0) {
        // A47: every disable/remove result is honoured. A hook that cannot be
        // cleanly taken out keeps its record (dead) and MinHook stays
        // initialised so its trampoline remains valid; the session is then
        // NOT torn down (refcount held at 1) and LastError says so.
        bool ok = true;
        for (auto it = g_hooks.begin(); it != g_hooks.end();) {
            void* target = it->first;
            bool disabled = !it->second.enabled || disable_checked(target);
            if (disabled) it->second.enabled = false;
            bool removed = disabled && remove_checked(target);
            if (removed) {
                if (it->second.dead && g_dead_count) --g_dead_count; // A48: a removed dead record no longer counts
                it = g_hooks.erase(it);
            } else {
                retain_dead_locked(target, "Shutdown: a hook could not be disabled/removed; record retained, session kept alive");
                ok = false;
                ++it;
            }
        }
        if (!ok) {
            g_init_refs = 1;
            return;
        }
        g_dead_count = 0;
        if (MH_Uninitialize() != MH_OK) {
            set_error("MH_Uninitialize failed");
            g_init_refs = 1;
        }
    }
}

bool HookVTableSlot(void* object, uint32_t slot_index, void* detour,
                    void** out_original)
{
    if (!object) return false;
    void** vtable = *static_cast<void***>(object);
    if (!vtable) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init_refs == 0 || g_in_transaction) return false;
    return hook_target_locked(vtable[slot_index], detour, out_original, true);
}

bool HookFunction(void* target, void* detour, void** out_original)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init_refs == 0 || g_in_transaction) return false;
    return hook_target_locked(target, detour, out_original, true);
}

bool BeginInstall()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init_refs == 0 || g_in_transaction) return false;
    g_in_transaction = true;
    g_transaction_owner = GetCurrentThreadId();
    g_pending.clear();
    g_last_error[0] = 0;
    return true;
}

bool PrepareHook(void* target, void* detour, void** out_original)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init_refs == 0 || !g_in_transaction || g_transaction_owner != GetCurrentThreadId()) return false;
    return hook_target_locked(target, detour, out_original, false);
}

bool PrepareVTableSlot(void* object, uint32_t slot_index, void* detour, void** out_original)
{
    if (!object) return false;
    void** vtable = *static_cast<void***>(object);
    if (!vtable) return false;
    return PrepareHook(vtable[slot_index], detour, out_original);
}

bool CommitInstall()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_in_transaction || g_transaction_owner != GetCurrentThreadId()) return false;
    // A46 (A45 review finding 2): hooks are enabled one at a time so the
    // exact enabled subset is known. On a failure, hooks that were never
    // enabled are removed and their originals nulled (nothing can be inside
    // them); hooks that WERE enabled are disabled again but their records and
    // trampolines are kept (a detour may still be executing with its
    // original pointer in hand), marked dead and counted; the caller learns
    // the partial outcome through the return value and LastError().
    bool ok = true;
    int index = 0;
    for (Pending& p : g_pending) {
        auto it = g_hooks.find(p.target);
        if (it == g_hooks.end()) { ++index; continue; }
        bool enabled = index != g_fail_enable_at_for_test && MH_EnableHook(p.target) == MH_OK;
        ++index;
        if (enabled) {
            it->second.enabled = true;
            continue;
        }
        ok = false;
        set_error("enable failed during commit; partially installed set rolled back where safe");
        break;
    }
    g_fail_enable_at_for_test = -1;
    if (!ok) {
        for (Pending& p : g_pending) {
            auto it = g_hooks.find(p.target);
            if (it == g_hooks.end()) continue;
            if (it->second.enabled) {
                if (disable_checked(p.target)) {
                    it->second.enabled = false;
                    retain_dead_locked(p.target, "enable failed during commit; partially installed set rolled back where safe");
                } else {
                    retain_dead_locked(p.target, "enable failed during commit AND a rollback disable failed; hook left enabled");
                }
            }
        }
        drop_never_enabled_locked();
    }
    g_pending.clear();
    g_in_transaction = false;
    return ok;
}

void AbandonInstall()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_in_transaction || g_transaction_owner != GetCurrentThreadId()) return;
    drop_never_enabled_locked();
    g_pending.clear();
    g_in_transaction = false;
}

void FailNextCommitEnableForTest(int index)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fail_enable_at_for_test = index;
}
void FailNextRemoveForTest()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fail_next_remove_for_test = true;
}
void FailNextDisableForTest()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fail_next_disable_for_test = true;
}

uint32_t LastError(char* out, uint32_t capacity)
{
    std::lock_guard<std::mutex> lock(g_mutex); // A47: synchronised copy
    uint32_t n = static_cast<uint32_t>(lstrlenA(g_last_error));
    if (out && capacity) lstrcpynA(out, g_last_error, static_cast<int>(capacity));
    return n;
}

uint32_t DeadHookCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_dead_count;
}

void* OriginalFor(void* target)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_hooks.find(target);
    return it == g_hooks.end() ? nullptr : it->second.trampoline;
}

int DetourDepth() { return t_detour_depth; }
DetourScope::DetourScope() : depth(t_detour_depth++) {}
DetourScope::~DetourScope() { --t_detour_depth; }

bool Unhook(void* target_function)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_in_transaction) return false; // A46: no mutation of an open transaction's world
    auto it = g_hooks.find(target_function);
    if (it == g_hooks.end() || it->second.dead) return false;
    // A47: results honoured; on failure the record is retained (dead) and
    // the caller's original pointer stays valid.
    if (it->second.enabled && !disable_checked(target_function)) {
        retain_dead_locked(target_function, "Unhook: disable failed; hook left enabled, record retained");
        return false;
    }
    it->second.enabled = false;
    if (!remove_checked(target_function)) {
        retain_dead_locked(target_function, "Unhook: removal failed after disable; record retained");
        return false;
    }
    g_hooks.erase(it);
    return true;
}

uint32_t InstalledCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<uint32_t>(g_hooks.size());
}

} // namespace na::hooking
