// D3: the DX9 ears on synthetic frames (portable; Linux and 32/64-bit Windows).
// A shader game with post-processing, a scene-in-back-buffer game with a HUD,
// a fixed-function game, a per-object-WVP-only game, and a Reset.
#include "host_dx9/dx9_ears.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace na::dx9;
using na::camera::Matrix;

namespace {

int g_fail = 0;
void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// Row-vector D3D matrices (v * M), stored row-major.
Matrix Mul(const Matrix& a, const Matrix& b)
{
    Matrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
Matrix PerspectiveLH(double fovy, double aspect, double zn, double zf)
{
    const double ys = 1.0 / std::tan(fovy / 2), xs = ys / aspect, q = zf / (zf - zn);
    return {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, 1, 0, 0, -zn * q, 0};
}
Matrix Translate(double x, double y, double z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }
Matrix View(double yaw, double x, double y, double z)
{
    // Camera rotated by yaw about Y, at (x, y, z): view = inverse(R * T).
    const double c = std::cos(yaw), s = std::sin(yaw);
    const Matrix world = Mul(Matrix{c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1}, Translate(x, y, z));
    Matrix v{};
    na::camera::Invert(world, v);
    return v;
}

// How an HLSL shader (column_major default) sees mul(v, M): registers hold M's columns.
void ToRegisters(const Matrix& m, float* out16)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) out16[c * 4 + r] = static_cast<float>(m[r * 4 + c]);
}

const ResourceInfo kScene{1280, 720, 21, 1, false};
const ResourceInfo kDepth{1280, 720, 75, 1, true};
const ResourceInfo kBack{1280, 720, 22, 1, false};
constexpr Id kSceneRt = 100, kDepthId = 200, kBackBuffer = 300, kTonemapOut = 400;

struct Game {
    bool post = true;          // scene into its own target, then a tonemap read
    bool fixed_function = false;
    bool camera_block = true;  // VP + inverse uploaded as shader constants
    int objects = 30;
    int frame = 0;
    // Results seen while running one frame.
    bool fired_at_read = false, fired_at_hud = false, fired_early = false, present_fire = false;

    void Run(Ears& e)
    {
        fired_at_read = fired_at_hud = fired_early = present_fire = false;
        const Matrix view = View(0.01 * frame, 1.0 + 0.1 * frame, 2.0, -5.0);
        const Matrix proj = PerspectiveLH(1.0, 16.0 / 9.0, 0.1, 1000.0);
        const Matrix vp = Mul(view, proj);
        Matrix inv{};
        na::camera::Invert(vp, inv);

        // Shadow-ish pass: small depth, few draws (must not be picked).
        e.SetRenderTarget0(0, {});
        e.SetDepth(900, {512, 512, 75, 1, true});
        e.SetZEnable(true);
        e.ClearDepth();
        for (int i = 0; i < 3; ++i) e.Draw(Vertices(4, 50));

        // Scene.
        e.SetRenderTarget0(post ? kSceneRt : kBackBuffer, post ? kScene : kBack);
        e.SetDepth(kDepthId, kDepth);
        e.SetZEnable(true);
        e.ClearDepth();
        e.SetVertexShader(!fixed_function);
        float regs[16];
        if (fixed_function) {
            float v[16], p[16];
            for (int i = 0; i < 16; ++i) { v[i] = static_cast<float>(view[i]); p[i] = static_cast<float>(proj[i]); }
            e.SetTransform(2, v);
            e.SetTransform(3, p);
        } else if (camera_block) {
            ToRegisters(vp, regs);
            e.SetVsConstants(8, regs, 4);
            ToRegisters(inv, regs);
            e.SetVsConstants(12, regs, 4);
        }
        for (int i = 0; i < objects; ++i) {
            if (!fixed_function) {
                // Per object: World x ViewProj in c0..c3 (changes every draw), as many DX9 games do.
                ToRegisters(Mul(Mul(Translate(i * 2.0, 0, i * 3.0), view), proj), regs);
                e.SetVsConstants(0, regs, 4);
                // Same camera values re-sent every draw: not a change.
                if (camera_block) {
                    ToRegisters(vp, regs);
                    e.SetVsConstants(8, regs, 4);
                }
            }
            if (e.BeforeDraw(Vertices(4, 1000))) fired_early = true;
            e.Draw(Vertices(4, 1000));
        }
        // Particles with depth test (still scene).
        e.Draw(Vertices(5, 200));

        if (post) {
            // Tonemap: read the scene into another target, then the back buffer.
            e.SetZEnable(false);
            e.SetRenderTarget0(kTonemapOut, kScene);
            if (e.BeforeRead(kSceneRt)) { fired_at_read = true; e.MarkFired(FirePoint::BeforeRead); }
            e.Read(kSceneRt);
            e.Draw(4);
            e.SetRenderTarget0(kBackBuffer, kBack);
            e.Read(kTonemapOut);
            e.Draw(4);
        }
        // HUD: depth test off, into the back buffer.
        e.SetZEnable(false);
        e.SetRenderTarget0(kBackBuffer, kBack);
        for (int i = 0; i < 5; ++i) {
            if (e.BeforeDraw(6)) { fired_at_hud = true; e.MarkFired(FirePoint::BeforeHudDraw); }
            e.Draw(6);
        }
        if (e.PresentShouldFire()) { present_fire = true; e.MarkFired(FirePoint::AtPresent); }
        e.Present();
        ++frame;
    }
};

} // namespace

