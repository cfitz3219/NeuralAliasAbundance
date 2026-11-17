// C13: discovery heuristics on a synthetic but realistic frame stream.
//  - ReShade generic_depth: the swapchain-aspect main depth wins over a shadow
//    map with more vertices (fails the aspect filter) and a half-resolution
//    depth (screen/buffer ratio 2.0 > 1.85); a buffer is not eligible in the
//    frame it first appears in or the next; a loading-screen frame (one
//    depth-stencil, <= 8 draws) does not replace the last real frame.
//  - Pre-HUD hint: the target written by 3D draws before the first
//    alpha-blended depth-disabled draw outranks the target receiving the HUD.
//  - Camera block: the root CBV bound for every draw outranks per-object CBVs.
//  - Unknown pipelines and dispatches never zero the ranking.
#include "test_support.hpp"
#include "providers/discovery/universal_heuristics.hpp"
#include <vector>
#include <chrono>
using namespace na::discovery;

namespace {
struct Feed {
    UniversalHeuristics h;
    na_observe_status status{};
    uint64_t op{}, generation{}, ticket{}, seq{};
    Feed() {
        status.session = 1;
        status.epoch = 2;
        status.size_bytes = sizeof(status);
    }
    template <class T> void Event(T e, uint32_t kind, const void* payload = nullptr, size_t n = 0) {
        na_event_batch b{};
        b.header_bytes = sizeof(b);
        b.abi_major = NA_OBSERVE_ABI_MAJOR;
        b.session = 1;
        b.epoch = 2;
        b.event_count = 1;
        b.payload_bytes = static_cast<uint32_t>((sizeof(e) + n + 7) & ~size_t(7));
        e.hdr.kind = kind;
        e.hdr.epoch = 2;
        e.hdr.size_bytes = b.payload_bytes;
        e.hdr.producer_thread = 1;
        e.hdr.thread_seq = ++seq;
        std::vector<unsigned char> bytes(sizeof(b) + b.payload_bytes);
        std::memcpy(bytes.data(), &b, sizeof(b));
        std::memcpy(bytes.data() + sizeof(b), &e, sizeof(e));
        if (n) std::memcpy(bytes.data() + sizeof(b) + sizeof(e), payload, n);
        const auto decoded = DecodeBatch(bytes.data(), bytes.size());
        CHECK(decoded.valid);
        h.Consume(decoded, status);
    }
    void Resource(uint64_t id, uint32_t w, uint32_t hgt, uint32_t format) {
        na_ev_resource_created r{};
        r.resource = id;
        r.width = w;
        r.height = hgt;
        r.format = format;
        Event(r, NA_EV_RESOURCE_CREATED);
    }
    void Pipeline(uint64_t id, uint32_t depth, uint32_t write, uint32_t blend) {
        na_ev_pipeline_created p{};
        p.pipeline = id;
        p.depth_enable = depth;
        p.depth_write = write;
        p.blend_enable_mask = blend;
        Event(p, NA_EV_PIPELINE_CREATED);
    }
    void Swapchain(uint32_t w, uint32_t hgt) {
        na_ev_swapchain_created s{};
        s.swapchain = 99;
        s.width = w;
        s.height = hgt;
        Event(s, NA_EV_SWAPCHAIN_CREATED);
    }
    void Reset() {
        op = 0;
        ++generation;
        na_ev_list_reset_close e{};
        e.list = 5;
        e.recording_generation = generation;
        Event(e, NA_EV_LIST_RESET);
    }
    void Op(na_ev_list_op e, const void* data = nullptr, size_t n = 0, uint32_t stride = 0) {
        e.list = 5;
        e.recording_generation = generation;
        e.op_index = ++op;
        if (n) {
            e.payload_offset = sizeof(e);
            e.payload_bytes = static_cast<uint32_t>(n);
            e.payload_stride = stride;
        }
        Event(e, NA_EV_LIST_OP, data, n);
    }
    void SetPipeline(uint64_t p) { na_ev_list_op e{}; e.op = NA_OP_SET_PIPELINE; e.a[0] = p; Op(e); }
    void Targets(std::vector<uint64_t> rtvs, uint64_t dsv) {
        std::vector<na_view_ref> v;
        for (auto r : rtvs) { na_view_ref x{}; x.resource = r; x.descriptor_kind = NA_DESC_RTV; v.push_back(x); }
        if (dsv) { na_view_ref x{}; x.resource = dsv; x.descriptor_kind = NA_DESC_DSV; v.push_back(x); }
        na_ev_list_op e{};
        e.op = NA_OP_OM_SET_RENDER_TARGETS;
        e.a[0] = rtvs.size();
        e.a[1] = dsv ? 1 : 0;
        Op(e, v.data(), v.size() * sizeof(na_view_ref), sizeof(na_view_ref));
    }
    void RootCbv(uint64_t param, uint64_t res, uint64_t off) {
        na_ev_list_op e{};
        e.op = NA_OP_SET_ROOT_VIEW;
        e.a[0] = res;
        e.a[1] = off;
        e.a[2] = param;
        e.a[3] = NA_DESC_CBV;
        Op(e);
    }
    void Draw(uint64_t vertices) { na_ev_list_op e{}; e.op = NA_OP_DRAW; e.a[0] = vertices; e.a[1] = 1; Op(e); }
    void Dispatch() { na_ev_list_op e{}; e.op = NA_OP_DISPATCH; e.a[0] = e.a[1] = e.a[2] = 8; Op(e); }
    void Execute(uint64_t list = 5, uint64_t gen = 0) {
        struct {
            na_ev_queue_execute e;
            na_list_ref ref;
        } x{};
        x.e.queue = 7;
        x.e.queue_ticket = ++ticket;
        x.e.list_count = 1;
        x.e.refs_offset = offsetof(decltype(x), ref);
        x.e.ref_stride = sizeof(na_list_ref);
        x.ref.list = list;
        x.ref.recording_generation = gen ? gen : generation;
        x.e.hdr.kind = NA_EV_QUEUE_EXECUTE;
        // Send the whole struct as the event (refs follow the fixed part).
        na_ev_queue_execute head = x.e;
        Event(head, NA_EV_QUEUE_EXECUTE, &x.ref, sizeof(x.ref));
    }
    void Present() { na_ev_present p{}; p.swapchain = 99; p.present_ordinal = ticket; Event(p, NA_EV_PRESENT); }

