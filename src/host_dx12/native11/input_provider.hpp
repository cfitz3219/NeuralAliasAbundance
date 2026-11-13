// Native DX11 input provider (WWII 40-50 fps plan, track B1; interface only, 2026-11-09).
//
// Purpose: a DX11 game renders through its own D3D11 driver (no D3D11On12). This provider watches
// the game's D3D11 contexts and publishes, per frame, the facts our existing DX12 backend needs:
// which texture is the scene colour and depth, where the scene ends (the insertion point, before the
// HUD), and the camera bytes and jitter that belong to that same scene. Astra's B2 handoff copies
// the named inputs into shared D3D12 resources, evaluates with the existing dlss::EvalDesc / motion /
// effects path, and writes the result back. One program: this module is selected by the same
// activation owner as the D3D11On12 route, never installed beside it.
//
// Ownership: Opus fills this (D3D11 hooks, frame model reused from the PR #90 frame_model.hpp /
// camera_pick.hpp, which already cover the ReShade depth rule, colour-drawn-with-depth, camera
// buffer ranking, the "first read after the last scene draw" point, deferred contexts and indirect
// draws). Astra consumes it (B2) and owns the camera math contract (B3 review).
//
// Threading: the provider calls the consumer on the game's immediate-context thread, inside the
// game's call, with the immediate context passed in. The consumer records its D3D11 copies/signal
// on that context there (GPU order = the game's order) and must not block on the GPU.
//
// Portable header: D3D11 objects are passed as opaque pointers so CPU tests build on Linux.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace na::native11 {

using Id = uint64_t; // identity of a game resource (its ID3D11Resource pointer); 0 = none

struct TextureFacts {
    Id resource = 0;
    uint64_t incarnation = 0;       // this object's lifetime id: a new object at a reused address gets a new value
    void* d3d11_resource = nullptr; // ID3D11Resource*, borrowed for the duration of the callback
    uint32_t width = 0, height = 0;  // base (mip 0) extent of the texture; mip/array_slice pick the subresource
    uint32_t active_x = 0, active_y = 0; // the scene viewport's origin inside that subresource
    uint32_t active_w = 0, active_h = 0; // the scene viewport's extent inside it (dynamic resolution)
    uint32_t format = 0;             // DXGI_FORMAT as created (may be typeless)
    uint32_t view_format = 0;        // DXGI_FORMAT of the view the scene used (RTV/DSV), typed
    uint32_t samples = 1;
    uint32_t mip = 0, array_slice = 0; // the subresource the scene rendered to
};

struct CameraFacts {
    Id buffer = 0;                 // the constant buffer the scene draws read the camera from
    int vs_slot = -1;              // VS slot it was bound at (-1 when it is a pixel-shader block)
    int ps_slot = -1;              // PS slot it was bound at (-1 when it is a vertex-shader block)
    uint32_t first_constant = 0;   // offset binding (VS/PSSetConstantBuffers1), in 16-byte constants
    uint32_t num_constants = 0;    // constants readable from first_constant (0 = whole buffer, plain bind)
    uint64_t version = 0;          // content version of the bytes below (bumps on every update)
    const uint8_t* bytes = nullptr; // CPU copy of the bytes the game uploaded (Map/Unmap or UpdateSubresource
    uint32_t size = 0;              // source data), never a GPU readback; valid for the callback only
    uint32_t bytes_first_constant = 0; // position of bytes[0] within the buffer
    bool held = false;             // this frame's camera range could not be read: the previous frame's camera, at most 3 frames
    bool raster_jitter_qualified = false; // this vertex binding and layout support the jitter write
    bool viewport_jitter = false;  // the raster sample is external to the camera matrix
    bool jitter_applied = false;   // the provider wrote this frame's jitter into the scene camera
    float jitter_px[2] = {0, 0};   // the jitter written, in pixels of the scene extent (DLSS convention)
    struct RelativePose {
        std::array<double, 16> projection{};
        std::array<double, 3> origin{};
    };
    std::optional<RelativePose> split_pose; // projection and origin corroborated on the same scene draws
    // The camera's view-projection when the game uploads no inverse beside it: admitted by the shared
    // start or continue rule (providers/camera/camera_admission.hpp), or re-read at the place learned
    // from one. The consumer uses it as given; it cannot repeat a proof that spans several frames.
    std::optional<std::array<double, 16>> no_inverse_pose;
};

enum class Boundary : uint8_t {
    BeforeRead,  // before the first read of the scene colour after its last scene draw (preferred)
    BeforeList,  // before ExecuteCommandList whose first colour read is that read (deferred contexts);
                 // only when the scene's draws were all submitted before the list and the list writes
                 // nothing to the scene colour before that read. Otherwise the frame is unplaceable.
    BeforeHud,   // scene-in-back-buffer games: before the first non-scene draw into the colour
};

