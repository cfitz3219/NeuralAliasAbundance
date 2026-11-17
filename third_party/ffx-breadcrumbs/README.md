# AMD FidelityFX Breadcrumbs (vendored, unmodified)

- Source: GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v1.1.4`, commit c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 (Breadcrumbs 1.0.1).
- License: MIT (`LICENSE.txt`, copied from the SDK root).
- Contents, byte-for-byte: `include/FidelityFX/host/` (ffx_breadcrumbs, ffx_interface, ffx_types, ffx_error,
  ffx_util, ffx_assert, ffx_message headers), `src/components/breadcrumbs/` (component), `src/shared/`
  (breadcrumbs list, object management, assert, message).
- Not vendored: AMD's DX12 backend library. `src/host_dx12/diag/gpu_breadcrumbs.cpp` supplies the four
  breadcrumbs callbacks of that backend (adapted from `sdk/src/backends/dx12/ffx_dx12.cpp`, MIT).

Do not edit these files. Fixes go in our backend, or a re-vendor at a new pin recorded here.
