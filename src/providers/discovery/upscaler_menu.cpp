#include "providers/discovery/upscaler_menu.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include <cstring>

namespace na::discovery {
UpscalerReport ReadUpscalerReport()
{
    const auto ngx = na::ngx::GetStats();
    UpscalerSnapshot snapshot{};
    auto& dlss = snapshot[0];
    dlss.observer_available = ngx.hooked_functions != 0;
    dlss.module_seen = ngx.module_attached != 0 || ngx.core_preloaded != 0;
    // sr_creates counts successful SR creation specifically. The current host
    // does not prove game/injector origin; C6/C7 must supply that provenance.
    // evaluates/create_failures span feature types and are deliberately unused.
    dlss.unknown_creates = ngx.sr_creates;
    // FSR/XeSS remain unmonitored until C7 provides actual SR observations.
    return DescribeUpscalers(snapshot);
}
namespace {
constexpr const char* kTitle = "Upscaler detection";
void DrawReport()
{
    const auto report = ReadUpscalerReport();
    ImGui::TextWrapped("%s", report.summary.c_str());
    for (const auto& row : report.rows) {
        na::ui::BeginRow(row.name, row.reason.c_str(), na::ui::BadgeWidth(row.badge));
        na::ui::Badge(row.badge);
        na::ui::EndRow();
    }
}
}
bool RegisterUpscalerReport()
{
    for (int i = 0; i < na::menu::Count(); ++i) {
        const auto* section = na::menu::At(i);
        if (section && section->title && std::strcmp(section->title, kTitle) == 0)
            return section->draw == &DrawReport;
    }
    return na::menu::Register({kTitle, na::menu::Availability::Available, nullptr, &DrawReport, na::menu::Page::Details});
}
} // namespace na::discovery
