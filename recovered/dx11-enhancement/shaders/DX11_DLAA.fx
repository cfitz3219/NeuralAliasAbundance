// UI bridge to the native addon. Reconstruction runs before game post-processing.
// This compute pass does not read or change the picture.
#ifndef DX11_DLAA_PRESET
#define DX11_DLAA_PRESET 0
#endif

#if DX11_DLAA_PRESET != 0 && (DX11_DLAA_PRESET < 10 || DX11_DLAA_PRESET > 13)
#error DX11_DLAA_PRESET must be 0 (Auto), 10 (J), 11 (K), 12 (L), or 13 (M). Older presets are removed, deprecated or reserved by NVIDIA.
#endif

void DLAA_ControlCS(uint3 id : SV_DispatchThreadID) {}

technique DLAA <
    ui_label = "DLAA";
    ui_tooltip = "Native-resolution NVIDIA anti-aliasing. Requires the DX11 Enhancement addon. Edit global preprocessor definitions: DX11_DLAA_PRESET = 0 (Auto), 10 (J), 11 (K), 12 (L), 13 (M). Start with Auto. Status is in the DLAA status panel. Effect order does not change its processing stage.";
    dx11_dlaa_preset = DX11_DLAA_PRESET;
> {
    pass {
        ComputeShader = DLAA_ControlCS<1, 1>;
        DispatchSizeX = 1;
        DispatchSizeY = 1;
    }
}
