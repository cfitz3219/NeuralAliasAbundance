#pragma once
#include <cstddef>

struct IDXGISwapChain;
struct ID3D12CommandQueue;
namespace na::native11 { struct SceneFrame; struct PresentReceipt; }

namespace na::native_d3d11 {
bool EvaluateFrame(void* user, void* context, const native11::SceneFrame& frame);
enum class PresentPicture { Game, WithHome };
void CapturePresent(IDXGISwapChain* swap, ID3D12CommandQueue* fallback_queue,
                    const native11::PresentReceipt& receipt, PresentPicture picture);
// Tells the scene finder what [j2] camera says (settings.hpp): whether a frame needs a camera to
// be handed over, or whether one is looked for at all. Cheap; called at install and at every
// present, so the menu's choice takes effect on the next frame.
void SyncCameraUse();
int FormatStatus(char* out, size_t capacity);
bool FormatMenuStatus(char* out, size_t capacity);
// Draw replay on this route (native11/draw_replay.hpp): one line for the menu, empty when the switch is off.
bool FormatReplayStatus(char* out, size_t capacity);
// DLSS Quality / Performance on this route (the fold, core/fold_shading.hpp): the Home menu's line, and
// the texture-detail step a level adds to the game's samplers (0 for DLAA, -1 while a level is on screen).
void FoldStatus(char* out, size_t capacity);
float FoldMipExtra();
}
