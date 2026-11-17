# AMD D3D12 Memory Allocator (vendored, unmodified)

- Source: GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator, tag `v3.0.1`, commit 9ef66bc14edd10dee0de3a545b98578363552f66.
- License: MIT (`LICENSE.txt`).
- Contents: `include/D3D12MemAlloc.h`, `src/D3D12MemAlloc.cpp`, byte-for-byte.
- Used only through `src/host_dx12/gpu/mem_alloc.hpp` for our own native/NVRHI scratch textures and buffers. HUD RTs start with a full clear; effects/J2/NR fully write or copy their outputs before reads. Native leases and NVRHI COM/fence owners retain allocation-backed resources through submission completion. No game/vendor-internal allocation interception.

Do not edit these files. Fixes go in our wrapper, or a re-vendor at a new pin recorded here.
