// Native controls. Change global definitions and Reload; only the neural helper restarts.
#ifndef DX11_NR_MODEL
#define DX11_NR_MODEL 0
#endif
#ifndef DX11_NR_STYLE
#define DX11_NR_STYLE 0
#endif
#ifndef DX11_NR_INTENSITY
#define DX11_NR_INTENSITY 1.0
#endif
#ifndef DX11_NR_STRUCTURE
#define DX11_NR_STRUCTURE 1.0
#endif
#ifndef DX11_NR_TONE
#define DX11_NR_TONE 0.0
#endif
#ifndef DX11_NR_SKIN
#define DX11_NR_SKIN 1.0
#endif
#ifndef DX11_NR_AUTO_SKIN
#define DX11_NR_AUTO_SKIN 1
#endif
#if DX11_NR_MODEL < 0 || DX11_NR_MODEL > 3 || DX11_NR_STYLE < 0 || DX11_NR_STYLE > 2 || DX11_NR_AUTO_SKIN < 0 || DX11_NR_AUTO_SKIN > 1
#error Neural model must be 0..3, style 0..2, auto skin 0 or 1.
#endif
void Neural_ControlCS(uint3 id : SV_DispatchThreadID) {}
technique NeuralRendering <
    ui_label = "Neural rendering (experimental)";
    ui_tooltip = "Requires DLAA. Global definitions: DX11_NR_MODEL 0=runtime choice, 1/2/3=requested hints (no difference observed in this runtime); STYLE 0/1/2=experimental profiles; INTENSITY=overall strength; STRUCTURE=fine detail; TONE=broad lighting/color (0 preserves original tone); SKIN=skin detail (-1 follows structure); AUTO_SKIN=skin detection 0/1 (not HUD protection). Strength range 0..2, start at 1. Reload after edits; only neural helper restarts. Game HUD is included and NOT protected by a UI mask. Status in DLAA status. Uncheck to compare with DLAA alone.";
    dx11_nr_model = DX11_NR_MODEL;
    dx11_nr_style = DX11_NR_STYLE;
    dx11_nr_intensity = 1.0 * DX11_NR_INTENSITY;
    dx11_nr_structure = 1.0 * DX11_NR_STRUCTURE;
    dx11_nr_tone = 1.0 * DX11_NR_TONE;
    dx11_nr_skin = 1.0 * DX11_NR_SKIN;
    dx11_nr_auto_skin = DX11_NR_AUTO_SKIN;
> {
    pass {
        ComputeShader = Neural_ControlCS<1, 1>;
        DispatchSizeX = 1;
        DispatchSizeY = 1;
    }
}
