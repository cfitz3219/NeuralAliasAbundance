// Standalone offline NGX NR replay. All resource downloads and GPU waits are test-only.
#include "test_support.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <nvsdk_ngx.h>
#include <vector>

using test::ComPtr;
using namespace std::filesystem;
constexpr UINT W = 3440, H = 1440;
constexpr auto readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Image
{
    ComPtr<ID3D12Resource> texture, staging;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 size{}, rowBytes{};
    bool Create(test::Device &device, DXGI_FORMAT format, bool output)
    {
        texture = test::CreateTexture(device.device.Get(), W, H, format,
                                      output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : readState,
                                      output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        if (!texture)
            return false;
        auto desc = texture->GetDesc();
        device.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &size);
        staging =
            test::CreateBuffer(device.device.Get(), size, output ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD,
                               output ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ);
        return !!staging;
    }
    void Copy(ID3D12GraphicsCommandList *cmd, bool download)
    {
        auto state = download ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : readState;
        D3D12_RESOURCE_BARRIER trans = test::Transition(
            texture.Get(), state, download ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &trans);
        D3D12_TEXTURE_COPY_LOCATION tex{}, buf{};
        tex.pResource = texture.Get();
        tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        buf.pResource = staging.Get();
        buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buf.PlacedFootprint = footprint;
        cmd->CopyTextureRegion(download ? &buf : &tex, 0, 0, 0, download ? &tex : &buf, nullptr);
        std::swap(trans.Transition.StateBefore, trans.Transition.StateAfter);
        cmd->ResourceBarrier(1, &trans);
    }
    bool File(const path &file, bool download)
    {
        std::fstream stream(file, std::ios::binary | (download ? (std::ios::out | std::ios::trunc) : std::ios::in));
        if (!stream || (!download && file_size(file) != rows * rowBytes))
            return false;
        char *pixels{};
        if (FAILED(staging->Map(0, nullptr, reinterpret_cast<void **>(&pixels))))
            return false;
        for (UINT y = 0; y < rows; ++y)
        {
            char *row = pixels + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
            if (download)
                stream.write(row, rowBytes);
            else
                stream.read(row, rowBytes);
        }
        staging->Unmap(0, nullptr);
        return !!stream;
    }
};
static void NVSDK_CONV NgxLog(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    if (message)
        fprintf(stderr, "NGX | %s\n", message);
}
int wmain(int argc, wchar_t **argv)
{
    REQUIRE(argc == 5);
    const path root = argv[3];
    const auto preset = static_cast<unsigned>(_wtoi(argv[4]));
    REQUIRE(preset == 1 || preset == 2);
    setvbuf(stdout, nullptr, _IONBF, 0);
    test::Device device;
    REQUIRE(test::CreateDevice(device, false));
    test::List list;
    REQUIRE(list.Create(device.device.Get()));
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {argv + 2, 1};
    info.LoggingInfo.LoggingCallback = NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    auto result =
        NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                            "NeuralAliasAbundance", root.c_str(), device.device.Get(), &info);
    printf("Route B SDK NGX init 0x%08X\n", unsigned(result));
    REQUIRE(result == NVSDK_NGX_Result_Success);
    auto get = NVSDK_NGX_D3D12_GetCapabilityParameters;
    auto allocate = NVSDK_NGX_D3D12_AllocateParameters;
    auto create = [](ID3D12GraphicsCommandList *cmd, unsigned id, NVSDK_NGX_Parameter *params, void **handle) {
        return NVSDK_NGX_D3D12_CreateFeature(cmd, static_cast<NVSDK_NGX_Feature>(id), params,
                                             reinterpret_cast<NVSDK_NGX_Handle **>(handle));
    };
    auto eval = [](ID3D12GraphicsCommandList *cmd, void *handle, NVSDK_NGX_Parameter *params, void *) {
        return NVSDK_NGX_D3D12_EvaluateFeature_C(cmd, static_cast<NVSDK_NGX_Handle *>(handle), params, nullptr);
    };
    auto release = [](void *handle) { return NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle *>(handle)); };
    auto destroy = NVSDK_NGX_D3D12_DestroyParameters;
    auto shutdown = NVSDK_NGX_D3D12_Shutdown1;
    NVSDK_NGX_Parameter *p{}, *caps{};
    REQUIRE(get(&caps) == 1 && caps);
    int available{};
    auto cap_result = caps->Get("DLSSNR.Available", &available);
    printf("availability query result 0x%08X\n", unsigned(cap_result));
    printf("DLSSNR.Available %d\n", available);
    for (const char *key : {"DLSSNR.NeedsUpdatedDriver", "DLSSNR.MinDriverVersionMajor", "DLSSNR.MinDriverVersionMinor",
                            "DLSSNR.FeatureInitResult", "SuperSampling.Available"})
    {
        unsigned val{};
        const auto rc = caps->Get(key, &val);
        printf("%s query=0x%08X value=0x%08X (%u)\n", key, unsigned(rc), val, val);
    }
    REQUIRE(allocate(&p) == 1 && p);
    for (const char *key : {"CreationNodeMask", "VisibilityNodeMask", "DLSSNR.Enabled", "DLSSNR.UseAutoMask"})
        p->Set(key, 1u);
    for (const char *key : {"Width", "OutWidth", "DLSSNR.Width"})
        p->Set(key, W);
    for (const char *key : {"Height", "OutHeight", "DLSSNR.Height"})
        p->Set(key, H);
    p->Set("DLSSNR.Hint.Render.Preset", preset);
    p->Set("DLSSNR.Style", 0u);
    p->Set("DLSSNR.UICorrection", 0u);
    p->Set("DLSSNR.Intensity", 1.f);
    p->Set("DLSSNR.LocalStructureStrength", 1.f);
    p->Set("DLSSNR.SkinStructureStrength", 1.f);
    p->Set("DLSSNR.LocalToneStrength", 0.f);
    void *handle{};
    const auto created = create(list.list.Get(), 18u, p, &handle);
    printf("NR create 0x%08X\n", created);
    REQUIRE(created == 1 && handle);
    auto submit = [&]() {
        if (FAILED(list.list->Close()))
            return false;
        ID3D12CommandList *seq[]{list.list.Get()};
        device.queue->ExecuteCommandLists(1, seq);
        device.WaitIdle();
        return SUCCEEDED(device.device->GetDeviceRemovedReason());
    };
    REQUIRE(submit());
    std::array<Image, 4> images{};
    DXGI_FORMAT fmts[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                          DXGI_FORMAT_R32G32B32A32_FLOAT};
    const char *slots[] = {"Color", "Depth", "MVec", "Output"};
    const char *files[] = {"color", "depth", "motion"};
    for (unsigned j = 0; j < 4; ++j)
    {
        REQUIRE(images[j].Create(device, fmts[j], j == 3));
        const std::string key = std::string("DLSSNR.") + slots[j];
        p->Set(key.c_str(), images[j].texture.Get());
        p->Set((key + "SubrectBaseX").c_str(), 0u);
        p->Set((key + "SubrectBaseY").c_str(), 0u);
        p->Set((key + "SubrectWidth").c_str(), W);
        p->Set((key + "SubrectHeight").c_str(), H);
    }
    p->Set("DLSSNR.DepthInverted", 1u);
    p->Set("DLSSNR.MVecScaleX", 1.f);
    p->Set("DLSSNR.MVecScaleY", 1.f);
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    ComPtr<ID3D12QueryHeap> queries;
    REQUIRE(SUCCEEDED(device.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries))));
    const auto timestamps =
        test::CreateBuffer(device.device.Get(), 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 hz{};
    REQUIRE(timestamps && SUCCEEDED(device.queue->GetTimestampFrequency(&hz)) && hz);
    for (unsigned frame = 0; frame < 9; ++frame)
    {
        list.Reset();
        for (unsigned j = 0; j < 3; ++j)
        {
            REQUIRE(images[j].File(root / (std::to_string(frame) + "-" + files[j] + ".bin"), false));
            images[j].Copy(list.list.Get(), false);
        }
        p->Set("DLSSNR.Reset", frame == 0 ? 1u : 0u);
        list.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        const auto code = eval(list.list.Get(), handle, p, nullptr);
        printf("frame %u evaluate 0x%08X ", frame, code);
        REQUIRE(code == 1);
        list.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestamps.Get(), 0);
        if (frame == 8)
            images[3].Copy(list.list.Get(), true);
        REQUIRE(submit());
        UINT64 *t{};
        REQUIRE(SUCCEEDED(timestamps->Map(0, nullptr, reinterpret_cast<void **>(&t))));
        printf("model_ms %.3f\n", (t[1] - t[0]) * 1000. / hz);
        timestamps->Unmap(0, nullptr);
    }
    REQUIRE(images[3].File(root / ("after-" + std::to_string(preset) + ".raw"), true));
    REQUIRE(release(handle) == 1);
    REQUIRE(destroy(p) == 1);
    REQUIRE(destroy(caps) == 1);
    REQUIRE(shutdown(device.device.Get()) == 1);
    return test::finish("route_b_standalone_nr");
}
