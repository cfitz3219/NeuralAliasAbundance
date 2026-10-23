#include "host_dx12/nvof/nvof_flow.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

#pragma warning(push, 3)
#include <nvOpticalFlowCommon.h>
#include <nvOpticalFlowD3D12.h>
#pragma warning(pop)

namespace na::nvof {
namespace {

using PfnCreateInstance = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);

const char* status_name(NV_OF_STATUS s)
{
    switch (s) {
    case NV_OF_SUCCESS: return "success";
    case NV_OF_ERR_OF_NOT_AVAILABLE: return "optical flow hardware not available";
    case NV_OF_ERR_UNSUPPORTED_DEVICE: return "unsupported device";
    case NV_OF_ERR_DEVICE_DOES_NOT_EXIST: return "device does not exist";
    case NV_OF_ERR_INVALID_PTR: return "invalid pointer";
    case NV_OF_ERR_INVALID_PARAM: return "invalid parameter";
    case NV_OF_ERR_INVALID_CALL: return "invalid call order";
    case NV_OF_ERR_INVALID_VERSION: return "invalid API version";
    case NV_OF_ERR_OUT_OF_MEMORY: return "out of memory";
    case NV_OF_ERR_NOT_INITIALIZED: return "not initialised";
    default: return "error";
    }
}

bool load_api(void** module, NV_OF_D3D12_API_FUNCTION_LIST** api, char* error, size_t cap)
{
    HMODULE m = LoadLibraryW(L"nvofapi64.dll");
    if (!m) {
        snprintf(error, cap, "nvofapi64.dll not found (NVIDIA driver required)");
        return false;
    }
    auto create = reinterpret_cast<PfnCreateInstance>(GetProcAddress(m, "NvOFAPICreateInstanceD3D12"));
    if (!create) {
        snprintf(error, cap, "NvOFAPICreateInstanceD3D12 missing from nvofapi64.dll");
        return false; // module stays loaded (never unloaded on any path)
    }
    auto* list = new NV_OF_D3D12_API_FUNCTION_LIST{};
    NV_OF_STATUS s = create(NV_OF_API_VERSION, list);
    if (s != NV_OF_SUCCESS) {
        snprintf(error, cap, "NvOFAPICreateInstanceD3D12 (API %u.%u): %s", NV_OF_API_MAJOR_VERSION, NV_OF_API_MINOR_VERSION, status_name(s));
        delete list;
        return false; // module stays loaded
    }
    *module = m;
    *api = list;
    return true;
}

} // namespace

bool QueryCaps(ID3D12Device* device, Caps* out, char* error, size_t error_cap)
{
    if (!device || !out) return false;
    *out = Caps{};
    void* module = nullptr;
    NV_OF_D3D12_API_FUNCTION_LIST* api = nullptr;
    char err[192] = {};
    if (!load_api(&module, &api, err, sizeof(err))) {
        if (error) strncpy_s(error, error_cap, err, _TRUNCATE);
        return false;
    }
    NvOFHandle h = nullptr;
    NV_OF_STATUS s = api->nvCreateOpticalFlowD3D12(device, &h);
    if (s != NV_OF_SUCCESS || !h) {
        if (error) snprintf(error, error_cap, "nvCreateOpticalFlowD3D12: %s", status_name(s));
        delete api;
        return false; // module stays loaded
    }
    uint32_t n = 0;
    if (api->nvOFGetCaps(h, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &n) == NV_OF_SUCCESS && n > 0 && n <= 8) {
        api->nvOFGetCaps(h, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, out->grid_sizes, &n);
        out->grid_count = n;
    }
    uint32_t one = 1;
    api->nvOFGetCaps(h, NV_OF_CAPS_WIDTH_MIN, &out->min_w, &one);
    one = 1; api->nvOFGetCaps(h, NV_OF_CAPS_HEIGHT_MIN, &out->min_h, &one);
    one = 1; api->nvOFGetCaps(h, NV_OF_CAPS_WIDTH_MAX, &out->max_w, &one);
    one = 1; api->nvOFGetCaps(h, NV_OF_CAPS_HEIGHT_MAX, &out->max_h, &one);
    out->available = true;
    api->nvOFDestroy(h);
    delete api;
    (void)module; // nvofapi64.dll stays loaded for the process lifetime (unloading it
                  // while D3D12 objects are alive crashed at exit; same policy as the NGX core)
    return true;
}

