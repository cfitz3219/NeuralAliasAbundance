#pragma once
// Provenance for synchronous calls made by our backend into hooked NGX exports.
// Unmarked calls remain unknown: absence of this marker does not prove game origin.
namespace na::dlss {
inline thread_local unsigned injector_call_depth = 0;
struct InjectorCallScope {
    InjectorCallScope() { ++injector_call_depth; }
    ~InjectorCallScope() { --injector_call_depth; }
    InjectorCallScope(const InjectorCallScope&) = delete;
    InjectorCallScope& operator=(const InjectorCallScope&) = delete;
};
inline bool IsInjectorCall() { return injector_call_depth != 0; }
}
