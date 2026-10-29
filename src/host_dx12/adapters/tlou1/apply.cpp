#include "host_dx12/adapters/tlou1/apply.hpp"
#include "host_dx12/hooking/vtable_hook.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/settings.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace na::tlou1 {
namespace {
constexpr uint32_t kFrame = 0x11c4ecc, kPredicate = 0x1f27be8, kCaller = 0x11ca0ce;
constexpr uint32_t kRebuild = 0x1f29350, kRebuildCaller = 0x11ca0fc;
constexpr size_t kDirty = 0x15a91;
constexpr char kHash[] = "088d37aff3b8e3f65715e7e325547c07e6c4d794966c0573cd0f7af5c6c3503a";
using FrameFn = uintptr_t (*)(void*);
using PredicateFn = bool (*)(void*);
using FreeResourcesFn = bool (__cdecl*)(uint32_t, uint32_t);
FrameFn g_frame = nullptr;
FrameFn g_rebuild = nullptr;
PredicateFn g_predicate = nullptr;
uintptr_t g_caller = 0;
uintptr_t g_rebuild_caller = 0;
FreeResourcesFn g_free_resources = nullptr;
size_t g_dirty = kDirty;
std::mutex g_mutex;
std::once_flag g_init;
ApplyRequest g_request;
bool g_available = false;
bool g_dispatch_active = false; // mutex protected; even cancelled invocations must return
char g_reason[160] = "This game has no automatic picture-mode adapter.";
std::atomic<bool> g_queued{false};
std::atomic<uint64_t> g_frame_calls{0}, g_predicate_calls{0}, g_triggers{0};
std::atomic<uint64_t> g_resource_releases{0}, g_resource_release_failures{0};
uint32_t g_trigger_thread = 0;
struct Invocation { void* context; uint64_t request; bool triggered = false, released = false; };
thread_local Invocation* t_invocation = nullptr;

Choice current_choice() {
    auto& st = na::settings::Get();
    auto opt = na::ngx::GetOptimalStats();
    Choice c{st.dlss_mode.load(), opt.last_user_w, opt.last_user_h, st.preset.load()};
    if (c.mode && c.mode < static_cast<uint32_t>(settings::DlssMode::Count)) {
        float scale = na::settings::ScaleForMode(static_cast<na::settings::DlssMode>(c.mode));
        c.width = (static_cast<uint32_t>(c.width * scale + .5f) + 7u) & ~7u;
        c.height = (static_cast<uint32_t>(c.height * scale + .5f) + 7u) & ~7u;
    }
    return c;
}
void refresh_locked() {
    auto prior = g_request.stage;
    g_request.Tick(GetTickCount64(), na::settings::Get().enabled.load() != 0, current_choice());
    if (g_request.Busy()) {
        auto c = na::ngx::GetSizeConfirmation();
        if (c.id != g_request.id || c.state == 4) g_request.stage = Stage::Failed;
        else g_request.Observe(c.id, c.state);
    }
    if (!g_request.Busy()) g_queued.store(false, std::memory_order_release);
    if (prior != g_request.stage) {
        if (g_request.stage == Stage::Confirmed) strcpy_s(g_reason, "Picture mode applied; the game is drawing at the requested resolution.");
        else if (g_request.stage == Stage::Cancelled) {
            strcpy_s(g_reason, "The selection changed. Choose Apply again.");
            if (na::ngx::GetSizeConfirmation().id == g_request.id) na::ngx::CancelSizeConfirmation();
        } else if (g_request.stage == Stage::Failed) strcpy_s(g_reason, "The game did not confirm the requested picture mode. See Details.");
    }
}

bool predicate_detour(void* context) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    // Always let the engine perform its own comparison and side effects first.
    const bool changed = g_predicate(context);
    g_predicate_calls.fetch_add(1, std::memory_order_relaxed);
    auto* invocation = t_invocation;
    if (!invocation || context != invocation->context || caller != g_caller) return changed;
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh_locked();
    if (!g_request.Trigger(invocation->request, true, true)) return changed;
    invocation->triggered = true;
    g_trigger_thread = GetCurrentThreadId();
    g_triggers.fetch_add(1, std::memory_order_relaxed);
    strcpy_s(g_reason, "The game is rebuilding its picture; checking the following frames.");
    return true; // caller performs its original waits, cleanup and size query
}

