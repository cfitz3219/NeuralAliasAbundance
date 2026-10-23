#pragma once
#include <cstdint>
namespace abundance {
// Read-only diagnostics, independent of any C++ objects crossing DLL boundaries.
struct ProxyStatus { uint32_t size, abi_version; uint64_t factory_calls; uint32_t system_loaded, reserved; };
using QueryProxyStatus=int(__cdecl*)(ProxyStatus*);
}
