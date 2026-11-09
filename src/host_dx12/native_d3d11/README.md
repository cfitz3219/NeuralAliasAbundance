# Native DX11 / existing DX12 effects handoff

Owner: Astra, coordinated with Opus's `native11/input_provider.hpp`. This is the
first B2 implementation increment from `docs/WWII-40-50-FPS-PLAN-20261109.md`.
It is compiled separately and **not activated in the game or installer**.

`SharedTexture` owns a single-subresource image accessible from both native
D3D11 and the existing D3D12 backend, on the same physical adapter. `SharedFences`
provides two GPU timelines, with one producer per timeline. The game stays on
its own D3D11 device, context and swap chain. No D3D11On12 unwrap is involved.

The provider callback must run in actual immediate-context submission order at
the established scene boundary. For a frame slot:

1. Retain the selected native source/destination resources and copy camera facts.
   Copy colour and prepared depth into separate shared input images. Select the
   actual mip/slice; validate copy dimensions and format families. An MSAA
   resolve or depth-format conversion belongs before this transport step.
2. `SignalNative` submits the copies and signal. `WaitEffects` orders the DX12
   queue after that signal. Neither call waits for GPU completion on the CPU.
3. Record the existing motion/DLSS/effects code. Images enter/leave DX12 in
   COMMON. Transition inputs to the states the existing backend requires, and
   a separate output to UAV. Restore COMMON before the native consumer. Use
   `dlss::BeforeExecute` and `dlss::OnExecuted` around the actual submission.
4. `SignalEffects` follows submission. Only then may `WaitNative` order the
   native output copy. Copy/composite to the actual scene destination before
   the game's later readers/HUD, preserving its state and active subrectangle.
5. `SignalNative` after the copy supplies the native retirement value. Keep the
   slot's images, command allocator/list and external resource references alive
   until `Retired(native, effects)` confirms **both** GPU timelines.

Flush is used only to submit native handoff boundaries, not once per game draw.
The provider must suppress its own observer callbacks around our native copies
and restore any state changed by conversion/composition. Shared image formats
must be accepted by both APIs; failed sharing returns the real HRESULT, and does
not reinterpret depth as colour or change existing game texture flags. API copy
compatibility does not establish DLSS format support or a colour-space convention.

Remaining B2 work: frame-slot ownership/retirement, conversion/subrect handling,
coupling the provider's current-frame camera/motion contract to the existing
backend and delivering output. These primitives alone do not enable DLAA.

## Checks

This directory can be configured standalone with CMake. The
`native_d3d11_copy_contract_test` executable uses no GPU. It checks exact mip/slice
extent, depth format-copy rules, both-sided retirement, device-removal sentinels,
and rejection of uninitialized handoffs. Four cases pass locally.

`native_d3d11_shared_test` is a compiled, **not yet executed** hardware fixture.
It sends changing pixels from a real native D3D11 source through shared DX12
images and back to a native destination over 12 frames and two sizes, then
compares actual pixels. Its bounded completion wait and staging readback are
fixture-only. It is being held while Cort plays; neither its compilation nor
the CPU cases prove hardware interop or game performance.

Primary API references used (implementation is original):
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle
- https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-opensharedfence
- https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-wait
- https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/shared-heaps
- https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html
