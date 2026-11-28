#include "host_dx12/j2/compute_flow.hpp"
#include "host_dx12/j2/flow_shaders.hpp"
#include "host_dx12/settings.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "core/first_setup.hpp"
#include "host_dx12/ffx_flow/ffx_flow.hpp"
#include "host_dx12/ffx_flow/ffx_flow_shaders.hpp"
#include "host_dx12/nvof/flow_expand.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/diag/vram_ledger.hpp"
#include "host_dx12/diag/event_log.hpp"
#include "host_dx12/diag/release_trace.hpp"
#include <windows.h>
#include <string>
#include "core/motion_arbiter_gpu.hpp"
#include "core/camera_fit_gpu.hpp"
#include "core/jitter_probe_gpu.hpp"
#include "core/screen_motion.hpp"
#include <d3dcompiler.h>
#include <mutex>
#include <thread>
#include <utility>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace na::j2::compute_flow {
std::atomic<uint32_t> g_loosen{settings::kObjectFlowLoosen}; // tests can exercise other thresholds
std::atomic<bool> g_candidates{true}; // SetFrameCandidates; read at each Record
std::atomic<bool> g_screen_fit{true}; // SetScreenFit; read at each Record
using Microsoft::WRL::ComPtr;using Lease=na::nr::native::RecordingLease;
namespace {
// Kept motion pictures made up front: at most two were ever in use at once in the fixtures (this frame's and the last;
// the GPU finishes a pair before the next is recorded); one more for a frame in flight. More are made on demand
// (Record), so fewer up front changes no output (2026-11-27: 8 -> 3, about 5 x 59 MB less at 3440x1440).
constexpr unsigned warm_images=3;constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
// Preserve small luminance differences at the same 32-bit storage cost as RGBA8. Alpha is
// always 1 and motion reads RGB only. Query once per context, not from the per-frame hot path.
DXGI_FORMAT stabilized_format(ID3D12Device* device){
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};support.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    constexpr UINT required=D3D12_FORMAT_SUPPORT1_TEXTURE2D|D3D12_FORMAT_SUPPORT1_SHADER_LOAD|
        D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE|D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
    if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof support)) &&
       (support.Support1&required)==required && (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return support.Format;
    return DXGI_FORMAT_R8G8B8A8_UNORM; // retain the existing path if packed10 is unsupported
}
D3D12_RESOURCE_BARRIER tr(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return x;}
void move(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){auto x=tr(r,a,b);l->ResourceBarrier(1,&x);}
void order(ID3D12GraphicsCommandList* l){D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;l->ResourceBarrier(1,&x);}
// Same layout and gate values as j2_flow.cpp's Constants/k* (the Anno-run tuning):
// snap 0.26 px (Cort 2026-11-06: 0.5 -> 0.375 fixed the sweatshirt blur, then 30% more; the slider does not move it), image ratio 0.5, temporal agreement (one previous pair, 60 deg, 3 px).
struct Constants {unsigned w,h,ew,eh,mode,block,cost,cost_max,offset,gw,gh;float ratio=.5f;float snap=.26f,margin=0.f,absolute=.08f,texture=.03f;
    unsigned have_hist=0,agree_min=1;float dj_x=0,dj_y=0;float agree_px=3.f,agree_cos=.5f,jitter_x=0,jitter_y=0;
    float history_dx=0,history_dy=0;unsigned frame_now=0,frame_reset=0;}; // frame_*: the scene-cut state pass (flow_shaders.hpp cut_state)