int main()
{
    std::printf("DX9 ears test, %d-bit\n", static_cast<int>(sizeof(void*) * 8));
    {
        Ears e;
        e.SetSwapChainSize(1280, 720);
        Game g;
        for (int i = 0; i < 4; ++i) g.Run(e);
        const auto& sel = e.Model().Current();
        Check(sel.color == kSceneRt && sel.depth == kDepthId, "post game: scene colour + depth picked (not the shadow map)");
        const Camera& c = e.LastCamera();
        Check(c.found && c.source == CameraSource::ShaderConstants, "post game: camera found in the shader constants");
        Check(c.vp_register == 8 && c.inverse_register == 12, "post game: camera at c8, inverse at c12 (per-object c0 blanked)");
        std::printf("     camera c%u / c%u, stable regs %u, unstable %u\n", c.vp_register, c.inverse_register, e.Stats().stable_registers,
                    e.Stats().unstable_registers);
        Check(e.Model().LearnedPointKind() == 1, "post game: DLSS point = before the tonemap read");
        g.Run(e);
        Check(g.fired_at_read && !g.fired_at_hud && !g.fired_early && !g.present_fire, "post game: fired at the tonemap read, once");
        Check(e.Model().GetStats().contradictions == 0, "post game: no scene draw after DLSS");
        // Camera moves every frame: the learned register still reads it.
        g.Run(e);
        Check(e.LastCamera().found && e.LastCamera().vp_register == 8, "post game: moving camera still found at c8");
    }
    {
        Ears e;
        e.SetSwapChainSize(1280, 720);
        Game g;
        g.post = false;
        for (int i = 0; i < 4; ++i) g.Run(e);
        const auto& sel = e.Model().Current();
        Check(sel.color == kBackBuffer && sel.depth == kDepthId, "back-buffer game: back buffer + depth picked");
        Check(e.Model().LearnedPointKind() == 2, "back-buffer game: DLSS point = before the first HUD draw");
        g.Run(e);
        Check(g.fired_at_hud && !g.fired_early && !g.present_fire, "back-buffer game: fired before the HUD, not during the scene");
        Check(e.Stats().fired_hud >= 1, "back-buffer game: counted as a HUD-rule fire");
    }
    {
        Ears e;
        e.SetSwapChainSize(1280, 720);
        Game g;
        g.fixed_function = true;
        for (int i = 0; i < 4; ++i) g.Run(e);
        const Camera& c = e.LastCamera();
        Check(c.found && c.source == CameraSource::FixedFunction, "fixed-function game: camera from SetTransform(VIEW x PROJECTION)");
    }
    {
        Ears e;
        e.SetSwapChainSize(1280, 720);
        Game g;
        g.camera_block = false;
        for (int i = 0; i < 4; ++i) g.Run(e);
        Check(e.Model().Current().valid(), "WVP-only game: scene still picked");
        Check(!e.LastCamera().found, "WVP-only game: no camera (per-object matrices are not a camera)");
    }
    {
        Ears e;
        e.SetSwapChainSize(1280, 720);
        Game g;
        for (int i = 0; i < 4; ++i) g.Run(e);
        e.Reset();
        Check(!e.Model().Current().valid() && !e.LastCamera().found, "Reset forgets the selection and camera");
        for (int i = 0; i < 4; ++i) g.Run(e);
        Check(e.Model().Current().color == kSceneRt && e.LastCamera().found, "after Reset it finds them again");
    }
    {
        // A single draw with few vertices and one depth: a loading screen keeps no selection.
        Ears e;
        e.SetSwapChainSize(1280, 720);
        for (int f = 0; f < 4; ++f) {
            e.SetRenderTarget0(kBackBuffer, kBack);
            e.SetDepth(kDepthId, kDepth);
            e.Draw(6);
            e.Present();
        }
        Check(!e.Model().Current().valid(), "loading screen: nothing picked");
    }
    std::printf("%s: DX9 ears\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