Flow::~Flow() { Shutdown(); }

void Flow::set_error(const char* what, int status)
{
    snprintf(error_, sizeof(error_), "%s: %s", what, status_name(static_cast<NV_OF_STATUS>(status)));
}

bool Flow::Init(ID3D12Device* device, uint32_t width, uint32_t height, uint32_t grid, uint32_t perf_level)
{
    if (registered_) {
        snprintf(error_, sizeof(error_), "Init refused: %u registrations outstanding", registered_);
        return false;
    }
    Shutdown();
    if (!device || width == 0 || height == 0 || (grid != 1 && grid != 2 && grid != 4)) {
        snprintf(error_, sizeof(error_), "invalid Init arguments");
        return false;
    }
    NV_OF_D3D12_API_FUNCTION_LIST* api = nullptr;
    if (!load_api(&module_, &api, error_, sizeof(error_))) return false;
    api_ = api;
    NvOFHandle h = nullptr;
    NV_OF_STATUS s = api->nvCreateOpticalFlowD3D12(device, &h);
    if (s != NV_OF_SUCCESS || !h) {
        set_error("nvCreateOpticalFlowD3D12", s);
        return false;
    }
    handle_ = h;
    width_ = width;
    height_ = height;
    grid_ = grid;
    NV_OF_INIT_PARAMS ip{};
    ip.width = width;
    ip.height = height;
    ip.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid);
    ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    ip.mode = NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel = static_cast<NV_OF_PERF_LEVEL>(perf_level);
    ip.enableExternalHints = NV_OF_FALSE;
    ip.enableOutputCost = NV_OF_FALSE;
    ip.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
    ip.enableRoi = NV_OF_FALSE;
    ip.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
    ip.enableGlobalFlow = NV_OF_FALSE;
    ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    s = api->nvOFInit(h, &ip);
    if (s != NV_OF_SUCCESS) {
        set_error("nvOFInit", s);
        return false;
    }
    return true;
}

bool Flow::RegisterInput(ID3D12Resource* bgra8, ID3D12Fence* fence, uint64_t wait_value, uint64_t signal_value, BufferHandle* out)
{
    if (!handle_ || !bgra8 || !fence || !out) return false;
    D3D12_RESOURCE_DESC d = bgra8->GetDesc();
    if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || d.Width < width_ || d.Height < height_) {
        snprintf(error_, sizeof(error_), "input must be B8G8R8A8_UNORM of at least %ux%u", width_, height_);
        return false;
    }
    auto* api = static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(api_);
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 rp{};
    rp.resource = bgra8;
    rp.inputFencePoint.fence = fence;
    rp.inputFencePoint.value = wait_value;
    NvOFGPUBufferHandle hb = nullptr;
    rp.hOFGpuBuffer = &hb;
    rp.outputFencePoint.fence = fence;
    rp.outputFencePoint.value = signal_value;
    NV_OF_STATUS s = api->nvOFRegisterResourceD3D12(static_cast<NvOFHandle>(handle_), &rp);
    if (s != NV_OF_SUCCESS) {
        set_error("nvOFRegisterResourceD3D12 (input)", s);
        return false;
    }
    *out = hb;
    ++registered_;
    return true;
}