static_assert(sizeof(Constants)==28*4,"root constants");
constexpr unsigned kTable=12,kUav=8; // descriptors per table: SRVs t0..t7, then UAVs u0..u3
constexpr unsigned kTables=8; // per recording: encode, accept or measure, the scene-cut state, the scene-cut picture, the frame candidates, the screen fit's three programs
constexpr unsigned kCutTile=32; // pixels per group axis in flow_shaders.hpp cut_show
constexpr unsigned kFitHead=20,kFitValues=na::motion::screen::kValues; // flow_shaders.hpp FIT_HEAD, FIT_VALUES
constexpr unsigned kReadback=272; // counters 0..39, timestamps 48..63, the frame candidates' words 64..103, the screen fit's head 112..191 (the far group, or the one fit of every depth), the near group's 192..271
constexpr unsigned kFitTile=5; // flow_shaders.hpp FIT_TILE
static_assert(kFitValues==81,"flow_shaders.hpp FIT_VALUES");
struct Image {ComPtr<ID3D12Resource> tex,raw_color;std::vector<Lease> uses;Lease producer;uint64_t frame=0,colour_epoch=0;unsigned generation=0;float jitter[2]{},raster_jitter[2]{};bool raster_confirmed=false;};
struct Slot {
    Lease lease;bool pair=false,submitted=false,fit_only=false,probe=false;uint64_t frame=0;
    bool total=false,cut_seed=false; // a measured-total recording; it also zeroes the scene-cut state for the first time
    bool candidates=false; // the frame-candidates pass ran in this recording (its words are in the readback)
    bool screen=false; // the screen fit ran in this recording (its head is in the readback)
    Lease after; // RecordCutShow on another list than Record's: the descriptors stay this recording's until that one has run
    ComPtr<ID3D12DescriptorHeap> heap,engine_heap,expand_heap,derive_heap;
    ComPtr<ID3D12Resource> readback;ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> color_input,depth_input;
    int hist_written=-1; // temporal history this recording's pair writes (published at Committed)
};
struct Context {
    // Video memory (diag/vram_ledger.hpp): this context's item, and when it last measured a frame (an
    // unused context of another size is given back after kDropAfterMs).
    std::string vram_name;ULONGLONG used_ms=0;
    ~Context(){if(!vram_name.empty())na::vram::Set(vram_name.c_str(),0,"measured motion at this size is no longer used: released");}
    void note_vram(const char* why);
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;unsigned w=0,h=0,stride=0;int last=-1;
    DXGI_FORMAT image_format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso_encode,pso_accept;
    ComPtr<ID3D12PipelineState> pso_measure,pso_cut_state,pso_cut_show;
    ComPtr<ID3D12Resource> raw,expanded,residual,valid,mask,counters,zero,hist[2];
    ComPtr<ID3D12Resource> total,cut_state; // measured total motion (RG32F); the scene cut's 16-byte state, always UAV
    // VARIANT: flow/frame-candidates (flow_shaders.hpp kCandidateShader). All four programs or none.
    ComPtr<ID3D12PipelineState> pso_cand_clear,pso_cand_count,pso_cand_pick,pso_cand_apply;
    ComPtr<ID3D12Resource> votes,grid,top; // 256x256 R32_UINT, always UAV; the grid handed on (R16G16_SINT, one vector per 8x8 block); 48 bytes, always UAV
    // VARIANT: motion/screen-fit (flow_shaders.hpp fit_sample / fit_accum / fit_solve). All three programs or none.
    ComPtr<ID3D12PipelineState> pso_fit_sample,pso_fit_accum,pso_fit_solve,pso_fit_reduce;
    ComPtr<ID3D12Resource> fit_grid,fit_model,fit_state; // RGBA32F, one sample per 8x8 block; RG32F 9x2, the model `measure` reads (a row per group of depths); the terms, the tiles' sums and the tiles' own words, always UAV
    unsigned fit_words=0; // fit_state's size in words
    bool cut_seeded=false; // cut_state has been zeroed by a submitted recording
    unsigned hist_read=0;uint64_t hist_frame=0;unsigned hist_generation=0; // last committed pair's candidates
    UINT64 frequency=0;
    na::ffx_flow::PairFlow engine;na::nvof::FlowExpand expand;na::motion::gpu::Arbiter derive;na::motion::gpu::CameraFit fit;
    na::jitter::gpu::Probe jitter_probe;na::jitter::ColourDomain colour_jitter{na::jitter::kStageFlow};
    na::jitter::Evidence jitter_window{};uint32_t jitter_window_frames=0;uint64_t jitter_last_frame=0;bool raster_confirmed=false;
    std::vector<std::unique_ptr<Image>> images;std::vector<std::unique_ptr<Slot>> leases;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(unsigned slot,unsigned table,unsigned i){auto a=leases[slot]->heap->GetCPUDescriptorHandleForHeapStart();a.ptr+=SIZE_T(table*kTable+i)*stride;return a;}
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(unsigned slot,unsigned table){auto a=leases[slot]->heap->GetGPUDescriptorHandleForHeapStart();a.ptr+=UINT64(table*kTable)*stride;return a;}
    bool tex(DXGI_FORMAT f,ComPtr<ID3D12Resource>& r){return tex(f,r,w,h,S);}
    bool tex(DXGI_FORMAT f,ComPtr<ID3D12Resource>& r,unsigned width,unsigned height,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=f;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));
    }
    bool buffer(unsigned bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& r){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(type==D3D12_HEAP_TYPE_DEFAULT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));
    }
    void srv(unsigned slot,unsigned table,unsigned i,ID3D12Resource* r,DXGI_FORMAT f){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;device->CreateShaderResourceView(r,&d,cpu(slot,table,i));}
    void uav(unsigned slot,unsigned table,unsigned i,ID3D12Resource* r,DXGI_FORMAT f){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=f;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(r,nullptr,&d,cpu(slot,table,i));}
    void defaults(unsigned slot,unsigned table){
        srv(slot,table,0,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);srv(slot,table,1,nullptr,DXGI_FORMAT_R32G32_FLOAT);
        srv(slot,table,2,nullptr,DXGI_FORMAT_R32G32_FLOAT);srv(slot,table,3,nullptr,DXGI_FORMAT_R32_FLOAT);
        srv(slot,table,4,nullptr,DXGI_FORMAT_R8_UINT);srv(slot,table,5,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);srv(slot,table,6,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);
        srv(slot,table,7,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(slot,table,kUav+0,nullptr,DXGI_FORMAT_R8G8B8A8_UNORM);uav(slot,table,kUav+1,nullptr,DXGI_FORMAT_R32_FLOAT);uav(slot,table,kUav+3,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT);
        raw_uav(slot,table,kUav+2,counters.Get(),12);
    }
    void raw_uav(unsigned slot,unsigned table,unsigned i,ID3D12Resource* r,unsigned words){
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=words;d.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(r,nullptr,&d,cpu(slot,table,i));
    }
    // Consecutive internal passes may keep the same heap and root signature.
    // Entry points that follow an external pass must bind them again.
    void bind_compute(ID3D12GraphicsCommandList* l,unsigned slot,unsigned table){
        ID3D12DescriptorHeap* heaps[]={leases[slot]->heap.Get()};
        l->SetDescriptorHeaps(1,heaps);
        l->SetComputeRootSignature(root.Get());
        l->SetComputeRootDescriptorTable(0,gpu(slot,table));
    }
    // one=true: a single group. With groups_x supplied, use that explicit grid.
    void dispatch_bound(ID3D12GraphicsCommandList* l,ID3D12PipelineState* pso,const Constants& c,bool one=false,unsigned groups_x=0,unsigned groups_y=0){
        l->SetPipelineState(pso);
        l->SetComputeRoot32BitConstants(1,sizeof(Constants)/4,&c,0);
        if(one)l->Dispatch(1,1,1);else if(groups_x)l->Dispatch(groups_x,groups_y,1);else l->Dispatch((w+7)/8,(h+7)/8,1);
    }
    void dispatch(ID3D12GraphicsCommandList* l,unsigned slot,unsigned table,ID3D12PipelineState* pso,const Constants& c,bool one=false,unsigned groups_x=0,unsigned groups_y=0){
        bind_compute(l,slot,table);
        dispatch_bound(l,pso,c,one,groups_x,groups_y);
    }
};
std::mutex mutex;std::shared_ptr<Context> current;Stats stats;uint64_t last_submit_tick=0,sample_tick=0;
// The contexts of other picture sizes on the same device and queue, kept whole (a folded DLSS level
// measures at its own size; switching levels must not build programs on the frame's own time), and
// the sizes Prepare is building on its own thread.
std::vector<std::shared_ptr<Context>> cached;std::vector<std::pair<unsigned,unsigned>> building;
std::vector<std::pair<unsigned,unsigned>> warm_failed; // sizes whose build on its own thread failed (Warm)
constexpr size_t kCachedContexts=3;
constexpr ULONGLONG kDropAfterMs=5000; // a context of another size not used this long is released
struct FailedSize{unsigned w,h;ULONGLONG at_ms;};
std::vector<FailedSize> failed_sizes; // under mutex
// The context's video memory as one item (its own textures; AMD's flow pyramids are a few MB more).
void Context::note_vram(const char* why){
    uint64_t bytes=0;
    for(auto* r:{raw.Get(),expanded.Get(),residual.Get(),valid.Get(),mask.Get(),counters.Get(),zero.Get(),hist[0].Get(),hist[1].Get(),total.Get(),cut_state.Get(),
                  votes.Get(),grid.Get(),top.Get(),fit_grid.Get(),fit_model.Get(),fit_state.Get()})bytes+=na::vram::SizeOf(r);
    for(const auto& i:images)if(i)bytes+=na::vram::SizeOf(i->tex.Get())+na::vram::SizeOf(i->raw_color.Get());
    if(vram_name.empty()){char n[48];std::snprintf(n,sizeof n,"motion_context_%ux%u",w,h);vram_name=n;}
    na::vram::Set(vram_name.c_str(),bytes,why);
}
// Contexts of other sizes nobody measured with for kDropAfterMs (a level left, an old picture size) are
// released: the GPU finished with them long before (a ticket still holding one keeps it until committed).
void drop_unused_locked(ULONGLONG now){
    // Released on a thread of their own: freeing a whole working set (dozens of textures) took a frame's worth of
    // time on the frame (the measured DX11 fixture: a 66 ms frame against 35 ms).
    std::vector<std::shared_ptr<Context>> dropped;
    for(auto it=cached.begin();it!=cached.end();){if(now-(*it)->used_ms>kDropAfterMs){dropped.push_back(std::move(*it));it=cached.erase(it);}else ++it;}
    // Normal priority (2026-11-27): a release below normal priority can hold the driver's lock while it is
    // pre-empted, and the game's thread then waits behind it. Timed for the log (release_trace).
    if(!dropped.empty())std::thread([d=std::move(dropped)]() mutable {
        for(auto& ctx:d){
            char what[64];std::snprintf(what,sizeof what,"motion_context_%ux%u",ctx->w,ctx->h);
            const ULONGLONG t0=GetTickCount64();LARGE_INTEGER q0{},q1{},hz{};QueryPerformanceCounter(&q0);
            ctx.reset();
            QueryPerformanceCounter(&q1);QueryPerformanceFrequency(&hz);
            const double ms=double(q1.QuadPart-q0.QuadPart)*1000.0/double(hz.QuadPart);
            na::diag::release_trace::Note(what,ms,t0);
            na::eventlog::Emit("release_trace","what=%s thread=background ms=%.2f at_tick=%llu",what,ms,static_cast<unsigned long long>(t0));
        }
    }).detach();
}
void reason(const char* r){strncpy_s(stats.reason,r,_TRUNCATE);}
bool compile_flow_shader(const char* source,size_t bytes,const char* name,const char* entry,ComPtr<ID3DBlob>& result){
    // Fixed source strings and entry point names. Bytecode has no dependency on
    // the device, queue, or extent. Each context still owns its own pipelines.
    // Called under the flow mutex, and by Prepare's thread without it: the cache has its own lock.
    static std::mutex cache_mutex;
    std::lock_guard<std::mutex> cache_lock(cache_mutex);
    struct Shader {
        const char* source;
        const char* entry;
        ComPtr<ID3DBlob> bytes;
    };
    static std::vector<Shader> cache;
    for(const auto& shader:cache){
        if(shader.source!=source||std::strcmp(shader.entry,entry)!=0)continue;
        result=shader.bytes;
        return true;
    }
    if(FAILED(D3DCompile(source,bytes,name,nullptr,nullptr,entry,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&result,nullptr)))return false;
    cache.push_back({source,entry,result});
    return true;
}
bool programs(Context& g){
#include "compute_flow_programs.inc"
}
bool build(Context& c,ID3D12Device* device,ID3D12CommandQueue* queue,unsigned w,unsigned h){
    c.device=device;c.queue=queue;c.w=w;c.h=h;c.stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    c.image_format=stabilized_format(device);
    if(!c.engine.Init(device,w,h,1)){reason(c.engine.Error());return false;}
    if(!programs(c) || !c.expand.Init(device,1) || !c.derive.Init(device,1))return false;
    c.fit.Init(device); // optional: without it the camera motion is used unchanged (Ready() false)
    c.jitter_probe.Init(device);
    for(unsigned i=0;i<warm_images;++i){auto image=std::make_unique<Image>();if(!c.tex(c.image_format,image->tex) || !c.tex(DXGI_FORMAT_R8G8B8A8_UNORM,image->raw_color))return false;c.images.push_back(std::move(image));}
    if(!c.tex(DXGI_FORMAT_R32G32_FLOAT,c.raw) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.expanded) || !c.tex(DXGI_FORMAT_R32G32_FLOAT,c.residual) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.valid) || !c.tex(DXGI_FORMAT_R32_FLOAT,c.mask) ||
       !c.tex(DXGI_FORMAT_R32G32_FLOAT,c.total) || !c.buffer(16,D3D12_HEAP_TYPE_DEFAULT,U,c.cut_state) ||
       !c.tex(DXGI_FORMAT_R16G16B16A16_FLOAT,c.hist[0]) || !c.tex(DXGI_FORMAT_R16G16B16A16_FLOAT,c.hist[1]) ||
       !c.buffer(48,D3D12_HEAP_TYPE_DEFAULT,U,c.counters) || !c.buffer(96,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,c.zero))return false;
    void* p=nullptr;if(FAILED(c.zero->Map(0,nullptr,&p)))return false;memset(p,0,96);c.zero->Unmap(0,nullptr);
    // The screen fit is optional: without its programs or working memory `measure` has no model to
    // follow, pixels the pictures cannot measure are not known, and the j2screenfit line says on=0.
    {const unsigned gw=(w+7)/8,gh=(h+7)/8;c.fit_words=kFitHead+((gw+7)/8)*((gh+7)/8)*(kFitValues+kFitTile)+3*kFitValues;} // a thread group is one tile of 8 x 8 samples (flow_shaders.hpp fit_tiles)
    if(c.pso_fit_solve && !(c.tex(DXGI_FORMAT_R32G32B32A32_FLOAT,c.fit_grid,(w+7)/8,(h+7)/8,S) && c.tex(DXGI_FORMAT_R32G32_FLOAT,c.fit_model,9,2,S) &&
        c.buffer(c.fit_words*4,D3D12_HEAP_TYPE_DEFAULT,U,c.fit_state))){
        c.pso_fit_sample.Reset();c.pso_fit_accum.Reset();c.pso_fit_solve.Reset();}
    // The frame candidates are optional: without their programs or working memory the search's grid is used as it is.
    if(c.pso_cand_apply && !(c.tex(DXGI_FORMAT_R32_UINT,c.votes,256,256,U) && c.tex(DXGI_FORMAT_R16G16_SINT,c.grid,(w+7)/8,(h+7)/8,S) && c.buffer(48,D3D12_HEAP_TYPE_DEFAULT,U,c.top))){
        c.pso_cand_clear.Reset();c.pso_cand_count.Reset();c.pso_cand_pick.Reset();c.pso_cand_apply.Reset();}
    queue->GetTimestampFrequency(&c.frequency);c.used_ms=GetTickCount64();
    c.note_vram("measured motion at this picture size (8 kept pictures and the measuring textures)");return true;
}
bool free(Image& i){i.uses.erase(std::remove_if(i.uses.begin(),i.uses.end(),[](const Lease& l){return l.Retired();}),i.uses.end());return i.uses.empty();}
void collect(Context& c){for(size_t index=0;index<c.leases.size();++index){auto& s=*c.leases[index];if((!s.pair && !s.probe) || !s.lease.Retired())continue;
    if(!s.submitted){s.pair=s.probe=false;c.jitter_probe.Discard(static_cast<uint32_t>(index));na::motion::gpu::FitResult drop;c.fit.Collect(static_cast<uint32_t>(index),&drop);continue;} // discarded commands never wrote telemetry
    if(s.probe){na::jitter::Source key;na::jitter::Evidence e;
        if(c.jitter_probe.Collect(static_cast<uint32_t>(index),key,e) && key==c.colour_jitter.Key() && e.frame>c.jitter_last_frame){
            c.jitter_last_frame=e.frame;auto& w=c.jitter_window;w.frame=e.frame;w.tested+=e.tested;
            w.unshifted+=e.unshifted;w.raster+=e.raster;w.opposite+=e.opposite;
            if(++c.jitter_window_frames>=60){
                if(c.colour_jitter.Observe(key,w))++stats.colour_jitter_changes;
                c.jitter_window={};c.jitter_window_frames=0;
            }
        }s.probe=false;
    }
    if(!s.pair)continue;
    {na::motion::gpu::FitResult r;if(c.fit.Collect(static_cast<uint32_t>(index),&r)){++stats.fit_collected;
        memcpy(stats.fit_theta,r.theta,sizeof r.theta);stats.fit_e0=r.energy0;stats.fit_e=r.energy;stats.fit_n0=r.samples0;stats.fit_n=r.samples;stats.fit_last_accepted=r.accepted;
        if(r.accepted){++stats.fit_accepted;stats.fit_e0_sum+=r.energy0;stats.fit_e_sum+=r.energy;}
        if(r.flow_accepted){++stats.fit_flow_passed;if(!r.accepted)++stats.fit_rejected_image;}
        else if(r.samples>=256 && r.samples0>=256 && r.energy0>.05f && r.energy<.7f*r.energy0)++stats.fit_rejected_jitter; // plain test passed, quantised/jitter test refused
        stats.fit_e_alt=r.energy_alt;stats.fit_photo_tested=r.photo_tested;stats.fit_photo_better=r.photo_better;stats.fit_photo_worse=r.photo_worse;
        stats.fit_photo_regions=r.photo_regions;stats.fit_photo_good=r.photo_regions_good;stats.fit_photo_e_fit=r.photo_e_fit;stats.fit_photo_e_cam=r.photo_e_cam;}}
    if(s.screen){void* p=nullptr;D3D12_RANGE range{112,272}; // the screen fit of this pair (flow_shaders.hpp FIT_HEAD: the fit buffer's first words), far group then near group
        if(SUCCEEDED(s.readback->Map(0,&range,&p))){unsigned v[kFitHead],nv[kFitHead];float f[kFitHead],nf[kFitHead];memcpy(v,static_cast<const unsigned char*>(p)+112,sizeof v);memcpy(f,v,sizeof f);
            memcpy(nv,static_cast<const unsigned char*>(p)+112+kFitHead*4,sizeof nv);memcpy(nf,nv,sizeof nf);const bool split=nf[19]>0;
            const na::motion::screen::Params fp;
            ++stats.screen_pairs;
            // Why a pair has no model, in the order the fit decides it.
            if(split)++stats.screen_split;if(split && nv[12])++stats.screen_near_valid;
            if(v[12] || (split && nv[12]))++stats.screen_valid;else if(f[13]<fp.min_samples)++stats.screen_few;else if(v[16])++stats.screen_unsolved;else ++stats.screen_disagree;
            if(s.frame>=stats.screen_frame){stats.screen_frame=s.frame;memcpy(stats.screen_theta,f,sizeof stats.screen_theta);stats.screen_nearness_limit=f[11];
                stats.screen_last_valid=v[12]!=0;stats.screen_samples=f[13];stats.screen_agree=f[14];stats.screen_energy=f[15];stats.screen_last_unsolved=v[16]!=0;stats.screen_steps=v[17];
                stats.screen_split_nearness=nf[19];memcpy(stats.screen_near_theta,nf,sizeof stats.screen_near_theta);stats.screen_near_last_valid=split && nv[12]!=0;
                stats.screen_near_samples=nf[13];stats.screen_near_agree=nf[14];stats.screen_near_energy=nf[15];stats.screen_near_steps=nv[17];}
            D3D12_RANGE none{};s.readback->Unmap(0,&none);}}
    if(s.candidates){void* p=nullptr;D3D12_RANGE range{64,104}; // the frame candidates of this pair (flow_shaders.hpp kCandidateShader, u2)
        if(SUCCEEDED(s.readback->Map(0,&range,&p))){unsigned v[10];memcpy(v,static_cast<const unsigned char*>(p)+64,sizeof v);
            ++stats.cand_pairs;if(v[8])++stats.cand_pairs_changed;stats.cand_changed+=v[8];stats.cand_offered+=v[9];
            if(s.frame>=stats.cand_frame){stats.cand_frame=s.frame;memcpy(stats.cand_top,v,sizeof stats.cand_top);stats.cand_last_changed=v[8];stats.cand_last_offered=v[9];}
            D3D12_RANGE none{};s.readback->Unmap(0,&none);}}
    if(!s.fit_only){void* p=nullptr;D3D12_RANGE range{0,64};
        if(SUCCEEDED(s.readback->Map(0,&range,&p))){const auto* b=static_cast<const unsigned char*>(p);const auto* v=reinterpret_cast<const unsigned*>(b);
            if(s.total){for(unsigned j=0;j<8;++j)stats.total_sum[j]+=v[j];++stats.total_sum_pairs;
                if(v[8]){++stats.total_cuts;if(s.frame>stats.total_cut_frame)stats.total_cut_frame=s.frame;}} // a scene cut, learned here with no wait
            else {for(unsigned j=0;j<7;++j)stats.sum[j]+=v[j];++stats.sum_pairs;}
            if(s.frame<stats.sample_frame){D3D12_RANGE none0{};s.readback->Unmap(0,&none0);s.pair=false;continue;}
            if(s.total){ // the last sample in the menu's terms: measured, nothing known, following the screen's motion
                stats.accepted=v[0]+v[7];stats.rejected_invalid=v[1];stats.rejected_small=stats.rejected_cost=stats.rejected_flicker=0;stats.rejected_offscreen=v[4];stats.rejected_image=v[2]+v[5];
                stats.total=0;for(unsigned j=0;j<8;++j)stats.total+=v[j];}
            else {stats.accepted=v[0];stats.rejected_invalid=v[1];stats.rejected_small=v[2];stats.rejected_cost=v[3];stats.rejected_offscreen=v[4];stats.rejected_image=v[5];stats.rejected_flicker=v[6];
                stats.total=0;for(unsigned j=0;j<7;++j)stats.total+=v[j];}
            stats.sample_frame=s.frame;sample_tick=GetTickCount64();++stats.samples;
            if(s.timestamps && c.frequency){UINT64 t[2]{};memcpy(t,b+48,16);stats.ms=t[1]>=t[0]?double(t[1]-t[0])*1000.0/double(c.frequency):0;}
            D3D12_RANGE none{};s.readback->Unmap(0,&none);}}
    s.pair=false;
}}
bool prepare_slot(Context& c,unsigned index){
    auto& slot=*c.leases[index];
    if(slot.heap)return true;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,kTables*kTable,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(c.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&slot.heap))) ||
       !c.buffer(kReadback,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,slot.readback)){slot.heap.Reset();return false;}
    D3D12_QUERY_HEAP_DESC qd{};qd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qd.Count=2;
    c.device->CreateQueryHeap(&qd,IID_PPV_ARGS(&slot.timestamps));
    // Null views and the default counter binding are immutable for this slot.
    // Each pass overwrites its own live bindings before dispatch. The only
    // optional live binding is measure's model SRV, which is set even if null.
    for(unsigned table=0;table<kTables;++table)c.defaults(index,table);
    return true;
}
}
Ticket Record(ID3D12GraphicsCommandList* l,ID3D12CommandQueue* queue,ID3D12Resource* color,ID3D12Resource* depth,
              const na::motion::gpu::Inputs& camera,const Frame& f,Tier3& out){
    na::nr::native::Maintain(); // nonblocking retirement collection, before our lock
    std::lock_guard<std::mutex> lock(mutex);out={};
    ++stats.attempts;
    if(!l || !queue || !color || !depth || !na::ffx_flow::Built()){++stats.unavailable;reason("AMD compute shaders unavailable or invalid frame");return {};}
    ComPtr<ID3D12Device> device;if(FAILED(l->GetDevice(IID_PPV_ARGS(&device))))return {};
    auto d=color->GetDesc();const unsigned w=static_cast<unsigned>(d.Width),h=d.Height;
    if(current && (current->device.Get()!=device.Get() || current->queue.Get()!=queue)){current.reset();cached.clear();}
    if(!current || current->w!=w || current->h!=h){
        // Another size: the context kept for it (its last picture pairs only with the frame it names), else a new one.
        std::shared_ptr<Context> next;
        for(auto it=cached.begin();it!=cached.end();++it)if((*it)->device.Get()==device.Get() && (*it)->queue.Get()==queue && (*it)->w==w && (*it)->h==h){next=*it;cached.erase(it);break;}
        if(next)++stats.context_swaps;
        else{
            reason("AMD working resources unavailable; camera motion used");
            const uint64_t began=GetTickCount64();
            next=std::make_shared<Context>();const bool built=build(*next,device.Get(),queue,w,h);
            na::eventlog::Emit("first_setup","motion working set %ux%u %s in %llu ms ON THE GAME'S THREAD (not warmed first)",w,h,built?"built":"FAILED to build",
                               static_cast<unsigned long long>(GetTickCount64()-began));
            if(!built)return {};++stats.resets;
        }
        if(current){cached.push_back(std::move(current));if(cached.size()>kCachedContexts)cached.erase(cached.begin());}
        current=std::move(next);
    }
    auto c=current;
    {const ULONGLONG now=GetTickCount64();c->used_ms=now;drop_unused_locked(now);}
    if(f.total && (!c->pso_measure || !c->pso_cut_state || !c->pso_cut_show)){++stats.unavailable;reason("Measured motion: its GPU programs did not build on this device");return {};}
    const na::jitter::Source colour_key{f.source_generation,w,h,static_cast<uint32_t>(d.Format)};
    if(!(colour_key==c->colour_jitter.Key())){
        c->colour_jitter.Bind(colour_key);c->jitter_window={};c->jitter_window_frames=0;c->jitter_last_frame=0;
    }
    collect(*c);
    const na::jitter::Sample raster{f.raster_sample?f.raster_jitter[0]:f.jitter[0],f.raster_sample?f.raster_jitter[1]:f.jitter[1]};
    // An old cached verdict about the matrix writer is not evidence against actual
    // viewport writes in this recording. Stabilise those real samples immediately.
    const auto encode=f.raster_confirmed?raster:c->colour_jitter.Resolve(raster);
    const auto convention=f.raster_confirmed?na::jitter::Convention::Raster:c->colour_jitter.Get();
    c->raster_confirmed=f.raster_confirmed;
    stats.colour_jitter_convention=static_cast<uint32_t>(convention);stats.colour_jitter_epoch=c->colour_jitter.Epoch();
    // Retain context through every submission/replay or discard of this exact recording.
    auto lease=na::nr::native::RetainRecording(l,c);if(!lease){++stats.unavailable;reason("AMD flow recording lifetime unavailable; camera motion used");return {};}
    for(const auto& s:c->leases)if(!s->lease.Retired() && s->lease.SameRecording(lease)){
        ++stats.busy;reason("AMD flow already belongs to this executable recording; camera motion used");return {};}
    const auto in_use=[](const Slot& s){return (s.lease && !s.lease.Retired()) || (s.after && !s.after.Retired());};
    unsigned work=0;while(work<c->leases.size() && in_use(*c->leases[work]))++work;
    if(work==c->leases.size())c->leases.push_back(std::make_unique<Slot>());
    auto& slot=*c->leases[work];if(!prepare_slot(*c,work)){++stats.unavailable;reason("AMD recording descriptor allocation failed; camera motion used");return {};}
    {uint32_t busy=1;for(unsigned j=0;j<c->images.size();++j)if(static_cast<int>(j)==c->last || !free(*c->images[j]))++busy; // +1: the one this recording takes
     if(busy>stats.images_busy_peak)stats.images_busy_peak=busy;}
    int image=-1;for(unsigned j=0;j<c->images.size();++j)if(static_cast<int>(j)!=c->last && free(*c->images[j])){image=static_cast<int>(j);break;}
    if(image<0){auto next=std::make_unique<Image>();if(!c->tex(c->image_format,next->tex) || !c->tex(DXGI_FORMAT_R8G8B8A8_UNORM,next->raw_color)){++stats.unavailable;reason("AMD history image allocation failed; camera motion used");return {};}
        image=static_cast<int>(c->images.size());c->images.push_back(std::move(next));c->note_vram("measured motion needed one more kept picture");}
    slot.lease=lease;slot.after={};slot.pair=slot.probe=false;slot.submitted=false;slot.fit_only=!f.residual && !f.total;slot.total=f.total;slot.cut_seed=false;slot.candidates=false;slot.screen=false;slot.hist_written=-1;slot.frame=f.frame;auto& im=*c->images[image];im.uses.push_back(lease);im.producer=lease;
    slot.color_input=color;slot.depth_input=depth;
    stats.recording_slots=static_cast<uint32_t>(c->leases.size());stats.image_slots=static_cast<uint32_t>(c->images.size());
    stats.stabilized_format=static_cast<uint32_t>(c->image_format);
    const int previous=c->last;
    // Why there is no pair, in the order the conditions are tested (Ticket::no_pair).
    const auto no_pair=static_cast<uint8_t>(f.reset?1:previous<0?2:(c->images[previous]->frame!=f.previous || !f.previous)?3:c->images[previous]->generation!=f.generation?4:
        c->images[previous]->colour_epoch!=c->colour_jitter.Epoch()?5:c->images[previous]->raster_confirmed!=f.raster_confirmed?6:!c->images[previous]->producer.Ready(queue)?7:0);
    const bool pair=!no_pair;if(no_pair)++stats.no_pair[no_pair];
    Ticket t;t.no_pair=no_pair;t.context=c;t.image=static_cast<unsigned>(image);t.previous=previous<0?0u:static_cast<unsigned>(previous);t.slot=work;t.frame=f;t.pair=pair;
    t.frame.jitter[0]=encode.x;t.frame.jitter[1]=encode.y;t.frame.raster_jitter[0]=raster.x;t.frame.raster_jitter[1]=raster.y;
    t.frame.raster_sample=true;t.colour_epoch=c->colour_jitter.Epoch();t.colour_convention=static_cast<uint32_t>(convention);
    stats.camera_reconstruct=f.camera_reconstruct;if(f.camera_reconstruct)++stats.reconstruct_frames;
    stats.total_mode=f.total;
    if(f.total){++stats.total_frames;
        // A scene cut measured on a pair newer than the last submitted reset: this frame carries the reset.
        t.cut_reset=stats.total_cut_frame>f.last_reset_frame;if(t.cut_reset)++stats.total_cut_resets;}
    stats.loosen=g_loosen.load(std::memory_order_relaxed);
    const Gates gates=GatesFor(stats.loosen);
    Constants cb{};cb.ratio=gates.image_ratio;cb.absolute=gates.image_abs;cb.texture=gates.texture_min;
    cb.agree_px=gates.agree_px;cb.agree_cos=gates.agree_cos;cb.agree_min=gates.agree_min;
    cb.w=cb.ew=w;cb.h=cb.eh=h;cb.mode=f.hdr?1u:0u;cb.block=8;cb.gw=(w+7)/8;cb.gh=(h+7)/8;
    // Jitter-stabilised encode (j2_flow fe8ea3e): sample texel q at q + this frame's jitter so both
    // images of a pair sit in the unjittered frame and the engine never sees DLAA's shake.
    cb.jitter_x=encode.x;cb.jitter_y=encode.y;
    cb.cost=(f.block_x>1?2u:1u)|((f.block_y>1?2u:1u)<<8); // encode only: the block the picture was shaded in
    if(slot.timestamps)l->EndQuery(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
    move(l,im.tex.Get(),S,U);move(l,im.raw_color.Get(),S,U);
    c->srv(work,0,0,color,d.Format);c->uav(work,0,kUav+0,im.tex.Get(),c->image_format);
    c->uav(work,0,kUav+3,im.raw_color.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
    c->dispatch(l,work,0,c->pso_encode.Get(),cb);cb.cost=0;order(l);move(l,im.tex.Get(),U,S);move(l,im.raw_color.Get(),U,S);++stats.recorded;
    if(!pair){++stats.waiting_history;reason(f.total?"Measured motion: this is the first picture of a pair":"AMD flow capturing matching scene history; camera motion used");return t;}
    auto& prev=*c->images[previous];prev.uses.push_back(lease);
    // Calibration reads the unshifted source pair: comparing two differently
    // bilinear-resampled encodes would confound sampling blur with real jitter.
    slot.probe=c->jitter_probe.Record(l,work,im.raw_color.Get(),prev.raw_color.Get(),colour_key,f.frame,
        {raster.x-prev.raster_jitter[0],raster.y-prev.raster_jitter[1]},{});
    if(!c->engine.Record(l,im.tex.Get(),prev.tex.Get(),0,&slot.engine_heap)){++stats.unavailable;reason("AMD flow refused image pair; camera motion used");return t;}
    // VARIANT: flow/frame-candidates
    // The search's grid reconsidered against the frame's own most common vectors (core/flow_candidates.hpp),
    // before the expansion, the camera fit and `measure` read it. A block keeps its own vector unless a
    // candidate explains the two pictures clearly better; blocks the search called still are not touched.
    ID3D12Resource* grid=c->engine.Grid();
    stats.cand_on=c->pso_cand_apply && g_candidates.load(std::memory_order_relaxed);
    if(stats.cand_on){
        Constants cc=cb;cc.cost_max=(std::max)(8u,cb.gw*cb.gh/100u); // the support a vector needs to be offered (flow_candidates.hpp MinimumSupport)
        const unsigned groups_x=(cb.gw+7)/8,groups_y=(cb.gh+7)/8;
        move(l,c->grid.Get(),S,U);
        move(l,c->top.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(c->top.Get(),0,c->zero.Get(),0,48);move(l,c->top.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
        c->srv(work,4,0,c->engine.Grid(),DXGI_FORMAT_R16G16_SINT);
        c->srv(work,4,5,im.tex.Get(),c->image_format);c->srv(work,4,6,prev.tex.Get(),c->image_format);
        c->uav(work,4,kUav+0,c->votes.Get(),DXGI_FORMAT_R32_UINT);c->uav(work,4,kUav+1,c->grid.Get(),DXGI_FORMAT_R16G16_SINT);c->raw_uav(work,4,kUav+2,c->top.Get(),12);
        c->bind_compute(l,work,4);
        c->dispatch_bound(l,c->pso_cand_clear.Get(),cc,false,32,32);order(l);
        c->dispatch_bound(l,c->pso_cand_count.Get(),cc,false,groups_x,groups_y);order(l);
        c->dispatch_bound(l,c->pso_cand_pick.Get(),cc,false,32,32);order(l); // visit each of the 256x256 vote bins once
        c->dispatch_bound(l,c->pso_cand_apply.Get(),cc,false,cb.gw,cb.gh);order(l); // one thread group per block, one thread per pixel
        move(l,c->grid.Get(),U,S);
        move(l,c->top.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l->CopyBufferRegion(slot.readback.Get(),64,c->top.Get(),0,40);move(l,c->top.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,U);
        grid=c->grid.Get();slot.candidates=true;
    }
    move(l,c->raw.Get(),S,U);move(l,c->expanded.Get(),S,U);
    // AMD outputs integral pixels; FlowExpand decodes NVOFA S10.5. Cancel
    // its /32 here. Grid spacing and value scale are separate conventions.
    c->expand.SetVectorScale(32.f);na::nvof::ExpandParams ep;ep.depth=depth;ep.validity_out=c->expanded.Get();ep.depth_clear=camera.depth_clear;ep.consistency_px=gates.consistency_px;ep.sky=gates.sky || f.total; // measured total: the sky moves too, and nothing else would give it a vector
    ep.image_to_raster_px[0]=encode.x;ep.image_to_raster_px[1]=encode.y;
    bool ok=c->expand.DispatchDepthAware(l,grid,8,ep,c->raw.Get(),w,h,0,&slot.expand_heap);
    order(l);move(l,c->raw.Get(),U,S);move(l,c->expanded.Get(),U,S);
    // M3 (camera_fit_gpu.hpp): fit the rigid camera correction on this exact pair, on the GPU, before
    // the residual is derived, so the residual and the arbiter's tier 2 use the same corrected camera.
    ID3D12Resource* fix=nullptr;
    // The pair's images feed the fit's image check; dj = the stabilisation jitter delta of this
    // pair (current - previous), the shift the encode adds when the colour carries no jitter.
    const float dj[2]={encode.x-prev.jitter[0],encode.y-prev.jitter[1]};
    cb.dj_x=f.raster_confirmed?0.f:dj[0];cb.dj_y=f.raster_confirmed?0.f:dj[1];
    cb.history_dx=-dj[0];cb.history_dy=-dj[1];
    if(ok && !f.total && camera.fit_basis && c->fit.Ready()){
        na::motion::gpu::FitImages images;images.current=im.tex.Get();images.previous=prev.tex.Get();images.jitter_delta_px[0]=cb.dj_x;images.jitter_delta_px[1]=cb.dj_y;
        images.raster_to_image_px[0]=-encode.x;images.raster_to_image_px[1]=-encode.y;
        fix=c->fit.Record(l,work,depth,c->raw.Get(),c->expanded.Get(),*camera.fit_basis,camera.jitter_current_px,camera.depth_clear,w,h,&images);
        if(fix)++stats.fit_recorded;}
    if(f.total){
        // VARIANT: motion/measured-total
        // No camera: no fit, no residual, nothing derived against a reprojection, none of the residual
        // gates. `measure` refines the search's whole-pixel vector and "nothing moved" on the two
        // pictures and writes the total field and its validity; no temporal history is read or written.
        move(l,c->mask.Get(),S,U);move(l,c->total.Get(),S,U);
        move(l,c->counters.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(c->counters.Get(),0,c->zero.Get(),0,40);move(l,c->counters.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
        cb.cost=2;cb.frame_now=static_cast<unsigned>(f.frame);cb.frame_reset=static_cast<unsigned>(t.cut_reset?f.frame:f.last_reset_frame);
        // `measure` and the fit read the depth's far end and the fit's step scale in these two words (flow_shaders.hpp depth_far, fit_scale).
        cb.history_dx=camera.depth_clear;cb.history_dy=0;
        // VARIANT: motion/screen-fit
        // The screen's own motion: one whole-picture camera movement fitted to the block centres the
        // two pictures can measure, with no camera and no lens from the game (core/screen_motion.hpp
        // is these programs on the CPU and says why). `measure` then follows it wherever the pictures
        // cannot measure, and the search's raw vector is never handed on. Cort's Sniper Elite
        // Resistance run of 2026-11-17 (3621e3a8): about two thirds of a real frame is in that case.
        ID3D12Resource* model=nullptr;
        stats.screen_on=c->pso_fit_solve && f.use_screen_fit && g_screen_fit.load(std::memory_order_relaxed);
        if(ok && stats.screen_on){
            const na::motion::screen::Params fp;
            Constants fc=cb;
            move(l,c->fit_grid.Get(),S,U);
            c->srv(work,5,0,depth,DXGI_FORMAT_R32_FLOAT);c->srv(work,5,1,c->raw.Get(),DXGI_FORMAT_R32G32_FLOAT);c->srv(work,5,3,c->expanded.Get(),DXGI_FORMAT_R32_FLOAT);
            c->srv(work,5,4,c->engine.SceneChange(),DXGI_FORMAT_R32_UINT);c->srv(work,5,5,im.tex.Get(),c->image_format);c->srv(work,5,6,prev.tex.Get(),c->image_format);
            c->uav(work,5,kUav+0,c->fit_grid.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);
            c->dispatch(l,work,5,c->pso_fit_sample.Get(),fc,false,(cb.gw+7)/8,(cb.gh+7)/8);order(l);move(l,c->fit_grid.Get(),U,S);
            // The terms start at zero; the head's other words are this pair's own.
            move(l,c->fit_state.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(c->fit_state.Get(),0,c->zero.Get(),0,kFitHead*4);move(l,c->fit_state.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
            move(l,c->fit_model.Get(),S,U);
            c->srv(work,6,7,c->fit_grid.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT);c->raw_uav(work,6,kUav+2,c->fit_state.Get(),c->fit_words);
            c->srv(work,7,7,c->fit_grid.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT); // fit_solve's mode 2 counts the samples' depths
            c->raw_uav(work,7,kUav+2,c->fit_state.Get(),c->fit_words);c->uav(work,7,kUav+0,c->fit_model.Get(),DXGI_FORMAT_R32G32_FLOAT);
            const unsigned tiles=((cb.gw+7)/8)*((cb.gh+7)/8);
            // VARIANT: motion/screen-fit/depth-layers
            // Where the samples' depths fall into two groups with an empty stretch between them, each
            // group gets its own fit and a pixel its own group's (screen_motion.hpp FitLayers says why:
            // a large near figure moving by itself otherwise decides what the still far scene is handed).
            fc.mode=3;fc.offset=0;
            l->SetComputeRootDescriptorTable(0,c->gpu(work,7));
            c->dispatch_bound(l,c->pso_fit_solve.Get(),fc,false,(cb.gw*cb.gh+255u)/256u,1);order(l);
            fc.mode=2;
            l->SetComputeRootDescriptorTable(0,c->gpu(work,7));
            c->dispatch_bound(l,c->pso_fit_solve.Get(),fc,true);order(l);
            // Row 0: the far group, or the one fit of every depth. Row 1: the near group; with no split
            // it holds no samples and its row says "not valid".
            for(unsigned row=0;row<2;++row){
                if(row){ // the terms and the stop state start again; the split (word 19) stays
                    move(l,c->fit_state.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(c->fit_state.Get(),0,c->zero.Get(),0,(kFitHead-1)*4);move(l,c->fit_state.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);}
                fc.offset=row;
                // screen_motion.hpp Fit: one plain step, the reweighted ones, then the fitted terms against the samples once more.
                // A pass uses the tile weights the pass before it left (TileScaleFor).
                for(int step=0;step<=fp.steps;++step){const bool last=step==fp.steps;
                    fc.history_dy=last?fp.robust_px:step>0?na::motion::screen::ScaleAt(step,fp):0.f;fc.mode=0;
                    const float tile_now=na::motion::screen::TileScaleFor(step,fp);
                    fc.agree_px=fp.tile_px>0?(tile_now>0?tile_now:-1.f):0.f;fc.agree_cos=na::motion::screen::TileScaleFor(step+1,fp);
                    l->SetComputeRootDescriptorTable(0,c->gpu(work,6));
                    c->dispatch_bound(l,c->pso_fit_accum.Get(),fc,false,tiles,1);order(l);
                    fc.mode=last?1u:0u;
                    l->SetComputeRootDescriptorTable(0,c->gpu(work,7));
                    if(!row){c->dispatch_bound(l,c->pso_fit_reduce.Get(),fc,false,((kFitValues+7)/8)*3,1);order(l);}
                    c->dispatch_bound(l,c->pso_fit_solve.Get(),fc,true);order(l);}
                move(l,c->fit_state.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l->CopyBufferRegion(slot.readback.Get(),112+row*kFitHead*4,c->fit_state.Get(),0,kFitHead*4);move(l,c->fit_state.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,U);}
            move(l,c->fit_model.Get(),U,S);
            model=c->fit_model.Get();slot.screen=true;
        }
        c->srv(work,1,4,c->engine.SceneChange(),DXGI_FORMAT_R32_UINT);
        c->srv(work,1,0,depth,DXGI_FORMAT_R32_FLOAT);c->srv(work,1,2,model,DXGI_FORMAT_R32G32_FLOAT);
        c->srv(work,1,1,c->raw.Get(),DXGI_FORMAT_R32G32_FLOAT);c->srv(work,1,3,c->expanded.Get(),DXGI_FORMAT_R32_FLOAT);
        c->srv(work,1,5,im.tex.Get(),c->image_format);c->srv(work,1,6,prev.tex.Get(),c->image_format);
        c->uav(work,1,kUav+0,c->total.Get(),DXGI_FORMAT_R32G32_FLOAT);c->uav(work,1,kUav+1,c->mask.Get(),DXGI_FORMAT_R32_FLOAT);
        if(ok){
            const bool sampled=f.sample_x>1 || f.sample_y>1;
            if(sampled && f.sample_pick_x<f.sample_x && f.sample_pick_y<f.sample_y){
                // flow_shaders.hpp sample_code / sample_size (accept's two words; accept does not run here).
                const unsigned sw=(w-f.sample_pick_x+f.sample_x-1)/f.sample_x,sh=(h-f.sample_pick_y+f.sample_y-1)/f.sample_y;
                Constants mc=cb;mc.have_hist=unsigned(f.sample_x)|(unsigned(f.sample_y)<<8)|(unsigned(f.sample_pick_x)<<16)|(unsigned(f.sample_pick_y)<<24);
                mc.agree_min=sw|(sh<<16);
                c->dispatch(l,work,1,c->pso_measure.Get(),mc,false,(sw+7)/8,(sh+7)/8);++stats.sampled_pairs;
            }else c->dispatch(l,work,1,c->pso_measure.Get(),cb);
        }
        order(l);move(l,c->mask.Get(),U,S);move(l,c->total.Get(),U,S);
        if(ok){
            // The scene cut's state, in execution order, for RecordCutShow after DLSS (flow_shaders.hpp cut_state).
            if(!c->cut_seeded){move(l,c->cut_state.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(c->cut_state.Get(),0,c->zero.Get(),0,16);
                move(l,c->cut_state.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);slot.cut_seed=true;}
            c->srv(work,2,4,c->engine.SceneChange(),DXGI_FORMAT_R32_UINT);c->raw_uav(work,2,kUav+2,c->cut_state.Get(),4);
            c->dispatch(l,work,2,c->pso_cut_state.Get(),cb,true);order(l);
        }
        c->engine.Finish(l);
        move(l,c->counters.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l->CopyBufferRegion(slot.readback.Get(),0,c->counters.Get(),0,40);move(l,c->counters.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,U);
        if(slot.timestamps){l->EndQuery(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);l->ResolveQueryData(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,slot.readback.Get(),48);}
        if(ok){out.total=c->total.Get();out.total_valid=c->mask.Get();out.screen_model=model;slot.pair=true;++stats.pairs;++stats.total_pairs;reason("Measured motion recorded before DLAA (no camera used)");}
        else {++stats.unavailable;reason("Measured motion: the flow expansion failed for this pair");}
        return t;
    }
    if(!f.residual){ // camera reconstruction only: no residual, no acceptance, no temporal history
        c->engine.Finish(l);
        if(slot.timestamps){l->EndQuery(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);l->ResolveQueryData(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,slot.readback.Get(),48);}
        if(ok && fix){out.fix=fix;slot.pair=true;++stats.pairs;++stats.fit_only;reason("AMD flow: camera motion reconstructed (object motion off)");}
        else {++stats.unavailable;reason("AMD flow camera fit unavailable; camera motion used");}
        return t;
    }
    na::motion::gpu::Inputs fitted=camera;fitted.fix=fix;
    move(l,c->residual.Get(),S,U);move(l,c->valid.Get(),S,U);
    // Stabilised images: the raw flow carries no jitter shift, so (jitter_prev - jitter_cur) must be 0.
    ok=ok && c->derive.DispatchDeriveResidual(l,c->raw.Get(),depth,fitted,camera.jitter_current_px,c->residual.Get(),c->valid.Get(),w,h,0,c->expanded.Get(),&slot.derive_heap);
    order(l);move(l,c->residual.Get(),U,S);move(l,c->valid.Get(),U,S);move(l,c->mask.Get(),S,U);
    move(l,c->counters.Get(),U,D3D12_RESOURCE_STATE_COPY_DEST);l->CopyBufferRegion(c->counters.Get(),0,c->zero.Get(),0,40);move(l,c->counters.Get(),D3D12_RESOURCE_STATE_COPY_DEST,U);
    cb.cost=2;c->srv(work,1,4,c->engine.SceneChange(),DXGI_FORMAT_R32_UINT);
    c->srv(work,1,1,c->raw.Get(),DXGI_FORMAT_R32G32_FLOAT);c->srv(work,1,2,c->residual.Get(),DXGI_FORMAT_R32G32_FLOAT);
    c->srv(work,1,3,c->valid.Get(),DXGI_FORMAT_R32_FLOAT);c->srv(work,1,5,im.tex.Get(),c->image_format);c->srv(work,1,6,prev.tex.Get(),c->image_format);c->uav(work,1,kUav+1,c->mask.Get(),DXGI_FORMAT_R32_FLOAT);
    // Temporal agreement (j2_flow 5c5fb0d/319e2ae): the previous pair's candidates, only when that
    // pair's current image is this pair's previous one (same generation); written here, published at Committed.
    const unsigned hist_out=c->hist_read^1u;
    cb.have_hist=(c->hist_frame!=0 && prev.frame==c->hist_frame && prev.generation==c->hist_generation)?1u:0u;
    c->srv(work,1,7,c->hist[c->hist_read].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);c->uav(work,1,kUav+3,c->hist[hist_out].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
    move(l,c->hist[hist_out].Get(),S,U);
    if(ok)c->dispatch(l,work,1,c->pso_accept.Get(),cb);order(l);move(l,c->mask.Get(),U,S);move(l,c->hist[hist_out].Get(),U,S);c->engine.Finish(l);
    move(l,c->counters.Get(),U,D3D12_RESOURCE_STATE_COPY_SOURCE);l->CopyBufferRegion(slot.readback.Get(),0,c->counters.Get(),0,40);move(l,c->counters.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,U);
    if(slot.timestamps){l->EndQuery(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);l->ResolveQueryData(slot.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,slot.readback.Get(),48);}
    if(ok){out={c->residual.Get(),c->valid.Get(),c->mask.Get(),fix};slot.pair=true;slot.hist_written=static_cast<int>(hist_out);++stats.pairs;reason("AMD same-frame optical flow recorded before DLAA");}
    else {++stats.unavailable;reason("AMD flow expansion/residual unavailable; camera motion used");}
    return t;
}
Gates GatesFor(uint32_t percent){
    const float k=float(percent>100?100:percent)/100.f;
    Gates g{};
    g.image_ratio=.5f+.5f*k;      // flow must beat camera-only by this ratio: 0.5 -> 1.0 (just not worse)
    g.image_abs=.08f+.12f*k;      // absolute image error allowed: 0.08 -> 0.20
    g.texture_min=.03f-.02f*k;    // local contrast needed to trust a match: 0.03 -> 0.01
    g.agree_px=3.f+3.f*k;         // temporal agreement distance: 3 -> 6 px
    g.agree_cos=.5f-.4f*k;        // temporal agreement direction: 60 -> ~84 degrees
    g.agree_min=k>=.5f?0u:1u;     // from 50: a first sighting counts without a previous agreeing pair
    g.consistency_px=2.f+2.f*k;   // neighbouring-block spread: 2 -> 4 px
    g.sky=k>0.f;                  // objects against clear depth (sky) are no longer rejected
    return g;
}
void RecordCutShow(ID3D12GraphicsCommandList* l,const Ticket& t,ID3D12Resource* colour,ID3D12Resource* output){
    if(!l || !t.context || !t.pair || !t.frame.total || !colour || !output)return;
    std::lock_guard<std::mutex> lock(mutex);auto c=std::static_pointer_cast<Context>(t.context);
    if(c!=current || t.slot>=c->leases.size() || !c->pso_cut_show)return;
    auto& slot=*c->leases[t.slot];if(!slot.pair || !slot.total)return; // this recording measured nothing: its state word was not written
    // The companion route records what follows DLSS on a second list: this recording's descriptors
    // stay its own until that list has run too.
    auto lease=na::nr::native::RetainRecording(l,c);if(!lease)return;
    if(!slot.lease.SameRecording(lease))slot.after=lease;
    const auto od=output->GetDesc();Constants cb{};cb.w=static_cast<unsigned>(od.Width);cb.h=od.Height;
    c->srv(t.slot,3,0,colour,colour->GetDesc().Format);c->uav(t.slot,3,kUav+0,output,od.Format);c->raw_uav(t.slot,3,kUav+2,c->cut_state.Get(),4);
    c->dispatch(l,t.slot,3,c->pso_cut_show.Get(),cb,false,(cb.w+kCutTile-1)/kCutTile,(cb.h+kCutTile-1)/kCutTile);
    order(l);
}
void SetLoosen(uint32_t percent){g_loosen.store(percent>100?100u:percent,std::memory_order_relaxed);}
void SetFrameCandidates(bool on){g_candidates.store(on,std::memory_order_relaxed);}
void SetScreenFit(bool on){g_screen_fit.store(on,std::memory_order_relaxed);}
void Committed(const Ticket& t){if(!t.context)return;std::lock_guard<std::mutex> lock(mutex);auto c=std::static_pointer_cast<Context>(t.context);if(c!=current && std::find(cached.begin(),cached.end(),c)==cached.end())return;auto& im=*c->images[t.image];im.frame=t.frame.frame;im.generation=t.frame.generation;memcpy(im.jitter,t.frame.jitter,sizeof im.jitter);memcpy(im.raster_jitter,t.frame.raster_jitter,sizeof im.raster_jitter);im.colour_epoch=t.colour_epoch;im.raster_confirmed=t.frame.raster_confirmed;c->last=static_cast<int>(t.image);auto& slot=*c->leases[t.slot];slot.submitted=true;++stats.submitted;if(slot.pair)++stats.submitted_pairs;
    if(slot.cut_seed){c->cut_seeded=true;slot.cut_seed=false;}
    if(slot.pair && slot.hist_written>=0){c->hist_read=static_cast<unsigned>(slot.hist_written);c->hist_frame=t.frame.frame;c->hist_generation=t.frame.generation;slot.hist_written=-1;}stats.last_submitted_frame=t.frame.frame;last_submit_tick=GetTickCount64();}
void Prepare(ID3D12Device* device,ID3D12CommandQueue* queue,unsigned w,unsigned h){
    if(!device || !queue || !w || !h || !na::ffx_flow::Built())return;
    std::lock_guard<std::mutex> lock(mutex);
    if(current && (current->device.Get()!=device || current->queue.Get()!=queue))return; // Record settles a new device first
    if(current && current->w==w && current->h==h)return;
    for(const auto& c:cached)if(c->w==w && c->h==h){c->used_ms=GetTickCount64();return;} // asked for: kept
    for(const auto& b:building)if(b.first==w && b.second==h)return;
    // A size whose working set could not be made (too small for the flow) is not tried again for a while: a new
    // build thread every frame would cost and would always look busy.
    for(const auto& f:failed_sizes)if(f.w==w && f.h==h && GetTickCount64()-f.at_ms<30000)return;
    building.emplace_back(w,h);
    ComPtr<ID3D12Device> dev(device);ComPtr<ID3D12CommandQueue> q(queue);
    std::thread([dev,q,w,h]{
        const uint64_t began=GetTickCount64();
        auto next=std::make_shared<Context>();const bool ok=build(*next,dev.Get(),q.Get(),w,h);
        na::eventlog::Emit("first_setup","motion working set %ux%u %s in %llu ms on its own thread (the game kept drawing)",w,h,ok?"built":"FAILED to build",
                           static_cast<unsigned long long>(GetTickCount64()-began));
        std::lock_guard<std::mutex> lock(mutex);
        building.erase(std::remove(building.begin(),building.end(),std::make_pair(w,h)),building.end());
        if(!ok){++stats.prepare_failed;warm_failed.emplace_back(w,h);failed_sizes.push_back({w,h,GetTickCount64()});if(failed_sizes.size()>8)failed_sizes.erase(failed_sizes.begin());return;}
        if(current && (current->device.Get()!=dev.Get() || current->queue.Get()!=q.Get()))return;
        cached.push_back(std::move(next));++stats.prepared;if(cached.size()>kCachedContexts)cached.erase(cached.begin());
    }).detach();
}
// First-time set-up never runs on the game's thread (Cort 2026-11-27: a game froze when it did).
// True: Record can take this size without building anything, or only Record can settle it (another
// device, no programs, or a build that already failed here: Record then says why). False: the size is
// being built on its own thread; the caller leaves this frame as the game drew it and asks again.
bool Warm(ID3D12Device* device,ID3D12CommandQueue* queue,unsigned w,unsigned h){
    if(!device || !queue || !w || !h || !na::ffx_flow::Built() || na::first_setup::InlineForTests().load(std::memory_order_relaxed))return true;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if(current && (current->device.Get()!=device || current->queue.Get()!=queue))return true;
        if(current && current->w==w && current->h==h)return true;
        for(const auto& c:cached)if(c->w==w && c->h==h)return true; // same device: taken; another: Record settles it
        for(const auto& f:warm_failed)if(f.first==w && f.second==h)return true;
    }
    Prepare(device,queue,w,h);
    return false;
}
bool Building(unsigned w,unsigned h){
    std::lock_guard<std::mutex> lock(mutex);
    for(const auto& b:building)if(b.first==w && b.second==h)return true;
    return false;
}
void Keep(unsigned w,unsigned h){
    std::lock_guard<std::mutex> lock(mutex);
    const ULONGLONG now=GetTickCount64();
    if(current && current->w==w && current->h==h)current->used_ms=now;
    for(const auto& c:cached)if(c->w==w && c->h==h)c->used_ms=now;
}
bool Ready(unsigned w,unsigned h){
    std::lock_guard<std::mutex> lock(mutex);
    if(current && current->w==w && current->h==h)return true;
    for(const auto& c:cached)if(c->w==w && c->h==h)return true;
    return false;
}
int FormatCutNumbers(char* out,size_t n){
    const Stats s=GetStats();
    const float agree=s.screen_samples>0?s.screen_agree/s.screen_samples:0.f;
    return std::snprintf(out,n,"screen_fit{frame=%llu model=%d samples=%.0f agree_share=%.3f energy_px2=%.3f translation_px=%.2f,%.2f} cuts_measured=%llu pairs=%llu",
        static_cast<unsigned long long>(s.screen_frame),s.screen_last_valid?1:0,s.screen_samples,agree,s.screen_energy,
        s.screen_theta[0],s.screen_theta[1],static_cast<unsigned long long>(s.total_cuts),static_cast<unsigned long long>(s.total_pairs));
}
void Reset(){std::lock_guard<std::mutex> lock(mutex);current.reset();cached.clear();last_submit_tick=sample_tick=0;stats={};}
Stats GetStats(){na::nr::native::Maintain();std::lock_guard<std::mutex> lock(mutex);if(current){collect(*current);stats.colour_jitter_convention=static_cast<uint32_t>(current->raster_confirmed?na::jitter::Convention::Raster:current->colour_jitter.Get());stats.colour_jitter_epoch=current->colour_jitter.Epoch();}const auto now=GetTickCount64();stats.submit_age_ms=last_submit_tick?now-last_submit_tick:0;stats.sample_age_ms=sample_tick?now-sample_tick:0;return stats;}
void DescribeStats(const Stats& s,char* text,size_t capacity){
    if(!text || !capacity)return;
    if(s.total_mode){ // no camera: the picture pair is measured, nothing is "accepted" against a camera
        const double all=double(s.total_sum[0]+s.total_sum[1]+s.total_sum[2]+s.total_sum[4]+s.total_sum[5]+s.total_sum[7]);
        if(!s.total_sum_pairs || all<=0){std::snprintf(text,capacity,"Measured motion (no camera): %llu pictures taken, %llu pairs measured. Waiting for the first completed pair. %s",
            (unsigned long long)s.total_frames,(unsigned long long)s.total_pairs,s.reason);return;}
        std::snprintf(text,capacity,"Measured motion (no camera): %llu pairs. Over all of them %.1f%% of the picture measured (%.1f%% moving by itself, %.1f%% with the screen), %.1f%% following the screen's motion, %.1f%% not known. Screen motion fitted on %llu of %llu pairs. %llu scene cuts. Last pair %.2f ms.",
            (unsigned long long)s.total_sum_pairs,100.0*double(s.total_sum[0]+s.total_sum[7])/all,100.0*double(s.total_sum[0])/all,100.0*double(s.total_sum[7])/all,
            100.0*double(s.total_sum[2]+s.total_sum[4]+s.total_sum[5])/all,100.0*double(s.total_sum[1])/all,(unsigned long long)s.screen_valid,(unsigned long long)s.screen_pairs,(unsigned long long)s.total_cuts,s.ms);return;}
    if(!s.submitted || s.submit_age_ms>2000){std::snprintf(text,capacity,"AMD flow: no new submission%s. %s Frames submitted %llu, paired %llu, skipped %llu.",
        s.submitted?" recently":" yet",s.reason,(unsigned long long)s.submitted,(unsigned long long)s.submitted_pairs,(unsigned long long)(s.busy+s.unavailable));return;}
    if(!s.samples || !s.total){std::snprintf(text,capacity,"AMD flow: frames submitting. Paired %llu / %llu, skipped %llu. Waiting for a completed pixel sample.",
        (unsigned long long)s.submitted_pairs,(unsigned long long)s.submitted,(unsigned long long)(s.busy+s.unavailable));return;}
    std::snprintf(text,capacity,"AMD flow: paired %llu / %llu frames, skipped %llu. Last completed sample: %.2f%% moving pixels, %.2f ms (frame %llu, %llu frames behind).",
        (unsigned long long)s.submitted_pairs,(unsigned long long)s.submitted,(unsigned long long)(s.busy+s.unavailable),100.0*double(s.accepted)/double(s.total),s.ms,
        (unsigned long long)s.sample_frame,(unsigned long long)(s.last_submitted_frame>=s.sample_frame?s.last_submitted_frame-s.sample_frame:0));
}
}
