#pragma once
#include "host_dx12/capture/j2_capture.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include "host_dx12/overlay/overlay.hpp"

namespace na::overlay {
inline void DrawLightingTrials(bool dlaa_on)
{
    using namespace na::ui;
    namespace capture = na::capture::j2;
    const auto stats = capture::GetStats();
    static const char* problem = nullptr;
    using Trial = capture::LightingTrial;
    const bool map = capture::FlatMapLightingTest();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float width = ButtonWidth("Still") + ButtonWidth("Turn 1") + ButtonWidth("Turn 2") + 2 * gap;
    BeginRow("Lighting test", map ? "Record still, then rotate twice. For Slide, move sideways across the map with heading and zoom fixed."
                                 : "Record still, then turn the view twice from the same spot. Sweep gently sideways and slightly up or down. Keep zoom fixed.",
             width + (map ? ButtonWidth("Slide") + gap : 0));
    ImGui::BeginDisabled(!dlaa_on || stats.capture_busy);
    capture::LightingTrial selected = capture::LightingTrial::None;
    if (Button("Still")) selected = Trial::Still;
    ImGui::SameLine();
    if (Button("Turn 1")) selected = Trial::Turn1;
    ImGui::SameLine();
    if (Button("Turn 2")) selected = Trial::Turn2;
    if (map) {
        ImGui::SameLine();
        if (Button("Slide")) selected = Trial::Slide;
    }
    ImGui::EndDisabled();
    EndRow();
    if (selected != capture::LightingTrial::None) {
        switch (capture::StartLightingTrial(selected)) {
        case capture::TrialStart::Started:
            problem = nullptr;
            na::overlay::SetOpen(false);
            break;
        case capture::TrialStart::Busy:
            problem = "A capture is already in progress.";
            break;
        case capture::TrialStart::NotApplicable:
            problem = "Slide applies to a flat strategy map.";
            break;
        case capture::TrialStart::FolderUnavailable:
            problem = "Could not create the capture folder. Check free disk space and try again.";
            break;
        }
    }
    if (problem) StatusLine(2, problem);
    if (stats.lighting_trial == capture::LightingTrial::None) return;
    const char* name = "";
    switch (stats.lighting_trial) {
    case Trial::Still: name = "Still"; break;
    case Trial::Turn1: name = "Turn 1"; break;
    case Trial::Turn2: name = "Turn 2"; break;
    case Trial::Slide: name = "Slide"; break;
    default: return;
    }
    const bool complete = stats.written == capture::kLightingFrames && !stats.without_present && !stats.errors;
    const char* status = complete ? "ready for analysis" : (stats.capture_busy ? "in progress" : "incomplete");
    char line[160];
    std::snprintf(line, sizeof line, "%s recording %s. %u of %u frames saved, %u missing presents, %u errors.",
                  name, status, stats.written, stats.requested, stats.without_present, stats.errors);
    StatusLine(complete ? 1 : 2, line);
}
} // namespace na::overlay