bool Flow::RegisterOutput(ID3D12Resource* rg16_sint, ID3D12Fence* fence, uint64_t wait_value, uint64_t signal_value, BufferHandle* out)
{
    if (!handle_ || !rg16_sint || !fence || !out) return false;
    D3D12_RESOURCE_DESC d = rg16_sint->GetDesc();
    if (d.Format != DXGI_FORMAT_R16G16_SINT || d.Width < OutWidth() || d.Height < OutHeight()) {
        snprintf(error_, sizeof(error_), "output must be R16G16_SINT of at least %ux%u", OutWidth(), OutHeight());
        return false;
    }
    auto* api = static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(api_);
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 rp{};
    rp.resource = rg16_sint;
    rp.inputFencePoint.fence = fence;
    rp.inputFencePoint.value = wait_value;
    NvOFGPUBufferHandle hb = nullptr;
    rp.hOFGpuBuffer = &hb;
    rp.outputFencePoint.fence = fence;
    rp.outputFencePoint.value = signal_value;
    NV_OF_STATUS s = api->nvOFRegisterResourceD3D12(static_cast<NvOFHandle>(handle_), &rp);
    if (s != NV_OF_SUCCESS) {
        set_error("nvOFRegisterResourceD3D12 (output)", s);
        return false;
    }
    *out = hb;
    ++registered_;
    return true;
}

bool Flow::Unregister(BufferHandle h)
{
    if (!api_ || !h) return false;
    NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 up{};
    up.hOFGpuBuffer = static_cast<NvOFGPUBufferHandle>(h);
    NV_OF_STATUS s = static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(api_)->nvOFUnregisterResourceD3D12(&up);
    if (s != NV_OF_SUCCESS) {
        set_error("nvOFUnregisterResourceD3D12", s);
        return false;
    }
    if (registered_) --registered_;
    return true;
}

bool Flow::Execute(BufferHandle input_current, BufferHandle reference_previous, BufferHandle output, ID3D12Fence* wait_fence,
                   uint64_t wait_value, ID3D12Fence* signal_fence, uint64_t signal_value, bool disable_temporal_hints)
{
    if (!handle_ || !input_current || !reference_previous || !output || !wait_fence || !signal_fence) return false;
    auto* api = static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(api_);
    NV_OF_FENCE_POINT in_fp{wait_fence, wait_value};
    NV_OF_FENCE_POINT out_fp{signal_fence, signal_value};
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 ip{};
    ip.inputFrame = static_cast<NvOFGPUBufferHandle>(input_current);
    ip.referenceFrame = static_cast<NvOFGPUBufferHandle>(reference_previous);
    ip.externalHints = nullptr;
    ip.disableTemporalHints = disable_temporal_hints ? NV_OF_TRUE : NV_OF_FALSE;
    ip.hPrivData = nullptr;
    ip.numRois = 0;
    ip.roiData = nullptr;
    ip.numFencePoints = 1;
    ip.fencePoint = &in_fp;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 op{};
    op.outputBuffer = static_cast<NvOFGPUBufferHandle>(output);
    op.outputCostBuffer = nullptr;
    op.hPrivData = nullptr;
    op.fencePoint = &out_fp;
    NV_OF_STATUS s = api->nvOFExecuteD3D12(static_cast<NvOFHandle>(handle_), &ip, &op);
    if (s != NV_OF_SUCCESS) {
        char last[160] = {};
        uint32_t n = sizeof(last) - 1;
        api->nvOFGetLastError(static_cast<NvOFHandle>(handle_), last, &n);
        snprintf(error_, sizeof(error_), "nvOFExecuteD3D12: %s%s%s", status_name(s), last[0] ? " - " : "", last);
        return false;
    }
    return true;
}

bool Flow::Shutdown()
{
    if (registered_) {
        snprintf(error_, sizeof(error_), "Shutdown refused: %u registrations outstanding", registered_);
        return false;
    }
    if (api_ && handle_) static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(api_)->nvOFDestroy(static_cast<NvOFHandle>(handle_));
    handle_ = nullptr;
    delete static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(api_);
    api_ = nullptr;
    module_ = nullptr; // never unloaded (see QueryCaps)
    return true;
}

} // namespace na::nvof
