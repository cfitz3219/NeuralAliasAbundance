// P4-0c increment 1: the AMD FidelityFX optical-flow passes compiled by DXC at build time.
// Checks every pass and both HDR permutations: the blob is a DXIL container, its embedded root
// signature is accepted by the device, and a compute PSO builds from it. Hardware first, then
// WARP. SKIP (77) when the build had no DXC, or the device lacks Shader Model 6.2.
// Results are printed as one summary line per pass and a final recap.
#include "test_support.hpp"

#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace ff = na::ffx_flow;

int main() {
    if (!ff::Built()) {
        std::printf("ffx_flow_shaders_test: SKIP (built without DXC, AMD optical flow is a stub)\n");
        return test::kSkip;
    }
    int failures = 0;
    // Blob sanity first: needs no device.
    for (int p = 0; p < static_cast<int>(ff::Pass::Count); ++p) {
        for (int hdr = 0; hdr < 2; ++hdr) {
            const ff::Blob b = ff::Shader(static_cast<ff::Pass>(p), hdr != 0);
            const bool ok = b.data && b.size > 64 && std::memcmp(b.data, "DXBC", 4) == 0;
            if (!ok) {
                std::printf("FAIL blob %s hdr=%d size=%zu\n", ff::PassName(static_cast<ff::Pass>(p)), hdr, b.size);
                ++failures;
            }
        }
    }

    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("ffx_flow_shaders_test");
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_2};
    if (FAILED(d.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_2) {
        std::printf("ffx_flow_shaders_test: blobs %s; SKIP PSOs (device below Shader Model 6.2)\n",
                    failures ? "FAILED" : "ok");
        return failures ? 1 : test::kSkip;
    }

    for (int p = 0; p < static_cast<int>(ff::Pass::Count); ++p) {
        for (int hdr = 0; hdr < 2; ++hdr) {
            const ff::Pass pass = static_cast<ff::Pass>(p);
            const ff::Blob b = ff::Shader(pass, hdr != 0);
            ComPtr<ID3D12RootSignature> rs;
            HRESULT hr = d.device->CreateRootSignature(0, b.data, b.size, IID_PPV_ARGS(&rs));
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(hr)) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
                cd.pRootSignature = rs.Get();
                cd.CS = {b.data, b.size};
                hr = d.device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso));
            }
            std::printf("%s %-18s hdr=%d size=%6zu hr=0x%08lX\n", SUCCEEDED(hr) ? "ok  " : "FAIL",
                        ff::PassName(pass), hdr, b.size, static_cast<unsigned long>(hr));
            if (FAILED(hr)) ++failures;
        }
    }
    std::printf("ffx_flow_shaders_test: %s (%d failure%s, device %s)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s", d.warp ? "WARP" : "hardware");
    return failures ? 1 : 0;
}
