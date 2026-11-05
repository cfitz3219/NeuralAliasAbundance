// Bridge frame record v4: validation and mode choice (portable, CPU only).
#include "bridge/frame_record.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace na::bridge;

static int g_failed = 0;
static void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

static FrameRecord good(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t seq)
{
    FrameRecord f;
    f.sequence = seq;
    f.render_w = rw; f.render_h = rh; f.output_w = ow; f.output_h = oh;
    f.mode = static_cast<uint32_t>(ModeForScale(rw, rh, ow, oh));
    f.jitter_x = 0.25f; f.jitter_y = -0.375f;
    f.mv_scale_x = -float(rw); f.mv_scale_y = -float(rh);
    return f;
}

int main()
{
    const SessionLimits dlaa{1920, 1080, 1920, 1080};
    const SessionLimits up{1280, 720, 1920, 1080};

    expect(ValidateLimits(dlaa) == nullptr, "DLAA limits are accepted");
    expect(ValidateLimits(up) == nullptr, "upscaling limits are accepted");
    expect(ValidateLimits(SessionLimits{2000, 1080, 1920, 1080}) != nullptr, "render larger than output is refused");
    expect(ValidateLimits(SessionLimits{0, 1080, 1920, 1080}) != nullptr, "zero size is refused");
    expect(ValidateLimits(SessionLimits{9000, 9000, 9000, 9000}) != nullptr, "sizes over 8192 are refused");

    expect(Validate(good(1920, 1080, 1920, 1080, 0), dlaa, 0) == nullptr, "a DLAA frame passes");
    expect(Validate(good(1280, 720, 1920, 1080, 7), up, 7) == nullptr, "an upscaling frame passes");
    expect(Validate(good(1100, 620, 1920, 1080, 7), up, 7) == nullptr, "a smaller dynamic-resolution frame passes");
    expect(Validate(good(1280, 720, 1920, 1080, 7), up, 8) != nullptr, "an out-of-order frame is refused");
    expect(Validate(good(1400, 720, 1920, 1080, 0), up, 0) != nullptr, "a frame above the session's render size is refused");
    expect(Validate(good(1280, 720, 2560, 1440, 0), up, 0) != nullptr, "a frame with another output size is refused");

    FrameRecord f = good(1280, 720, 1920, 1080, 0);
    f.version = 1;
    expect(Validate(f, up, 0) != nullptr, "a v1 (Alien) record is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.jitter_x = std::numeric_limits<float>::quiet_NaN();
    expect(Validate(f, up, 0) != nullptr, "NaN jitter is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.jitter_y = 3.f;
    expect(Validate(f, up, 0) != nullptr, "jitter beyond a pixel is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.mv_scale_x = 0.f;
    expect(Validate(f, up, 0) != nullptr, "zero motion scale is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.pre_exposure = 0.f;
    expect(Validate(f, up, 0) != nullptr, "zero exposure is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.frame_ms = 5000.f;
    expect(Validate(f, up, 0) != nullptr, "a 5 s frame time is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.flags = 1u << 20;
    expect(Validate(f, up, 0) != nullptr, "unknown flags are refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.mode = static_cast<uint32_t>(Mode::DLAA);
    expect(Validate(f, up, 0) != nullptr, "DLAA with a smaller render size is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.mode = 9;
    expect(Validate(f, up, 0) != nullptr, "an unknown mode is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.flags = kReset | kDepthInverted | kMvJittered | kAutoExposure;
    expect(Validate(f, up, 0) == nullptr, "known flags are accepted");
    f = good(1280, 720, 1920, 1080, 0);
    f.flags = kCameraMotion;
    expect(Validate(f, up, 0) == nullptr, "camera motion with the identity reprojection is accepted");
    f.reproject[5] = std::numeric_limits<float>::quiet_NaN();
    expect(Validate(f, up, 0) != nullptr, "a NaN camera reprojection is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.flags = kCameraMotion;
    f.depth_clear = 2.f;
    expect(Validate(f, up, 0) != nullptr, "an empty-depth value above 1 is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.flags = kCameraMotion | kFlowMotion;
    expect(Validate(f, up, 0) == nullptr, "flow motion on top of camera motion is accepted");
    f.flow_loosen = 101;
    expect(Validate(f, up, 0) != nullptr, "a flow looseness above 100 is refused");
    f = good(1280, 720, 1920, 1080, 0);
    f.flags = kFlowMotion;
    expect(Validate(f, up, 0) != nullptr, "flow motion without camera motion is refused");

    expect(ModeForScale(1920, 1080, 1920, 1080) == Mode::DLAA, "equal sizes pick DLAA");
    expect(ModeForScale(1280, 720, 1920, 1080) == Mode::Quality, "1.5x picks Quality");
    expect(ModeForScale(1114, 626, 1920, 1080) == Mode::Balanced, "1.72x picks Balanced");
    expect(ModeForScale(960, 540, 1920, 1080) == Mode::Performance, "2x picks Performance");
    expect(ModeForScale(640, 360, 1920, 1080) == Mode::UltraPerformance, "3x picks Ultra Performance");
    expect(ModeForScale(1477, 831, 1920, 1080) == Mode::UltraQuality, "1.3x picks Ultra Quality");
    expect(std::strcmp(ModeName(Mode::Balanced), "Balanced") == 0, "mode names read plainly");

    std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: frame record v3\n", g_failed);
    return g_failed ? 1 : 0;
}