struct SceneFrame {
    uint64_t frame = 0;            // provider frame counter (advances at Present)
    uint64_t stream = 0;           // input stream id: stays while colour/depth/camera objects stay; new on any change
    TextureFacts color, depth;
    bool reversed_depth = false;   // from the depth func/clear value of the scene draws
    CameraFacts camera;
    Boundary boundary = Boundary::BeforeRead;
    uint32_t scene_draws = 0;      // scene draws this frame before the boundary
    bool selection_changed = false; // stream differs from the previous fired frame (reset history)
};

// The consumer (B2). It copies what it keeps (COM refs, camera bytes) during the callback. Return false if this frame was not evaluated (the provider counts the reason).
// `context` is the game's ID3D11DeviceContext* (immediate). The consumer may record D3D11 work on it.
using SceneCallback = bool (*)(void* user, void* context, const SceneFrame& frame);

struct ProviderStats {
    uint64_t frames = 0, fired = 0, missed = 0, unplaceable = 0, contradictions = 0, selection_changes = 0;
    uint64_t twin_across_ranges = 0; // frames whose camera was the positioned copy found in another range
    uint64_t camera_found = 0, camera_missing = 0, jitter_written = 0;
    // The delivered camera's path, frame to frame: `steps` moved, `reversals` moved back against the
    // previous step (a camera that hops between two sources), `jumps` moved more than 50 units.
    uint64_t camera_steps = 0, camera_reversals = 0, camera_jumps = 0;
    uint64_t camera_held = 0; // frames delivered with the previous frame's camera (at most three in a row)
    uint64_t pose_native = 0, pose_legacy = 0, pose_generic = 0, pose_split = 0;
    // Cameras without an inverse beside them, by the shared rule that admitted them, and re-reads of
    // the place learned from one.
    uint64_t pose_started = 0, pose_continued = 0, pose_no_inverse = 0;
    // Missed frames, by what the no-inverse rules saw (camera_admission.hpp NotAdmitted).
    uint64_t start_no_candidate = 0, start_waiting = 0, start_inverse_game = 0, start_position_game = 0;
    uint64_t pose_no_packet = 0, pose_extent = 0, pose_repeat = 0, pose_origin = 0, pose_shape = 0;
    // The relative camera's layout learned from this game's draws, and attempts that found a projection
    // but no world range witnessing a position (providers/camera/relative_camera.hpp RelativeLearner).
    uint64_t relative_learned = 0, relative_learn_failed = 0, relative_ambiguous = 0;
    // Frames on which the relative-camera vote gave no camera, by reason (relative_camera.hpp RelativeVerdict).
    uint64_t split_no_votes = 0, split_no_projection = 0, split_few_draws = 0, split_no_majority = 0, split_small_share = 0;
    const char* last_miss = "";
};

// Installs the D3D11 hooks on `device` (ID3D11Device*) and its immediate context. Idempotent.
bool Install(void* device, SceneCallback cb, void* user, bool host_owns_present = false);
// The hosted path receives DXGI events through the overlay's existing hooks.
bool ObservePresent(void* swapchain);
void BeforeResize(void* swapchain);
void AfterResize(void* swapchain, long result);
class OwnWorkScope {
public:
    OwnWorkScope();
    ~OwnWorkScope();
    OwnWorkScope(const OwnWorkScope&) = delete;
    OwnWorkScope& operator=(const OwnWorkScope&) = delete;
private:
    bool was_;
};
bool IsGameWork(void* context);
bool IsOwnWork();
void Uninstall();
// Jitter for the NEXT frame's scene camera, in pixels (the backend's sequence). Covers that one frame
// only: a frame with no request (missed or failed evaluation) renders with the game's own camera. The provider writes
// it into the camera bytes the game uploads (the same write the DX12 route makes) and reports it back
// in CameraFacts so colour, depth, camera and jitter always belong to one scene.
void SetNextJitter(float x_px, float y_px);
// How SetNextJitter's sample reaches the picture.
//  Matrix (default):  written into the scene camera's ViewProjection at the game's upload. Needs a
//                     camera layout the adapter has qualified for the raster transform.
//  Viewport:          the game's viewports are moved by the sample on every depth-tested draw into
//                     the scene depth (the DX12 host's viewport jitter). Needs no camera layout;
//                     CameraFacts::jitter_applied then means this frame's scene draws carried it.
enum class JitterRoute { Matrix, Viewport };
void SetJitterRoute(JitterRoute route);
ProviderStats Stats();
int FormatStatus(char* out, unsigned long n); // one status line: "native11 frames= fired= ..."

} // namespace na::native11
