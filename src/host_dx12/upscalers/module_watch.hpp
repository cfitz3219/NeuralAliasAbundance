// C7: one shared DLL-load watcher for the upscaler interceptors (FSR, XeSS).
// LdrRegisterDllNotification only queues a matching module; a worker thread
// runs the installer OUTSIDE the loader lock (hooking rules in vtable_hook.hpp).
// Modules already loaded when a watch is added are queued at once.
#pragma once

#include <windows.h>

#include <cstddef>

namespace na::upscalers {

using InstallFn = void (*)(HMODULE module);
// `names`: file names (case-insensitive, no path), null-terminated list kept
// alive by the caller (static storage). Returns false if the watcher cannot run.
bool AddModuleWatch(const wchar_t* const* names, InstallFn install);
void StopModuleWatches(); // joins the worker; hooks stay
// File-name part of `name` equals one of `names` (case-insensitive).
bool NameMatches(const wchar_t* name, size_t len, const wchar_t* const* names);

// Gate 2 R-302 test seam. The loader-notification callback runs under the
// Windows loader lock and takes the watcher's mutex, so the watcher must NEVER
// call into the loader while holding that mutex. `probe` (tests only) runs
// right before every loader call the watcher makes; WatchLockHeldByThisThread()
// tells it whether that rule is being broken.
void SetLoaderCallProbeForTests(void (*probe)());
bool WatchLockHeldByThisThread();

} // namespace na::upscalers
