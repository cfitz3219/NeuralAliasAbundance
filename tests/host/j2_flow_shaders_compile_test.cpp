// The J2 flow programs compile with the real HLSL compiler: every entry point, with the target and
// flags compute_flow uses (compute_flow_programs.inc). No device needed, so this runs in CI.
// Why it exists: the camera-free programs (`measure`, `cut_state`, `cut_show`) are compiled when the
// game starts, and a failure there is quiet by design (the residual path keeps working and measured
// motion reports itself unavailable). Here a program that does not compile is a failed test, with
// the compiler's own message.
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

#include "host_dx12/j2/flow_shaders.hpp"

int main()
{
    static const char* const kEntries[] = {"encode", "accept", "measure", "cut_state", "cut_show"};
    int failures = 0;
    for (const char* entry : kEntries) {
        Microsoft::WRL::ComPtr<ID3DBlob> code, messages;
        const HRESULT hr = D3DCompile(na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1, "j2_flow", nullptr, nullptr, entry,
                                      "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
        const bool ok = SUCCEEDED(hr) && code && code->GetBufferSize() > 0;
        std::printf("j2_flow_shaders_compile_test: %s %s (%zu bytes, hr=0x%08lx)\n", entry, ok ? "compiles" : "DOES NOT COMPILE",
                    code ? static_cast<size_t>(code->GetBufferSize()) : size_t(0), static_cast<unsigned long>(hr));
        if (messages && messages->GetBufferSize()) { // warnings are shown too: they name the line
            const char* text = static_cast<const char*>(messages->GetBufferPointer());
            const size_t n = strnlen(text, messages->GetBufferSize());
            std::printf("%.*s\n", static_cast<int>(n < 1800 ? n : 1800), text);
        }
        if (!ok) ++failures;
    }
    std::printf("j2_flow_shaders_compile_test: %s (%d of %zu programs failed)\n", failures ? "FAIL" : "PASS", failures,
                sizeof kEntries / sizeof kEntries[0]);
    return failures ? 1 : 0;
}
