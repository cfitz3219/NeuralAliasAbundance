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
    std::atomic<uint32_t> follow_game_resolution{0}; // preserve engine render size/PQ; override presets only
    std::atomic<int32_t> preset{-1};    // -1 = let NVIDIA choose; 1..13 = A..M
    std::atomic<int32_t> rr_preset{0};  // Ray Reconstruction: 0 = let NVIDIA choose; 4 D, 5 E, 6 F (its only usable presets)
    std::atomic<uint32_t> overlay_open{0};
    std::atomic<uint32_t> text_bold{0}; // A44: 0 standard weight, 1 bold (replaces text_size)
    std::atomic<uint32_t> fill_screen{0}; // render-small: keep the game windowed but borderless over the whole monitor (opt-in since the virtual screen makes borderless and fullscreen work directly)
    // A44: remembered menu window placement in client pixels (0 = default).
    std::atomic<int32_t> win_x{0}, win_y{0}, win_w{0}, win_h{0};
    // Neural Rendering (A30, [nr] section). Off by default; percentages 0..100.
    std::atomic<uint32_t> nr_enabled{0};
    // Optional Donut scene grade/Bloom; disabled by default, [donut].
    std::atomic<uint32_t> donut_tone{0},donut_bloom{0},donut_amount{100},donut_bias{800},donut_white{3000};
    std::atomic<uint32_t> donut_speed_up{1000},donut_speed_down{500},donut_curve{0};
    std::atomic<uint32_t> donut_bloom_amount{100},donut_bloom_sigma{16000},donut_bloom_threshold{1000};
    // Optional Intel XeGTAO contact shading; [xegtao], remembered per game.
    std::atomic<uint32_t> xegtao_enabled{0}, xegtao_amount{100}, xegtao_quality{2}, xegtao_radius_x1000{500};
    std::atomic<uint32_t> xegtao_denoise{1}, xegtao_radius_multiplier{1457}, xegtao_falloff{615}, xegtao_sample_distribution{2000};
    std::atomic<uint32_t> xegtao_thin_occluder{0}, xegtao_final_power{2200}, xegtao_depth_mip_offset{3300};
    std::atomic<uint32_t> nr_intensity{100};
    std::atomic<uint32_t> j2_sharpness{50}; // J2 sharpening, menu scale 0..100 = RCAS 0..200 (0 = off; 50 = standard RCAS full strength)
    // Render-small HUD: 0 = the HUD goes through DLSS with the scene; 1..100 = HUD pixels are
    // kept out of DLSS and upscaled by NIS at this sharpness. [j2] hud_nis.
    std::atomic<uint32_t> j2_hud_nis{50};
    std::atomic<uint32_t> j2_flow_loosen{50}; // J2 moving-object flow: 0 = the original acceptance rules, 100 = loosest (Cort 2026-11-03)
    std::atomic<uint32_t> j2_jitter_r2{0}; // jitter sequence: 0 Halton 2,3 (default), 1 R2; [j2] jitter_sequence; live
    std::atomic<uint32_t> aniso16{0}; // anisotropic filtering level on the game's surface textures: 0 off, 4, 8, 16; [j2] anisotropic; read at game start
    std::atomic<int32_t> j2_mip_bias_x100{-100}; // J2 texture detail: [j2] mip_bias x 100 (0, -50, -100); read at game start
    std::atomic<bool> j2_inline{false}; // legacy [j2] inline=1: same as route=inline (diagnostic override)
    std::atomic<uint32_t> j2_route{0};  // [j2] route: 0 auto (default), 1 between, 2 inline (diagnostic overrides)
    // J2 evidence capture ([j2] per game): frames per capture, hotkey (virtual-key code),
    // and an optional automatic capture N seconds after start (0 = only on request).
    std::atomic<uint32_t> j2_capture_frames{30};
    std::atomic<uint32_t> j2_capture_key{0x77}; // F8
    std::atomic<uint32_t> j2_capture_after_s{0};
    std::atomic<uint32_t> j2_capture_auto{0};        // automatic capture sessions per run (0 = off)
    std::atomic<uint32_t> j2_capture_cooldown_s{20}; // seconds between automatic captures
    std::atomic<uint32_t> j2_scale_mode{0}; // P4-4 slider route: DlssMode the game renders at ([j2] scale_mode; 0 = DLAA)
    // Remembered jitter conventions (na::jitter::Convention; 0 = not learned yet), per game in the
    // user ini ([j2] jitter_conv_flow / jitter_conv_final): the calibration starts from them.
    std::atomic<uint32_t> j2_jitter_conv_flow{0}, j2_jitter_conv_final{0};
    // [j2] jitter_shift: the colour DLSS gets is resampled by the declared jitter so the picture
    // does not shake (1 = on, -1 = on with the opposite sign, 0 = off). Cort 2026-11-04.
    std::atomic<int32_t> j2_jitter_shift{1};
    // [j2] viewport_jitter: real sub-pixel samples. The game's scene draws (live scene depth bound,
    // depth test on) get viewports moved by the jitter, and DLSS is told exactly that value; the
    // camera-matrix write and jitter_shift are bypassed while on (1 = on, -1 = on, declared with
    // the opposite sign, 0 = off: the camera-matrix route). Cort 2026-11-04. Loading the ini
    // turns it on unless the ini says otherwise; without a Load (tests) the camera-matrix route stays.
    std::atomic<int32_t> j2_viewport_jitter{0};
    // Draw replay ([j2] draw_replay, 1 = on): per-object motion from the game's own draws replayed with
    // stream output, fed as engine motion (Cort 2026-11-06, SE5 sweatshirt smear). Off without a Load.
    std::atomic<uint32_t> j2_draw_replay{0};
    std::atomic<uint32_t> j2_preset{255};    // J2 DLSS render preset ([j2] preset_v2; 255 = Automatic: L for DLAA/Quality, M for Balanced/Performance; 12 = L, 0 = NVIDIA default)
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
    std::atomic<uint32_t> fg_profile_override{0}; // per-deployment opt-in
    std::atomic<uint32_t> fg_preset{1}; // NVIDIA driver-profile request: A=1 (default), B=2, C=3; restart after change
    std::atomic<uint32_t> fg_multiplier{2};
    std::atomic<uint32_t> fg_target_fps{0};
    // A69: the freeze watchdog switched Frame Generation off at this time
    // (seconds since 1970, 0 = never). Shown in the menu until FG is re-enabled.
    std::atomic<uint32_t> fg_frozen_at{0};
    // The GAME's own frame generation (Streamline DLSS-G), [native_fg] section:
    // requests applied to the game's slDLSSGSetOptions by the SL boundary.
    //  mode: 0 the game's setting (default, no change), 1 Off, 2 On, 3 Dynamic
    //  frames: generated frames per real frame (1 = 2x ... 5 = 6x); 0 = the game's
    std::atomic<uint32_t> native_fg_mode{0};
    std::atomic<uint32_t> native_fg_frames{0};
};

