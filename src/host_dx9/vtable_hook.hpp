// Vtable method hooks for COM objects (DX9 plan D2/D3). Every object of one
// implementation shares one vtable, so a method is hooked once per
// implementation (9On12's, and system d3d9's on fallback); the detour finds
// its original through the object's own vtable entry. Windows only.
#pragma once

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <mutex>

namespace na::dx9 {

struct HookSlot {
    std::atomic<void*> target{nullptr};
    std::atomic<void*> original{nullptr};
};

struct Method {
    static constexpr int kMaxImpl = 4;
    Method(int i, void* d) : index(i), detour(d) {}
    int index;
    void* detour;
    HookSlot slots[kMaxImpl];

    template <class F>
    F Original(const void* self) const
    {
        void* target = (*reinterpret_cast<void* const* const*>(self))[index];
        for (const auto& s : slots)
            if (s.target.load(std::memory_order_acquire) == target) return reinterpret_cast<F>(s.original.load(std::memory_order_relaxed));
        return nullptr;
    }
};

inline std::mutex& HookMutex()
{
    static std::mutex m;
    return m;
}

// Hooks `m` on `object`'s implementation. Returns false when it could not.
inline bool HookMethod(Method& m, void* object)
{
    if (!object) return false;
    void* target = (*reinterpret_cast<void***>(object))[m.index];
    std::lock_guard<std::mutex> l(HookMutex());
    static const bool mh = [] {
        const MH_STATUS s = MH_Initialize();
        return s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED;
    }();
    if (!mh) return false;
    for (auto& s : m.slots)
        if (s.target.load() == target) return true; // already hooked
    for (auto& s : m.slots) {
        if (s.target.load()) continue;
        void* orig = nullptr;
        if (MH_CreateHook(target, m.detour, &orig) != MH_OK) return false;
        // Published before the hook goes live, so the detour always finds its original.
        s.original.store(orig, std::memory_order_relaxed);
        s.target.store(target, std::memory_order_release);
        return MH_EnableHook(target) == MH_OK;
    }
    return false;
}

void Log(const char* fmt, ...); // the proxy's per-run log (d3d9_proxy.cpp)

} // namespace na::dx9
