// DX11 frame model + camera pick (portable, CPU only): synthetic frames in the
// shape of a real DX11 game (shadow pass, scene, mid-scene read, tonemap read,
// HUD), immediate and deferred, plus a camera block with VP + inverse.
#include "host_dx12/native11/camera_pick.hpp"
#include "host_dx12/native11/frame_model.hpp"
#include "providers/camera/camera_tracker.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace na::native11;

static int g_failed = 0;
static void expect(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

enum : Id { kShadowDepth = 1, kShadowRt = 2, kDepth = 3, kColor = 4, kHalfColor = 5, kBackBuffer = 6, kCamCb = 7, kObjCb = 8, kBloom = 9 };

static Event Draw(Id rt, Id depth, uint64_t verts, Id cam = kCamCb, Id obj = kObjCb)
{
    Event e;
    e.kind = Kind::Draw;
    e.rt0 = rt;
    e.depth = depth;
    e.depth_test = depth != 0;
    e.vertices = verts;
    e.cb[0] = cam;
    e.cb[1] = obj;
    return e;
}
static Event Read(Id r) { Event e; e.kind = Kind::Read; e.resource = r; return e; }
static Event Upd(Id r) { Event e; e.kind = Kind::CbUpdate; e.resource = r; return e; }

// One frame as a list of events. The DLSS point should be right before the
// tonemap's read of kColor (the 2nd read: the 1st is a mid-scene refraction copy).
static std::vector<Event> Frame(int scene_draws = 40)
{
    std::vector<Event> f;
    f.push_back(Upd(kCamCb)); // shadow camera
    for (int i = 0; i < 30; ++i) f.push_back(Draw(kShadowRt, kShadowDepth, 300, kCamCb, kObjCb));
    f.push_back(Upd(kCamCb)); // main camera
    for (int i = 0; i < scene_draws / 2; ++i) { f.push_back(Upd(kObjCb)); f.push_back(Draw(kColor, kDepth, 900)); }
    f.push_back(Read(kColor)); // refraction copy mid-scene
    for (int i = 0; i < scene_draws / 2; ++i) { f.push_back(Upd(kObjCb)); f.push_back(Draw(kColor, kDepth, 900)); }
    f.push_back(Read(kColor)); // tonemap: <- DLSS point
    f.push_back(Draw(kBackBuffer, 0, 3, 0, 0));
    f.push_back(Read(kColor)); // bloom reads it again
    f.push_back(Draw(kBloom, 0, 3, 0, 0));
    for (int i = 0; i < 20; ++i) f.push_back(Draw(kBackBuffer, 0, 6, 0, 0)); // HUD
    return f;
}

// Runs one frame through the model the way the hooks do (immediate context).
// Returns the index of the event we fired before, or -1.
static int RunImmediate(FrameModel& m, const std::vector<Event>& f)
{
    int fired = -1;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i].kind == Kind::Read && m.ShouldFireBeforeRead(f[i].resource)) { m.MarkFired(); fired = int(i); }
        m.Feed(f[i]);
    }
    m.EndFrame();
    return fired;
}

static void Setup(FrameModel& m)
{
    m.SetSwapChainSize(1920, 1080);
    m.SetInfo(kShadowDepth, {2048, 2048, 0, 1, true});
    m.SetInfo(kShadowRt, {2048, 2048, 0, 1, false});
    m.SetInfo(kDepth, {1920, 1080, 0, 1, true});
    m.SetInfo(kColor, {1920, 1080, 0, 1, false});
    m.SetInfo(kBackBuffer, {1920, 1080, 0, 1, false});
    m.SetInfo(kBloom, {960, 540, 0, 1, false});
}

static std::vector<uint8_t> CameraBlock(float cx, float cz, bool column_major)
{
    // Column-vector VP = P * V for a camera at (cx, 1, cz) looking down +Z, D3D style.
    const double fy = 1.0 / std::tan(0.5 * 1.0), fx = fy / (16.0 / 9.0), n = 0.1, fa = 1000.0;
    na::camera::Matrix P{fx, 0, 0, 0, 0, fy, 0, 0, 0, 0, fa / (fa - n), -n * fa / (fa - n), 0, 0, 1, 0};
    na::camera::Matrix V{1, 0, 0, -cx, 0, 1, 0, -1, 0, 0, 1, -cz, 0, 0, 0, 1};
    na::camera::Matrix VP{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) VP[r * 4 + c] += P[r * 4 + k] * V[k * 4 + c];
    na::camera::Matrix inv{};
    na::camera::Invert(VP, inv);
    std::vector<uint8_t> b(512, 0);
    const float junk[4] = {0.5f, 0.25f, 3.f, 1.f};
    std::memcpy(b.data(), junk, 16);
    StoreMatrix(b.data(), b.size(), 64, column_major ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor, VP);
    StoreMatrix(b.data(), b.size(), 192, column_major ? na::camera::Layout::ColumnMajor : na::camera::Layout::RowMajor, inv);
    return b;
}

