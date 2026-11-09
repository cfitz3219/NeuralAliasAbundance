#pragma once
#include <cstddef>

namespace na::native11 { struct SceneFrame; }

namespace na::native_d3d11 {
bool EvaluateFrame(void* user, void* context, const native11::SceneFrame& frame);
int FormatStatus(char* out, size_t capacity);
bool FormatMenuStatus(char* out, size_t capacity);
}
