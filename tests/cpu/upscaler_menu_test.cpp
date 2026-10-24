// A4: real menu registration and draw callback, no graphics device required.
#include "test_support.hpp"
#include "providers/discovery/upscaler_menu.hpp"
#include "host_dx12/overlay/menu.hpp"
#include <imgui.h>
int main() { return test::run({
    {"register after clear, idempotent, draw and register again", [] {
        na::menu::Clear();
        CHECK(na::discovery::RegisterUpscalerReport());
        CHECK(na::discovery::RegisterUpscalerReport());
        CHECK(na::menu::Count() == 1);
        const auto* s = na::menu::At(0);
        CHECK(s && s->page == na::menu::Page::Details && s->draw);
        CHECK(na::discovery::ReadUpscalerReport().game_routes == 0);
        ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.DisplaySize = ImVec2(800, 600); io.DeltaTime = 1.0f/60.0f;
        io.IniFilename = nullptr;
        // ImGui 1.92 uses dynamic atlas requests. This CPU-only test generates
        // draw commands; it does not upload atlas textures or claim GPU rendering.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        for (int frame = 0; frame < 2; ++frame) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImVec2(780, 560));
            ImGui::Begin("A4 test"); s->draw(); ImGui::End(); ImGui::Render();
        }
        CHECK(ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0);
        ImGui::DestroyContext();
        na::menu::Clear();
        CHECK(na::discovery::RegisterUpscalerReport());
        CHECK(na::menu::Count() == 1);
        na::menu::Clear();
    }},
}); }
