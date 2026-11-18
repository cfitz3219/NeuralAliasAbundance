// The J2 flow programs compile with the real HLSL compiler: every entry point of both sources, with the target and
// flags compute_flow uses (compute_flow_programs.inc). No device needed, so this runs in CI.
// Why it exists: the camera-free programs (`measure`, `cut_state`, `cut_show`, and the screen fit's
// `fit_sample`, `fit_accum`, `fit_solve`) are compiled when the
// game starts, and a failure there is quiet by design (the residual path keeps working and measured
// motion reports itself unavailable). Here a program that does not compile is a failed test, with
// the compiler's own message.
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

#include "host_dx12/j2/flow_shaders.hpp"
#include "host_dx12/j2/replay_motion_shader.hpp"

int main()
{
    // The frame-candidates programs are a second source (kCandidateShader): its registers hold other kinds of resource.
    struct Entry { const char* name; const char* source; size_t size; };
    static const Entry kEntries[] = {
        {"encode", na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1},       {"accept", na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1},
        {"measure", na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1},      {"cut_state", na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1},
        {"fit_sample", na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1},   {"fit_accum", na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1},
        {"fit_solve", na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1},
        {"cut_show", na::j2::kFlowShader, sizeof na::j2::kFlowShader - 1},     {"cand_clear", na::j2::kCandidateShader, sizeof na::j2::kCandidateShader - 1},
        {"cand_count", na::j2::kCandidateShader, sizeof na::j2::kCandidateShader - 1}, {"cand_pick", na::j2::kCandidateShader, sizeof na::j2::kCandidateShader - 1},
        {"cand_apply", na::j2::kCandidateShader, sizeof na::j2::kCandidateShader - 1},
    };
    int failures = 0;
    for (const Entry& e : kEntries) {
        const char* entry = e.name;
        Microsoft::WRL::ComPtr<ID3DBlob> code, messages;
        const HRESULT hr = D3DCompile(e.source, e.size, "j2_flow", nullptr, nullptr, entry,
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
    // The draw-replay motion pass (replay_motion.cpp), with its own targets and flags: its pixel
    // program also counts, per draw, the pixels apart from the screen's own motion (core/replay_movers.hpp).
    static const char* const kReplay[][2] = {{"vs", "vs_5_0"}, {"ps", "ps_5_0"}};
    for (const auto& e : kReplay) {
        Microsoft::WRL::ComPtr<ID3DBlob> code, messages;
        const HRESULT hr = D3DCompile(na::j2::replay_motion::kShader, sizeof na::j2::replay_motion::kShader - 1, "replay_motion.hlsl", nullptr, nullptr,
                                      e[0], e[1], D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &messages);
        const bool ok = SUCCEEDED(hr) && code && code->GetBufferSize() > 0;
        std::printf("j2_flow_shaders_compile_test: replay_motion %s %s (%zu bytes, hr=0x%08lx)\n", e[0], ok ? "compiles" : "DOES NOT COMPILE",
                    code ? static_cast<size_t>(code->GetBufferSize()) : size_t(0), static_cast<unsigned long>(hr));
        if (messages && messages->GetBufferSize()) {
            const char* text = static_cast<const char*>(messages->GetBufferPointer());
            const size_t n = strnlen(text, messages->GetBufferSize());
            std::printf("%.*s\n", static_cast<int>(n < 1800 ? n : 1800), text);
        }
        if (!ok) ++failures;
    }
    std::printf("j2_flow_shaders_compile_test: %s (%d of %zu programs failed)\n", failures ? "FAIL" : "PASS", failures,
                sizeof kEntries / sizeof kEntries[0] + sizeof kReplay / sizeof kReplay[0]);
    return failures ? 1 : 0;
}
