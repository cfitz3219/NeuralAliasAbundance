#pragma once
#include <cstddef>

struct IDXGISwapChain;
struct ID3D12CommandQueue;
namespace na::native11 { struct SceneFrame; }

namespace na::native_d3d11 {
bool EvaluateFrame(void* user, void* context, const native11::SceneFrame& frame);
void CapturePresent(IDXGISwapChain* swap, ID3D12CommandQueue* queue);
// Tells the scene finder what [j2] camera says (settings.hpp): whether a frame needs a camera to
// be handed over, or whether one is looked for at all. Cheap; called at install and at every
// present, so the menu's choice takes effect on the next frame.
void SyncCameraUse();
int FormatStatus(char* out, size_t capacity);
bool FormatMenuStatus(char* out, size_t capacity);
}