uintptr_t rebuild_detour(void* context) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto* invocation = t_invocation;
    if (invocation && invocation->triggered && !invocation->released &&
        context == invocation->context && caller == g_rebuild_caller) {
        // The pinned caller has already waited for previous GPU work. TLOU's
        // own cleanup deliberately skips SL when its cached upscaler type is
        // DLSS (2). SL 1.x only notices mode/output-size changes, not our
        // overridden internal size. Release its viewport through the public
        // API here, so its next Evaluate rebuilds all cached DLSS resources.
        // The pinned game's DLSS instance is 0 (its feature-constant calls).
        const bool released = g_free_resources(0, 0);
        invocation->released = true; // exactly one attempt per invocation
        if (released) g_resource_releases.fetch_add(1, std::memory_order_relaxed);
        else g_resource_release_failures.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (released) na::ngx::MarkSizeConfirmationRestored(invocation->request);
        else if (g_request.id == invocation->request) {
            g_request.stage = Stage::Failed;
            na::ngx::CancelSizeConfirmation();
            strcpy_s(g_reason, "DLSS could not release its previous setup. Restart the game before retrying.");
        }
    }
    return g_rebuild(context);
}

uintptr_t frame_detour(void* context) {
    g_frame_calls.fetch_add(1, std::memory_order_relaxed);
    if (!context || t_invocation || !g_queued.load(std::memory_order_acquire)) return g_frame(context);
    uint64_t id = 0;
    {
        std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock()) return g_frame(context);
        refresh_locked();
        if (!g_dispatch_active && g_request.Enter(GetTickCount64(), na::settings::Get().enabled.load() != 0, current_choice())) {
            id = g_request.id;
            g_dispatch_active = true;
            g_queued.store(false, std::memory_order_release);
        }
    }
    if (!id) return g_frame(context);
    // This byte belongs to the engine-owned frame context, never a persistent
    // settings object. Its verified consumers take the normal rebuild path.
    // 11c9f74 recomputes it before calling the predicate. No quality field is
    // edited; the existing optimal-settings override supplies the new size.
    auto* dirty = static_cast<unsigned char*>(context) + g_dirty;
    const unsigned char before = *dirty;
    *dirty = 1;
    Invocation invocation{context, id};
    t_invocation = &invocation;
    const uintptr_t result = g_frame(context);
    t_invocation = nullptr;
    if (!invocation.triggered) {
        // No verified rebuild branch was visited. Do not leave our marker on
        // this invocation's context. Wait for a later eligible frame using the
        // same request and deadline; once triggered, a rebuild is never retried.
        if (*dirty == 1) *dirty = before;
        std::lock_guard<std::mutex> lock(g_mutex);
        refresh_locked();
        g_request.Leave(id);
        if (g_request.stage == Stage::Queued) {
            g_queued.store(true, std::memory_order_release);
            strcpy_s(g_reason, "Waiting for the game's next graphics-settings update.");
        }
    }
    { std::lock_guard<std::mutex> lock(g_mutex); g_dispatch_active = false; }
    return result;
}

bool hash_file(const wchar_t* path, char (&hex)[65]) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 65536> bytes{};
    DWORD n = 0;
    while (ok) {
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr)) { ok = false; break; }
        if (!n) break;
        ok = BCryptHashData(hash, bytes.data(), n, 0) >= 0;
    }
    std::array<unsigned char, 32> digest{};
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (ok) for (size_t i = 0; i < digest.size(); ++i) sprintf_s(hex + i * 2, 65 - i * 2, "%02x", digest[i]);
    return ok;
}
bool code_matches(uintptr_t base, uint32_t rva, const unsigned char* bytes, size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    auto* p = reinterpret_cast<const void*>(base + rva);
    if (!VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
        !(mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
        (mi.Protect & PAGE_GUARD) || reinterpret_cast<uintptr_t>(mi.AllocationBase) != base) return false;
    if (base + rva + n > reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize) return false;
    return std::memcmp(p, bytes, n) == 0;
}
bool install(uintptr_t frame, uintptr_t predicate, uintptr_t caller, uintptr_t rebuild, uintptr_t rebuild_caller) {
    g_caller = caller;
    g_rebuild_caller = rebuild_caller;
    if (!na::hooking::BeginInstall()) return false;
    bool ok = na::hooking::PrepareHook(reinterpret_cast<void*>(frame), reinterpret_cast<void*>(&frame_detour), reinterpret_cast<void**>(&g_frame)) &&
              na::hooking::PrepareHook(reinterpret_cast<void*>(predicate), reinterpret_cast<void*>(&predicate_detour), reinterpret_cast<void**>(&g_predicate)) &&
              na::hooking::PrepareHook(reinterpret_cast<void*>(rebuild), reinterpret_cast<void*>(&rebuild_detour), reinterpret_cast<void**>(&g_rebuild));
    if (!ok) { na::hooking::AbandonInstall(); return false; }
    return na::hooking::CommitInstall();
}
} // namespace

