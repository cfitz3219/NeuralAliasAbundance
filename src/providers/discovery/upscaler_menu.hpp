// A4 Windows bridge; call registration after menu::Clear at host startup.
#pragma once
#include "providers/discovery/upscaler_report.hpp"
namespace na::ngx { struct Stats; }
namespace na::ffx { struct Stats; }
namespace na::xess { struct Stats; }
namespace na::dlss { struct Stats; }
namespace na::discovery {
UpscalerReport ReadUpscalerReport();
// Producer snapshot mapping, also usable without live hooks in bridge tests.
UpscalerReport DescribeHostUpscalers(const na::ngx::Stats&, const na::ffx::Stats&,
                                   const na::xess::Stats&, const na::dlss::Stats&);
bool RegisterUpscalerReport();
}
