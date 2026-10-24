// A4: real menu registration and draw callback, no graphics device required.
#include "test_support.hpp"
#include "providers/discovery/upscaler_menu.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/route_a_panel.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include <imgui.h>
int main() { return test::run({
    {"C7 producer mapping preserves requests, success and origin", [] {
        na::ngx::Stats ngx{}; na::ffx::Stats fsr{};
        na::xess::Stats xess{}; na::dlss::Stats dlss{};
        fsr.modules_hooked = 1; fsr.hooked_functions = 3;
        xess.hooked_functions = 4;
        auto r = na::discovery::DescribeHostUpscalers(ngx, fsr, xess, dlss);
        CHECK(r.rows[1].detection == na::discovery::Detection::ModuleOnly);
        CHECK(std::string(r.rows[2].badge) == "NOT OBSERVED");
        fsr.upscale_dispatches = 8; fsr.replaced = 5; fsr.fallbacks = 3;
        xess.executes = 9; xess.replaced = 9;
        dlss.evaluates = 14;
        r = na::discovery::DescribeHostUpscalers(ngx, fsr, xess, dlss);
        CHECK(r.game_routes == 0);
        CHECK(r.rows[0].detection == na::discovery::Detection::InjectorOnly);
        CHECK(r.rows[1].detection == na::discovery::Detection::Requested);
        CHECK(r.rows[2].detection == na::discovery::Detection::Requested);
        fsr.upscale_contexts = 1; xess.contexts_initialized = 1; ngx.sr_creates = 1;
        r = na::discovery::DescribeHostUpscalers(ngx, fsr, xess, dlss);
        CHECK(r.game_routes == 6);
        CHECK(r.rows[0].detection == na::discovery::Detection::SourceUnknown);
        CHECK(r.rows[1].detection == na::discovery::Detection::Created);
        CHECK(r.rows[2].detection == na::discovery::Detection::Created);
    }},
    {"host startup registers the report after clearing the menu", [] {
        for (int pass = 0; pass < 2; ++pass) {
            na::overlay::RegisterRouteAMenu();
            int matches = 0;
            for (int i = 0; i < na::menu::Count(); ++i)
                if (std::string(na::menu::At(i)->title) == "Upscaler detection") ++matches;
            CHECK(matches == 1);
        }
        na::menu::Clear();
    }},
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
