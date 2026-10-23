// reconstructed 2026-10 — test-only spelling shim, NOT a Windows runtime shim.
#pragma once
#if !defined(_WIN32) && !defined(__cdecl)
#define __cdecl
#define NA_CPU_UNDEFINE_CDECL
#endif
#include "host_dx12/host_observe.h"
#ifdef NA_CPU_UNDEFINE_CDECL
#undef __cdecl
#undef NA_CPU_UNDEFINE_CDECL
#endif
