// Test-only fake NGX runtime for the C6 DLSS backend (shared by the backend
// and FSR-interception tests). Records creates/evaluates/releases and the
// parameter values each call saw. Needs the NGX SDK headers.
#pragma once
#include "fake_ngx_params.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"

#include <nvsdk_ngx.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fake {
inline int allocs = 0, destroys = 0, creates = 0, evals = 0;
inline std::vector<uintptr_t> released;
inline NVSDK_NGX_Result create_result = NVSDK_NGX_Result_Success;
inline NVSDK_NGX_Result eval_result = NVSDK_NGX_Result_Success;
inline bool available = true;
inline std::map<std::string, test::FakeParams::Value> last_create, last_eval;
inline uintptr_t last_eval_handle = 0, next_handle = 0x1000;
inline void (*eval_hook)() = nullptr; // runs inside EvaluateFeature (tests of lock ordering)

inline NVSDK_NGX_Result NVSDK_CONV Optimal(NVSDK_NGX_Parameter* p)
{
    unsigned w = 0, h = 0;
    int q = -1;
    p->Get(NVSDK_NGX_Parameter_Width, &w);
    p->Get(NVSDK_NGX_Parameter_Height, &h);
    p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &q);
    const unsigned rw = q == NVSDK_NGX_PerfQuality_Value_MaxQuality ? w * 2 / 3 : w / 2;
    const unsigned rh = q == NVSDK_NGX_PerfQuality_Value_MaxQuality ? h * 2 / 3 : h / 2;
    p->Set(NVSDK_NGX_Parameter_OutWidth, rw);
    p->Set(NVSDK_NGX_Parameter_OutHeight, rh);
    // Old runtimes report no dynamic range: only the minimum is given here,
    // so the backend must fall back to the optimal size for the maximum.
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, rw / 2);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, rh / 2);
    return NVSDK_NGX_Result_Success;
}
inline NVSDK_NGX_Result NVSDK_CONV GetCap(NVSDK_NGX_Parameter** out)
{
    auto* p = new test::FakeParams();
    ++allocs;
    p->Set(NVSDK_NGX_Parameter_SuperSampling_Available, available ? 1 : 0);
    p->Set(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, 0);
    p->Set(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, reinterpret_cast<void*>(&Optimal));
    *out = p;
    return NVSDK_NGX_Result_Success;
}
inline NVSDK_NGX_Result NVSDK_CONV Alloc(NVSDK_NGX_Parameter** out)
{
    *out = new test::FakeParams();
    ++allocs;
    return NVSDK_NGX_Result_Success;
}
inline NVSDK_NGX_Result NVSDK_CONV Destroy(NVSDK_NGX_Parameter* p)
{
    delete static_cast<test::FakeParams*>(p);
    ++destroys;
    return NVSDK_NGX_Result_Success;
}
inline NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** h)
{
    ++creates;
    if (!list || id != NVSDK_NGX_Feature_SuperSampling) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (NVSDK_NGX_FAILED(create_result)) return create_result;
    last_create = static_cast<test::FakeParams*>(p)->values;
    *h = reinterpret_cast<NVSDK_NGX_Handle*>(next_handle++);
    return NVSDK_NGX_Result_Success;
}
inline NVSDK_NGX_Result NVSDK_CONV Eval(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p, void*)
{
    ++evals;
    if (eval_hook) eval_hook();
    if (!list || !h) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    last_eval = static_cast<const test::FakeParams*>(p)->values;
    last_eval_handle = reinterpret_cast<uintptr_t>(h);
    return eval_result;
}
inline NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* h)
{
    released.push_back(reinterpret_cast<uintptr_t>(h));
    return NVSDK_NGX_Result_Success;
}
inline na::dlss::Api MakeApi()
{
    na::dlss::Api a;
    a.get_capability_parameters = reinterpret_cast<void*>(&GetCap);
    a.allocate_parameters = reinterpret_cast<void*>(&Alloc);
    a.destroy_parameters = reinterpret_cast<void*>(&Destroy);
    a.create_feature = reinterpret_cast<void*>(&Create);
    a.evaluate_feature = reinterpret_cast<void*>(&Eval);
    a.release_feature = reinterpret_cast<void*>(&Release);
    return a;
}
inline bool Released(uintptr_t h)
{
    for (uintptr_t x : released) if (x == h) return true;
    return false;
}
inline unsigned U(const std::map<std::string, test::FakeParams::Value>& m, const char* k)
{
    auto it = m.find(k);
    return it == m.end() ? 0xFFFFFFFFu : static_cast<unsigned>(it->second.u);
}
inline int I(const std::map<std::string, test::FakeParams::Value>& m, const char* k)
{
    auto it = m.find(k);
    return it == m.end() ? -999 : static_cast<int>(it->second.i);
}
inline double F(const std::map<std::string, test::FakeParams::Value>& m, const char* k)
{
    auto it = m.find(k);
    return it == m.end() ? -999.0 : it->second.d;
}
inline void* P(const std::map<std::string, test::FakeParams::Value>& m, const char* k)
{
    auto it = m.find(k);
    return it == m.end() ? reinterpret_cast<void*>(0xDEAD) : it->second.p;
}
} // namespace fake
