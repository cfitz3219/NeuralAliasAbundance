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
        unsigned char* pixels = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        ImGui::NewFrame(); ImGui::Begin("A4 test"); s->draw(); ImGui::End(); ImGui::Render();
        ImGui::DestroyContext();
        na::menu::Clear();
        CHECK(na::discovery::RegisterUpscalerReport());
        CHECK(na::menu::Count() == 1);
        na::menu::Clear();
    }},
}); }
