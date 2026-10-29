#include "test_support.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/motion/live_flow.hpp"
#include "host_dx12/overlay/route_a_panel.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/na_style.hpp"
int main() {
    auto& st=na::settings::Get();
    st.motion_enabled=0; st.nr_paper_white=200; st.dlss_mode=4; st.fg_preset=3;
    auto ms=na::motion::GetStats();
    std::printf("motion requested=0 observed=%u\n",ms.enabled);
    CHECK(ms.enabled==0);
    ImGui::CreateContext();
    auto& io=ImGui::GetIO(); io.DisplaySize={3440,1440}; io.DeltaTime=1.f/60;
    io.IniFilename=nullptr;
    io.Fonts->AddFontDefault();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    na::ui::ApplyStyle();
    na::overlay::RegisterRouteAMenu();
    for(int page=1;page<=4;++page) {
        std::printf("drawing page %d\n",page); std::fflush(stdout);
        ImGui::NewFrame(); ImGui::SetNextWindowSize({1400,800}); ImGui::Begin("NeuralAlias");
        na::menu::DrawPage(static_cast<na::menu::Page>(page),true);
        ImGui::End(); ImGui::Render();
    }
    ImGui::DestroyContext();
    return test::finish("route_a_menu_test");
}

