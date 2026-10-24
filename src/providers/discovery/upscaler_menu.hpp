// A4 Windows bridge; call registration after menu::Clear at host startup.
#pragma once
#include "providers/discovery/upscaler_report.hpp"
namespace na::discovery {
UpscalerReport ReadUpscalerReport();
bool RegisterUpscalerReport();
}