    // One realistic frame. Ids: 3 main depth, 4 shadow map, 5 half-res depth,
    // 6 scene colour, 7 back buffer, 20 camera CBV, 21 per-object CBV.
    void Frame(bool with_unknown_pso_and_dispatch = false) {
        Reset();
        SetPipeline(10);
        RootCbv(0, 20, 256); // per-frame view block, bound for every draw
        Targets({}, 4);
        for (int i = 0; i < 50; ++i) Draw(3000); // shadow pass: most vertices
        Targets({6}, 3);
        for (int i = 0; i < 100; ++i) {
            RootCbv(1, 21, static_cast<uint64_t>(i) * 256); // per-object constants
            Draw(1000);
        }
        if (with_unknown_pso_and_dispatch) {
            Dispatch();
            SetPipeline(77); // never created: unknown state
            Draw(500);
            SetPipeline(10);
        }
        Targets({}, 5);
        for (int i = 0; i < 10; ++i) Draw(100);
        SetPipeline(11); // HUD: depth off, alpha blend
        Targets({7}, 0);
        Draw(6);
        Execute();
        Present();
    }
};
} // namespace

int main(int argc,char** argv) {
    const bool benchmark=argc>1&&!std::strcmp(argv[1],"--rank-benchmark");
    return test::run({
        {"direct camera rank matches sorted ranking, including ties and missing blocks", [benchmark] {
             Feed f;f.Swapchain(1920,1080);f.Resource(3,1920,1080,40);f.Resource(6,1920,1080,10);f.Pipeline(10,1,1,0);
             f.Reset();f.SetPipeline(10);f.Targets({6},3);
             for(unsigned i=0;i<256;++i){
                 f.RootCbv(0,1000+i%16,(i/16)*256);
                 for(unsigned j=0;j<1+i%24;++j)f.Draw(100);
             }
             f.Execute();f.Present();
             CHECK(f.h.RankCameraBlocks(512).size()>100);
             const auto reference=[&](unsigned i,size_t top){
                 const auto ranked=f.h.RankCameraBlocks(top);
                 for(size_t k=0;k<ranked.size();++k)
                     if(ranked[k].resource==1000+i%16&&ranked[k].offset==(i/16)*256)return k+1;
                 return size_t{0};
             };
             for(size_t top: {size_t{0},size_t{1},size_t{4},size_t{8},size_t{512}})
                 for(unsigned i=0;i<320;++i)CHECK(f.h.CameraRank(1000+i%16,(i/16)*256,top)==reference(i,top));
             if(benchmark){
                 for(bool direct:{false,true,true,false}){
                     uint64_t sum=0;const auto start=std::chrono::steady_clock::now();
                     constexpr unsigned queries=65536;
                     for(unsigned n=0;n<queries;++n){const unsigned i=n%320;
                         sum+=direct?f.h.CameraRank(1000+i%16,(i/16)*256,4):reference(i,4);
                     }
                     const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                     std::printf("camera_rank direct=%u queries=%u total_ms=%.4f per_64_ms=%.6f checksum=%llu\n",unsigned(direct),queries,ms,ms*64/queries,static_cast<unsigned long long>(sum));
                 }
             }
         }},
        {"successful resize updates depth aspect; rejected resize leaves it unchanged", [] {
             Feed f;
             f.Swapchain(1024,768);
             f.Resource(3,853,480,40);
             f.Resource(4,2048,2048,40);
             f.Resource(6,853,480,10);
             f.Resource(7,1280,720,28);
             f.Pipeline(10,1,1,0);f.Pipeline(11,0,0,1);
             for(int i=0;i<3;++i)f.Frame();
             auto rank=f.h.RankDepth(4);
             CHECK(!rank.empty() && rank[0].score==0);
             na_ev_swapchain_resized resize{};
             resize.swapchain=99;resize.width=1280;resize.height=720;
             resize.hresult=static_cast<int32_t>(0x80004005u);
             f.Event(resize,NA_EV_SWAPCHAIN_RESIZED);
             CHECK(f.h.SwapchainWidth()==1024 && f.h.SwapchainHeight()==768);
             resize.hresult=0;f.Event(resize,NA_EV_SWAPCHAIN_RESIZED);
             for(int i=0;i<3;++i)f.Frame();
             rank=f.h.RankDepth(4);
             CHECK(!rank.empty() && rank[0].resource==3 && rank[0].score>0);
             CHECK(f.h.RecentTopDepths().Count(3)>=2);
             resize.width=1024;resize.height=768;resize.hresult=static_cast<int32_t>(0x80004005u);
             f.Event(resize,NA_EV_SWAPCHAIN_RESIZED);
             CHECK(f.h.SwapchainWidth()==1280 && f.h.SwapchainHeight()==720);
         }},
        {"generic_depth picks the swapchain-aspect main depth over shadow and half-res buffers", [] {
             Feed f;
             f.Swapchain(1920, 1080);
             f.Resource(3, 1920, 1080, 40);
             f.Resource(4, 2048, 2048, 40);
             f.Resource(5, 960, 540, 40);
             f.Resource(6, 1920, 1080, 10);
             f.Resource(7, 1920, 1080, 28);
             f.Pipeline(10, 1, 1, 0);
             f.Pipeline(11, 0, 0, 1);
             f.Frame();
             auto first = f.h.RankDepth(4);
             CHECK(!first.empty());
             bool waits = true;
             for (const auto& c : first) waits = waits && c.score == 0; // first frame: generic_depth waits
             CHECK(waits);
             f.Frame();
             f.Frame();
             const auto ranked = f.h.RankDepth(4);
             CHECK(ranked.size() == 3);
             CHECK(ranked[0].resource == 3 && ranked[0].score > 0 && ranked[0].exact_match);
             CHECK(ranked[0].vertices == 100000 && ranked[0].draws == 100);
             bool shadow_filtered = false, half_filtered = false;
             for (const auto& c : ranked) {
                 if (c.resource == 4) shadow_filtered = c.score == 0 && !c.aspect_match;
                 if (c.resource == 5) half_filtered = c.score == 0 && !c.aspect_match; // 1920/960 = 2.0 > 1.85
             }
             CHECK(shadow_filtered && half_filtered);
             CHECK(ranked[0].last.queue == 7 && ranked[0].last.list == 5 && ranked[0].last.ticket == 3);
         }},
        {"render-small Performance: a half-size scene depth passes, shadow and quarter-size still fail", [] {
             // Anno at Performance renders 1720x720 for a 3440x1440 swapchain (2026-11-03).
             Feed f;
             f.h.AllowRenderScale(0.5);
             f.Swapchain(1920, 1080);
             f.Resource(3, 960, 540, 40);   // the scene depth, exactly half
             f.Resource(4, 2048, 2048, 40); // shadow map
             f.Resource(5, 480, 270, 40);   // quarter-size side buffer
             f.Resource(6, 960, 540, 10);
             f.Resource(7, 1920, 1080, 28);
             f.Pipeline(10, 1, 1, 0);
             f.Pipeline(11, 0, 0, 1);
             f.Frame();
             f.Frame();
             f.Frame();
             const auto ranked = f.h.RankDepth(4);
             CHECK(!ranked.empty());
             CHECK(ranked[0].resource == 3 && ranked[0].score > 0 && ranked[0].aspect_match);
             bool shadow_filtered = false, quarter_filtered = false;
             for (const auto& c : ranked) {
                 if (c.resource == 4) shadow_filtered = c.score == 0 && !c.aspect_match;
                 if (c.resource == 5) quarter_filtered = c.score == 0 && !c.aspect_match;
             }
             CHECK(shadow_filtered && quarter_filtered);
         }},
        {"native Ultra Performance: one-third scene remains eligible while shadow and quarter-size are rejected", [] {
             Feed f;f.h.AllowRenderScale(1.0/3.0);f.Swapchain(1920,1080);
             f.Resource(3,640,360,40);f.Resource(4,2048,2048,40);f.Resource(5,480,270,40);
             f.Resource(6,640,360,10);f.Resource(7,1920,1080,28);f.Pipeline(10,1,1,0);f.Pipeline(11,0,0,1);
             f.Frame();f.Frame();f.Frame();const auto ranked=f.h.RankDepth(4);
             CHECK(!ranked.empty());CHECK(ranked[0].resource==3&&ranked[0].score>0&&ranked[0].aspect_match);
             for(const auto& candidate:ranked)if(candidate.resource==4||candidate.resource==5)CHECK(candidate.score==0&&!candidate.aspect_match);
         }},
        {"a loading-screen frame does not replace the last real frame", [] {
             Feed f;
             f.Swapchain(1920, 1080);
             f.Resource(3, 1920, 1080, 40);
             f.Resource(4, 2048, 2048, 40);
             f.Resource(5, 960, 540, 40);
             f.Resource(6, 1920, 1080, 10);
             f.Resource(7, 1920, 1080, 28);
             f.Pipeline(10, 1, 1, 0);
             f.Pipeline(11, 0, 0, 1);
             for (int i = 0; i < 3; ++i) f.Frame();
             f.Reset(); // loading screen: one depth-stencil, 4 draws
             f.SetPipeline(10);
             f.Targets({}, 5);
             for (int i = 0; i < 4; ++i) f.Draw(6);
             f.Execute();
             f.Present();
             const auto ranked = f.h.RankDepth(1);
             CHECK(!ranked.empty() && ranked[0].resource == 3 && ranked[0].score > 0);
         }},
        {"pre-HUD scene colour outranks the HUD composite target", [] {
             Feed f;
             f.Swapchain(1920, 1080);
             f.Resource(3, 1920, 1080, 40);
             f.Resource(4, 2048, 2048, 40);
             f.Resource(5, 960, 540, 40);
             f.Resource(6, 1920, 1080, 10);
             f.Resource(7, 1920, 1080, 28);
             f.Pipeline(10, 1, 1, 0);
             f.Pipeline(11, 0, 0, 1);
             f.Frame();
             const auto colour = f.h.RankSceneColor(4);
             CHECK(!colour.empty() && colour[0].resource == 6 && colour[0].written_before_hud && !colour[0].receives_hud);
             bool composite = false;
             for (const auto& c : colour) if (c.resource == 7) composite = c.receives_hud && c.score < colour[0].score;
             CHECK(composite);
             CHECK(colour[0].last.hud_begin > colour[0].last.scene_end && colour[0].last.scene_end > 0);
             CHECK(f.h.Stats().hud_boundaries >= 1);
         }},
        {"the per-frame camera block is the CBV bound for the most draws", [] {
             Feed f;
             f.Swapchain(1920, 1080);
             f.Resource(3, 1920, 1080, 40);
             f.Resource(6, 1920, 1080, 10);
             f.Pipeline(10, 1, 1, 0);
             f.Pipeline(11, 0, 0, 1);
             f.Frame();
             const auto blocks = f.h.RankCameraBlocks(4);
             // Per-object ranges get one draw each (< 8, unranked); the last one
             // stays bound for the remaining passes and ranks far behind.
             CHECK(!blocks.empty() && blocks[0].resource == 20 && blocks[0].offset == 256 && blocks[0].draws == 161 && blocks[0].share > 0.99);
             for (size_t i = 1; i < blocks.size(); ++i) CHECK(blocks[i].resource == 21 && blocks[i].draws * 4 < blocks[0].draws);
             CHECK(f.h.CameraRank(20, 256) == 1 && f.h.CameraRank(21, 0) == 0);
         }},
        {"camera discovery can stop and resume without changing depth and colour selection", [] {
             Feed on,off;off.h.SetCameraDiscovery(false); // also survives the first session/epoch reset
             for(auto* f:{&on,&off}){
                 f->Swapchain(1920,1080);f->Resource(3,1920,1080,40);f->Resource(6,1920,1080,10);
                 f->Pipeline(10,1,1,0);f->Pipeline(11,0,0,1);
             }
             for(unsigned frame=0;frame<5;++frame){
                 on.Frame();off.Frame();
                 CHECK(off.h.RankCameraBlocks(512).empty() && off.h.CameraRank(20,256)==0);
                 const auto a=on.h.RankDepth(8),b=off.h.RankDepth(8);
                 CHECK(a.size()==b.size());
                 for(size_t i=0;i<std::min(a.size(),b.size());++i){CHECK(a[i].resource==b[i].resource && a[i].score==b[i].score && a[i].draws==b[i].draws);}
                 const auto x=on.h.RankSceneColor(8,3),y=off.h.RankSceneColor(8,3);
                 CHECK(x.size()==y.size());
                 for(size_t i=0;i<std::min(x.size(),y.size());++i){CHECK(x[i].resource==y[i].resource && x[i].score==y[i].score && x[i].draws==y[i].draws);}
             }
             CHECK(on.h.CameraRank(20,256)==1);
             off.h.SetCameraDiscovery(true);off.Frame();CHECK(off.h.CameraRank(20,256)==1);
             off.h.SetCameraDiscovery(false);CHECK(off.h.RankCameraBlocks(512).empty());
             off.Execute();off.Present();CHECK(off.h.RankCameraBlocks(512).empty()); // cached recording cannot revive camera data
         }},
        {"R41-1: a truncated target array with a wide stride reads only the carried prefix", [] {
             Feed f;
             f.Reset();
             // Advertises 2 RTVs with a 2-element stride but carries one stride of bytes.
             std::vector<unsigned char> payload(2 * sizeof(na_view_ref));
             na_view_ref v{};
             v.resource = 6;
             v.descriptor_kind = NA_DESC_RTV;
             std::memcpy(payload.data(), &v, sizeof(v));
             na_ev_list_op e{};
             e.op = NA_OP_OM_SET_RENDER_TARGETS;
             e.a[0] = 2;
             e.hdr.flags = NA_EVF_TRUNCATED;
             f.Op(e, payload.data(), payload.size(), static_cast<uint32_t>(payload.size()));
             f.SetPipeline(10);
             f.Draw(3);
             f.Execute();
             CHECK(f.h.Stats().draws == 1); // reached without reading past the payload (ASAN-checked)
         }},
        {"R41-2: an evicted list returning at capacity keeps its own recording", [] {
             Feed f;
             const auto OpOn = [&](uint64_t list, uint32_t kind) {
                 na_ev_list_op e{};
                 e.op = kind;
                 e.a[0] = 10;
                 e.list = list;
                 e.recording_generation = 1;
                 e.op_index = 1;
                 f.Event(e, NA_EV_LIST_OP);
             };
             for (uint64_t list = 1; list <= 4097; ++list) OpOn(list, NA_OP_SET_PIPELINE);
             OpOn(1, NA_OP_SET_PIPELINE); // list 1 was evicted; it must not evict itself
             OpOn(1, NA_OP_DRAW);
             f.Execute(1, 1);
             CHECK(f.h.Stats().draws == 1);
         }},
        {"R41-4: a retired camera buffer leaves the camera ranking", [] {
             Feed f;
             f.Swapchain(1920, 1080);
             f.Resource(3, 1920, 1080, 40);
             f.Resource(4, 2048, 2048, 40);
             f.Resource(6, 1920, 1080, 10);
             f.Pipeline(10, 1, 1, 0);
             f.Pipeline(11, 0, 0, 1);
             f.Frame();
             f.Frame();
             bool before = false;
             for (const auto& b : f.h.RankCameraBlocks(4)) before = before || b.resource == 20;
             CHECK(before);
             na_ev_object_retired r{};
             r.object = 20;
             f.Event(r, NA_EV_OBJECT_RETIRED);
             for (const auto& b : f.h.RankCameraBlocks(4)) CHECK(b.resource != 20);
             CHECK(f.h.CameraRank(20, 256, 4) == 0);
         }},
        {"scene colour is the target drawn with the selected depth, not a bigger earlier pass (Anno pattern)", [] {
             Feed f;
             f.Swapchain(1920, 1080);
             f.Resource(3, 1920, 1080, 40);
             f.Resource(6, 1920, 1080, 10);  // HDR scene colour (drawn with depth 3)
             f.Resource(8, 1920, 1080, 26);  // earlier full-screen pass, more geometry, no depth
             f.Resource(7, 1920, 1080, 28);
             f.Pipeline(10, 1, 1, 0);
             f.Pipeline(11, 0, 0, 1);
             for (int frame = 0; frame < 3; ++frame) {
                 f.Reset();
                 f.SetPipeline(10);
                 f.Targets({8}, 0);
                 for (int i = 0; i < 200; ++i) f.Draw(5000);   // 1M vertices, no depth bound
                 f.Targets({6}, 3);
                 for (int i = 0; i < 100; ++i) f.Draw(1000);   // 100k vertices with the scene depth
                 f.SetPipeline(11);
                 f.Targets({7}, 0);
                 f.Draw(6);
                 f.Execute();
                 f.Present();
             }
             const auto c = f.h.RankSceneColor(2);
             CHECK(c.size() == 2 && c[0].resource == 6 && c[0].paired_depth == 3 && c[0].paired_draws == 100);
             CHECK(c[1].resource == 8 && c[1].paired_draws == 0);
         }},
        {"unknown pipelines and dispatches weaken reasons but never zero the ranking", [] {
             Feed f;
             f.Swapchain(1920, 1080);
             f.Resource(3, 1920, 1080, 40);
             f.Resource(4, 2048, 2048, 40);
             f.Resource(5, 960, 540, 40);
             f.Resource(6, 1920, 1080, 10);
             f.Resource(7, 1920, 1080, 28);
             f.Pipeline(10, 1, 1, 0);
             f.Pipeline(11, 0, 0, 1);
             for (int i = 0; i < 3; ++i) f.Frame(true);
             const auto ranked = f.h.RankDepth(1);
             CHECK(!ranked.empty() && ranked[0].resource == 3 && ranked[0].score > 0);
             CHECK(ranked[0].draws == 101 && ranked[0].vertices == 100500);
             bool noted = false;
             for (const auto& r : ranked[0].reasons) noted = noted || r.find("unobserved pipelines") != std::string::npos;
             CHECK(noted && f.h.Stats().unknown_pipelines >= 1);
         }},
    });
}
