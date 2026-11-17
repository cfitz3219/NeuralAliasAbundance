// P4-0c: the AMD FidelityFX optical-flow passes (third_party/ffx-opticalflow, MIT), as DXIL that
// DXC compiled at build time (cmake/ffx_flow_shaders.cmake). Each blob carries its own root
// signature (FFX_OPTICALFLOW_EMBED_ROOTSIG), so a PSO is created straight from the blob.
// Without DXC at build time every blob is empty and Built() is false (the stub).
#pragma once

#include <cstddef>

namespace na::ffx_flow {

enum class Pass : int {
    PrepareLuma = 0,   // colour -> luma level 0 (16x16 groups)
    LuminancePyramid,  // SPD: luma levels 1..6 (256 threads, CB2 root signature)
    ScdHistogram,      // scene-change histogram (32x8)
    ScdDivergence,     // scene-change divergence (256)
    Search,            // block search, per level (v5, 64 threads)
    Filter,            // flow filter, per level (v5, 16x4)
    Scale,             // upscale flow to the next finer level (v5, 4x4x4)
    Count
};

struct Blob {
    const void* data = nullptr;
    size_t size = 0;
};

// hdr selects AMD's FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT permutation (only prepare luma and
// the scene-change histogram read colour; both permutations exist for every pass).
Blob Shader(Pass pass, bool hdr);

// True when the shaders were compiled into this build.
bool Built();

const char* PassName(Pass pass);

}  // namespace na::ffx_flow
