# AMD FidelityFX optical flow (vendored, unmodified)

- Source: GPUOpen FidelityFX FSR3 SDK, tag `release-FSR3-3.0.3`, commit a0632abf1350bb64c098573d84c42f053f053a6e ("FidelityFX FSR3 3.0.3").
- License: MIT (`LICENSE.txt`, copied from the SDK root).
- Contents, byte-for-byte from the SDK:
  - `include/FidelityFX/gpu/`: `ffx_common_types.h`, `ffx_core*.h`
  - `include/FidelityFX/gpu/opticalflow/`: all 10 headers
  - `include/FidelityFX/gpu/spd/`: `ffx_spd.h`, `ffx_spd_resources.h` (luminance pyramid)
  - `shaders/dx12/`: the 7 DX12 pass entry files (`sdk/src/backends/dx12/shaders/opticalflow/`)
- Not vendored: AMD's host runtime and backend. Our own dispatcher (`src/host_dx12/ffx_flow/`) binds each
  pass through its embedded root signature (`FFX_OPTICALFLOW_EMBED_ROOTSIG`).
- Built by `cmake/ffx_flow_shaders.cmake` with DXC from the Windows SDK; the DXIL lands in the build folder.

Do not edit these files. Fixes go in our dispatcher, or a re-vendor at a new pin recorded here.