Settings& Get();
enum class NrWriteSource : uint32_t { DefaultIni=1, UserIni=2, Overlay=3, SafeStartup=4 };
struct NrWrite {
    uint64_t sequence=0, tick_ms=0;
    uint32_t thread=0, before=0, after=0;
    NrWriteSource source=NrWriteSource::DefaultIni;
    uint32_t input_flags=0; // observed: mouse-release1, pad-A2, Enter4, Space8; not inferred intent
};
struct NrWriteHistory { NrWrite entries[16]{}; uint32_t count=0; };
// Runtime NR-setting writers use this for a bounded in-memory receipt. No
// file I/O here; the existing status worker emits the records later.
void SetNrEnabled(bool enabled,NrWriteSource source,uint32_t input_flags=0);
NrWriteHistory GetNrWrites();

// Deploy-time defaults ini (beside the proxy DLL; may be read-only).
void SetIniPath(const wchar_t* path);
// Where user changes are saved: %LOCALAPPDATA%\NeuralAlias\NeuralAliasSL.ini
const wchar_t* UserIniPath();
void Load();  // defaults ini, then user ini overrides when present
// Event log: emits one "setting" event per changed persisted field since the last call.
void WatchChanges(const char* source = nullptr);
// One line with every effective J2/DLSS value and where it came from (user / game / builtin).
const char* EffectiveSummary();
// Settings -> user ini (only our keys). Returns false when any write failed;
// the menu then says the choice is active for this session but not saved.
bool Save();
// Production dispatch is queued to the existing init/control worker after
// renderer authority. Standalone hosts/tests retain synchronous Save().
using SaveDispatcher=bool(*)();
void DeferForRenderer(SaveDispatcher dispatcher);
bool BeginRendererSession(bool journal_allowed=true); // worker only; returns shared-persistence ownership
void PublishRendererReady();
bool EffectsReady();
bool CanPersist();
bool SaveFromWorker();
void SetSavePending();
void FlushSessionFailure();
// Result of most recent Save():0 never,1 saved,2 failed/read-only,3 queued.
uint32_t LastSaveResult();
// Startup/exit worker calls only. No settings file is rewritten by safe start.
void BeginSession(const wchar_t* marker_path=nullptr);
void EndSession(uint32_t exit_code);
void MarkSessionFailed(const char* reason);
bool SafeStart();
const char* SafeStartReason();
bool SafeProfileRestoreRequired();

} // namespace na::settings
