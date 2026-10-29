#include "host_dx12/adapters/tlou1/apply.hpp"
// Route A menu content (A30 redesign): the Picture page (status, master
// switch, mode cards, preset), the Enhancements rows (Ray Reconstruction;
// Neural Rendering and Frame Generation placeholders) and the Details rows
// (Connection, Save pictures for analysis). The host draws the chrome.
// Copy rules: docs/UI-STYLE.md (plain language, one sentence per instruction,
// requested and confirmed state kept distinct).
#include "host_dx12/overlay/route_a_panel.hpp"
#include "providers/discovery/upscaler_menu.hpp"

#include <shlobj.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <ctime>

#include <imgui.h>

#include "host_dx12/capture/frame_capture.hpp"
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/fg/frame_gen.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/fg/preset_profile.hpp"
#include "host_dx12/motion/live_flow.hpp"
#include "host_dx12/overlay/presenter.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/nr/nr_pass.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include "host_dx12/overlay/overlay.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/latency/nvidia_latency.hpp"

namespace na::overlay {
namespace {

using na::settings::DlssMode;

// ---- Requested vs confirmed (DESIGN.md "State contract") ----
// A click records the request and the evidence counters at that moment.
// "Active" needs, after the request: a successful DLSS feature creation
// whose width AND height match what we asked for, at least one evaluate
// since, and evaluates still arriving. Nothing is marked active from the
// act of saving a preference.
struct Request {
    uint64_t sr_creates_at = 0;
    uint64_t evaluates_at = 0;
};
Request g_req;
wchar_t g_capture_dir[MAX_PATH] = {};
uint64_t g_apply_ok_at = 0, g_apply_fail_at = 0; // A46: counters when Apply was clicked
bool g_apply_clicked = false;
constexpr uint64_t kInterruptedAfterPresents = 240; // ~4 s at 60 fps

void note_request()
{
    na::ngx::Stats s = na::ngx::GetStats();
    g_req.sr_creates_at = s.sr_creates;
    g_req.evaluates_at = s.evaluates;
}

void requested_size(const na::ngx::OptimalStats& opt, int mode, unsigned* rw, unsigned* rh)
{
    float scale = na::settings::ScaleForMode(static_cast<DlssMode>(mode));
    *rw = opt.last_user_w;
    *rh = opt.last_user_h;
    if (mode != 0) {
        *rw = ((static_cast<unsigned>(opt.last_user_w * scale + 0.5f)) + 7u) & ~7u;
        *rh = ((static_cast<unsigned>(opt.last_user_h * scale + 0.5f)) + 7u) & ~7u;
    }
}

enum class Conn { NoDlss, Interrupted, Connected };

Conn connection(const na::ngx::Stats& ngx)
{
    if (!ngx.module_attached || ngx.evaluates == 0) return Conn::NoDlss;
    if (na::overlay::PresentsSinceLastEvaluate() > kInterruptedAfterPresents) return Conn::Interrupted;
    return Conn::Connected;
}

// True when the game created its DLSS feature after our request with the
// requested size and has evaluated it since.
bool confirmed(const na::ngx::Stats& ngx, unsigned rw, unsigned rh)
{
    if (ngx.sr_creates <= g_req.sr_creates_at) return false;
    if (ngx.evaluates <= g_req.evaluates_at) return false;
    if (ngx.last_sr_in_w == 0xFFFFFFFFu || ngx.last_sr_in_h == 0xFFFFFFFFu) return false;
    return ngx.last_sr_in_w == rw && ngx.last_sr_in_h == rh;
}

static const char* kModes[] = {"DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
static const char* kModeLine1[] = {"Full resolution", "More detail", "Middle ground", "More speed", "Maximum speed"};
static const char* kModeLine2[] = {"Highest GPU cost", "Less GPU work", "Detail & speed", "Less fine detail", "Lowest input resolution"};
static const char* kModeHelp[] = {
    "DLAA smooths edges while keeping the game's full drawing resolution.",
    "Quality draws fewer pixels and reconstructs the full-size picture.",
    "Balanced reduces drawing resolution further to favor performance.",
    "Performance prioritizes speed with a larger reduction in drawing resolution.",
    "Ultra Performance draws at one third of each output dimension. Preset L is the recommended starting point."};
// Presets offered: only the ones NVIDIA's SDK marks as usable (A42). Aâ€“D are
// removed, E/F deprecated, G/H/I/N/O revert to default; J and K are the
// current models for DLAA/Quality/Balanced, L and M are the defaults for
// Ultra Performance / Performance. Values are the SDK's numbers.
static const char* kPresets[] = {"Automatic", "J", "K", "L", "M"};
static const int kPresetValues[] = {-1, 10, 11, 12, 13};
static int preset_index(int value)
{
    for (int i = 1; i < 5; ++i)
        if (kPresetValues[i] == value) return i;
    return 0;
}
// Ray Reconstruction's own presets: the SDK marks only D, E and F usable.
static const char* kRrPresets[] = {"Automatic", "D (transformer)", "E (latest transformer)", "F (Ray Reconstruction 2)"};
static const int kRrPresetValues[] = {0, 4, 5, 6};
static int rr_preset_index(int value)
{
    for (int i = 1; i < 4; ++i)
        if (kRrPresetValues[i] == value) return i;
    return 0;
}

void DrawPicture()
{
    using namespace na::ui;
    na::settings::Settings& st = na::settings::Get();
    na::ngx::Stats ngx = na::ngx::GetStats();
    na::ngx::OptimalStats opt = na::ngx::GetOptimalStats();
    char buf[200];

    auto engine_apply = na::tlou1::GetApplyStatus();
    bool engine_busy = engine_apply.stage == na::tlou1::Stage::Queued || engine_apply.stage == na::tlou1::Stage::Entered || engine_apply.stage == na::tlou1::Stage::Rebuilding;
    bool enabled = st.enabled.load() != 0;
    int mode = static_cast<int>(st.dlss_mode.load());
    if (mode < 0 || mode >= static_cast<int>(DlssMode::Count)) mode = 0;
    unsigned rw = 0, rh = 0;
    if (opt.last_user_w && opt.last_user_h) requested_size(opt, mode, &rw, &rh);
    Conn conn = connection(ngx);
    bool blocked = conn != Conn::Connected;

    // Status banner.
    int state = 2;
    const char* text = "";
    if (conn == Conn::NoDlss) {
        text = "Waiting for DLSS. Enable it in the game's video settings.";
    } else if (conn == Conn::Interrupted) {
        text = "Connection interrupted. Active picture settings cannot be confirmed.";
    } else if (!enabled) {
        if (ngx.sr_creates > g_req.sr_creates_at) {
            state = 1;
            text = "NeuralAlias is off. The game's own DLSS settings are in use.";
        } else {
            text = "Returning control to the game. Change the DLSS mode once in the game's settings to apply.";
        }
    } else if (engine_busy) {
        text = engine_apply.reason;
    } else if (engine_apply.stage == na::tlou1::Stage::Failed && engine_apply.choice.mode == static_cast<uint32_t>(mode) && engine_apply.choice.width == rw && engine_apply.choice.height == rh) {
        text = engine_apply.reason;
    } else if (rw && confirmed(ngx, rw, rh)) {
        state = 1;
        snprintf(buf, sizeof(buf), "%s is active.", kModes[mode]);
        text = buf;
    } else {
        snprintf(buf, sizeof(buf), "%s selected. Choose Apply to update the picture.",
                 kModes[mode]);
        text = buf;
    }
    StatusBanner(state, text);

    // Master switch.
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::BeginDisabled(engine_busy || (blocked && conn == Conn::NoDlss));
    BeginRow("Use NeuralAlias", "Choose your picture settings here instead of in the game.", SwitchWidth());
    if (Switch("master", &enabled)) {
        st.enabled.store(enabled ? 1u : 0u);
        note_request();
        na::settings::Save();
    }
    EndRow();
    ImGui::EndDisabled();

    // Mode cards.
    SectionTitle("Picture mode");
    ImGui::BeginDisabled(!enabled || blocked || engine_busy);
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = avail < S(640) ? 2 : 4; // A44: reflow by available width only
    float gap = S(9);
    float card_w = (avail - gap * (cols - 1)) / cols;
    for (int i = 0; i < static_cast<int>(DlssMode::Count); ++i) {
        if (i % cols != 0) ImGui::SameLine(0.0f, gap);
        if (ModeCard(kModes[i], kModeLine1[i], kModeLine2[i], i == mode, card_w) && i != mode) {
            mode = i;
            st.dlss_mode.store(static_cast<uint32_t>(mode));
            note_request();
            na::settings::Save();
        }
    }
    ImGui::EndDisabled();
    Hint(kModeHelp[mode]);

    // Preset + Apply. (A46: the disabled Sharpness row is gone until a
    // sharpening backend exists; a dead control only invites clicks.)
    ImGui::Dummy(ImVec2(0, S(4)));
    BeginGroupBox("picture-group");
    {
        ImGui::BeginDisabled(!enabled || blocked || engine_busy);
        // The game's upscaler decides which preset list applies: Ray Reconstruction
        // (feature 13) has its own keys and only presets D/E/F.
        const bool rr_upscaler = ngx.rr_seen != 0 && ngx.sr_creates == 0;
        int sel = 0;
        if (rr_upscaler) {
            sel = rr_preset_index(st.rr_preset.load());
            BeginRow("Ray Reconstruction preset",
                     "This game upscales with Ray Reconstruction, which has its own models.\nLeave on Automatic unless one looks better. E is required by games that use its depth-of-field guide.",
                     DropdownWidth());
            if (Combo("rr_preset", &sel, kRrPresets, 4, DropdownWidth())) {
                st.rr_preset.store(kRrPresetValues[sel]);
                note_request();
                na::settings::Save();
            }
        } else {
            sel = preset_index(st.preset.load());
            BeginRow("DLSS preset",
                     "Leave on Automatic unless a particular preset looks better.\nJ and K suit DLAA, Quality and Balanced; L and M are meant for Performance modes.",
                     DropdownWidth());
            if (Combo("preset", &sel, kPresets, 5, DropdownWidth())) {
                st.preset.store(kPresetValues[sel]);
                note_request();
                na::settings::Save();
            }
        }
        EndRow();
        ImGui::EndDisabled();
        if (ngx.preset_rejected) {
            // The runtime refused a preset hint; the detour already reset the
            // setting in memory (A42). Persist it and say so.
            static bool saved = false;
            if (!saved) { na::settings::Save(); saved = true; }
            StatusLine(2, "NVIDIA's software refused the requested preset, so Automatic is used instead.");
        } else if (sel == 0) {
            Hint("Automatic lets NVIDIA's software choose.");
        } else {
            snprintf(buf, sizeof(buf), "Preset %s requested. Whether NVIDIA's software uses it is not confirmed.",
                     rr_upscaler ? kRrPresets[sel] : kPresets[sel]);
            Hint(buf);
        }
        if (ngx.quality_rejected)
            StatusLine(2, "NVIDIA's software refused the requested mode for this size; the game's own mode is in use.");
        RowDivider();
        bool size_matches = rw && ngx.last_sr_in_w == rw && ngx.last_sr_in_h == rh;
        static bool engine_clicked = false;
        static bool admission_failed = false;
        static bool rebuild_clicked = false;   // generic: window nudge so the game rebuilds its DLSS
        static bool rebuild_refused = false;
        static uint32_t rebuild_attempts_at = 0;
        // Any game upscaler counts: plain DLSS (feature 1) or Ray Reconstruction (13).
        const bool have_upscaler = ngx.sr_creates != 0 || ngx.rr_seen != 0;
        // A game with a hand-written engine adapter (TLOU) changes its own picture
        // mode; every other game gets the generic rebuild (A63/A64): the window is
        // nudged by one pixel and back, the engine re-creates its render targets and
        // asks DLSS for the size again, and the armed confirmation reports whether
        // the new size was actually rendered.
        const bool engine_adapter = na::tlou1::GetApplyStatus().available;
        ImGui::BeginDisabled(!enabled || blocked || !have_upscaler || engine_busy);
        BeginRow("Apply changes", "Update the picture mode and preset together.", ButtonWidth("Apply"));
        if (Button("Apply")) {
            admission_failed = false;
            engine_clicked = rebuild_clicked = rebuild_refused = false;
            g_apply_clicked = false;
            // Plain DLSS can take a new PRESET in place (A46). A size change, or any
            // change while the game uses Ray Reconstruction, needs the game to
            // re-create its DLSS feature.
            const bool in_place = size_matches && ngx.sr_creates != 0 && !rr_upscaler;
            if (in_place) {
                na::ngx::RequestReapply();
                g_apply_ok_at = ngx.reapplies;
                g_apply_fail_at = ngx.reapply_failures;
                g_apply_clicked = true;
            } else if (engine_adapter) {
                engine_clicked = true;
                admission_failed = !na::tlou1::RequestApply({static_cast<uint32_t>(mode), rw, rh, st.preset.load()});
            } else {
                rebuild_clicked = true;
                rebuild_attempts_at = na::overlay::GetPictureRebuild().attempts;
                rebuild_refused = !na::overlay::RequestPictureRebuild(rw, rh);
            }
        }
        EndRow();
        ImGui::EndDisabled();
        if (!have_upscaler && enabled)
            Hint("Apply becomes available once the game has started DLSS.");
        if (rebuild_clicked) {
            const auto pr = na::overlay::GetPictureRebuild();
            const auto check = na::ngx::GetSizeConfirmation();
            if (rebuild_refused)
                StatusLine(2, "A previous Apply is still in progress. Try again in a moment.");
            else if (pr.state == 4)
                StatusLine(2, pr.reason[0] ? pr.reason : "The picture could not be rebuilt.");
            else if (pr.attempts == rebuild_attempts_at || pr.state == 1 || pr.state == 2)
                StatusLine(0, "Asking the game to rebuild its picture...");
            else if (check.id && check.id == pr.confirm_id && check.state == 3)
                StatusLine(1, "Applied: the game is now rendering at the selected mode.");
            else if (check.id && check.id == pr.confirm_id && check.state == 4) {
                snprintf(buf, sizeof(buf), "This game did not rebuild its DLSS when asked (it rendered %ux%u). Change the DLSS or ray-tracing setting once in the game's own menu to apply.",
                         check.last_w, check.last_h);
                StatusLine(2, buf);
            } else
                StatusLine(0, "Picture rebuilt; checking the new size...");
        }
        if (engine_clicked) {
            auto progress = na::tlou1::GetApplyStatus();
            if (admission_failed)
                StatusLine(2, progress.available ? "Another change is in progress, or the selection changed. Try Apply again." : progress.reason);
            else if (progress.choice.mode != st.dlss_mode.load() || progress.choice.preset != st.preset.load())
                StatusLine(2, "Selection changed. Choose Apply to update the picture.");
            else
                StatusLine(progress.stage == na::tlou1::Stage::Confirmed ? 1 :
                           (progress.stage == na::tlou1::Stage::Failed || progress.stage == na::tlou1::Stage::Cancelled ? 2 : 0), progress.reason);
        } else if (ngx.reapply_pending)
            StatusLine(0, "Applying the preset at the next frame.");
        else if (g_apply_clicked && ngx.reapplies > g_apply_ok_at)
            StatusLine(1, "Preset applied. The picture mode already matches your selection.");
        else if (g_apply_clicked && ngx.reapply_failures > g_apply_fail_at)
            StatusLine(2, "The preset could not be applied. The previous setup is still in use.");
        if (BeginDetails("Apply details")) {
            auto progress = na::tlou1::GetApplyStatus();
            auto check = na::ngx::GetSizeConfirmation();
            Hint(progress.available ? (progress.stage == na::tlou1::Stage::Confirmed ? "The requested resolution was observed with coherent inputs. Visual quality is checked separately." : "Uses this game's graphics-settings update and waits for matching rendered frames.") : progress.reason);
            snprintf(buf, sizeof(buf), "Request %llu; engine updates %llu; rebuilds requested %llu; thread %u.",
                     static_cast<unsigned long long>(progress.request_id), static_cast<unsigned long long>(progress.frame_calls),
                     static_cast<unsigned long long>(progress.triggers), progress.trigger_thread);
            Hint(buf);
            if (progress.request_id && progress.request_id == check.id) {
                snprintf(buf, sizeof(buf), "Requested %ux%u; observed %ux%u; %u of %u consecutive frames; result %u/%u.",
                         check.req_w, check.req_h, check.last_w, check.last_h, check.streak, check.need, check.state, check.reason);
                Hint(buf);
            }

        }

    }
    EndGroupBox();

    if (BeginDetails("How this affects the picture")) {
        if (rw) {
            snprintf(buf, sizeof(buf), "Requested drawing size %ux%u, shown at %ux%u.", rw, rh,
                     opt.last_user_w, opt.last_user_h);
            Hint(buf);
        } else {
            Hint("No drawing size has been requested yet; the game has not asked about DLSS sizes.");
        }
        if (ngx.sr_creates && ngx.last_sr_in_w != 0xFFFFFFFFu) {
            snprintf(buf, sizeof(buf), "The game last created DLSS drawing at %ux%u for a %ux%u picture.",
                     ngx.last_sr_in_w, ngx.last_sr_in_h, ngx.last_sr_out_w, ngx.last_sr_out_h);
            Hint(buf);
        } else {
            Hint("The game has not created a DLSS pass yet, so no confirmed drawing size exists.");
        }
        Hint("Changing a saved preference does not mean the game has applied it. The status above reports that separately.");
    }
}

// Neural Rendering (A30, live pass phase 3). The switch is real; the pass
// reports its own state and reason. Sliders are percentages, applied live.
void DrawNeuralRenderingRows()
{
    using namespace na::ui;
    na::settings::Settings& st = na::settings::Get();
    na::nr::Stats nr = na::nr::GetStats();
    char buf[200];
    bool on = st.nr_enabled.load() != 0;
    bool can = nr.runtime_present != 0;
    const char* help = can ? "AI processing for the finished picture. Costs graphics-card time; see below."
                           : "AI processing for the finished picture.\nWaiting for the DLSS connection.";
    BeginRow("Neural Rendering", help, SwitchWidth());
    ImGui::BeginDisabled(!can);
    if (Switch("nr", &on)) {
        st.nr_enabled.store(on ? 1u : 0u);
        if (on) na::nr::ResetLatch();
        na::settings::Save();
    }
    ImGui::EndDisabled();
    EndRow();
    if (on) {
        int state = nr.latched_off ? 2 : (nr.active ? 1 : 0);
        StatusLine(state, nr.reason);
        if (nr.gpu_samples) {
            snprintf(buf, sizeof(buf), "Graphics-card time for Neural Rendering: %.1f ms per frame (average %.1f ms).",
                     nr.last_ms, nr.avg_ms);
            Hint(buf);
        }
        auto pct_row = [&](const char* label, const char* h, std::atomic<uint32_t>& field) {
            int v = static_cast<int>(field.load());
            RowDivider();
            BeginRow(label, h, S(220));
            ImGui::SetNextItemWidth(S(220));
            PushLabel();
            ImGui::PushID(label);
            if (ImGui::SliderInt("##s", &v, 0, 100, "%d%%")) field.store(static_cast<uint32_t>(v));
            if (ImGui::IsItemDeactivatedAfterEdit()) na::settings::Save();
            ImGui::PopID();
            PopFont();
            EndRow();
        };
        pct_row("Intensity", "How strongly the picture is processed.", st.nr_intensity);
        pct_row("Structure", "Fine detail and texture emphasis.", st.nr_structure);
        pct_row("Tone", "Local contrast. Off at zero.", st.nr_tone);
        pct_row("Skin", "Protects skin from over-processing.", st.nr_skin);
        RowDivider();
        // NVIDIA's runtime (310.9.x) carries two models, chosen at creation by
        // DLSSNR.Hint.Render.Preset: 2 = the newer default, 1 = the original.
        static const char* kModels[] = {"Newer (faster, subtler)", "Original (stronger, slower)"};
        int model = static_cast<int>(st.nr_model.load());
        BeginRow("Model", "NVIDIA ships two Neural Rendering models. The original changes the picture more and costs roughly twice the time.",
                 DropdownWidth());
        if (Combo("nrmodel", &model, kModels, 2, DropdownWidth())) {
            st.nr_model.store(static_cast<uint32_t>(model));
            na::settings::Save();
        }
        EndRow();
        RowDivider();
        static const char* kHdrModes[] = {"Automatic", "HDR picture", "Standard picture"};
        int hm = static_cast<int>(st.nr_hdr_mode.load());
        BeginRow("Picture type", "Automatic follows the game's HDR setting. Change only if the picture looks wrong.",
                 DropdownWidth());
        if (Combo("hdrmode", &hm, kHdrModes, 3, DropdownWidth())) {
            st.nr_hdr_mode.store(static_cast<uint32_t>(hm));
            na::settings::Save();
        }
        EndRow();
        if (nr.hdr || hm == 1) {
            int pw = static_cast<int>(st.nr_paper_white.load());
            RowDivider();
            BeginRow("HDR brightness reference", "Brightness of white in the game's HDR settings (nits).", S(220));
            ImGui::SetNextItemWidth(S(220));
            PushLabel();
            if (ImGui::SliderInt("##pw", &pw, 80, 1000, "%d")) st.nr_paper_white.store(static_cast<uint32_t>(pw));
            if (ImGui::IsItemDeactivatedAfterEdit()) na::settings::Save();
            PopFont();
            EndRow();
        }
    }
}

// A52: live hardware optical flow (experimental, off by default). Produces
// the motion field the universal pipeline needs for games without motion
// vectors; in a DLSS game it is only measured here, nothing consumes it yet.
void DrawMotionRow()
{
    using namespace na::ui;
    na::settings::Settings& st = na::settings::Get();
    na::motion::Stats ms = na::motion::GetStats();
    char buf[200];
    bool on = st.motion_enabled.load() != 0;
    BeginRow("Motion analysis (experimental)",
             "Measures how the picture moves using NVIDIA's optical-flow hardware. Groundwork for games without their own motion data; it changes nothing on screen and costs graphics-card time.",
             SwitchWidth());
    if (Switch("motion", &on)) {
        st.motion_enabled.store(on ? 1u : 0u);
        if (on) na::motion::ResetLatch();
        na::settings::Save();
    }
    EndRow();
    if (on) {
        StatusLine(ms.latched_off ? 2 : (ms.active ? 1 : 0), ms.reason);
        if (ms.frames) {
            snprintf(buf, sizeof(buf), "Motion fields produced: %llu at %ux%u (grid %u), engine time %.1f ms per frame (average %.1f ms).",
                     static_cast<unsigned long long>(ms.frames), ms.width, ms.height, ms.grid, ms.flow_ms, ms.avg_ms);
            Hint(buf);
        }
    }
}

// A59 Frame Generation: NVIDIA's DLSS Frame Generation driven by NeuralAlias
// (the game never asked for it). Off / Fixed / Dynamic with a multiplier.
void DrawFrameGenerationRow()
{
    using namespace na::ui;
    na::settings::Settings& st = na::settings::Get();
    na::ngx::Stats ngx = na::ngx::GetStats();
    na::fg::Stats fs = na::fg::GetStats();
    na::present::Stats pr = na::present::GetStats();
    char buf[240];
    bool on = st.fg_enabled.load() != 0;
    bool possible = na::present::Enabled();
    const char* help = ngx.fg_seen
        ? "Creates extra pictures between rendered frames.\nThis game runs NVIDIA's own Frame Generation; switching this on as well is not recommended."
        : "Creates extra pictures between rendered frames using NVIDIA's Frame Generation. Smoother motion; the delay between your input and the picture grows a little.";
    BeginRow("Frame Generation", possible ? help : "Creates extra pictures between rendered frames.\nNot available on this route.", SwitchWidth());
    ImGui::BeginDisabled(!possible);
    if (Switch("fg", &on)) {
        st.fg_enabled.store(on ? 1u : 0u);
        if (on) { na::fg::ResetLatch(); st.fg_frozen_at.store(0); } // A69: re-enabling clears the safe-mode note
        na::settings::Save();
    }
    ImGui::EndDisabled();
    EndRow();
    RowDivider();
    int preset_choice = static_cast<int>(st.fg_preset.load()) - 1;
    if (preset_choice < 0 || preset_choice > 2) preset_choice = 0;
    const char* presets[] = {"Preset A", "Preset B", "Preset C (Performance)"};
    BeginRow("Frame Generation preset", "Chooses NVIDIA's driver model request for this game. Restart the game after changing it.", DropdownWidth());
    if (Combo("fg_preset", &preset_choice, presets, 3, DropdownWidth())) {
        st.fg_preset.store(static_cast<uint32_t>(preset_choice + 1));
        na::settings::Save(); na::fg::preset::Request(static_cast<uint32_t>(preset_choice + 1));
    }
    EndRow();
    const auto preset_status = na::fg::preset::GetStatus();
    if (preset_status.pending) Hint("Saving the driver preset...");
    else if (!preset_status.available) Hint("Preset saved for the next supported game launch.");
    else if (preset_status.result != 0) Hint("The driver did not accept the preset. See Details.");
    else if (preset_status.restart) Hint("Preset saved. Restart the game to use the new request.");
    else Hint("Driver preset request saved; the runtime decides which model is supported.");
    if (uint32_t frozen = st.fg_frozen_at.load()) { // A69: the watchdog switched it off after a freeze
        time_t tt = static_cast<time_t>(frozen);
        struct tm lt{};
        localtime_s(&lt, &tt);
        snprintf(buf, sizeof(buf), "Frame Generation was switched off after the game froze on %04d-%02d-%02d at %02d:%02d. The freeze was recorded for Claude (status log and a dump file). Switch it on again only if asked to.",
                 lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
        StatusLine(2, buf);
    }
    if (!on || !possible) return;
    static const char* kFgModes[] = {"Fixed", "Dynamic"};
    int mode = st.fg_mode.load() ? 1 : 0;
    BeginRow("Mode", "Fixed always shows the chosen number of pictures per rendered frame. Dynamic picks the multiplier for you: at least 2x, more when needed to reach your display's refresh rate, up to the maximum. Frame Generation never switches itself off while it is on; if the pictures exceed what the display can show, the game's own V-Sync setting decides (off: tearing above the refresh rate; on: the game is paced down, as in any game with native Frame Generation).", DropdownWidth());
    if (Combo("fgmode", &mode, kFgModes, 2, DropdownWidth())) {
        st.fg_mode.store(mode ? 1u : 0u);
        na::settings::Save();
    }
    EndRow();
    static const char* kMults[] = {"2x", "3x", "4x", "5x", "6x"};
    int mult = static_cast<int>(st.fg_multiplier.load()) - 2;
    if (mult < 0) mult = 0;
    if (mult > 4) mult = 4;
    int allowed = fs.max_supported ? static_cast<int>(fs.max_supported) : 5; // items shown = supported multipliers
    if (allowed > 5) allowed = 5;
    BeginRow(mode ? "Maximum" : "Multiplier",
             mode ? "The most pictures shown per rendered frame in Dynamic mode." : "Pictures shown per rendered frame (2x = one generated picture between each pair).",
             DropdownWidth());
    if (Combo("fgmult", &mult, kMults, allowed, DropdownWidth())) {
        st.fg_multiplier.store(static_cast<uint32_t>(mult + 2));
        na::settings::Save();
    }
    EndRow();
    StatusLine(fs.latched_off ? 2 : (fs.active ? 1 : 0), fs.reason);
    if (fs.active || fs.generated) {
        if (pr.shown_fps > 0.f)
            snprintf(buf, sizeof(buf), "Submitted %.0f pictures per second from %.0f rendered (display %.0f Hz). Last frame: %u generated, graphics-card time %.1f ms (average %.1f ms).",
                     pr.shown_fps, pr.base_fps, pr.display_hz, fs.last_count, pr.fg_ms, pr.fg_avg_ms);
        else
            snprintf(buf, sizeof(buf), "Generated pictures so far: %llu.", static_cast<unsigned long long>(fs.generated));
        Hint(buf);
    }
}

// Which DLSS upscaler the game itself created (status only). Ray Reconstruction cannot
// be switched on from outside: it replaces the game's ray-tracing denoiser and needs
// the un-denoised lighting plus albedo / normal / roughness guides that only the game
// produces. The former on/off switch saved a choice nothing read; removed (Cort).
void DrawDlssInUseRow()
{
    using namespace na::ui;
    na::ngx::Stats ngx = na::ngx::GetStats();
    const bool rr = ngx.rr_seen != 0, sr = ngx.sr_creates != 0;
    const char* badge = rr && sr ? "BOTH SEEN" : rr ? "RAY RECONSTRUCTION" : sr ? "SUPER RESOLUTION" : "NOT YET SEEN";
    const char* help =
        rr && sr ? "The game has used both: plain DLSS upscaling and Ray Reconstruction (its ray-traced DLSS). It switches when its ray-tracing settings change."
        : rr     ? "The game upscales with Ray Reconstruction, the DLSS that also cleans up its ray-traced lighting. Turning ray reconstruction off in the game's settings switches it to plain DLSS."
        : sr     ? "The game upscales with plain DLSS Super Resolution."
                 : "The game has not started DLSS yet. Enable DLSS in its video settings.";
    BeginRow("DLSS in use", help, BadgeWidth(badge));
    Badge(badge);
    EndRow();
}

void DrawConnectionRow()
{
    using namespace na::ui;
    na::ngx::Stats ngx = na::ngx::GetStats();
    Conn conn = connection(ngx);
    const char* help;
    if (conn == Conn::Connected)
        help = ngx.core_preloaded ? "Connected to DLSS from startup. Picture settings can be confirmed."
                                  : "Connected to DLSS after startup. Picture settings can be confirmed.";
    else if (conn == Conn::Interrupted)
        help = "DLSS stopped running. Nothing can be confirmed until it resumes.";
    else
        help = "Not connected to DLSS. Enable it in the game's video settings.";
    BeginRow("Connection", help, BadgeWidth("ROUTE A"));
    Badge("ROUTE A");
    EndRow();
}

void DrawLatencyRow()
{
    using namespace na::ui;
    const auto s = na::latency::GetStats();
    int choice = na::settings::Get().reflex_mode.load();
    const char* choices[] = {"Off", "On", "On + Boost"};
    BeginRow("NVIDIA Reflex", "Reduces the delay between your input and the game drawing its response.", DropdownWidth());
    ImGui::BeginDisabled(!s.available);
    if (Combo("reflex", &choice, choices, 3, DropdownWidth())) { na::settings::Get().reflex_mode.store(choice); na::settings::Save(); }
    ImGui::EndDisabled(); EndRow();
    if (choice == 2) Hint("Boost keeps graphics clocks higher and can use more power.");
    Hint(s.engine_complete && !s.engine_errors ? (s.low_latency ? "On â€” connected to the game's input and frame timings." : "Off â€” frame timing remains connected.") : !s.available ? "NVIDIA low-latency controls are unavailable." : !s.status_valid ? "Waiting for driver confirmation." : s.low_latency ? (s.game_sleep ? "Driver confirms On; game timing calls are present." : "Driver confirms On; engine-timed Reflex is not connected.") : "Driver reports low-latency mode Off.");
    if (s.available && na::settings::Get().reflex_mode.load() >= 0 && s.set_result != 0) Hint("The driver did not accept the requested setting; see Details.");
}

void DrawLatencyDetails()
{
    using namespace na::ui;
    const auto s = na::latency::GetStats(); const auto p = na::present::GetStats();
    char text[512];
    snprintf(text, sizeof(text), "Generated pictures ready: %.1f ms (95%% %.1f, %u samples). Real picture submitted: %.1f ms (95%% %.1f). These start at game Present, not at your input, and exclude scanout.", p.ready_age.mean, p.ready_age.p95, p.ready_age.count, p.real_age.mean, p.real_age.p95); Hint(text);
    snprintf(text, sizeof(text), "Submission interval %.2f ms (95%% %.2f); Present call %.2f ms. V-Sync interval %u; driver V-Sync override %s; fullscreen VRR %s.", p.submit_interval.mean, p.submit_interval.p95, p.present_call.mean, p.requested_sync, !s.status_valid ? "unknown" : s.vsync_override ? "On" : "Off", !s.status_valid || !s.foreground ? "unknown" : s.fullscreen_vrr ? "On" : "not reported"); Hint(text);
    snprintf(text, sizeof(text), "Native latency samples %llu; simulation-to-GPU %.2f ms (not full FG/display latency). Driver set/query results %d/%d.", static_cast<unsigned long long>(s.native_samples), s.native_render_ms, s.set_result, s.query_result); Hint(text);
    snprintf(text, sizeof(text), "Engine timing: %llu complete frames, %llu rejected phases, %llu driver errors. Matched frame IDs are carried through the presentation queue.", static_cast<unsigned long long>(s.engine_complete), static_cast<unsigned long long>(s.engine_rejected), static_cast<unsigned long long>(s.engine_errors)); Hint(text);
    snprintf(text, sizeof(text), "Incoming timing calls: sleep %llu, simulation start %llu, input sample %llu. Observation %s; lost observations %llu. Call counts alone do not verify engine placement.", static_cast<unsigned long long>(s.audit.calls[na::latency::NativeAudit::Sleep].count), static_cast<unsigned long long>(s.audit.calls[0].count), static_cast<unsigned long long>(s.audit.calls[6].count), s.sleep_observer && s.marker_observer ? "active" : "incomplete", static_cast<unsigned long long>(s.audit_dropped)); Hint(text);
}

void DrawCaptureRow()
{
    using namespace na::ui;
    na::capture::Stats cap = na::capture::GetStats();
    char buf[MAX_PATH + 64];
    // A44: the capture starts 6 s after the click and the menu closes itself,
    // so the user can turn the camera and the frames contain real movement.
    constexpr uint32_t kCaptureDelayMs = 6000;
    BeginRow("Save pictures for analysis",
             "Saves two frames for neural-rendering tests. The menu closes and the pictures are taken 6 seconds later: use that time to turn the camera.",
             ButtonWidth("Save 2 frames"));
    ImGui::BeginDisabled(cap.frames_requested != 0 && cap.frames_written < cap.frames_requested);
    if (Button("Save 2 frames")) {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        wchar_t* local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
            wchar_t dir[MAX_PATH];
            swprintf_s(dir, L"%s\\NeuralAlias\\captures", local);
            CreateDirectoryW(dir, nullptr);
            swprintf_s(g_capture_dir, L"%s\\NeuralAlias\\captures\\%llu", local,
                       static_cast<unsigned long long>(qpc.QuadPart));
            CoTaskMemFree(local);
            na::capture::Request(2, g_capture_dir, kCaptureDelayMs);
            na::overlay::SetOpen(false); // let the user move straight away
        }
    }
    ImGui::EndDisabled();
    EndRow();
    if (cap.frames_requested) {
        uint32_t wait_ms = na::capture::DelayRemainingMs();
        if (wait_ms) {
            snprintf(buf, sizeof(buf), "Pictures will be taken in %u seconds. Close the menu and turn the camera.", (wait_ms + 999) / 1000);
        } else if (cap.frames_written < cap.frames_requested) {
            snprintf(buf, sizeof(buf), "Saving picture %u of %u.", cap.frames_written + 1, cap.frames_requested);
        } else if (cap.errors) {
            snprintf(buf, sizeof(buf), "Saved %u of %u pictures, with errors. Check the log.",
                     cap.frames_written, cap.frames_requested);
        } else {
            char dir_a[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, g_capture_dir, -1, dir_a, sizeof(dir_a), nullptr, nullptr);
            snprintf(buf, sizeof(buf), "Saved %u pictures to %s", cap.frames_written, dir_a);
        }
        Hint(buf);
    }
}

// J2: live DLAA for games without an upscaler. A switch to compare with and
// without, and what it is doing (or waiting for) right now.
void DrawLiveDlaa()
{
    using namespace na::ui;
    // Our DLAA injector for games WITHOUT DLSS ([j2] enabled=1 per game). In a game
    // that runs its own DLSS it never starts, and "Not running yet ... 0 frames
    // improved" only confused (Cort, Phantom Blade Zero): draw nothing there.
    if (!na::j2::Configured()) return;
    const na::j2::Stats st = na::j2::GetStats();
    BeginGroupBox("Live DLAA");
    bool on = !na::j2::Paused();
    BeginRow("NeuralAlias DLAA (test)", "NVIDIA DLSS at full resolution on this game's 3D picture. Switch off to compare.", SwitchWidth());
    if (Switch("j2", &on)) na::j2::SetPaused(!on);
    EndRow();
    Hint("For sharpest results and best FPS, set Anti-Aliasing to Off in the game's video settings.");
    {
        na::settings::Settings& ss = na::settings::Get();
        int sharp = static_cast<int>(ss.j2_sharpness.load());
        BeginRow("Sharpness", "Sharpening after DLAA; the HUD is never sharpened. 0-100%: AMD RCAS (halo-free, up to its designed maximum). 101-200%: NVIDIA Image Scaling sharpening added on top for extra bite; edge outlines can appear near 200%. 0 = off.", S(220));
        ImGui::SetNextItemWidth(S(220));
        if (ImGui::SliderInt("##j2sharp", &sharp, 0, 200, sharp ? (sharp > 100 ? "%d%% (RCAS + NIS)" : "%d%%") : "Off")) {
            ss.j2_sharpness.store(static_cast<uint32_t>(sharp));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) na::settings::Save();
        EndRow();
    }
    {
        // DLSS render preset. NGX values: J 10, K 11, L 12, M 13; 0 = NVIDIA's default.
        static const char* kLivePresets[] = {"L (default)", "M", "K", "J", "NVIDIA default"};
        static const uint32_t kLiveValues[] = {12, 13, 11, 10, 0};
        na::settings::Settings& ss = na::settings::Get();
        const uint32_t cur = ss.j2_preset.load();
        int idx = 0;
        for (int i = 0; i < 5; ++i) if (kLiveValues[i] == cur) idx = i;
        BeginRow("DLSS preset", "Which DLSS model runs. L and M are the newest (DLSS 4.5) transformer models; K and J are the earlier transformer models. Changing it restarts DLAA's history for one frame.", S(220));
        ImGui::SetNextItemWidth(S(220));
        if (ImGui::Combo("##j2preset", &idx, kLivePresets, 5)) {
            ss.j2_preset.store(kLiveValues[idx]);
            na::settings::Save();
        }
        EndRow();
    }
    bool jit = na::j2::Jitter();
    BeginRow("Camera jitter", "Shifts the camera by a fraction of a pixel each frame so DLAA can sharpen as well as smooth. Switch off if the picture shimmers or flickers.", SwitchWidth());
    if (Switch("j2jit", &jit)) na::j2::SetJitter(jit);
    EndRow();
    {
        static const char* kViews[] = {"Off (normal picture)", "Motion vectors", "Fine motion (sub-pixel)", "Depth"};
        int dv = na::j2::DebugView();
        BeginRow("Debug view", "Shows what DLAA is given instead of its result. Motion: colour = direction, brightness = speed, black = still. Fine motion makes tiny false motion visible. Magenta = broken values.", S(220));
        ImGui::SetNextItemWidth(S(220));
        if (ImGui::Combo("##j2dbg", &dv, kViews, 4)) na::j2::SetDebugView(dv);
        EndRow();
    }
    {
        // Texture detail (negative mip bias, NVIDIA DLSS guide 3.5: -1 at native).
        // Games build their samplers at load, so a change applies at the next start;
        // the setting lives in this game's own NeuralAliasHost.ini (per game).
        static const char* kDetail[] = {"Off (game's own)", "Light (-0.5)", "Standard (-1, NVIDIA)"};
        static const int kDetailX100[] = {0, -50, -100};
        na::settings::Settings& ss = na::settings::Get();
        const int cur = ss.j2_mip_bias_x100.load();
        int idx = 2;
        for (int i = 0; i < 3; ++i) if (kDetailX100[i] == cur) idx = i;
        const auto ml = na::observe::GetMipLiveStats();
        const bool live = ml.enabled && (ml.twin_pipelines > 0 || ml.twin_root_signatures > 0);
        BeginRow("Texture detail", "Samples surface textures sharper so DLAA can rebuild their full detail (NVIDIA's DLSS guide recommends -1 at native resolution). Very fine patterns such as fabric weaves can shimmer at -1; Light is gentler. Screen effects are never changed. Switches live (from the next frame); remembered for this game.", S(220));
        ImGui::SetNextItemWidth(S(220));
        if (ImGui::Combo("##j2detail", &idx, kDetail, 3)) {
            ss.j2_mip_bias_x100.store(kDetailX100[idx]);
            na::observe::SetSamplerMipBiasLive(kDetailX100[idx] / 100.0f);
            na::settings::Save();
        }
        EndRow();
        uint64_t ns = 0, nst = 0;
        na::observe::MipBiasCounts(&ns, &nst);
        na::observe::NativeBiasStats nb;
        na::observe::NativeMipBiases(&nb);
        char tb[400];
        const float active = na::observe::SamplerMipBias();
        std::snprintf(tb, sizeof tb, "Now: %.1f (game loaded at %.1f; %llu root signatures and %llu pipelines prepared for switching, %llu fixed, %llu fall-backs). Game's own bias on its surface textures: %llu below 0, %llu at 0, %llu above.",
                      active, ml.game_bias, static_cast<unsigned long long>(ml.twin_root_signatures), static_cast<unsigned long long>(ml.twin_pipelines),
                      static_cast<unsigned long long>(ml.untwinned_pipelines), static_cast<unsigned long long>(ml.fallbacks),
                      static_cast<unsigned long long>(nb.negative), static_cast<unsigned long long>(nb.zero), static_cast<unsigned long long>(nb.positive));
        StatusLine(0, tb);
        (void)ns; (void)nst;
        if (!live && static_cast<int>(ml.game_bias * -100.0f + 0.5f) != -cur)
            StatusLine(2, "Live switching is not available in this session (no prepared pipelines): restart the game to see this change.");
    }
    {
        // Where DLAA is placed in this game's frame, chosen automatically (or forced in the ini).
        const auto& rt = st.route;
        static const char* kWhere[] = {"not placed yet", "between the game's command lists", "inside the game's scene list", "handled by the game's own upscaler call"};
        char pl[360];
        std::snprintf(pl, sizeof pl, "Placement: %s%s%s (%s). %s%s%s", kWhere[rt.route < 4 ? rt.route : 0],
                      rt.mode ? ", forced in settings" : "", rt.state ? "" : ", still learning",
                      rt.mode ? "override" : "automatic", rt.reason, rt.missing[0] ? " Missing: " : "", rt.missing);
        StatusLine(0, pl);
    }
    {
        // Evidence capture: N consecutive DLAA frames (inputs, DLAA output, presented
        // picture) for analysis. Also on the capture key (F8 by default) and the ini.
        const auto cs = na::capture::j2::GetStats();
        const uint32_t n = na::settings::Get().j2_capture_frames.load();
        char lbl[64];
        std::snprintf(lbl, sizeof lbl, "Capture %u frames", n);
        BeginRow("Capture DLAA frames", "Saves what DLAA was given, what it produced and the picture you saw, for the next few frames, to LocalAppData\\NeuralAlias\\captures. Large files; for diagnosis only.", ButtonWidth(lbl));
        ImGui::BeginDisabled(!on || na::capture::j2::Armed());
        if (Button(lbl)) na::capture::j2::RequestTimestamped(n);
        ImGui::EndDisabled();
        EndRow();
        if (cs.requested) {
            char cl[200];
            std::snprintf(cl, sizeof cl, "Capture: %u of %u recorded, %u saved, %u skipped (busy), %u without the presented picture, %u errors.",
                          cs.recorded, cs.requested, cs.written, cs.dropped_busy + cs.discarded + cs.retired, cs.without_present, cs.errors);
            StatusLine(0, cl);
        }
    }
    char buf[320];
    if (!on) {
        StatusLine(0, "Off: you are seeing the game's own picture.");
    } else if (st.active) {
        std::snprintf(buf, sizeof buf, "Running: %ux%u, %llu frames improved so far.", st.width, st.height,
                      static_cast<unsigned long long>(st.frames));
        StatusLine(1, buf);
    } else {
        std::snprintf(buf, sizeof buf, "Not running yet: %s (%llu frames improved so far)", st.reason[0] ? st.reason : "starting.",
                      static_cast<unsigned long long>(st.frames));
        StatusLine(2, buf);
    }
    EndGroupBox();
}

} // namespace

void RegisterLiveDlaaMenu()
{
    na::menu::Register({"Live DLAA", na::menu::Availability::Available, nullptr, &DrawLiveDlaa, na::menu::Page::Picture});
}

void RegisterRouteAMenu()
{
    using na::menu::Availability;
    using na::menu::Page;
    na::menu::Clear();
    note_request(); // startup: whatever the game creates first is evidence for the loaded settings
    na::menu::Register({"Picture", Availability::Available, nullptr, &DrawPicture, Page::Picture});
    na::menu::Register({"Neural Rendering", Availability::Available, nullptr, &DrawNeuralRenderingRows,
                        Page::Enhancements});
    na::menu::Register({"Frame Generation", Availability::Available, nullptr, &DrawFrameGenerationRow,
                        Page::Enhancements});
    na::menu::Register({"Responsiveness", Availability::Available, nullptr, &DrawLatencyRow, Page::Enhancements});
    na::menu::Register({"Latency and display", Availability::Available, nullptr, &DrawLatencyDetails, Page::Details});
    na::menu::Register({"DLSS in use", Availability::Available, nullptr, &DrawDlssInUseRow, Page::Enhancements});
    na::menu::Register({"Motion analysis", Availability::Available, nullptr, &DrawMotionRow, Page::Enhancements});
    na::menu::Register({"Connection", Availability::Available, nullptr, &DrawConnectionRow, Page::Details});
    na::menu::Register({"Save pictures for analysis", Availability::Available, nullptr, &DrawCaptureRow,
                        Page::Details});
    if (!na::discovery::RegisterUpscalerReport())
        OutputDebugStringA("NeuralAlias: could not register Upscaler detection menu.\n");
}

} // namespace na::overlay
