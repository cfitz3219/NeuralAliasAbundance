#pragma once
#include <cstddef>

struct IDXGISwapChain;
struct ID3D12CommandQueue;
namespace na::native11 { struct SceneFrame; }

namespace na::native_d3d11 {
bool EvaluateFrame(void* user, void* context, const native11::SceneFrame& frame);
void CapturePresent(IDXGISwapChain* swap, ID3D12CommandQueue* queue);
int FormatStatus(char* out, size_t capacity);
bool FormatMenuStatus(char* out, size_t capacity);
}
