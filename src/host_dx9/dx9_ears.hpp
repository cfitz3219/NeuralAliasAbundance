// DX9 "ears" (DX9 plan step D3, docs/DX9-PLAN.md): turns what a DX9 game does
// into the same frame-model events as DX11 and Vulkan, and finds the camera.
//
//  - Targets: SetRenderTarget(0) and SetDepthStencilSurface. A surface that is
//    level 0 of a texture is named by its texture, so a later SetTexture of
//    that texture is a read of the same thing (the host does that mapping).
//  - Draws: with depth only while D3DRS_ZENABLE is on, so a HUD drawn with
//    depth test off is not a scene draw.
//  - Reads: SetTexture of the scene colour's texture, and StretchRect /
//    GetRenderTargetData from it (how DX9 games resolve MSAA or copy).
//  - Depth clear: Clear with D3DCLEAR_ZBUFFER.
//  - Frame: Present (device or swap chain). Reset forgets everything.
//
// Camera, shader games: the 256 float4 vertex-shader constant registers act as
// one block. Registers whose value changed at more than a quarter of the scene
// draws (per-object constants: World, WorldViewProj) are blanked, and the rest,
// as they were at the frame's first scene draw, are scanned with the same
// FindMatrices rule as DX11/DX12: a ViewProjection with its inverse in the
// block. A game with only per-object WVP has no camera here (optical flow then).
// Fixed-function games: SetTransform(VIEW) x SetTransform(PROJECTION) directly.
//
// DLSS point: the frame model's (before the first read of the finished scene
// colour), else the HUD rule (before the first non-scene draw into the scene
// colour after its last scene draw), else Present.
//
// Game-agnostic and portable C++17 (Linux-tested): the d3d9 hooks fill it.
#pragma once

#include "host_dx11/camera_pick.hpp"
#include "host_dx11/frame_model.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace na::dx9 {

using na::dx11::Id;
using na::dx11::ResourceInfo;

constexpr uint32_t kVsRegisters = 256;
constexpr Id kVsBlockId = 0xD9C0'0000'0000'0001ull; // the register file's id in the frame model

// D3DPRIMITIVETYPE -> vertices drawn.
inline uint64_t Vertices(uint32_t prim_type, uint32_t prim_count)
{
    switch (prim_type) {
    case 1: return prim_count;             // POINTLIST
    case 2: return uint64_t(prim_count) * 2; // LINELIST
    case 3: return uint64_t(prim_count) + 1; // LINESTRIP
    case 4: return uint64_t(prim_count) * 3; // TRIANGLELIST
    case 5: return uint64_t(prim_count) + 2; // TRIANGLESTRIP
    case 6: return uint64_t(prim_count) + 2; // TRIANGLEFAN
    default: return uint64_t(prim_count) * 3;
    }
}

enum class CameraSource : uint8_t { None, ShaderConstants, FixedFunction };
enum class FirePoint : uint8_t { None, BeforeRead, BeforeHudDraw, AtPresent };

struct Camera {
    bool found = false;
    CameraSource source = CameraSource::None;
    uint32_t vp_register = 0, inverse_register = 0; // shader constants
    na::dx11::CameraPick pick;                      // vp in column-vector convention
};

struct EarsStats {
    uint64_t frames = 0, camera_frames = 0, fired_read = 0, fired_hud = 0, fired_present = 0;
    uint32_t stable_registers = 0, unstable_registers = 0;
    uint32_t shader_scene_draws = 0, ff_scene_draws = 0;
};

class Ears {
public:
    Ears() { model_.EnableOverlayRule(true); Reset(); }

    // ---- state
    void SetSwapChainSize(uint32_t w, uint32_t h) { model_.SetSwapChainSize(w, h); }
    void SetRenderTarget0(Id id, const ResourceInfo& info)
    {
        rt0_ = id;
        if (id) model_.SetInfo(id, info);
    }
    void SetDepth(Id id, const ResourceInfo& info)
    {
        depth_ = id;
        if (id) {
            ResourceInfo d = info;
            d.depth = true;
            model_.SetInfo(id, d);
        }
    }
    void SetZEnable(bool on) { zenable_ = on; }
    void SetVertexShader(bool bound) { vs_bound_ = bound; }
    // SetVertexShaderConstantF(start, data, count): count float4 registers.
    void SetVsConstants(uint32_t start, const float* data, uint32_t count)
    {
        if (!data || start >= kVsRegisters) return;
        if (count > kVsRegisters - start) count = kVsRegisters - start;
        for (uint32_t r = start; r < start + count; ++r) {
            const float* v = data + (r - start) * 4;
            float* s = regs_.data() + r * 4;
            if (std::memcmp(s, v, 16) == 0) continue; // same values again: not a change
            std::memcpy(s, v, 16);
            if (!dirty_[r]) { dirty_[r] = 1; dirty_list_.push_back(static_cast<uint16_t>(r)); }
        }
    }
    // SetTransform: D3DTS_VIEW = 2, D3DTS_PROJECTION = 3 (row-vector matrices).
    void SetTransform(uint32_t state, const float* m)
    {
        if (!m) return;
        if (state == 2) { std::memcpy(view_.data(), m, 64); have_view_ = true; }
        if (state == 3) { std::memcpy(proj_.data(), m, 64); have_proj_ = true; }
    }

