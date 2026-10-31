#pragma once
// One Apply for the DLSS picture mode, whatever way the game chooses its
// render size. Games differ in WHEN and WHERE they decide it:
//   - they ask Streamline for the size again at runtime   -> Streamline layer
//     (rewrites the requested mode; pins dynamic-resolution bounds);
//   - Unreal: the engine's r.ScreenPercentage, read every frame -> Unreal layer;
//   - an engine settings path verified for this executable (Ghost, TLOU) -> exact-build layer;
//   - a window rebuild that makes the engine ask again   -> rebuild layer.
// The selector tries the layers that are available here, cheapest and least
// invasive first, and moves on only when a layer reports it could not change
// the size. Success is always the size the game's DLSS actually renders, as
// confirmed by the layer (never a write, a rebuild or a restart). The
// Streamline request stays active while later layers run, so an engine that
// re-asks Streamline during their refresh gets the selected mode too. The
// layer that worked is remembered for this game ([mode_control] layer in the
// game's user ini) and tried first next time. If none can, the result says
// so plainly; nothing is claimed as live.
#include <cstddef>
#include <cstdint>

namespace na::mode {

enum class Layer : uint32_t { None = 0, Streamline = 1, Unreal = 2, ExactBuild = 3, Rebuild = 4, Count = 5 };
enum class State : uint32_t { Idle = 0, Trying = 1, Confirmed = 2, Failed = 3, Cancelled = 4 };

struct Request {
    uint32_t mode = 0;        // settings::DlssMode (DLAA 0 ... Ultra Performance 4)
    int32_t preset = 0;       // settings preset (0 automatic, 10..13 = J..M)
    uint32_t out_w = 0, out_h = 0;   // the game's DLSS output size
    uint32_t render_w = 0, render_h = 0; // expected render size for size-based layers
    float unreal_percent = 0.f;       // Unreal screen percentage for this mode
};

struct Status {
    State state = State::Idle;
    Layer layer = Layer::None;       // layer running now, or the one that finished
    Layer learned = Layer::None;     // remembered for this game
    uint32_t tried_mask = 0;         // bit (1 << Layer) per layer tried for this request
    uint32_t available_mask = 0;     // layers available in this game right now
    uint64_t request_id = 0;
    char reason[220] = "";
};

// Shared with the overlay and deferred saved-settings activation. Preserve the
// existing 8-pixel native alignment and observed Unreal startup ratio table.
uint32_t AlignedRenderExtent(uint32_t output,int mode);
float UnrealPercentForMode(int mode);
bool BuildSavedRequest(uint32_t mode,int32_t preset,uint32_t out_w,uint32_t out_h,Request& request);
void Start();                        // loads the learned layer; starts the worker
bool RequestApply(const Request& request); // false when busy or nothing can act
// Withdraws a running Apply. `hand_back` also returns resolution control to
// the game (Streamline request cleared, Unreal value restored where still ours).
void Cancel(bool hand_back);
Status GetStatus();
const char* LayerName(Layer layer);
uint32_t AvailableMask();
// One status-log line ("picture_mode ..."): the selector plus the Streamline layer.
int FormatStatus(char* out, size_t cap);

// Pure ordering rule (unit-tested): learned layer first, then the default order,
// restricted to available layers. Returns the count written to `out`.
uint32_t PlanOrder(uint32_t available_mask, Layer learned, Layer* out, uint32_t cap);

} // namespace na::mode
