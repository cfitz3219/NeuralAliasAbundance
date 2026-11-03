// Test-only NVSDK_NGX_Parameter implementation (C3, 2026-10). Stores every
// Set() by key; Get() returns NVSDK_NGX_Result_Fail for keys never set, so
// code under test sees exactly what a runtime that lacks a key reports.
// Requires the locally pinned NVIDIA NGX SDK headers (NA_HAVE_NGX_SDK).
#pragma once
#include <d3d12.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include <map>
#include <string>
#include <cstring>

struct ID3D11Resource;

namespace test {
class FakeParams final : public NVSDK_NGX_Parameter {
public:
    struct Value { enum Kind { U, I, F, D, P } kind; unsigned long long u; long long i; double d; void* p; };
    std::map<std::string, Value> values;
    mutable unsigned non_fg_reads = 0;
    bool Has(const char* k) const { return values.count(k) != 0; }

    void Set(const char* n, unsigned long long v) override { values[n] = {Value::U, v, static_cast<long long>(v), static_cast<double>(v), nullptr}; }
    void Set(const char* n, float v) override { values[n] = {Value::F, static_cast<unsigned long long>(v), static_cast<long long>(v), v, nullptr}; }
    void Set(const char* n, double v) override { values[n] = {Value::D, static_cast<unsigned long long>(v), static_cast<long long>(v), v, nullptr}; }
    void Set(const char* n, unsigned int v) override { values[n] = {Value::U, v, static_cast<long long>(v), static_cast<double>(v), nullptr}; }
    void Set(const char* n, int v) override { values[n] = {Value::I, static_cast<unsigned long long>(v), v, static_cast<double>(v), nullptr}; }
    void Set(const char* n, ID3D11Resource* v) override { values[n] = {Value::P, 0, 0, 0, v}; }
    void Set(const char* n, ID3D12Resource* v) override { values[n] = {Value::P, 0, 0, 0, v}; }
    void Set(const char* n, void* v) override { values[n] = {Value::P, 0, 0, 0, v}; }

    NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const override { auto v = find(n); if (!v || !o) return NVSDK_NGX_Result_Fail; *o = v->u; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, float* o) const override { auto v = find(n); if (!v || !o) return NVSDK_NGX_Result_Fail; *o = static_cast<float>(v->d); return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, double* o) const override { auto v = find(n); if (!v || !o) return NVSDK_NGX_Result_Fail; *o = v->d; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, unsigned int* o) const override { auto v = find(n); if (!v || !o) return NVSDK_NGX_Result_Fail; *o = static_cast<unsigned int>(v->u); return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, int* o) const override { auto v = find(n); if (!v || !o) return NVSDK_NGX_Result_Fail; *o = static_cast<int>(v->i); return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const override { auto v = find(n); if (!v || !o) return NVSDK_NGX_Result_Fail; *o = static_cast<ID3D11Resource*>(v->p); return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const override { auto v = find(n); if (!v || !o) return NVSDK_NGX_Result_Fail; *o = static_cast<ID3D12Resource*>(v->p); return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, void** o) const override { auto v = find(n); if (!v || !o) return NVSDK_NGX_Result_Fail; *o = v->p; return NVSDK_NGX_Result_Success; }
    void Reset() override { values.clear(); }

    unsigned int U(const char* n) const { auto v = find(n); return v ? static_cast<unsigned int>(v->u) : 0xFFFFFFFFu; }
    int I(const char* n) const { auto v = find(n); return v ? static_cast<int>(v->i) : -999; }

private:
    const Value* find(const char* n) const {
        if (std::strncmp(n, "DLSSG.", 6) != 0) ++non_fg_reads;
        auto it = values.find(n); return it == values.end() ? nullptr : &it->second;
    }
};
} // namespace test
