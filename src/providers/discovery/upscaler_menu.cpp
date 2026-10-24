#include "providers/discovery/upscaler_menu.hpp"
#include "host_dx12/ngx_observe/ngx_observe.hpp"
#include "host_dx12/upscalers/ffx_intercept.hpp"
#include "host_dx12/upscalers/xess_intercept.hpp"
#include "host_dx12/dlss/dlss_backend.hpp"
#include "host_dx12/overlay/menu.hpp"
#include "host_dx12/overlay/na_style.hpp"
#include <cstring>
#include <algorithm>

namespace na::discovery {
UpscalerReport ReadUpscalerReport()
{
    return DescribeHostUpscalers(na::ngx::GetStats(), na::ffx::GetStats(),
                                na::xess::GetStats(), na::dlss::GetStats());
}
UpscalerReport DescribeHostUpscalers(const na::ngx::Stats& ngx, const na::ffx::Stats& fsr,
                                   const na::xess::Stats& xess, const na::dlss::Stats& backend)
{
    UpscalerSnapshot snapshot{};
    auto& dlss = snapshot[0];
    dlss.observer_available = ngx.hooked_functions != 0;
    dlss.module_seen = ngx.module_attached != 0 || ngx.core_preloaded != 0;
    // Only explicitly unmarked creations remain unknown. Marked backend calls
    // are counted once through C6, even when the NGX observer also saw them.
    dlss.unknown_creates = ngx.sr_creates_unknown;
    // These producers overlap and can reset at different times. Keep the
    // larger observed count as evidence, never sum them as distinct creations.
    dlss.injector_creates = (std::max)(backend.features_created, ngx.sr_creates_injector);
    dlss.injector_evaluates = backend.evaluates;
    auto& f = snapshot[1];
    f.observer_available = fsr.hooked_functions != 0;
    f.module_seen = fsr.modules_hooked != 0;
    f.game_creates = fsr.upscale_contexts;
    f.game_requests = fsr.upscale_dispatches;
    auto& x = snapshot[2];
    x.observer_available = xess.hooked_functions != 0;
    x.module_seen = xess.modules_hooked != 0;
    x.game_creates = xess.contexts_initialized;
    x.game_requests = xess.executes;
    // Requests include failed and replaced calls. Neither dispatches minus
    // replaced nor fallbacks proves native success. Injector results belong
    // to DLSS, counted once by C6 rather than under FSR/XeSS.
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
