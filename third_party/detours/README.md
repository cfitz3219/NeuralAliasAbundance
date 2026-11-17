# Microsoft Detours (vendored, unmodified)

- Source: microsoft/Detours, tag `v4.0.1`, commit e4bfd6b03e50de46b47abfbd1e46b384f0c5f833.
- License: MIT (`LICENSE.md`).
- Contents: the library sources of `src/` (detours, modules, disasm + offline disassemblers, image, creatwth,
  uimports), byte-for-byte. Built by CMake on MSVC only.
- Used only by `src/host_dx12/hooking/vtable_hook.cpp` as an opt-in engine (`[hooking] engine=detours`);
  MinHook stays the default.

Do not edit these files. Fixes go in our hooking core, or a re-vendor at a new pin recorded here.
