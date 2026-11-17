// P4-4 Stage M pass map: a slider-reduced frame yields reduced passes, the stretch to full
// size (with what it reads), and what runs after it.
#include "test_support.hpp"
#include "observe_abi.hpp"
#include "providers/discovery/pass_map.hpp"

#include <cstring>
#include <vector>

using na::discovery::PassMap;
namespace {
struct Ev {
    std::vector<uint8_t> bytes;
    const na_ev_list_op& op() const { return *reinterpret_cast<const na_ev_list_op*>(bytes.data()); }
};
Ev op(uint64_t list, uint32_t code, std::initializer_list<uint64_t> a, const void* payload = nullptr, uint32_t n = 0, uint32_t stride = 0)
{
    Ev e;
    e.bytes.resize(sizeof(na_ev_list_op) + n);
    na_ev_list_op o{};
    o.hdr.size_bytes = static_cast<uint32_t>(e.bytes.size());
    o.hdr.kind = NA_EV_LIST_OP;
    o.list = list;
    o.recording_generation = 1;
    o.op = code;
    size_t i = 0;
    for (uint64_t v : a) o.a[i++] = v;
    if (n) { o.payload_offset = sizeof(na_ev_list_op); o.payload_bytes = n; o.payload_stride = stride; }
    std::memcpy(e.bytes.data(), &o, sizeof o);
    if (n) std::memcpy(e.bytes.data() + sizeof o, payload, n);
    return e;
}
void feed(PassMap& m, const Ev& e) { m.OnOp(e.op(), e.bytes.data()); }
void targets(PassMap& m, uint64_t list, std::initializer_list<uint64_t> rts, uint64_t ds)
{
    std::vector<na_view_ref> v;
    for (uint64_t r : rts) { na_view_ref x{}; x.resource = r; x.descriptor_kind = NA_DESC_RTV; v.push_back(x); }
    if (ds) { na_view_ref x{}; x.resource = ds; x.descriptor_kind = NA_DESC_DSV; v.push_back(x); }
    feed(m, op(list, NA_OP_OM_SET_RENDER_TARGETS, {rts.size(), ds ? 1u : 0u}, v.data(), static_cast<uint32_t>(v.size() * sizeof(na_view_ref)),
               sizeof(na_view_ref)));
}
void viewport(PassMap& m, uint64_t list, float w, float h)
{
    uint64_t wh = 0;
    const float f[2] = {w, h};
    std::memcpy(&wh, f, sizeof wh);
    const float all[6] = {0, 0, w, h, 0, 1};
    feed(m, op(list, NA_OP_RS_VIEWPORT, {1, 0, wh, 0}, all, sizeof all, 24));
}
void resource(PassMap& m, uint64_t id, uint32_t w, uint32_t h, uint32_t f = 10)
{
    na_ev_resource_created r{};
    r.resource = id;
    r.width = w;
    r.height = h;
    r.format = f;
    m.OnResource(r);
}
} // namespace