    // ---- events. Each Should*/fire answer means "run DLSS now, before forwarding the call".
    bool BeforeDraw(uint64_t vertices)
    {
        const na::dx11::Event e = DrawEvent(vertices);
        return model_.ShouldFireBeforeDraw(e);
    }
    void Draw(uint64_t vertices)
    {
        const na::dx11::Event e = DrawEvent(vertices);
        const bool scene = model_.IsSceneDraw(e);
        model_.Feed(e);
        if (!scene) return;
        if (vs_bound_) ++frame_shader_scene_draws_;
        else ++frame_ff_scene_draws_;
        ++frame_scene_draws_;
        for (uint16_t r : dirty_list_) {
            dirty_[r] = 0;
            ++changes_[r];
        }
        dirty_list_.clear();
        if (frame_scene_draws_ == 1) {
            snapshot_ = regs_;
            if (have_view_ && have_proj_) { snap_view_ = view_; snap_proj_ = proj_; snap_ff_ = true; }
        }
    }
    bool BeforeRead(Id id) const { return id && model_.ShouldFireBeforeRead(id); }
    void Read(Id id)
    {
        if (!id) return;
        na::dx11::Event e;
        e.kind = na::dx11::Kind::Read;
        e.resource = id;
        model_.Feed(e);
    }
    void ClearDepth()
    {
        if (!depth_) return;
        na::dx11::Event e;
        e.kind = na::dx11::Kind::ClearDepth;
        e.resource = depth_;
        model_.Feed(e);
    }
    // No point learned (or it was not reached): fire at Present with the HUD inside.
    bool PresentShouldFire() const { return !model_.FiredThisFrame() && model_.Current().valid() && model_.SceneDrawsThisFrame() > 0; }
    void MarkFired(FirePoint where)
    {
        model_.MarkFired();
        if (where == FirePoint::BeforeRead) ++stats_.fired_read;
        if (where == FirePoint::BeforeHudDraw) ++stats_.fired_hud;
        if (where == FirePoint::AtPresent) ++stats_.fired_present;
    }
    // The camera as of this frame's scene draws (valid from the first scene draw on).
    bool CameraNow(Camera* out) const
    {
        if (!frame_scene_draws_) return false;
        return FindCameraIn(out, /*now=*/true);
    }
    void Present()
    {
        ++stats_.frames;
        // Camera for this frame (used by the next frame's motion too).
        Camera c;
        last_camera_ = frame_scene_draws_ && FindCameraIn(&c, false) ? c : Camera{};
        if (last_camera_.found) ++stats_.camera_frames;
        if (last_camera_.source == CameraSource::ShaderConstants) learned_ = last_camera_;
        stats_.shader_scene_draws = frame_shader_scene_draws_;
        stats_.ff_scene_draws = frame_ff_scene_draws_;
        model_.EndFrame();
        frame_scene_draws_ = frame_shader_scene_draws_ = frame_ff_scene_draws_ = 0;
        changes_.fill(0);
        for (uint16_t r : dirty_list_) dirty_[r] = 0;
        dirty_list_.clear();
        snap_ff_ = false;
    }
    // Device Reset / loss: every surface is gone.
    void Reset()
    {
        model_ = na::dx11::FrameModel{};
        model_.EnableOverlayRule(true);
        rt0_ = depth_ = 0;
        zenable_ = true; // D3DRS_ZENABLE defaults to TRUE when there is an auto depth buffer
        vs_bound_ = false;
        regs_.fill(0);
        snapshot_.fill(0);
        changes_.fill(0);
        dirty_.fill(0);
        dirty_list_.clear();
        have_view_ = have_proj_ = snap_ff_ = false;
        frame_scene_draws_ = frame_shader_scene_draws_ = frame_ff_scene_draws_ = 0;
        last_camera_ = learned_ = Camera{};
    }
    void Forget(Id id) { model_.Forget(id); }

