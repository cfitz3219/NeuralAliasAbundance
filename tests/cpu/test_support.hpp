// reconstructed 2026-10 — checks remain active in Release/NDEBUG builds.
#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>

namespace test {
inline void require(bool condition, const char* expression, const char* file, int line) {
    if (!condition) throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + expression);
}
inline void near(double actual, double expected, double tolerance, const char* file, int line) {
    require(std::isfinite(actual) && std::isfinite(expected) && std::abs(actual - expected) <= tolerance,
            "numeric comparison outside tolerance", file, line);
}
template<class F> void invalid_argument(F f) {
    bool caught = false;
    try { f(); } catch (const std::invalid_argument&) { caught = true; }
    require(caught, "expected std::invalid_argument", __FILE__, __LINE__);
}
inline int run(std::initializer_list<std::pair<const char*, std::function<void()>>> cases) {
    unsigned passed = 0, failed = 0;
    for (const auto& item : cases) {
        try { item.second(); ++passed; std::printf("PASS %s\n", item.first); }
        catch (const std::exception& e) { ++failed; std::fprintf(stderr, "FAIL %s: %s\n", item.first, e.what()); }
    }
    std::printf("%u passed, %u failed\n", passed, failed);
    return failed == 0 && passed > 0 ? 0 : 1;
}
}
#define CHECK(...) test::require((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define NEAR(a, b, tolerance) test::near((a), (b), (tolerance), __FILE__, __LINE__)
