#pragma once
#include <cstddef>

// Diagnostic CPU/GPU usage sampler for test packages ([observe] usage_log=1 or profile_hooks=1).
// A background thread samples every 5 s: whole-machine CPU, this process's CPU, its busiest
// threads (a CPU-bound game usually shows one thread near 100 % while the total stays low),
// and GPU engine utilisation (all processes and this process). It never touches rendering.
namespace na::usage {
void Start();                                   // idempotent; call after boot, never from DllMain
int FormatLine(char* out, std::size_t size);    // "usage ...\r\n", or 0 when not started / no sample yet
}
