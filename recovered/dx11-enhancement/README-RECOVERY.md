# Recovered: dx11-enhancement (Alien: Isolation DX11 mod), v0.11.1

The original project was lost when Cort's PC was wiped (it was never pushed to GitHub).
These files were rebuilt on 2026-11-04 from the review packet "Independent review packet:
DX11 Enhancement v0.11.1" (GEMINI-REVIEW.md) that Cort found. Every file below matched the
SHA-256 recorded in that packet, so they are byte-exact copies of the originals (CRLF kept;
`.gitattributes` stops git from changing line endings).

A later change set (`complete-changes.patch`: camera-cut detector, post-sharpen neural stage)
is applied in a separate commit so the original v0.11.1 bytes stay visible in history.

## What is here (the bridge)
- `src/core/bridge_protocol.hpp`: the 56-byte frame record shared by the 32-bit and 64-bit sides.
- `src/core/bridge_transport.hpp`: legacy keyed-mutex shared textures, adapter matching, helper process, shared-memory metadata.
- `src/core/neural_transport.hpp`: NT-handle shared textures and shared D3D11 fences.
- `src/core/neural_bridge.{hpp,cpp}`: DX11 (game) side: copies colour/depth/motion in, signals, waits (100 ms deadline), copies the result back.
- `src/core/neural_helper.cpp`: x64 DX12 helper: opens the shared handles, runs NGX, signals back.
- Tests for transport and bridge round trips, plus the Alien adapter and ReShade add-on glue.

## Not recovered (not in the packet)
`live_bridge.{hpp,cpp}` and `dlaa_helper.cpp` (the DLAA/DLSS bridge pair), `alien_isolation.cpp`,
`depth_preview`, `raw_snapshot`, `projection_verifier`, `capture_trigger`, `dlaa_preset`,
`frame_history` (a copy lives in src/core/), `diagnostics.cpp`, `addon.cpp` and most tests.
The DLAA pair used the same transport as the neural pair, so it can be rebuilt from it.

This folder is reference material. It is not built by the main CMakeLists.txt.
