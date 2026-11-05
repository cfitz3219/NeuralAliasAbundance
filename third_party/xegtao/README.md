# Intel XeGTAO

Unmodified upstream `XeGTAO.h`, `XeGTAO.hlsli`, and `LICENSE` from
https://github.com/GameTechDev/XeGTAO at commit
`a5b1686c7ea37788eeb3576b5be47f7c03db532c` (MIT).

Intel authors: Filip Strugar and Steve McCalla. Algorithm based on Jimenez et al.,
*Practical Realtime Strategies for Accurate Indirect Occlusion* (2016).

Our DX12 binding, input extraction, resource ownership and colour composition
live in `src/host_dx12/effects/`. `cmake/xegtao_shaders.cmake` reuses the existing
Windows SDK DXC discovery and embeds compiled shaders in the DLL. No runtime
shader files, Intel engine/sample framework or new redistributable DLL is needed.

Preserve these upstream files. Ship `LICENSE` and the attribution with packages
containing this effect. Changes belong in our wrapper or an explicitly repinned
upstream version, not an undocumented edit to Intel's algorithm.
