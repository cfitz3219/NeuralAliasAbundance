#include "test_support.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/adapters/mode_control.hpp"
#include "host_dx12/motion/live_flow.hpp"
#include "host_dx12/overlay/route_a_panel.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/na_style.hpp"
int main() {
    na::mode::Request saved;
    CHECK(na::mode::BuildSavedRequest(2,13,3440,1440,saved));
    CHECK(saved.render_w==2000 && saved.render_h==840 && saved.unreal_percent==58.f);
    CHECK(na::mode::BuildSavedRequest(0,13,3440,1440,saved) && saved.render_w==3440 && saved.render_h==1440);
    CHECK(!na::mode::BuildSavedRequest(99,13,3440,1440,saved));
    auto& st=na::settings::Get();
    CHECK(!st.donut_tone && !st.donut_bloom && !st.xegtao_enabled);
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
    CHECK(na::menu::kPageCount==5);
    CHECK(!strcmp(na::menu::PageName(na::menu::Page::Experimental),"Experimental"));
    int experimental=0;
    for(int i=0;i<na::menu::Count();++i){
        const auto* section=na::menu::At(i);
        if(!strcmp(section->title,"XeGTAO") || !strcmp(section->title,"Lighting")){
            CHECK(na::menu::ResolvePage(*section)==(!strcmp(section->title,"XeGTAO")?na::menu::Page::Enhancements:na::menu::Page::Experimental));++experimental;
        }
    }
    CHECK(experimental==2);
    for(auto page:na::menu::kPages) {
        na::menu::SetCurrentPage(page);CHECK(na::menu::CurrentPage()==page);
        std::printf("drawing page %s\n",na::menu::PageName(page)); std::fflush(stdout);
        ImGui::NewFrame(); ImGui::SetNextWindowSize({1400,800}); ImGui::Begin("NeuralAlias");
        na::menu::DrawPage(page,true);
        ImGui::End(); ImGui::Render();
    }
    // Live-DLAA-only games must expose the same pages without Route A registration.
    na::menu::Clear();na::overlay::RegisterLiveDlaaMenu();int effects=0;
    for(int i=0;i<na::menu::Count();++i){const auto* section=na::menu::At(i);
        if(!strcmp(section->title,"XeGTAO")){CHECK(section->page==na::menu::Page::Enhancements);++effects;}
        if(!strcmp(section->title,"Lighting")||!strcmp(section->title,"GPU infrastructure")){CHECK(section->page==na::menu::Page::Experimental);++effects;}
    }
    CHECK(effects==3);
    ImGui::DestroyContext();
    return test::finish("route_a_menu_test");
}

