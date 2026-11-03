#include "test_support.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/adapters/mode_control.hpp"
#include "host_dx12/motion/live_flow.hpp"
#include "host_dx12/overlay/route_a_panel.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/j2_flow.hpp"
#include <cstring>
int main() {
    na::mode::Request saved;
    CHECK(na::mode::BuildSavedRequest(2,13,3440,1440,saved));
    CHECK(saved.render_w==2000 && saved.render_h==840 && saved.unreal_percent==58.f);
    CHECK(na::mode::BuildSavedRequest(0,13,3440,1440,saved) && saved.render_w==3440 && saved.render_h==1440);
    CHECK(!na::mode::BuildSavedRequest(99,13,3440,1440,saved));
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
    const auto section_count=[](const char* title,na::menu::Page page) {
        int count=0;
        for(int i=0;i<na::menu::Count();++i) {
            const auto* s=na::menu::At(i);
            if(s && std::strcmp(s->title,title)==0 && na::menu::ResolvePage(*s)==page && s->draw) ++count;
        }
        return count;
    };
    // SE5 starts with Route A disabled. Its feature registrations must stand
    // alone; drawing the native-DLSS menu cannot prove J2-only visibility.
    na::menu::Clear();
    na::j2::SetEnabled(true);
    na::overlay::RegisterLiveDlaaMenu();
    CHECK(section_count("Live DLAA",na::menu::Page::Picture)==1);
    CHECK(section_count("Optical flow",na::menu::Page::Enhancements)==1);
    CHECK(na::menu::PageHasContent(na::menu::Page::Enhancements));
    CHECK(!na::j2::flow::Enabled());
    ImGui::NewFrame(); ImGui::SetNextWindowSize({1400,800}); ImGui::Begin("J2 only");
    na::menu::DrawPage(na::menu::Page::Enhancements,true);
    ImGui::End(); ImGui::Render();
    na::overlay::RegisterRouteAMenu();
    na::overlay::RegisterLiveDlaaMenu();
    CHECK(section_count("Optical flow",na::menu::Page::Enhancements)==1);
    for(int page=1;page<=4;++page) {
        std::printf("drawing page %d\n",page); std::fflush(stdout);
        ImGui::NewFrame(); ImGui::SetNextWindowSize({1400,800}); ImGui::Begin("NeuralAlias");
        na::menu::DrawPage(static_cast<na::menu::Page>(page),true);
        ImGui::End(); ImGui::Render();
    }
    na::j2::SetEnabled(false);
    ImGui::DestroyContext();
    return test::finish("route_a_menu_test");
}