void InitializeApply() {
    std::call_once(g_init, [] {
        wchar_t path[32768]{};
        DWORD n = GetModuleFileNameW(nullptr, path, 32768);
        if (!n || n == 32768) return;
        const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
        if (_wcsicmp(name, L"tlou-i.exe") != 0) return;
        char hash[65]{};
        if (!hash_file(path, hash) || std::strcmp(hash, kHash)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            strcpy_s(g_reason, "Automatic picture-mode Apply is not available for this game version."); return;
        }
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        constexpr unsigned char frame[] = {0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
        constexpr unsigned char pred[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x30,0x33,0xdb,0x48,0x8b};
        constexpr unsigned char caller[] = {0xe8,0x1a,0xdb,0xd5,0x00,0x84,0xc0,0x74,0x2a};
        constexpr unsigned char dirty_read[] = {0x44,0x8a,0xfb,0x38,0x99,0x91,0x5a,0x01,0x00,0x74,0x03,0x41,0xb7,0x01};
        constexpr unsigned char rebuild[] = {0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10,0x48,0x89,0x70,0x18,0x48,0x89,0x78,0x20,0x41,0x56,0x48,0x83,0xec};
        constexpr unsigned char rebuild_caller[] = {0xe8,0x54,0xf2,0xd5,0x00};
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!code_matches(base, kFrame, frame, sizeof(frame)) || !code_matches(base, kPredicate, pred, sizeof(pred)) ||
            !code_matches(base, kCaller - 5, caller, sizeof(caller)) || !code_matches(base, 0x11c67f7, dirty_read, sizeof(dirty_read)) ||
            !code_matches(base, kRebuild, rebuild, sizeof(rebuild)) || !code_matches(base, kRebuildCaller - 5, rebuild_caller, sizeof(rebuild_caller))) {
            strcpy_s(g_reason, "Automatic picture-mode Apply found a different engine layout."); return;
        }
        g_available = install(base + kFrame, base + kPredicate, base + kCaller, base + kRebuild, base + kRebuildCaller);
        strcpy_s(g_reason, g_available ? "Automatic picture-mode Apply is ready." : "Automatic picture-mode Apply could not attach.");
    });
}
bool RequestApply(Choice choice) {
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh_locked();
    if (!g_available || g_dispatch_active || g_request.Busy() || !na::settings::Get().enabled.load() || !Same(choice, current_choice()) ||
        choice.mode >= static_cast<uint32_t>(settings::DlssMode::Count) || !choice.width || !choice.height) return false;
    if (!g_free_resources) {
        // The original SL module is already loaded by the game by this point.
        // Never load a library or resolve an export inside a rendering detour.
        HMODULE sl = GetModuleHandleW(L"sl_interposer_real.dll");
        if (sl) g_free_resources = reinterpret_cast<FreeResourcesFn>(reinterpret_cast<void*>(GetProcAddress(sl, "slFreeResources")));
        if (!g_free_resources) {
            strcpy_s(g_reason, "The game's Streamline rebuild API is unavailable.");
            return false;
        }
    }
    uint64_t id = na::ngx::ArmSizeConfirmation(choice.width, choice.height, 30, 2500, 6000, 15000);
    if (!g_request.Arm(id, GetTickCount64(), choice)) return false;
    strcpy_s(g_reason, "Waiting for the game's next graphics-settings update.");
    g_queued.store(true, std::memory_order_release);
    return true;
}
ApplyStatus GetApplyStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh_locked();
    ApplyStatus s{};
    s.available = g_available; s.stage = g_request.stage; s.request_id = g_request.id; s.choice = g_request.choice;
    s.frame_calls = g_frame_calls.load(); s.predicate_calls = g_predicate_calls.load(); s.triggers = g_triggers.load();
    s.resource_releases = g_resource_releases.load(); s.resource_release_failures = g_resource_release_failures.load();
    s.trigger_thread = g_trigger_thread; strcpy_s(s.reason, g_reason); return s;
}
#ifdef NA_TLOU_APPLY_TEST
// Linked only into the CPU fixture, never either deployed proxy.
bool InstallApplyForTest(uintptr_t frame, uintptr_t predicate, uintptr_t caller, size_t dirty,
                         uintptr_t rebuild, uintptr_t rebuild_caller, FreeResourcesFn release) {
    std::lock_guard<std::mutex> lock(g_mutex); g_dirty = dirty; g_free_resources = release;
    g_available = install(frame, predicate, caller, rebuild, rebuild_caller); return g_available;
}
#endif
} // namespace na::tlou1
