#pragma once

#include <algorithm>
#include <imgui.h>

namespace na::ui {

// Only the surrounding menu fades. The actual slider keeps its ID, hit box and mouse grab.
struct SharpnessPreview {
    int slider_frame = -1;
    ImVec2 lo{}, hi{};
    double pressed_at = -1, left_at = -1;
    bool inspecting = false, dismiss_press = false;
    float opacity = 1;

    void ObserveSlider()
    {
        if (!ImGui::IsItemVisible()) return;
        slider_frame = ImGui::GetFrameCount();
        lo = ImGui::GetItemRectMin();
        hi = ImGui::GetItemRectMax();
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl)
            pressed_at = ImGui::GetTime();
        if (!ImGui::IsItemActive() || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            pressed_at = -1;
    }

    void BeginFrame(bool reopened)
    {
        const auto& io = ImGui::GetIO();
        if (reopened || io.AppFocusLost || slider_frame != ImGui::GetFrameCount() - 1) {
            *this = {};
            return;
        }
        const double now = ImGui::GetTime();
        const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool over = ImGui::IsMouseHoveringRect(lo, hi, false);
        if (dismiss_press && !held) dismiss_press = false;
        if (inspecting && !over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) dismiss_press = true;
        if (dismiss_press || io.NavVisible || io.MouseWheel || io.MouseWheelH || io.KeyCtrl) {
            inspecting = false;
            pressed_at = left_at = -1;
            opacity = 1;
            return;
        }
        if (held && pressed_at >= 0 && now - pressed_at >= .30 && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            inspecting = true;
        if (over || (held && pressed_at >= 0) || !inspecting) left_at = -1;
        else if (left_at < 0) left_at = now;
        else if (now - left_at >= .60) inspecting = false;

        // A small nonzero alpha keeps the original ImGui window and its active item alive.
        const float step = (std::min)(io.DeltaTime, .05f) / .16f;
        opacity = inspecting ? (std::max)(.001f, opacity - step) : (std::min)(1.f, opacity + step);
    }
};

inline SharpnessPreview& PicturePreview()
{
    static SharpnessPreview preview;
    return preview;
}
} // namespace na::ui
