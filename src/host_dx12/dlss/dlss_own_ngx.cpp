// C6: our own NGX instance, for games with no DLSS of their own. Built only
// when the NGX static library (nvsdk_ngx_s.lib, /MT) is in the local SDK folder
// (NA_HAVE_NGX_LIB). The library finds the driver's NGX core itself; the DLSS
// model (nvngx_dlss.dll) must sit beside the executable or in the driver store.
#include "host_dx12/dlss/dlss_backend.hpp"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#include <cstdio>
#include <initializer_list>

namespace na::dlss {

namespace {
// Custom-engine project id (NVIDIA: any GUID for NVSDK_NGX_ENGINE_TYPE_CUSTOM).
constexpr const char* kProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
constexpr const char* kEngineVersion = "NeuralAliasAbundance";
} // namespace

bool OwnInit(ID3D12Device* device, const wchar_t* data_path, Api* api, char* reason)
{
    if (!device || !data_path || !api) {
        std::snprintf(reason, 128, "DLSS start-up was called without a device or data folder.");
        return false;
    }
    // Route B live Frame Generation: the package's own folder is one more read-only place NVIDIA's
    // runtime looks for its feature files (nvngx_dlssg.dll). Nothing is copied anywhere.
    // NeuralAlias's own DLSS file's folder comes first: it is found before an old file a game ships
    // beside its program (measured 2026-11-27: with 2.4.0.0 beside the program and 310.9.16 here, 310.9.16 is used).
    NVSDK_NGX_FeatureCommonInfo info{};
    const wchar_t* paths[2] = {};
    unsigned count = 0;
    for (const wchar_t* folder : {OwnDlssFolder(), OwnSearchFolder()})
        if (folder && folder[0]) paths[count++] = folder;
    if (count) {
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = count;
    }
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                                   kEngineVersion, data_path, device,
                                                                   count ? &info : nullptr);
    if (NVSDK_NGX_FAILED(r)) {
        std::snprintf(reason, 128, "The NVIDIA DLSS runtime did not start (code 0x%08X). Is this an RTX GPU with a current driver?",
                      static_cast<unsigned>(r));
        return false;
    }
    using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    api->get_capability_parameters = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_GetCapabilityParameters);
    api->allocate_parameters = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_AllocateParameters);
    api->destroy_parameters = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_DestroyParameters);
    api->create_feature = reinterpret_cast<void*>(static_cast<CreateFn>(&NVSDK_NGX_D3D12_CreateFeature));
    api->evaluate_feature = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_EvaluateFeature_C);
    api->release_feature = reinterpret_cast<void*>(&NVSDK_NGX_D3D12_ReleaseFeature);
    return true;
}

void OwnShutdown(ID3D12Device* device)
{
    NVSDK_NGX_D3D12_Shutdown1(device);
}

} // namespace na::dlss
