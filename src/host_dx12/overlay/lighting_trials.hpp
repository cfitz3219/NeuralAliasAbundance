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
    BeginRow("Lighting test", "Both methods use the same 30 frames. The menu closes immediately. Hold still or pan steadily for 15 seconds, starting at the click.",
             ButtonWidth("Record still") + ImGui::GetStyle().ItemSpacing.x + ButtonWidth("Record pan"));
    ImGui::BeginDisabled(!dlaa_on || stats.capture_busy);
    capture::LightingTrial selected = capture::LightingTrial::None;
    if (Button("Record still")) selected = capture::LightingTrial::Still;
    ImGui::SameLine();
    if (Button("Record pan")) selected = capture::LightingTrial::Pan;
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
        case capture::TrialStart::FolderUnavailable:
            problem = "Could not create the capture folder. Check free disk space and try again.";
            break;
        }
    }
    if (problem) StatusLine(2, problem);
    if (stats.lighting_trial == capture::LightingTrial::None) return;
    const char* name = stats.lighting_trial == capture::LightingTrial::Still ? "Still" : "Pan";
    const bool complete = stats.written == capture::kLightingFrames && !stats.without_present && !stats.errors;
    const char* status = complete ? "ready for analysis" : (stats.capture_busy ? "in progress" : "incomplete");
    char line[160];
    std::snprintf(line, sizeof line, "%s recording %s. %u of %u frames saved, %u missing presents, %u errors.",
                  name, status, stats.written, stats.requested, stats.without_present, stats.errors);
    StatusLine(complete ? 1 : 2, line);
}
} // namespace na::overlay