    const na::dx11::FrameModel& Model() const { return model_; }
    const Camera& LastCamera() const { return last_camera_; }
    const EarsStats& Stats() const { return stats_; }
    Id CurrentTarget() const { return rt0_; }
    Id CurrentDepth() const { return depth_; }

private:
    na::dx11::Event DrawEvent(uint64_t vertices) const
    {
        na::dx11::Event e;
        e.kind = na::dx11::Kind::Draw;
        e.rt0 = rt0_;
        e.depth = zenable_ ? depth_ : 0;
        e.depth_test = zenable_ && depth_;
        e.vertices = vertices;
        if (vs_bound_) e.vs_cb[0] = kVsBlockId;
        return e;
    }

    bool FindCameraIn(Camera* out, bool now) const
    {
        *out = Camera{};
        const bool shader = frame_shader_scene_draws_ >= frame_ff_scene_draws_;
        if (!shader) {
            if (!snap_ff_) return false;
            // Row-vector D3D: clip = v * View * Proj. Column-vector VP = transpose(View * Proj).
            na::camera::Matrix vp{};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) {
                    double s = 0;
                    for (int k = 0; k < 4; ++k) s += double(snap_view_[r * 4 + k]) * snap_proj_[k * 4 + c];
                    vp[c * 4 + r] = s;
                }
            na::camera::Candidate cand{};
            if (!na::camera::ViewProjection(vp, cand)) return false;
            out->found = true;
            out->source = CameraSource::FixedFunction;
            out->pick.found = true;
            out->pick.vp = vp;
            out->pick.reversed = cand.reversed;
            return true;
        }
        // Shader constants: blank the per-object registers, scan the rest.
        const uint32_t draws = frame_scene_draws_;
        const uint32_t allowed = draws < 8 ? 2 : draws / 4;
        std::vector<uint8_t> bytes(kVsRegisters * 16);
        const auto& src = now ? regs_ : snapshot_;
        uint32_t stable = 0;
        for (uint32_t r = 0; r < kVsRegisters; ++r) {
            const uint32_t ch = changes_[r] + (now && dirty_[r] ? 1 : 0);
            if (ch > allowed) continue; // per-object: left as zeros
            std::memcpy(bytes.data() + r * 16, src.data() + r * 4, 16);
            ++stable;
        }
        stats_.stable_registers = stable;
        stats_.unstable_registers = kVsRegisters - stable;
        na::dx11::CameraPick p;
        if (!(learned_.found && na::dx11::CheckLearned(bytes, learned_.pick, &p))) {
            p = na::dx11::CameraPick{};
            na::camera::Snapshot s{};
            s.session = s.epoch = s.submission = s.content_version = s.resource = 1;
            s.complete = s.lifetime_valid = true;
            s.sync = na::camera::Sync::AppSyncUnknown;
            s.bytes = bytes;
            const auto scan = na::camera::FindMatrices(s);
            for (const auto& c : scan.candidates) {
                if (c.kind != na::camera::Kind::ViewProjection || !c.has_inverse_pair) continue;
                if (c.byte_offset % 16 || c.inverse_byte_offset % 16) continue; // matrices start at a register
                p.found = true;
                p.offset = c.byte_offset;
                p.inverse_delta = static_cast<int64_t>(c.inverse_byte_offset) - static_cast<int64_t>(c.byte_offset);
                p.layout = c.layout;
                p.vp = c.matrix;
                p.reversed = c.reversed;
                break;
            }
        }
        if (!p.found) return false;
        out->found = true;
        out->source = CameraSource::ShaderConstants;
        out->vp_register = static_cast<uint32_t>(p.offset / 16);
        out->inverse_register = static_cast<uint32_t>((static_cast<int64_t>(p.offset) + p.inverse_delta) / 16);
        out->pick = p;
        return true;
    }

    na::dx11::FrameModel model_;
    Id rt0_ = 0, depth_ = 0;
    bool zenable_ = true, vs_bound_ = false;
    std::array<float, kVsRegisters * 4> regs_{}, snapshot_{};
    std::array<uint32_t, kVsRegisters> changes_{};
    std::array<uint8_t, kVsRegisters> dirty_{};
    std::vector<uint16_t> dirty_list_;
    std::array<float, 16> view_{}, proj_{}, snap_view_{}, snap_proj_{};
    bool have_view_ = false, have_proj_ = false, snap_ff_ = false;
    uint32_t frame_scene_draws_ = 0, frame_shader_scene_draws_ = 0, frame_ff_scene_draws_ = 0;
    Camera last_camera_, learned_;
    mutable EarsStats stats_; // register counts are filled in by the (const) camera search
};

} // namespace na::dx9