int main() { return test::run({
    {"slider frame: reduced scene, stretch to full size, HUD after", [] {
        PassMap m;
        resource(m, 1, 1720, 720);       // scene colour at 67 %
        resource(m, 2, 1720, 720, 40);   // depth
        resource(m, 3, 2580, 1080);      // full-size upscaled colour
        resource(m, 4, 2580, 1080, 28);  // back buffer
        m.OnSwapchain(2580, 1080);
        m.OnDescriptor(5, 0, NA_DESC_SRV, 1);   // staged in a CPU heap...
        m.OnDescriptorCopy(5, 0, 77, 10, 1);    // ...and copied to the shader-visible heap
        m.Arm(0);
        m.OnPresent(); // the captured frame starts
        const uint64_t L = 500;
        targets(m, L, {1}, 2);
        viewport(m, L, 1720, 720);
        for (int i = 0; i < 3; ++i) feed(m, op(L, NA_OP_DRAW_INDEXED, {36, 1}));
        targets(m, L, {3}, 0);
        viewport(m, L, 2580, 1080);
        feed(m, op(L, NA_OP_SET_ROOT_TABLE, {77, 10, 1, 0, 0}));
        feed(m, op(L, NA_OP_DRAW, {3, 1})); // the game's stretch
        feed(m, op(L, NA_OP_COPY_RESOURCE, {4, 3}));
        targets(m, L, {4}, 0);
        feed(m, op(L, NA_OP_DRAW, {6, 40})); // HUD
        const na_list_ref ref{L, 1};
        m.OnExecute(&ref, 1);
        m.OnPresent(); // done
        CHECK(m.Done());
        const auto ps = m.Passes();
        CHECK(ps.size() == 4);
        CHECK(ps[0].ops == 3 && ps[0].vp_w == 1720 && ps[0].targets.size() == 2);
        CHECK(ps[1].reads.size() == 1 && ps[1].reads[0] == 1 && ps[1].approx_reads);
        const std::string r = m.Report();
        std::printf("%s", r.c_str());
        CHECK(r.find("#0 list 500 draw x3") != std::string::npos && r.find("REDUCED") != std::string::npos);
        CHECK(r.find("#1 list 500 draw x1") != std::string::npos && r.find("<- STRETCH CANDIDATE") != std::string::npos);
        CHECK(r.find("stretch candidate #1 (draw) writes [R3 2580x1080 f10] from reduced [R1 1720x720 f10]; 2 passes after it") != std::string::npos);
    }},
    {"a full-size frame reports no stretch, and nothing outside the captured frame is mapped", [] {
        PassMap m;
        resource(m, 3, 2580, 1080);
        m.OnSwapchain(2580, 1080);
        m.Arm(1);
        const uint64_t L = 9;
        targets(m, L, {3}, 0);
        viewport(m, L, 2580, 1080);
        feed(m, op(L, NA_OP_DRAW, {3, 1}));
        const na_list_ref ref{L, 1};
        m.OnExecute(&ref, 1); // before the window: ignored
        m.OnPresent();        // skipped
        m.OnPresent();        // window starts
        m.OnExecute(&ref, 1);
        m.OnPresent();
        CHECK(m.Done() && m.Passes().size() == 1);
        const std::string r = m.Report();
        CHECK(r.find("no stretch candidate") != std::string::npos && r.find("is the render scale below 100") != std::string::npos);
    }},
    {"bounded: an unrecorded list is counted, not invented", [] {
        PassMap m;
        m.Arm(0);
        m.OnPresent();
        const na_list_ref ref{42, 3};
        m.OnExecute(&ref, 1);
        m.OnPresent();
        CHECK(m.Report().find("unrecorded 1") != std::string::npos);
    }},
    {"a later recording does not erase the generation being submitted", [] {
        PassMap m;m.Arm(0);m.OnPresent();resource(m,1,32,16);resource(m,2,32,16);
        targets(m,9,{1},0);viewport(m,9,32,16);feed(m,op(9,NA_OP_DRAW,{3,1}));
        auto later=op(9,NA_OP_SET_PIPELINE,{99});
        reinterpret_cast<na_ev_list_op*>(later.bytes.data())->recording_generation=2;feed(m,later);
        const na_list_ref ref{9,1};m.OnExecute(&ref,1);m.OnPresent();
        CHECK(m.Passes().size()==1);CHECK(m.Report().find("unrecorded 0")!=std::string::npos);
    }},
    {"rearm retains executable cached recordings and resets per-frame counters", [] {
        PassMap m;m.Arm(0);resource(m,1,32,16);targets(m,9,{1},0);
        viewport(m,9,32,16);feed(m,op(9,NA_OP_DRAW,{3,1}));const na_list_ref ref{9,1};
        m.OnPresent();m.OnExecute(&ref,1);m.OnPresent();CHECK(m.Passes().size()==1);
        m.Arm(0);m.OnPresent();m.OnExecute(&ref,1);m.OnPresent();
        CHECK(m.Passes().size()==1);CHECK(m.Report().find("lists executed 1 (unrecorded 0")!=std::string::npos);
    }},
    {"pipeline changes preserve separate executable pass identities", [] {
        PassMap m;m.Arm(0);m.OnPresent();resource(m,1,32,16);targets(m,9,{1},0);viewport(m,9,32,16);
        feed(m,op(9,NA_OP_SET_PIPELINE,{10}));feed(m,op(9,NA_OP_DRAW,{3,1}));
        feed(m,op(9,NA_OP_SET_PIPELINE,{11}));feed(m,op(9,NA_OP_DRAW_INDEXED,{36,1}));
        const na_list_ref ref{9,1};m.OnExecute(&ref,1);m.OnPresent();
        const auto p=m.Passes();CHECK(p.size()==2);if(p.size()==2)CHECK(p[0].pipeline==10&&p[1].pipeline==11);
    }},
    {"native scene plus half-size effects is a composite, not a render-scale stretch", [] {
        PassMap m;m.Arm(0);m.OnPresent();m.OnSwapchain(3440,1440);
        resource(m,294,3440,1440,10);resource(m,295,3440,1440,19);
        resource(m,1261,1720,720,26);resource(m,299,3440,1440,28);
        targets(m,117,{1261},0);viewport(m,117,1720,720);feed(m,op(117,NA_OP_DRAW,{3,1}));
        m.OnDescriptor(5,0,NA_DESC_SRV,1261);m.OnDescriptor(5,1,NA_DESC_SRV,294);m.OnDescriptor(5,2,NA_DESC_SRV,295);
        targets(m,117,{299},0);viewport(m,117,3440,1440);
        feed(m,op(117,NA_OP_SET_ROOT_TABLE,{5,0,1,0,0}));feed(m,op(117,NA_OP_DRAW,{3,1}));
        const na_list_ref ref{117,1};m.OnExecute(&ref,1);m.OnPresent();
        const auto report=m.Report();CHECK(report.find("STRETCH CANDIDATE")==std::string::npos);
        CHECK(report.find("MIXED-SIZE COMPOSITE")!=std::string::npos);
    }},
}); }
