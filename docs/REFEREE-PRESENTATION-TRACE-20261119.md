# Accepted presentation and regional flicker evidence

The Referee now joins three images from each frame in a pair: the color actually submitted to DLAA, its output, and the back buffer just before Present. The copies run on the verified submission queue and retire by fence. A receipt for the same swap chain and ordinal must report an accepted full Present before either native reader challenge or the new regional comparison can vote. Test, rejected, occluded and partial Present calls cannot supply that proof. An accepted flip-model Present is evidence of submission to DXGI. The Desktop Window Manager and the display can still present or discard queued frames later.

The extra preceding output and screen copies run at most once every 750 ms in production. There is no GPU or CPU wait attached to the game callback. The comparison uses the existing 128×72 GPU summaries. It checks adjacent frames of a continuous verified scene, filters out major changes, and looks for a regional brightness transition while most regions remain quiet. Two pairs must show a comparable reversal in the same neighbourhood. The Results script reports the earliest measured stage and the region, frame pair, and input and proof identities. Native off-by-a-few-row centered registration is eligible after the existing independent registration vote. A rescaled or geometrically unregistered comparison can still provide ordinary delivery measurements.

Each eligible replacement still goes through the existing short coded-pixel challenge before it can steer. The DX11 path restores the marked source after the reader. The DX12 challenge associates its sample with the submitted recording, queue, presentation ordinal and return status. A rejection cannot approve the replacement. For the periodic-light case, the relevant evidence is multiple independent regions in accepted frames, so a normal every-other-frame image cannot be mistaken for the challenge. Existing ordinary-path fallback and per-reader retirement apply. The new stage finding is diagnostic evidence. Its localized brightness comparison alone offers no alternate route and cannot authorize a settings change or a whole-route rollback.

## What the previous failures teach

- Shadow of War showed a repeating difference in the input and a separate false-motion effect in the output. Fable's round 13 replay held the recorded sequence steady and changed one input at a time. Removing background motion in same-parity replays reduced the measured slow field by 67–86 percent. Reaching the screen and having correct motion are independent questions.
- Shadow of War's scene arrived at 3440×1433 inside a 3440×1440 presentation. Registering the actual pixels changed the conclusion. Equal screen sizes or a visually close image are weak evidence for ownership of the final pass.
- Sniper's deferred submissions and late descriptors made recording-time guesses unreliable. Store the executed writer, actual resource view, context version and the queue handoff before drawing conclusions from a read. A queue-relative fence orders capture and measurement on supported paths.
- A full-frame average hides small reflective and HUD regions. Separate regions give a better way to probe known failure classes. A stable foreground plus an oscillating sky can leave whole-screen delivery and motion judgements positive.
- Scene changes, view-size changes, frame gaps and ordinary native illumination can all resemble flicker. Correlate independent regional evidence and preserve the established renderer when the evidence is ambiguous.

## Scope of the first implementation

This traces image stages and independently preflights observed readers. The native D3D11 and D3D12 Present/Present1 hooks supply exact acceptance. Streamline boundaries, frame-generated proxy frames, and obscure copies without a verified queue and ordinal still need their own accepted-frame link before they can provide this proof. The regional evidence can establish recurring localized luminance changes. It does not certify every fine edge, detect every geometric warp, expose all opaque compute dispatches, or identify a safe alternative that discovery has never seen. Automatically rating the quality of an approved alternative over held-out gameplay remains separate from proving that its reader can feed a presentation.

## Primary references

- [IDXGISwapChain::Present and queue semantics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
- [DXGI Present flags and nonblocking failure](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present)
- [D3D12 synchronization and queue fences](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)
- [NVIDIA DLSS input and frame guidance](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