int main()
{
    // ---- selection and the DLSS point, immediate context ----
    {
        FrameModel m;
        Setup(m);
        expect(RunImmediate(m, Frame()) == -1, "frame 1: nothing selected yet, nothing fired");
        const Selection& s = m.Current();
        expect(s.depth == kDepth && s.color == kColor, "scene depth and colour are found (shadow map skipped by aspect)");
        expect(RunImmediate(m, Frame()) == -1, "frame 2: learns the point with the selection, does not guess");
        const auto f = Frame();
        const int at = RunImmediate(m, f);
        int tonemap = -1, reads = 0;
        for (size_t i = 0; i < f.size(); ++i)
            if (f[i].kind == Kind::Read && f[i].resource == kColor && ++reads == 2) tonemap = int(i);
        expect(at == tonemap, "frame 3: DLSS fires right before the tonemap read (not the mid-scene refraction copy)");
        expect(m.GetStats().learned_read == 2, "the learned point is the 2nd read of the scene colour");
        expect(m.GetStats().contradictions == 0, "no scene draws after the point");
        const auto cands = m.CameraCandidates(4);
        expect(!cands.empty() && cands[0].first == kCamCb && cands[0].second == 0, "the camera block (bound for every scene draw) ranks first");
        m.SetCamera(kCamCb, 0);
        RunImmediate(m, Frame());
        expect(m.GetStats().camera_share > 0.99, "the camera block covers all scene draws");
        expect(m.GetStats().fired == 2, "fires every frame once learned");
    }
    // ---- loading screen does not change the choice ----
    {
        FrameModel m;
        Setup(m);
        RunImmediate(m, Frame());
        RunImmediate(m, Frame());
        std::vector<Event> loading = {Draw(kBackBuffer, kDepth, 6, 0, 0), Draw(kBackBuffer, kDepth, 6, 0, 0)};
        RunImmediate(m, loading);
        RunImmediate(m, loading);
        RunImmediate(m, loading);
        RunImmediate(m, loading);
        expect(m.Current().color == kColor && m.GetStats().loading_frames >= 4, "a loading screen keeps the selection");
        expect(m.GetStats().missed >= 1, "a frame without the point is counted as missed, not forced");
    }
    // ---- a selection change needs 3 frames ----
    {
        FrameModel m;
        Setup(m);
        m.SetInfo(kHalfColor, {1920, 1080, 0, 1, false});
        RunImmediate(m, Frame());
        std::vector<Event> other;
        for (int i = 0; i < 100; ++i) other.push_back(Draw(kHalfColor, kDepth, 900));
        RunImmediate(m, other);
        expect(m.Current().color == kColor, "one odd frame does not flip the colour");
        RunImmediate(m, other);
        RunImmediate(m, other);
        expect(m.Current().color == kHalfColor && m.GetStats().selection_changes == 1, "three frames in a row do");
    }
    // ---- deferred: tonemap read in its own command list -> fire before that list ----
    {
        FrameModel m;
        Setup(m);
        RunImmediate(m, Frame());
        RunImmediate(m, Frame());
        const auto f = Frame();
        // Split: scene list = everything up to the tonemap read; post list = the rest.
        int reads = 0;
        size_t split = 0;
        for (size_t i = 0; i < f.size(); ++i)
            if (f[i].kind == Kind::Read && f[i].resource == kColor && ++reads == 2) { split = i; break; }
        const std::vector<Event> scene(f.begin(), f.begin() + long(split)), post(f.begin() + long(split), f.end());
        bool fired_scene = m.ShouldFireBeforeList(m.Summarize(scene));
        for (const auto& e : scene) m.Feed(e);
        const bool fired_post = m.ShouldFireBeforeList(m.Summarize(post));
        if (fired_post) m.MarkFired();
        for (const auto& e : post) m.Feed(e);
        m.EndFrame();
        expect(!fired_scene && fired_post, "deferred: fires before the post-processing command list");
        // Now one list holding scene AND tonemap: unplaceable, counted, never forced.
        const bool fired_all = m.ShouldFireBeforeList(m.Summarize(f));
        for (const auto& e : f) m.Feed(e);
        m.EndFrame();
        expect(!fired_all && m.GetStats().unplaceable == 1, "deferred: scene and tonemap in one list is refused with a reason");
        // A list whose first colour read follows a non-scene draw into the colour (e.g. a decal or
        // sky pass with no depth test): inputs copied before it would be stale, so also refused.
        std::vector<Event> scene2(f.begin(), f.begin() + long(split)), post2(f.begin() + long(split), f.end());
        Event late = Draw(kColor, 0, 6);
        late.depth_test = false;
        post2.insert(post2.begin(), late);
        for (const auto& e : scene2) m.Feed(e);
        const bool fired_written = m.ShouldFireBeforeList(m.Summarize(post2));
        for (const auto& e : post2) m.Feed(e);
        m.EndFrame();
        expect(!fired_written && m.GetStats().unplaceable == 2, "deferred: a colour write before the first read in the list is refused");
    }
    // ---- too-early point is detected ----
    {
        FrameModel m;
        Setup(m);
        RunImmediate(m, Frame());
        RunImmediate(m, Frame());
        auto f = Frame();
        // An extra scene draw after the tonemap read this frame (transparents drawn late),
        // before the bloom read.
        int reads = 0;
        for (size_t i = 0; i < f.size(); ++i)
            if (f[i].kind == Kind::Read && f[i].resource == kColor && ++reads == 2) {
                f.insert(f.begin() + long(i) + 1, Draw(kColor, kDepth, 900));
                break;
            }
        RunImmediate(m, f);
        expect(m.GetStats().contradictions == 1, "a scene draw after the point is counted as a contradiction");
        RunImmediate(m, f);
        expect(m.GetStats().learned_read == 3, "and the point is re-learned as the first read after the late scene draw");
    }
    // ---- camera versions: the main camera, not the shadow camera ----
    {
        FrameModel m;
        Setup(m);
        RunImmediate(m, Frame());
        RunImmediate(m, Frame());
        m.SetCamera(kCamCb, 0);
        std::vector<Event> f = Frame();
        uint32_t main_version = 0;
        int updates = 0;
        for (const auto& e : f) {
            if (e.kind == Kind::Read && m.ShouldFireBeforeRead(e.resource)) {
                expect(m.CameraVersionThisFrame() == main_version, "at the point, the camera version is the main camera's upload");
                m.MarkFired();
            }
            const uint32_t v = m.Feed(e);
            if (e.kind == Kind::CbUpdate && e.resource == kCamCb && ++updates == 2) main_version = v;
        }
        m.EndFrame();
    }
    // ---- camera pick ----
    for (bool cm : {false, true}) {
        const auto b = CameraBlock(2.f, -5.f, cm);
        const CameraPick p = FindCamera(b, 1);
        expect(p.found && p.offset == 64 && p.inverse_delta == 128, cm ? "column-major VP + inverse found at offset 64" : "row-major VP + inverse found at offset 64");
        CameraPick again;
        const auto b2 = CameraBlock(2.5f, -4.f, cm);
        expect(CheckLearned(b2, p, &again) && std::fabs(again.vp[3] - p.vp[3]) > 1e-3, "the learned offset is re-read next frame (camera moved)");
        na::camera::CameraTracker t;
        const auto f1 = t.Update(p.vp, 1, 1920, 1080);
        const auto f2 = t.Update(again.vp, 2, 1920, 1080);
        expect(f1.valid && f2.valid && !f2.camera_cut, "the existing camera tracker accepts the DX11 camera");
    }
    {
        std::vector<uint8_t> noise(512);
        for (size_t i = 0; i < noise.size(); ++i) noise[i] = uint8_t(i * 37 + 11);
        expect(!FindCamera(noise, 1).found, "random bytes give no camera");
    }

    // ---- pixel-stage camera block, re-uploaded per draw (WWII shape): still a candidate ----
    {
        enum : Id { kPassCb = 20 };
        FrameModel m;
        Setup(m);
        auto frame = [&] {
            std::vector<Event> f;
            for (int i = 0; i < 40; ++i) {
                f.push_back(Upd(kObjCb));
                f.push_back(Upd(kPassCb)); // rewritten per shader variant, pose repeated in it
                Event d = Draw(kColor, kDepth, 900, 0, kObjCb);
                d.cb[kPsCbBase + 0] = kPassCb;
                d.cb[kPsCbBase + 1] = kObjCb; // the same buffer on both stages: searched once
                f.push_back(d);
            }
            f.push_back(Read(kColor));
            f.push_back(Draw(kBackBuffer, 0, 3, 0, 0));
            return f;
        };
        for (int i = 0; i < 3; ++i) RunImmediate(m, frame());
        const auto cands = m.CameraCandidates(8);
        bool pass = false;
        int obj = 0;
        for (const auto& c : cands) {
            if (c.first == kPassCb && c.second == kPsCbBase) pass = true;
            if (c.first == kObjCb) ++obj;
        }
        expect(pass, "a pixel-shader block rewritten every draw is still searched for the camera (PS slot kept)");
        expect(obj == 1, "a buffer bound on both stages is listed once");
    }
    // ---- tags (Vulkan): the point is learned as the tag of the first read after the scene ----
    {
        FrameModel m;
        Setup(m);
        auto tagged = [](uint64_t refraction_tag, uint64_t tonemap_tag, uint64_t bloom_tag) {
            std::vector<Event> f = Frame();
            int n = 0;
            for (Event& e : f)
                if (e.kind == Kind::Read) e.tag = n == 0 ? refraction_tag : n == 1 ? tonemap_tag : bloom_tag, ++n;
            return f;
        };
        for (int i = 0; i < 2; ++i) {
            for (const Event& e : tagged(11, 22, 33)) m.Feed(e);
            m.EndFrame();
        }
        expect(m.LearnedTag() == 22, "the tonemap read's tag is learned (not the mid-scene copy, not bloom)");
        for (const Event& e : tagged(11, 22, 22)) m.Feed(e);
        m.EndFrame();
        expect(m.LearnedTag() == 0 && m.GetStats().tag_not_unique == 1, "a tag that names two reads is not learned");
        for (const Event& e : tagged(11, 22, 33)) m.Feed(e);
        m.EndFrame();
        expect(m.LearnedTag() == 22, "and is learned again once it is unique");
    }

    // ---- scene/after-layers: a layered (deferred) renderer ----
    // Shape recorded from a real game (run p15540): the most-drawn target with the scene depth is
    // an 8-bit material layer (format 28) written with other targets; lighting reads it; the rest
    // of the scene (sky, glass, smoke) is then drawn with the same depth into a floating-point
    // target (format 10), which the tonemap reads. The picture is that later target.
    {
        enum : Id { kNormals = 20, kLit = 21, kGlow = 22, kOther = 30 };
        const auto targets = [](Event e, uint8_t n) { e.rt_count = n; return e; };
        struct Shape {
            int layer_draws = 200, later_draws = 12;
            uint8_t layer_targets = 4;
            bool lit_read = true, late_world_marks = false;
            int early_lit_draws = 0; // draws into the picture with the scene depth before the layers are read
            int glow_draws = 0;      // a second floating-point target drawn with the scene depth before the layers are read
            int other_targets = 0;   // more 8-bit targets of the same size, each drawn more than the picture
        };
        const auto frame = [&](const Shape& k) {
            std::vector<Event> f;
            f.push_back(Upd(kCamCb));
            for (int i = 0; i < 30; ++i) f.push_back(Draw(kShadowRt, kShadowDepth, 300));
            f.push_back(Upd(kCamCb));
            for (int i = 0; i < k.layer_draws; ++i) f.push_back(targets(Draw(kNormals, kDepth, 900), k.layer_targets));
            for (int t = 0; t < k.other_targets; ++t)
                for (int i = 0; i < 20; ++i) f.push_back(targets(Draw(Id(kOther + t), kDepth, 90), 1));
            for (int i = 0; i < k.early_lit_draws; ++i) f.push_back(targets(Draw(kLit, kDepth, 600), 1));
            for (int i = 0; i < k.glow_draws; ++i) f.push_back(targets(Draw(kGlow, kDepth, 60), 1));
            f.push_back(Read(kNormals));                       // lighting reads the layers
            f.push_back(targets(Draw(kLit, 0, 3, 0, 0), 1));   // a full-screen pass into the picture, no depth
            for (int i = 0; i < k.later_draws; ++i) f.push_back(targets(Draw(kLit, kDepth, 600), 1));
            if (k.lit_read) f.push_back(Read(kLit));           // tonemap: <- DLSS point
            f.push_back(Draw(kBackBuffer, 0, 3, 0, 0));
            if (k.lit_read) f.push_back(Read(kLit));           // bloom reads it again
            if (k.glow_draws) f.push_back(Read(kGlow));        // and adds the glow
            if (k.late_world_marks)
                for (int i = 0; i < 5; ++i) f.push_back(targets(Draw(kBackBuffer, kDepth, 30), 1)); // depth-tested marks after post
            for (int i = 0; i < 20; ++i) f.push_back(Draw(kBackBuffer, 0, 6, 0, 0));
            return f;
        };
        // As the DX11 hooks do: only reads of the scene colour and of the watched targets are reported.
        const auto run = [](FrameModel& m, const std::vector<Event>& f) {
            const Id color = m.Current().color;
            const std::vector<Id> watch = m.Watch();
            int fired = -1;
            for (size_t i = 0; i < f.size(); ++i) {
                if (f[i].kind == Kind::Read) {
                    bool seen = f[i].resource == color;
                    for (Id w : watch) seen = seen || w == f[i].resource;
                    if (!seen) continue;
                    if (m.ShouldFireBeforeRead(f[i].resource)) { m.MarkFired(); fired = int(i); }
                }
                m.Feed(f[i]);
            }
            m.EndFrame();
            return fired;
        };
        const auto setup = [&](FrameModel& m, bool lit_float = true, bool layer_float = false) {
            Setup(m);
            m.SetInfo(kNormals, {1920, 1080, 28, 1, false, layer_float});
            m.SetInfo(kLit, {1920, 1080, 10, 1, false, lit_float});
            m.SetInfo(kGlow, {1920, 1080, 10, 1, false, true});
            for (Id t = 0; t < 12; ++t) m.SetInfo(kOther + t, {1920, 1080, 28, 1, false, false});
        };
        const auto tonemap_index = [](const std::vector<Event>& f) {
            int last_scene = -1;
            for (size_t i = 0; i < f.size(); ++i)
                if (f[i].kind == Kind::Draw && f[i].depth == kDepth && (f[i].rt0 == kNormals || f[i].rt0 == kLit)) last_scene = int(i);
            for (size_t i = size_t(last_scene + 1); i < f.size(); ++i)
                if (f[i].kind == Kind::Read && f[i].resource == kLit) return int(i);
            return -1;
        };
        {
            FrameModel m;
            setup(m);
            const Shape shape;
            run(m, frame(shape));
            expect(m.Current().color == kNormals && !m.Layered(), "layered game, frame 1: the most-drawn target is taken first, as before");
            int fired = -1, frames = 1;
            for (; frames < 12 && m.Current().color != kLit; ++frames) run(m, frame(shape));
            expect(m.Current().color == kLit && m.Current().depth == kDepth && frames <= 5,
                   "the picture becomes the floating-point target the scene goes on in after the layers are read");
            expect(m.Layered() && m.Layer() == kNormals && m.LastChoice().rule == FrameModel::PictureRule::AfterLayers &&
                   m.LastChoice().most_drawn == kNormals && m.LastChoice().picture == kLit,
                   "and the choice says why: material layers, then the picture");
            run(m, frame(shape)); // learns the point with the new selection
            const auto f = frame(shape);
            m.SetCamera(kCamCb, 0);
            fired = run(m, f);
            expect(fired == tonemap_index(f) && fired > 0, "DLSS runs right before the tonemap's read of the picture, not at the layers' read");
            expect(m.GetStats().last_scene_draws == 212 && m.GetStats().camera_scene_draws == 212 && m.GetStats().contradictions == 0,
                   "the scene is the layers' draws and the picture's draws: the camera is ranked over all of them");
            // An indoor view: nothing more is drawn into the picture after lighting.
            Shape indoor;
            indoor.later_draws = 0;
            bool kept = true, fired_all = true;
            for (int i = 0; i < 6; ++i) {
                const auto g = frame(indoor);
                const int at = run(m, g);
                kept = kept && m.Current().color == kLit && m.Layered();
                fired_all = fired_all && at == tonemap_index(g);
            }
            expect(kept, "a frame with no draw of its own into the picture keeps the picture (it is still read after the layers)");
            expect(fired_all && m.GetStats().selection_changes == 1, "and DLSS still runs before the tonemap on those frames");
            // Depth-tested marks drawn into the back buffer after post do not move the point.
            Shape marks;
            marks.late_world_marks = true;
            bool steady = true;
            for (int i = 0; i < 4; ++i) {
                const auto g = frame(marks);
                steady = steady && run(m, g) == tonemap_index(g);
            }
            expect(steady && m.GetStats().contradictions == 0, "depth-tested draws into another target after post are not scene draws");
            // The picture in use is drawn with the scene depth only before the lighting on some frames.
            Shape early;
            early.later_draws = 0;
            early.early_lit_draws = 6;
            bool held = true;
            for (int i = 0; i < 6; ++i) {
                const auto g = frame(early);
                const int at = run(m, g);
                held = held && m.Current().color == kLit && m.Layered() && at == tonemap_index(g);
            }
            expect(held && m.GetStats().selection_changes == 1,
                   "the picture in use stays while it is still read after the layers, wherever its own draws fall");
        }
        {
            // Ten more 8-bit targets of the same size, each drawn more than the picture: the picture
            // is still watched and found, and an indoor frame keeps it.
            FrameModel m;
            setup(m);
            Shape busy;
            busy.other_targets = 10;
            int frames = 0;
            for (; frames < 12 && m.Current().color != kLit; ++frames) run(m, frame(busy));
            bool listed = false;
            for (const auto& n : m.LastChoice().candidates) listed = listed || n.id == kLit;
            expect(m.Current().color == kLit && m.Layered() && frames <= 5 && listed,
                   "a picture drawn less than many other targets is still followed, and the note lists it");
            run(m, frame(busy)); // learns the point with the new selection
            busy.later_draws = 0;
            bool kept = true, fired_all = true;
            for (int i = 0; i < 6; ++i) {
                const auto g = frame(busy);
                const int at = run(m, g);
                kept = kept && m.Current().color == kLit && m.Layered();
                fired_all = fired_all && at == tonemap_index(g);
            }
            expect(kept, "and it is kept on frames that draw nothing more into it");
            expect(fired_all, "with DLSS still before the tonemap");
        }
        const auto settles_on = [&](const Shape& shape, bool lit_float, bool layer_float, const char** why) {
            FrameModel m;
            setup(m, lit_float, layer_float);
            for (int i = 0; i < 8; ++i) run(m, frame(shape));
            if (why) *why = m.LastChoice().why;
            return m.Layered() ? Id(0) : m.Current().color;
        };
        const char* why = "";
        Shape alone;
        alone.layer_targets = 1;
        expect(settles_on(alone, true, false, &why) == kNormals && std::strstr(why, "drawn alone"),
               "a most-drawn target drawn alone is not a layer: the old rule stands (and says why)");
        expect(settles_on(Shape{}, true, true, &why) == kNormals && std::strstr(why, "floating point"),
               "a floating-point most-drawn target can be the picture: the old rule stands");
        expect(settles_on(Shape{}, false, false, &why) == kNormals && std::strstr(why, "no floating-point target"),
               "no floating-point target after the layers: the old rule stands");
        Shape glow; // an 8-bit picture written with a second target, and a floating-point glow target drawn before the picture is read
        glow.glow_draws = 8;
        expect(settles_on(glow, false, false, &why) == kNormals && std::strstr(why, "only before"),
               "a floating-point target drawn only before the most-drawn target is read is not the picture: the old rule stands");
        Shape early_only; // from a fresh start, draws only before the lighting do not make a picture
        early_only.later_draws = 0;
        early_only.early_lit_draws = 6;
        expect(settles_on(early_only, true, false, &why) == kNormals && std::strstr(why, "only before"),
               "a target not yet in use must be drawn after the layers are read to become the picture");
        Shape unread;
        unread.lit_read = false;
        expect(settles_on(unread, true, false, &why) == kNormals && std::strstr(why, "never read"),
               "a later target that is never read gives no place to run: the old rule stands");
    }
    // The first shape in this file (one colour target, drawn alone) never becomes layered.
    {
        FrameModel m;
        Setup(m);
        for (int i = 0; i < 6; ++i) RunImmediate(m, Frame());
        expect(m.Current().color == kColor && !m.Layered() && m.Layer() == 0 && m.GetStats().layered_frames == 0 &&
               m.LastChoice().rule == FrameModel::PictureRule::MostDrawn,
               "a game that draws its scene into one target keeps the most-drawn rule");
    }

    std::printf(g_failed ? "FAIL: %d check(s) failed\n" : "PASS: DX11 frame model\n", g_failed);
    return g_failed ? 1 : 0;
}
