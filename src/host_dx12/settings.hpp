// Route A runtime settings: the single source the ini, the menu and the NGX
// override layer all agree on. Every field is a std::atomic so cross-thread
// reads are tear-free without locks on the hot path.
//
// Model (A19): the menu mirrors a game's DLSS page. `enabled` is the master
// switch (OFF = the game's own DLSS settings apply, NeuralAlias passes
// everything through). `dlss_mode` selects the render scale NVIDIA defines
// for each quality mode; NeuralAlias answers the engine's optimal-settings
// query with that size. Future features (frame generation, neural
// rendering) add their own fields here and their own menu sections.
//
// A30 (proposal S-001, additive) added a text preference under [overlay];
// A44 replaced it with `text_bold` (0 standard, 1 bold) and added the
// remembered window placement (win_x/y/w/h). A sharpness field is
// deliberately NOT added until a backend exists.
#pragma once

#include <atomic>
#include <cstdint>

namespace na::settings {

enum class DlssMode : uint32_t {
    DLAA = 0,         // 100% â€” render at output resolution
    Quality = 1,      // 66.7%
    Balanced = 2,     // 58%
    Performance = 3,  // 50%
    UltraPerformance = 4, // 33.3%
    Count = 5
};

// NVIDIA's published per-mode scale factors.
inline float ScaleForMode(DlssMode m)
{
    switch (m) {
    case DlssMode::DLAA: return 1.0f;
    case DlssMode::Quality: return 0.66666667f;
    case DlssMode::Balanced: return 0.58f;
    case DlssMode::Performance: return 0.5f;
    case DlssMode::UltraPerformance: return 0.33333334f;
    default: return 1.0f;
    }
}
inline const char* NameForMode(DlssMode m)
{
    switch (m) {
    case DlssMode::DLAA: return "DLAA";
    case DlssMode::Quality: return "Quality";
    case DlssMode::Balanced: return "Balanced";
    case DlssMode::Performance: return "Performance";
    case DlssMode::UltraPerformance: return "Ultra Performance";
    default: return "?";
    }
}

struct Settings {
    std::atomic<uint32_t> enabled{1};   // master switch (NeuralAlias controls DLSS)
    std::atomic<uint32_t> dlss_mode{0}; // DlssMode
    std::atomic<int32_t> preset{-1};    // -1 = let NVIDIA choose; 1..13 = A..M
    std::atomic<uint32_t> ray_reconstruction{0}; // honored only where the game uses RR
    std::atomic<uint32_t> overlay_open{0};
    std::atomic<uint32_t> text_bold{0}; // A44: 0 standard weight, 1 bold (replaces text_size)
    // A44: remembered menu window placement in client pixels (0 = default).
    std::atomic<int32_t> win_x{0}, win_y{0}, win_w{0}, win_h{0};
    // Neural Rendering (A30, [nr] section). Off by default; percentages 0..100.
    std::atomic<uint32_t> nr_enabled{0};
    std::atomic<uint32_t> nr_intensity{100};
    std::atomic<uint32_t> j2_sharpness{100}; // J2 DLAA sharpening 0..200: RCAS 0..100, + NIS 101..200 (0 = off; Cort's default 100)
    std::atomic<int32_t> j2_mip_bias_x100{-100}; // J2 texture detail: [j2] mip_bias x 100 (0, -50, -100); read at game start
    std::atomic<bool> j2_inline{false}; // legacy [j2] inline=1: same as route=inline (diagnostic override)
    std::atomic<uint32_t> j2_route{0};  // [j2] route: 0 auto (default), 1 between, 2 inline (diagnostic overrides)
    std::atomic<uint32_t> j2_preset{12};     // J2 DLAA: DLSS render preset ([j2] preset=L; 12 = L, 0 = NVIDIA default)
    std::atomic<uint32_t> nr_structure{100};
    std::atomic<uint32_t> nr_tone{0};
    std::atomic<uint32_t> nr_skin{100};
    std::atomic<uint32_t> nr_paper_white{200}; // nits, HDR chains only
    std::atomic<uint32_t> nr_model{0};         // 0 newer model (NVIDIA default: faster, subtler), 1 original model (stronger, slower)
    std::atomic<uint32_t> nr_hdr_mode{0};      // 0 auto (swap chain colour space), 1 force HDR10, 2 force SDR
    // A52: live hardware optical flow ([motion] section). Experimental, off by default.
    std::atomic<uint32_t> motion_enabled{0};
    std::atomic<uint32_t> motion_grid{4};      // 1, 2 or 4 (NVOFA grid)
    std::atomic<uint32_t> motion_perf{5};      // 5 best, 10 medium, 20 fast
    std::atomic<uint32_t> motion_downscale{2}; // A58: engine input = display / downscale (1, 2 or 4); 2 â‰ˆ 4x cheaper
    // A59: Frame Generation ([fg] section). Off by default.
    //  mode: 0 Fixed (always `multiplier`), 1 Dynamic (up to `multiplier`,
    //        chosen every frame to reach `target_fps`, or the display's
    //        refresh rate when target_fps is 0).
    //  multiplier: 2..6 (frames shown per rendered frame).
    std::atomic<int32_t> reflex_mode{1}; // 0 Off, 1 On (default), 2 On + Boost; engine adapter supplies timings
    std::atomic<uint32_t> fg_enabled{0};
    std::atomic<uint32_t> fg_mode{1};
    std::atomic<uint32_t> fg_preset{1}; // NVIDIA driver-profile request: A=1 (default), B=2; restart after change
    std::atomic<uint32_t> fg_multiplier{2};
    std::atomic<uint32_t> fg_target_fps{0};
    // A69: the freeze watchdog switched Frame Generation off at this time
    // (seconds since 1970, 0 = never). Shown in the menu until FG is re-enabled.
    std::atomic<uint32_t> fg_frozen_at{0};
};

Settings& Get();

// Deploy-time defaults ini (beside the proxy DLL; may be read-only).
void SetIniPath(const wchar_t* path);
// Where user changes are saved: %LOCALAPPDATA%\NeuralAlias\NeuralAliasSL.ini
const wchar_t* UserIniPath();
void Load();  // defaults ini, then user ini overrides when present
// Settings -> user ini (only our keys). Returns false when any write failed;
// the menu then says the choice is active for this session but not saved.
bool Save();
// Result of the most recent Save(): 0 never saved, 1 ok, 2 failed.
uint32_t LastSaveResult();

} // namespace na::settings
