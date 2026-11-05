NVIDIA Donut, MIT. Pinned commit 52efbe37d7f550ea1c5c7ee02b75404ec7c72c09.
https://github.com/NVIDIA-RTX/Donut/tree/52efbe37d7f550ea1c5c7ee02b75404ec7c72c09
Original files kept unmodified here. Our compute adaptation is src/host_dx12/effects/donut.hlsl.
Host wrapper changes: quarter-resolution histogram sampling, guarded black/nonfinite input, finite-kernel Gaussian normalization, optional bright extraction, preserved subrect/alpha/encoding and selectable Falcor curve. The source-linear result is an additional creative scene grade, not a replacement game display transform.
