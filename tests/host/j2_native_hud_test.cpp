// Render-small native-resolution HUD (j2/native_hud) and the HUD-mask fallback fix:
//  1. Pure helpers: formats, grey values, scene shape, viewport/scissor scaling.
//  2. HUD-mask classify kernel (Sol HUD-R2): a HUD element that disappears over a FLAT
//     background loses its HUD evidence; flag 4 forgets old evidence.
//  3. End to end on a real D3D12 device with the observer installed (WARP on CI): the game
//     draws a scene and three HUD draws (alpha "over", additive, opaque) into a render-size
//     "proxy"; the HUD draws must land in the native layers (not the proxy), the game's own
//     targets, viewports and scissors must be handed back, and the layer composite over the
//     scene must match the same draws made directly at native size.
#include "test_support.hpp"
#include "host_dx12/j2/live_dlaa.hpp"
#include "host_dx12/j2/native_hud.hpp"
#include "host_dx12/j2/hud_picture.hpp"
#include "core/jitter_domain.hpp"
#include "host_dx12/observe/producers.hpp"
#include "host_dx12/observe/operation_snapshot.hpp"
#include "host_dx12/nr/native_fg.hpp"
#include "host_dx12/observe/shader_dimensions.hpp"
#include "host_dx12/capture/j2_capture.hpp"

#include <d3dcompiler.h>
#include <d3d12sdklayers.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nh = na::j2::native_hud;
using test::ComPtr;

namespace {
void __cdecl sink(const na_event_batch*, void*) {}

bool upload(test::Device& d, ID3D12Resource* tex, const void* pixels, uint32_t bpp, D3D12_RESOURCE_STATES after)
{
    ID3D12Device* dev = d.device.Get();
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto up = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) return false;
    uint8_t* p = nullptr;
    if (FAILED(up->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(p + fp.Offset + y * fp.Footprint.RowPitch, static_cast<const uint8_t*>(pixels) + size_t(y) * desc.Width * bpp, size_t(desc.Width) * bpp);
    up->Unmap(0, nullptr);
    test::List l;
    if (!l.Create(dev)) return false;
    D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    src.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    l.list->ResourceBarrier(1, &b);
    if (FAILED(l.list->Close())) return false;
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    return true;
}
// Reads a texture (in `state`, returned to it) into tightly packed rows.
bool readback(test::Device& d, ID3D12Resource* tex, D3D12_RESOURCE_STATES state, uint32_t bpp, std::vector<uint8_t>& out)
{
    ID3D12Device* dev = d.device.Get();
    const D3D12_RESOURCE_DESC desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = test::CreateBuffer(dev, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    test::List l;
    if (!rb || !l.Create(dev)) return false;
    auto b = test::Transition(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = fp;
    l.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = test::Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    l.list->ResourceBarrier(1, &b);
    if (FAILED(l.list->Close())) return false;
    ID3D12CommandList* lists[] = {l.list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    out.resize(size_t(desc.Width) * desc.Height * bpp);
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(out.data() + size_t(y) * desc.Width * bpp, p + fp.Offset + y * fp.Footprint.RowPitch, size_t(desc.Width) * bpp);
    rb->Unmap(0, nullptr);
    return true;
}

void helpers()
{
    CHECK(nh::TypelessOf(DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_R8G8B8A8_TYPELESS);
    CHECK(nh::TypelessOf(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == DXGI_FORMAT_B8G8R8A8_TYPELESS);
    CHECK(nh::TypelessOf(DXGI_FORMAT_R10G10B10A2_UNORM) == DXGI_FORMAT_R10G10B10A2_TYPELESS);
    CHECK(nh::TypelessOf(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_TYPELESS);
    CHECK(nh::TypelessOf(DXGI_FORMAT_R11G11B10_FLOAT) == DXGI_FORMAT_UNKNOWN);
    CHECK(std::fabs(nh::GreyFor(DXGI_FORMAT_R8G8B8A8_UNORM) - 128.f / 255.f) < 1e-6f);
    CHECK(std::fabs(nh::GreyFor(DXGI_FORMAT_R16G16B16A16_FLOAT) - 1.f) < 1e-6f);
    CHECK(nh::GreyFor(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) < 0.25f); // linear value of the stored grey
    CHECK(nh::SceneShaped(true, 0, 3, 1));        // full-screen triangle, opaque
    CHECK(nh::SceneShaped(true, 0, 6, 1));        // quad
    CHECK(!nh::SceneShaped(true, 1, 3, 1));       // blended: HUD
    {
        const float vp[6] = {0, 0, 64, 32, 0, 1}, half[6] = {0, 0, 32, 32, 0, 1};
        const int32_t all[4] = {0, 0, 64, 32}, part[4] = {0, 0, 64, 16};
        CHECK(nh::CoversTarget(vp, all, 64, 32));
        CHECK(!nh::CoversTarget(half, all, 64, 32));
        CHECK(!nh::CoversTarget(vp, part, 64, 32));
        CHECK(!nh::CoversTarget(nullptr, all, 64, 32));
    }
    CHECK(!nh::SceneShaped(true, 0, 600, 1));     // geometry: HUD
    CHECK(!nh::SceneShaped(true, 0, 6, 40));      // instanced quads (glyphs): HUD
    CHECK(!nh::SceneShaped(false, 0, 3, 1));      // unknown blending: never taken for the scene
    const float vp[6] = {10.f, 4.f, 64.f, 32.f, 0.f, 1.f};
    float o[6];
    nh::ScaleViewport(vp, 1.5f, 1.5f, o);
    CHECK(o[0] == 15.f && o[1] == 6.f && o[2] == 96.f && o[3] == 48.f && o[4] == 0.f && o[5] == 1.f);
    const int32_t sc[4] = {8, 4, 41, 21};
    int32_t r[4];
    nh::ScaleScissor(sc, 1.5f, 1.5f, 96, 48, r);
    CHECK(r[0] == 12 && r[1] == 6 && r[2] == 62 && r[3] == 32); // grows outward, never loses a covered pixel
    const int32_t big[4] = {-5, -5, 100, 100};
    nh::ScaleScissor(big, 1.5f, 1.5f, 96, 48, r);
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 96 && r[3] == 48);
}

float half_value(uint16_t x)
{
    const int exp=(x>>10)&31;const float sign=(x&0x8000)?-1.f:1.f;
    return sign*std::ldexp(exp?1.f+float(x&1023)/1024.f:float(x&1023)/1024.f,exp?exp-15:-14);
}
int picture_paths(test::Device& d)
{
    constexpr UINT W=16,H=8,OW=32,OH=16;constexpr auto F=DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,R=D3D12_RESOURCE_STATE_RENDER_TARGET;
    const float g=128.f/255.f;
    auto pack=[](float a,float b,float c){return 0xff000000u|UINT(std::lround(a*255))|(UINT(std::lround(b*255))<<8)|(UINT(std::lround(c*255))<<16);};
    for(bool clean:{false,true})for(bool changed:{false,true}) {
        std::vector<UINT> scene(W*H,pack(.2f,.4f,.6f)),full=scene,final=scene,n0(OW*OH,0xff000000),n1(OW*OH,pack(g,g,g)),base(OW*OH,pack(.1f,.1f,.1f));
        std::vector<float> mv(W*H*2,0.f);for(UINT i=0;i<W*H;++i)mv[2*i]=4.f;
        for(UINT y=2;y<6;++y)for(UINT x=4;x<12;++x)full[y*W+x]=pack(.68f,.28f,.30f);
        final=full;if(changed)final[W+1]=pack(.8f,.1f,.3f); // one real processing change invalidates clean substitution
        for(UINT y=4;y<12;++y)for(UINT x=8;x<24;++x){n0[y*OW+x]=pack(.6f,.12f,.06f);n1[y*OW+x]=pack(.6f+.4f*g,.12f+.4f*g,.06f+.4f*g);}
        auto make=[&](UINT w,UINT h,const void* data,DXGI_FORMAT format,UINT bpp,D3D12_RESOURCE_STATES after,D3D12_RESOURCE_FLAGS flags){
            auto r=test::CreateTexture(d.device.Get(),w,h,format,D3D12_RESOURCE_STATE_COPY_DEST,flags);
            if(r&&!upload(d,r.Get(),data,bpp,after))r.Reset();return r;
        };
        auto source=make(W,H,scene.data(),F,4,S,D3D12_RESOURCE_FLAG_NONE);
        auto fsource=make(W,H,full.data(),F,4,S,D3D12_RESOURCE_FLAG_NONE);
        auto frame=make(W,H,final.data(),F,4,S,D3D12_RESOURCE_FLAG_NONE);
        auto a=make(OW,OH,n0.data(),F,4,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),b=make(OW,OH,n1.data(),F,4,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto motion=make(W,H,mv.data(),DXGI_FORMAT_R32G32_FLOAT,8,S,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto out=make(OW,OH,base.data(),F,4,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        REQUIRE(source&&fsource&&frame&&a&&b&&motion&&out);
        auto picture=nh::Picture::Create(d.device.Get(),source->GetDesc(),OW,OH);REQUIRE(picture);
        test::List l;REQUIRE(l.Create(d.device.Get()));
        REQUIRE(picture->SnapshotScene(l.list.Get(),source.Get(),S)&&picture->SnapshotFull(l.list.Get(),fsource.Get(),S));
        REQUIRE(picture->Prepare(l.list.Get(),frame.Get(),F,motion.Get(),a.Get(),b.Get(),F,g,false,clean,F));
        REQUIRE(picture->Composite(l.list.Get(),out.Get()));
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        std::vector<uint8_t> input,mask,motion_bytes,result;
        REQUIRE(readback(d,picture->Input(),S,8,input)&&readback(d,picture->Mask(),S,2,mask)&&readback(d,motion.Get(),S,8,motion_bytes)&&readback(d,out.Get(),U,4,result));
        const UINT inside=3*W+6,outside=W+2;
        uint16_t v=0;std::memcpy(&v,input.data()+inside*8,2);const float input_red=half_value(v);
        std::memcpy(&v,mask.data()+inside*2,2);const float protection=half_value(v);
        float mx=0;std::memcpy(&mx,motion_bytes.data()+inside*8,4);
        const float expected=(clean&&!changed)?51.f/255.f:float(full[inside]&255)/255.f;
        CHECK(std::abs(input_red-expected)<.002f);
        CHECK((!clean&&!changed)?protection>.5f:protection==0.f);
        CHECK(mx==((!clean&&!changed)?0.f:4.f));
        std::memcpy(&v,mask.data()+outside*2,2);CHECK(half_value(v)==0.f);
        const UINT native_inside=6*OW+12,native_outside=OW+2;
        if(changed)CHECK(result==std::vector<uint8_t>(reinterpret_cast<uint8_t*>(base.data()),reinterpret_cast<uint8_t*>(base.data())+base.size()*4));
        else CHECK(result[native_inside*4]>(clean?150:160)); // native alpha-over or protected original UI
        CHECK(std::abs(int(result[native_outside*4])-26)<=1);
        std::printf("HUD GPU path clean=%d changed=%d input=%.3f mask=%.3f motion=%.1f\n",clean,changed,input_red,protection,mx);
        CHECK(d.device->GetDeviceRemovedReason()==S_OK);
        bool identity=false,used_clean=false;CHECK(picture->ProofAfterCompletion(identity,used_clean));
        CHECK(identity==!changed&&used_clean==(clean&&!changed));CHECK(!picture->ProofAfterCompletion(identity,used_clean));
        if(clean&&!changed){
            picture->BeginCycle();test::List reuse;REQUIRE(reuse.Create(d.device.Get()));
            REQUIRE(picture->SnapshotScene(reuse.list.Get(),source.Get(),S));
            CHECK(!picture->Prepare(reuse.list.Get(),frame.Get(),F,motion.Get(),a.Get(),b.Get(),F,g,false,true,F));
            REQUIRE(picture->SnapshotFull(reuse.list.Get(),fsource.Get(),S));
            REQUIRE(picture->Prepare(reuse.list.Get(),frame.Get(),F,motion.Get(),a.Get(),b.Get(),F,g,false,true,F));
            REQUIRE(picture->Composite(reuse.list.Get(),out.Get()));REQUIRE(SUCCEEDED(reuse.list->Close()));
            ID3D12CommandList* replay[]={reuse.list.Get()};d.queue->ExecuteCommandLists(1,replay);d.WaitIdle();
            CHECK(picture->ProofAfterCompletion(identity,used_clean)&&identity&&used_clean);
        }
    }
    // Float HDR must retain values above one and the reconstruction alpha.
    {
        constexpr UINT w=4,h=2,ow=8,oh=4;constexpr auto F16=DXGI_FORMAT_R16G16B16A16_FLOAT;
        auto rgba=[](UINT n,std::initializer_list<uint16_t> color){std::vector<uint16_t> v;for(UINT i=0;i<n;++i)v.insert(v.end(),color);return v;};
        const auto scene=rgba(w*h,{0x4000,0x3c00,0x3800,0x3a00}),full=rgba(w*h,{0x4500,0x3c00,0x3800,0x3a00});
        const auto a=rgba(ow*oh,{0x4400,0x3800,0x3400,0x3c00}),b=rgba(ow*oh,{0x4480,0x3c00,0x3a00,0x3c00});
        auto make=[&](UINT x,UINT y,const std::vector<uint16_t>& v,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags){auto r=test::CreateTexture(d.device.Get(),x,y,F16,D3D12_RESOURCE_STATE_COPY_DEST,flags);if(r&&!upload(d,r.Get(),v.data(),8,state))r.Reset();return r;};
        auto s=make(w,h,scene,S,D3D12_RESOURCE_FLAG_NONE),f=make(w,h,full,S,D3D12_RESOURCE_FLAG_NONE);
        auto n0=make(ow,oh,a,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),n1=make(ow,oh,b,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto out=make(ow,oh,rgba(ow*oh,{0x4000,0x3c00,0x3800,0x3a00}),U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        std::vector<float> zero(w*h*2);auto mv=test::CreateTexture(d.device.Get(),w,h,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        REQUIRE(s&&f&&n0&&n1&&out&&mv&&upload(d,mv.Get(),zero.data(),8,S));
        auto p=nh::Picture::Create(d.device.Get(),s->GetDesc(),ow,oh);REQUIRE(p);test::List l;REQUIRE(l.Create(d.device.Get()));
        REQUIRE(p->SnapshotScene(l.list.Get(),s.Get(),S)&&p->SnapshotFull(l.list.Get(),f.Get(),S));
        REQUIRE(p->Prepare(l.list.Get(),f.Get(),F16,mv.Get(),n0.Get(),n1.Get(),F16,1.f,false,true,F16)&&p->Composite(l.list.Get(),out.Get()));
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        std::vector<uint8_t> pixels;REQUIRE(readback(d,out.Get(),U,8,pixels));uint16_t value[4]{};std::memcpy(value,pixels.data(),8);
        CHECK(half_value(value[0])==5.f&&half_value(value[1])==1.f&&half_value(value[2])==.5f&&half_value(value[3])==.75f);
    }
    return 0;
}

// The pre-HUD compute list and final Present list can be recorded before either
// executes. Their shader descriptors must not overwrite one another. Also keep
// HDR highlights and the game's destination alpha through the native redraw.
int native_picture_phases(test::Device& d)
{
    constexpr UINT W=4,H=2,OW=8,OH=4;constexpr auto F=DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,R=D3D12_RESOURCE_STATE_RENDER_TARGET;
    auto rgba=[](UINT n,std::initializer_list<uint16_t> c){std::vector<uint16_t> v;for(UINT i=0;i<n;++i)v.insert(v.end(),c);return v;};
    auto make=[&](UINT w,UINT h,const std::vector<uint16_t>& data,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags){
        auto r=test::CreateTexture(d.device.Get(),w,h,F,D3D12_RESOURCE_STATE_COPY_DEST,flags);
        if(r&&!upload(d,r.Get(),data.data(),8,state))r.Reset();return r;};
    for(bool bad_alpha:{false,true}){
        const auto scene=rgba(W*H,{0x4000,0x3c00,0x3800,0x3a00}); // 2,1,.5,.75
        auto source=make(W,H,scene,S,D3D12_RESOURCE_FLAG_NONE);
        auto check=make(W,H,rgba(W*H,{0x4000,0x3c00,0x3800,uint16_t(bad_alpha?0:0x3a00)}),S,D3D12_RESOURCE_FLAG_NONE);
        auto native=make(OW,OH,rgba(OW*OH,{0x4400,0x4000,0x3c00,0}),U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto out=make(OW,OH,rgba(OW*OH,{0,0,0,0}),U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto n0=make(OW,OH,rgba(OW*OH,{0,0,0,0}),R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto n1=make(OW,OH,rgba(OW*OH,{0x3c00,0x3c00,0x3c00,0}),R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        std::vector<float> zero(W*H*2);auto mv=test::CreateTexture(d.device.Get(),W,H,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        REQUIRE(source&&check&&native&&out&&n0&&n1&&mv&&upload(d,mv.Get(),zero.data(),8,S));
        auto p=nh::Picture::Create(d.device.Get(),source->GetDesc(),OW,OH);REQUIRE(p);
        test::List seed,finish;REQUIRE(seed.Create(d.device.Get())&&finish.Create(d.device.Get()));
        REQUIRE(p->SnapshotScene(seed.list.Get(),source.Get(),S)&&p->SnapshotFull(seed.list.Get(),source.Get(),S));
        REQUIRE(p->Prepare(seed.list.Get(),source.Get(),F,mv.Get(),n0.Get(),n1.Get(),F,1.f,false,true,F));
        REQUIRE(p->SeedNativeAlpha(seed.list.Get(),native.Get()));
        auto barrier=test::Transition(native.Get(),U,S);seed.list->ResourceBarrier(1,&barrier);
        REQUIRE(SUCCEEDED(seed.list->Close()));
        REQUIRE(p->PresentNative(finish.list.Get(),source.Get(),F,native.Get(),check.Get(),out.Get()));
        REQUIRE(SUCCEEDED(finish.list->Close()));
        ID3D12CommandList* lists[]={seed.list.Get(),finish.list.Get()};d.queue->ExecuteCommandLists(2,lists);d.WaitIdle();
        std::vector<uint8_t> pixels;REQUIRE(readback(d,out.Get(),U,8,pixels));uint16_t value[4]{};std::memcpy(value,pixels.data(),8);
        CHECK(half_value(value[0])==(bad_alpha?2.f:4.f));CHECK(half_value(value[3])==.75f);
        bool identity=false,clean=false;CHECK(p->ProofAfterCompletion(identity,clean)&&identity==!bad_alpha&&clean==!bad_alpha);
    }
    // Stripping optional RDEF must not hide executable UAV writes from replay admission.
    ComPtr<ID3DBlob> code,stripped,error;
    constexpr char cs[]="RWByteAddressBuffer b:register(u0);[numthreads(1,1,1)]void main(){b.Store(0,1);}";
    REQUIRE(SUCCEEDED(D3DCompile(cs,sizeof(cs),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&error)));
    REQUIRE(SUCCEEDED(D3DStripShader(code->GetBufferPointer(),code->GetBufferSize(),D3DCOMPILER_STRIP_REFLECTION_DATA,&stripped)));
    CHECK(!na::observe::ExecutableReadOnly({stripped->GetBufferPointer(),stripped->GetBufferSize()}));
    return 0;
}

int jitter_evidence(test::Device& d)
{
    constexpr uint32_t W=256,H=128;
    constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto* dev=d.device.Get();
    std::vector<uint32_t> prev(W*H),cur(W*H);
    std::vector<float> motion(W*H*2,0.f);
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        const uint32_t v=((x*73856093u)^(y*19349663u))&255u;
        prev[y*W+x]=0xff000000u|v|(v<<8)|(v<<16);
    }
    auto tp=test::CreateTexture(dev,W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    auto mv=test::CreateTexture(dev,W,H,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(tp && mv && upload(d,tp.Get(),prev.data(),4,S) && upload(d,mv.Get(),motion.data(),8,S));
    // Use the actual production compute kernel: stationary colour, a known
    // raster displacement, and the opposite sign must give different votes.
    for(int mode=0;mode<3;++mode){
        for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
            const float q=float(x)+(mode==1?-.75f:mode==2?.75f:0.f);
            const int a=(std::max)(0,(std::min)(int(W)-1,int(std::floor(q))));
            const int b=(std::max)(0,(std::min)(int(W)-1,int(std::floor(q))+1));
            const float t=q-std::floor(q);
            const auto v=uint32_t(std::lround((prev[y*W+a]&255u)*(1-t)+(prev[y*W+b]&255u)*t));
            cur[y*W+x]=0xff000000u|v|(v<<8)|(v<<16);
        }
        auto tc=test::CreateTexture(dev,W,H,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto counts=test::CreateBuffer(dev,16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        REQUIRE(tc && counts && upload(d,tc.Get(),cur.data(),4,S));
        test::List l;REQUIRE(l.Create(dev));
        REQUIRE(na::j2::RecordJitterWitnessForTests(d.queue.Get(),l.list.Get(),tc.Get(),tp.Get(),mv.Get(),W,H,.75f,0.f,counts.Get()));
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
        void* p=nullptr;REQUIRE(SUCCEEDED(counts->Map(0,nullptr,&p)));uint32_t v[4];std::memcpy(v,p,16);counts->Unmap(0,nullptr);
        std::printf("jitter evidence mode=%d tested=%u unjittered=%u raster=%u opposite=%u\n",mode,v[0],v[1],v[2],v[3]);
        CHECK(v[0]>3000 && v[mode+1]>v[0]*9/10);
        na::jitter::ColourDomain source;const na::jitter::Source key{1,W,H,28};source.Bind(key);
        CHECK(!source.Observe(key,{60,v[0],v[1],v[2],v[3]}));
        CHECK(source.Observe(key,{120,v[0],v[1],v[2],v[3]}));
        const auto sample=source.Resolve({.75f,0.f});
        CHECK(sample.x==(mode==0?0.f:mode==1?.75f:-.75f));
    }
    return 0;
}

int native_capture(test::Device& d)
{
    constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::vector<uint32_t> pixels(32,0xff804020u);
    auto source=test::CreateTexture(d.device.Get(),8,4,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    auto out=test::CreateTexture(d.device.Get(),8,4,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(source&&out&&upload(d,source.Get(),pixels.data(),4,S)&&upload(d,out.Get(),pixels.data(),4,U));
    wchar_t temp[MAX_PATH]{},dir[MAX_PATH]{};REQUIRE(GetTempPathW(MAX_PATH,temp));
    swprintf_s(dir,L"%sNA-HUD-capture-%lu",temp,GetCurrentProcessId());
    na::capture::j2::Request(1,dir,0);
    na::capture::j2::Identity id{};id.presentation_only=true;id.evaluation_succeeded=true;id.owns_present=true;id.width=8;id.height=4;
    na::capture::j2::Planes planes{};planes.color=source.Get();planes.output=out.Get();
    test::List list;REQUIRE(list.Create(d.device.Get()));
    auto invalid=id;invalid.presentation_only=false;
    CHECK(!na::capture::j2::RecordEvaluation(list.list.Get(),planes,invalid)); // never fabricate missing evaluation guides
    const auto token=na::capture::j2::RecordEvaluation(list.list.Get(),planes,id);REQUIRE(token);
    REQUIRE(SUCCEEDED(list.list->Close()));ID3D12CommandList* lists[]={list.list.Get()};d.queue->ExecuteCommandLists(1,lists);
    na::capture::j2::Submitted(token,d.queue.Get(),1);d.WaitIdle();na::capture::j2::Shutdown();
    const auto st=na::capture::j2::GetStats();CHECK(st.written==1&&st.errors==0);
    const std::wstring base=std::wstring(dir)+L"\\00000-";
    CHECK(GetFileAttributesW((base+L"color.raw").c_str())!=INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((base+L"depth.raw").c_str())==INVALID_FILE_ATTRIBUTES);
    FILE* f=nullptr;REQUIRE(_wfopen_s(&f,(base+L"meta.json").c_str(),L"rb")==0&&f);
    char text[16384]{};std::fread(text,1,sizeof(text)-1,f);std::fclose(f);
    CHECK(std::strstr(text,"\"capture_kind\": \"hud_presentation\"")&&std::strstr(text,"\"evaluation_succeeded\": true"));
    return 0;
}
int classify(test::Device& d)
{
    ID3D12Device* dev = d.device.Get();
    constexpr uint32_t W = 16, H = 16;
    std::vector<uint32_t> cur(W * H), prev(W * H);
    std::vector<float> evidence(W * H, 1.f), motion(W * H * 2, 0.f);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            cur[y * W + x] = 0xFF808080u;                                              // flat background now
            prev[y * W + x] = (x >= 4 && x < 12 && y >= 4 && y < 12) ? 0xFFFFFFFFu : 0xFF808080u; // a HUD square was there
        }
    auto tcur = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto tprev = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto tmv = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    auto tev = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(tcur && tprev && tmv && tev);
    constexpr auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    REQUIRE(upload(d, tcur.Get(), cur.data(), 4, NPSR) && upload(d, tprev.Get(), prev.data(), 4, NPSR) &&
            upload(d, tmv.Get(), motion.data(), 8, NPSR) && upload(d, tev.Get(), evidence.data(), 4, UAV));
    for (uint32_t flags : {0u, 4u}) {
        test::List l;
        REQUIRE(l.Create(dev));
        REQUIRE(na::j2::RecordHudClassifyForTests(d.queue.Get(), l.list.Get(), tcur.Get(), tprev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                                  tmv.Get(), tev.Get(), W, H, flags));
        REQUIRE(SUCCEEDED(l.list->Close()));
        ID3D12CommandList* lists[] = {l.list.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        d.WaitIdle();
        std::vector<uint8_t> raw;
        REQUIRE(readback(d, tev.Get(), UAV, 4, raw));
        float inside = 0.f, outside = 0.f;
        std::memcpy(&inside, raw.data() + (8 * W + 8) * 4, 4);
        std::memcpy(&outside, raw.data() + (1 * W + 1) * 4, 4);
        std::printf("classify flags=%u: vanished HUD %.3f, untouched flat %.3f\n", flags, inside, outside);
        if (flags == 0) {
            CHECK(inside < 0.75f);   // was 1.0 forever before the fix (contrast gate)
            CHECK(outside > 0.99f);  // nothing changed there: evidence kept
        } else {
            CHECK(inside == 0.f);    // forgotten, then counted against
            CHECK(outside == 0.f);
        }
    }
    // Motion-conditioned learning (2026-11-04 fallback): a detailed overlay that stays put while the
    // camera vector moves the world 4 px gains evidence; the same picture with a still camera
    // teaches nothing; a detailed area that follows the motion loses evidence.
    {
        std::vector<uint32_t> pic(W * H);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) pic[y * W + x] = ((x + y) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
        std::vector<float> half(W * H, 0.5f), still_mv(W * H * 2, 0.f), moving(W * H * 2, 0.f);
        for (uint32_t i = 0; i < W * H; ++i) moving[i * 2] = 3.f; // odd shift: the checkerboard does not follow it
        auto a = test::CreateTexture(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        auto mv0 = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        auto mv1 = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
        auto ev = test::CreateTexture(dev, W, H, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        REQUIRE(a && mv0 && mv1 && ev);
        REQUIRE(upload(d, a.Get(), pic.data(), 4, NPSR) && upload(d, mv0.Get(), still_mv.data(), 8, NPSR) &&
                upload(d, mv1.Get(), moving.data(), 8, NPSR) && upload(d, ev.Get(), half.data(), 4, UAV));
        float got[2] = {};
        for (int pass = 0; pass < 2; ++pass) {
            test::List l;
            REQUIRE(l.Create(dev));
            REQUIRE(na::j2::RecordHudClassifyForTests(d.queue.Get(), l.list.Get(), a.Get(), a.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                                      pass ? mv1.Get() : mv0.Get(), ev.Get(), W, H, 0u));
            REQUIRE(SUCCEEDED(l.list->Close()));
            ID3D12CommandList* lists[] = {l.list.Get()};
            d.queue->ExecuteCommandLists(1, lists);
            d.WaitIdle();
            std::vector<uint8_t> raw;
            REQUIRE(readback(d, ev.Get(), UAV, 4, raw));
            std::memcpy(&got[pass], raw.data() + (8 * W + 8) * 4, 4);
        }
        std::printf("classify motion: still camera %.4f, moving camera %.4f (from 0.5)\n", got[0], got[1]);
        CHECK(got[0] == 0.5f);  // a still camera teaches nothing
        CHECK(got[1] > 0.51f);  // screen-fixed while the world moves: HUD evidence
    }
    return 0;
}

constexpr char kDraw[] = R"(
cbuffer C : register(b0) { float4 color; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
float4 ps() : SV_Target { return color; })";
enum class Blend { Opaque, Over, Add };
ComPtr<ID3D12PipelineState> pipeline(ID3D12Device* dev, ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps, Blend b, DXGI_FORMAT dsv,bool vertex_input=false)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    const D3D12_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    if(vertex_input)pd.InputLayout={&element,1};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    rt.BlendEnable = b != Blend::Opaque;
    rt.SrcBlend = b == Blend::Over ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
    rt.DestBlend = b == Blend::Over ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = dsv;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
    return pso;
}
struct Hud { Blend blend; float color[4]; int32_t rect[4]; };
// Scene colour, then the HUD (render pixels).
constexpr float kScene[4] = {0.2f, 0.4f, 0.6f, 1.f};
const Hud kHud[3] = {{Blend::Over, {1.f, 0.2f, 0.1f, 0.6f}, {8, 4, 40, 20}},
                     {Blend::Add, {0.1f, 0.1f, 0.3f, 1.f}, {24, 12, 56, 28}},
                     {Blend::Opaque, {0.9f, 0.9f, 0.9f, 1.f}, {0, 26, 10, 32}}};
test::Device* native_fixture=nullptr;
unsigned native_fixture_calls=0;
bool native_fixture_renderer(ID3D12CommandQueue* queue,ID3D12Resource*,uint32_t,uint32_t,
                             ID3D12Resource* out,uint32_t,uint32_t,const nh::Layer&,ID3D12CommandList* replay)
{
    // Explicit fake reconstruction, real native HUD draws and actual DX12.
    // Waiting is test-only. This does not qualify DLSS model/image quality.
    if(!native_fixture)return false;
    na::observe::HostWorkScope host;
    ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(FAILED(native_fixture->device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))))return false;
    auto rtv=heap->GetCPUDescriptorHandleForHeapStart();native_fixture->device->CreateRenderTargetView(out,nullptr,rtv);
    test::List seed;if(!seed.Create(native_fixture->device.Get()))return false;
    auto b=test::Transition(out,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);seed.list->ResourceBarrier(1,&b);
    seed.list->ClearRenderTargetView(rtv,kScene,0,nullptr);b=test::Transition(out,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);seed.list->ResourceBarrier(1,&b);
    if(FAILED(seed.list->Close()))return false;
    ID3D12CommandList* work[]={seed.list.Get(),replay};queue->ExecuteCommandLists(2,work);native_fixture->WaitIdle();++native_fixture_calls;return true;
}

int source_lineage(test::Device& d)
{
    // The actual handoff shader reads t17. A plausible, stale picture at t0
    // must not become its source merely because it occurs in a table prefix.
    auto* dev=d.device.Get();
    constexpr UINT W=64,H=32;
    constexpr auto F=DXGI_FORMAT_R8G8B8A8_UNORM;
    auto actual=test::CreateTexture(dev,W,H,F,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_NONE);
    auto stale=test::CreateTexture(dev,W,H,F,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_NONE);
    auto target=test::CreateTexture(dev,W,H,F,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(actual && stale && target);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,32,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ComPtr<ID3D12DescriptorHeap> heap,rtv;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv))));
    dev->CreateRenderTargetView(target.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=F;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;
    const UINT inc=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto h=heap->GetCPUDescriptorHandleForHeapStart();
    for(UINT i=0;i<32;++i){dev->CreateShaderResourceView(i==17?actual.Get():stale.Get(),&sv,h);h.ptr+=inc;}
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,32,0,0,0};
    D3D12_ROOT_PARAMETER param{};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;param.DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&param,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sig,err,vs,ps;ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root))));
    constexpr char code[]=R"(
Texture2D<float4> image:register(t17);
float4 vs(uint id:SV_VertexID):SV_Position{float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),.5,1);}
float4 ps(float4 p:SV_Position):SV_Target{return image.Load(int3(p.xy,0));}
)";
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof(code)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&err)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof(code)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&err)));
    auto pso=pipeline(dev,root.Get(),vs.Get(),ps.Get(),Blend::Opaque,DXGI_FORMAT_UNKNOWN);REQUIRE(pso);
    test::List l;REQUIRE(l.Create(dev));REQUIRE(SUCCEEDED(l.list->Close()));
    REQUIRE(SUCCEEDED(l.list->Reset(l.alloc.Get(),nullptr)));
    l.list->SetGraphicsRootSignature(root.Get());l.list->SetPipelineState(pso.Get());
    ID3D12DescriptorHeap* heaps[]={heap.Get()};l.list->SetDescriptorHeaps(1,heaps);
    l.list->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
    const auto rt=rtv->GetCPUDescriptorHandleForHeapStart();l.list->OMSetRenderTargets(1,&rt,FALSE,nullptr);
    const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};const D3D12_RECT sc{0,0,LONG(W),LONG(H)};
    l.list->RSSetViewports(1,&vp);l.list->RSSetScissorRects(1,&sc);l.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    na::observe::NativeOperation op;REQUIRE(na::observe::ListNativeOperation(l.list.Get(),false,op));
    CHECK(op.reflection_complete && op.descriptors.size()==32);
    CHECK(!op.descriptors[0].used && op.descriptors[17].used);
    uint64_t source=999;const auto destination=na::observe::IdOf(target.Get());
    CHECK(nh::PictureSource(op,destination,W,H,source));CHECK(source==na::observe::IdOf(actual.Get()));
    op.reflection_complete=false;CHECK(!nh::PictureSource(op,destination,W,H,source) && source==0);op.reflection_complete=true;
    op.descriptors[0].used=true;op.descriptors[0].resource=stale;
    CHECK(!nh::PictureSource(op,destination,W,H,source) && source==0); // actually two sources = ambiguous
    op.descriptors[0].used=false;op.descriptors[17].view.srv.Texture2D.MostDetailedMip=1;
    CHECK(nh::PictureSource(op,destination,W,H,source) && source==0); // not a whole-base image
    l.list->DrawInstanced(3,1,0,0);REQUIRE(SUCCEEDED(l.list->Close()));
    ID3D12CommandList* lists[]={l.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason()==S_OK);
    return 0;
}

int canvas_probe(test::Device& d)
{
    // Final handoff bindings need not be known. Discovery compares immutable
    // actual GPU pictures and cannot remove/change either picture on its own.
    constexpr UINT W=64,H=32;constexpr auto F=DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr auto S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::vector<uint8_t> a(W*H*4),b(W*H*4);
    for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){const UINT i=(y*W+x)*4;
        a[i]=uint8_t(20+x*3);a[i+1]=uint8_t(30+y*5);a[i+2]=90;a[i+3]=255;}
    auto source=test::CreateTexture(d.device.Get(),W,H,F,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    auto final=test::CreateTexture(d.device.Get(),W,H,F,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    REQUIRE(source&&final&&upload(d,source.Get(),a.data(),4,S)&&upload(d,final.Get(),a.data(),4,S));
    auto p=nh::Picture::Create(d.device.Get(),source->GetDesc(),W*2,H*2);REQUIRE(p);
    for(int changed=0;changed<2;++changed){
        p->BeginCycle();test::List l;REQUIRE(l.Create(d.device.Get()));
        CHECK(!p->Probe(l.list.Get(),final.Get(),F)); // no stale snapshot permission
        REQUIRE(p->SnapshotFull(l.list.Get(),source.Get(),S));
        // One changed pixel, including at the edge, rejects the complete match.
        if(changed){b=a;b[(W*H-1)*4]=240;
            auto barrier=test::Transition(final.Get(),S,D3D12_RESOURCE_STATE_COPY_DEST);l.list->ResourceBarrier(1,&barrier);
            REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* q[]={l.list.Get()};d.queue->ExecuteCommandLists(1,q);d.WaitIdle();
            REQUIRE(upload(d,final.Get(),b.data(),4,S));REQUIRE(l.Create(d.device.Get()));}
        REQUIRE(p->Probe(l.list.Get(),final.Get(),F));
        REQUIRE(SUCCEEDED(l.list->Close()));ID3D12CommandList* q[]={l.list.Get()};d.queue->ExecuteCommandLists(1,q);d.WaitIdle();
        bool identity=false,clean=true;REQUIRE(p->ProofAfterCompletion(identity,clean));
        CHECK(identity==!changed);CHECK(!clean);CHECK(!p->ProofAfterCompletion(identity,clean));
        std::vector<uint8_t> pixels;REQUIRE(readback(d,source.Get(),S,4,pixels));CHECK(pixels==a);
        REQUIRE(readback(d,final.Get(),S,4,pixels));CHECK(pixels==(changed?b:a));
    }
    return 0;
}

int draw_handoff(test::Device& d,bool unresolved=false,bool native=false)
{
    auto* dev=d.device.Get();constexpr UINT W=64,H=32,OW=128,OH=64;constexpr auto F=DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr auto R=D3D12_RESOURCE_STATE_RENDER_TARGET,S=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,U=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto upstream=test::CreateTexture(dev,W,H,F,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto target=test::CreateTexture(dev,W,H,F,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto stale=test::CreateTexture(dev,W,H,F,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_NONE);
    auto target2=test::CreateTexture(dev,W,H,F,R,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    REQUIRE(upstream&&target&&stale);nh::Layer learned;CHECK(!nh::TakeFrame(target.Get(),W,H,OW,OH,&learned,d.queue.Get()));
    REQUIRE(target2);CHECK(!nh::TakeFrame(target2.Get(),W,H,OW,OH,&learned,d.queue.Get()));
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};ComPtr<ID3D12DescriptorHeap> rtvs,heap;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))));
    const UINT ri=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);auto a=rtvs->GetCPUDescriptorHandleForHeapStart(),b=a;b.ptr+=ri;
    dev->CreateRenderTargetView(upstream.Get(),nullptr,a);dev->CreateRenderTargetView(target.Get(),nullptr,b);
    auto b2=b;b2.ptr+=ri;dev->CreateRenderTargetView(target2.Get(),nullptr,b2);
    hd={D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,32,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))));
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();const UINT inc=dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=F;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    for(UINT i=0;i<32;++i){dev->CreateShaderResourceView(i==17?upstream.Get():stale.Get(),&sv,cpu);cpu.ptr+=inc;}
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,32,0,0,0};D3D12_ROOT_PARAMETER param{};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;param.DescriptorTable={1,&range};
    D3D12_ROOT_SIGNATURE_DESC rd{1,&param,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> sig,err,vs,ps,processed;
    ComPtr<ID3D12RootSignature> transfer_root,hud_root;REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&transfer_root))));
    constexpr char code[]=R"(
Texture2D<float4> image:register(t17);
float4 vs(uint id:SV_VertexID):SV_Position{float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),.5,1);}
float4 ps(float4 p:SV_Position):SV_Target{return image.Load(int3(p.xy,0));}
float4 altered(float4 p:SV_Position):SV_Target{return image.Load(int3(p.xy,0))*.8;}
)";
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&err)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&err)));
    REQUIRE(SUCCEEDED(D3DCompile(code,sizeof code-1,nullptr,nullptr,nullptr,"altered","ps_5_0",0,0,&processed,&err)));
    if(unresolved){
        ComPtr<ID3DBlob> stripped;
        REQUIRE(SUCCEEDED(D3DStripShader(ps->GetBufferPointer(),ps->GetBufferSize(),D3DCOMPILER_STRIP_REFLECTION_DATA,&stripped)));ps=stripped;
        stripped.Reset();REQUIRE(SUCCEEDED(D3DStripShader(processed->GetBufferPointer(),processed->GetBufferSize(),D3DCOMPILER_STRIP_REFLECTION_DATA,&stripped)));processed=stripped;
    }
    auto copy_pso=pipeline(dev,transfer_root.Get(),vs.Get(),ps.Get(),Blend::Opaque,DXGI_FORMAT_UNKNOWN);
    auto process_pso=pipeline(dev,transfer_root.Get(),vs.Get(),processed.Get(),Blend::Opaque,DXGI_FORMAT_UNKNOWN);REQUIRE(copy_pso&&process_pso);
    D3D12_ROOT_PARAMETER color{};color.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;color.Constants={0,0,4};
    rd={1,&color,0,nullptr,native?D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT:D3D12_ROOT_SIGNATURE_FLAG_NONE};REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err)));
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&hud_root))));
    REQUIRE(SUCCEEDED(D3DCompile(kDraw,sizeof kDraw-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&err)));
    if(native){constexpr char vb_code[]="float4 main(float2 p:POSITION):SV_Position{return float4(p,.5,1);}";
        REQUIRE(SUCCEEDED(D3DCompile(vb_code,sizeof vb_code-1,nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vs,&err)));}
    if(unresolved){ComPtr<ID3DBlob> stripped;
        REQUIRE(SUCCEEDED(D3DStripShader(vs->GetBufferPointer(),vs->GetBufferSize(),D3DCOMPILER_STRIP_REFLECTION_DATA,&stripped)));vs=stripped;
        stripped.Reset();REQUIRE(SUCCEEDED(D3DStripShader(ps->GetBufferPointer(),ps->GetBufferSize(),D3DCOMPILER_STRIP_REFLECTION_DATA,&stripped)));ps=stripped;}
    auto scene_pso=pipeline(dev,hud_root.Get(),vs.Get(),ps.Get(),Blend::Opaque,DXGI_FORMAT_UNKNOWN,native);
    auto hud_pso=pipeline(dev,hud_root.Get(),vs.Get(),ps.Get(),Blend::Over,DXGI_FORMAT_UNKNOWN,native);
    auto add_pso=pipeline(dev,hud_root.Get(),vs.Get(),ps.Get(),Blend::Add,DXGI_FORMAT_UNKNOWN,native);REQUIRE(scene_pso&&hud_pso&&add_pso);
    auto vertices=test::CreateBuffer(dev,24,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto vertex_upload=test::CreateBuffer(dev,24,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(vertex_upload);
    auto indices=test::CreateBuffer(dev,12,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);REQUIRE(vertices&&indices);
    void* mapped=nullptr;REQUIRE(SUCCEEDED(vertex_upload->Map(0,nullptr,&mapped)));const float xy[]={-1,-1,-1,3,3,-1};std::memcpy(mapped,xy,sizeof xy);vertex_upload->Unmap(0,nullptr);
    test::List init;REQUIRE(init.Create(dev));init.list->CopyBufferRegion(vertices.Get(),0,vertex_upload.Get(),0,sizeof xy);
    auto ready=test::Transition(vertices.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);init.list->ResourceBarrier(1,&ready);
    REQUIRE(SUCCEEDED(init.list->Close()));ID3D12CommandList* setup[]={init.list.Get()};d.queue->ExecuteCommandLists(1,setup);d.WaitIdle();
    REQUIRE(SUCCEEDED(indices->Map(0,nullptr,&mapped)));const UINT ids[]={0,1,2};std::memcpy(mapped,ids,sizeof ids);indices->Unmap(0,nullptr);
    const D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(),24,8};const D3D12_INDEX_BUFFER_VIEW ib{indices->GetGPUVirtualAddress(),12,DXGI_FORMAT_R32_UINT};
    if(native){native_fixture=&d;nh::SetPreHudRendererForTests(&native_fixture_renderer);}
    ComPtr<ID3D12QueryHeap> query;D3D12_QUERY_HEAP_DESC qd{};qd.Type=D3D12_QUERY_HEAP_TYPE_OCCLUSION;qd.Count=1;
    REQUIRE(SUCCEEDED(dev->CreateQueryHeap(&qd,IID_PPV_ARGS(&query))));
    test::List game;REQUIRE(game.Create(dev));REQUIRE(SUCCEEDED(game.list->Close()));
    const D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};const D3D12_RECT all{0,0,LONG(W),LONG(H)},hud{8,4,40,20};
    const float hc[4]={1,.2f,.1f,.6f};
    auto validate=[&](ID3D12Resource* shown,UINT frame)->int {
        nh::Layer layer;const bool found=nh::TakeFrame(shown,W,H,OW,OH,&layer,d.queue.Get());
        std::printf("draw handoff frame=%u found=%d picture=%d clean=%d\n",frame,found,!!layer.picture,layer.clean_allowed);
        if(unresolved)std::printf("canvas replay: %s\n",nh::StatusLine().c_str());
        if(layer.probe){
            const auto state=native?D3D12_RESOURCE_STATE_PRESENT:R;
            test::List probe;REQUIRE(probe.Create(dev));auto barrier=test::Transition(shown,state,S);probe.list->ResourceBarrier(1,&barrier);
            REQUIRE(layer.probe->Probe(probe.list.Get(),shown,F));barrier=test::Transition(shown,S,state);probe.list->ResourceBarrier(1,&barrier);
            REQUIRE(SUCCEEDED(probe.list->Close()));ID3D12CommandList* lists[]={probe.list.Get()};d.queue->ExecuteCommandLists(1,lists);d.WaitIdle();
            bool identity=false,clean=false;REQUIRE(layer.probe->ProofAfterCompletion(identity,clean));CHECK(identity);
            nh::ReportCanvasProbe(layer.probe_source,layer.probe_generation,identity);
        }
        if(frame<(unresolved?2u:3u)||frame==6){CHECK(!found);return 0;}REQUIRE(found&&layer.picture&&layer.owner);CHECK(layer.clean_allowed==(frame!=5));
        if(native&&frame!=7){
            REQUIRE(layer.native_picture&&layer.native_check);
            auto out=test::CreateTexture(dev,OW,OH,F,U,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);REQUIRE(out);
            test::List finish;REQUIRE(finish.Create(dev));auto barrier=test::Transition(shown,D3D12_RESOURCE_STATE_PRESENT,S);finish.list->ResourceBarrier(1,&barrier);
            REQUIRE(layer.picture->PresentNative(finish.list.Get(),shown,F,layer.native_picture,layer.native_check,out.Get()));
            barrier=test::Transition(shown,S,D3D12_RESOURCE_STATE_PRESENT);finish.list->ResourceBarrier(1,&barrier);REQUIRE(SUCCEEDED(finish.list->Close()));
            ID3D12CommandList* work[]={finish.list.Get()};d.queue->ExecuteCommandLists(1,work);d.WaitIdle();
            bool identity=false,clean=false;REQUIRE(layer.picture->ProofAfterCompletion(identity,clean));CHECK(identity==(frame!=4));CHECK(clean==identity);
            std::vector<uint8_t> pixels,original;REQUIRE(readback(d,out.Get(),U,4,pixels));
            REQUIRE(readback(d,shown,D3D12_RESOURCE_STATE_PRESENT,4,original));
            CHECK(frame==4?std::abs(int(pixels[(20*OW+40)*4])-int(original[(10*W+20)*4]))<=1:pixels[(20*OW+40)*4]>=170);
            if(frame!=4)CHECK(std::abs(int(pixels[(OH-2)*OW*4])-51)<=1);
            nh::ReportNativePictureResult(identity);return 0;
        }
        if(native&&frame==7)CHECK(!layer.native_picture&&nh::GetStats().native_resource_changed>0);
        const auto shown_state=native?D3D12_RESOURCE_STATE_PRESENT:R;
        std::vector<uint8_t> pixels;REQUIRE(readback(d,shown,shown_state,4,pixels));CHECK(pixels[(10*W+20)*4]>(frame==4?125:160));
        std::vector<float> mv(W*H*2,0);auto motion=test::CreateTexture(dev,W,H,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        REQUIRE(motion&&upload(d,motion.Get(),mv.data(),8,S));
        auto out=test::CreateTexture(dev,OW,OH,F,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);REQUIRE(out);
        std::vector<UINT> dark(OW*OH,0xff1a1a1a);REQUIRE(upload(d,out.Get(),dark.data(),4,U));test::List finish;REQUIRE(finish.Create(dev));
        auto t=test::Transition(shown,shown_state,S);finish.list->ResourceBarrier(1,&t);
        REQUIRE(layer.picture->Prepare(finish.list.Get(),shown,F,motion.Get(),layer.n0,layer.n1,layer.view,layer.grey,layer.srgb,layer.clean_allowed,F));
        REQUIRE(layer.picture->Composite(finish.list.Get(),out.Get()));t=test::Transition(shown,S,shown_state);finish.list->ResourceBarrier(1,&t);
        REQUIRE(SUCCEEDED(finish.list->Close()));ID3D12CommandList* done[]={finish.list.Get()};d.queue->ExecuteCommandLists(1,done);d.WaitIdle();
        REQUIRE(readback(d,out.Get(),U,4,pixels));CHECK(pixels[(20*OW+40)*4]>(frame==4?20:150));
        if(frame==4)CHECK(pixels[(20*OW+40)*4]==26);
        bool identity=false,clean=false;CHECK(layer.picture->ProofAfterCompletion(identity,clean));CHECK(identity==(frame!=4));CHECK(clean==(frame!=4&&frame!=5));
        nh::ReportPictureResult(identity,clean);
        if(frame==3){REQUIRE(readback(d,layer.picture->Input(),S,8,pixels));uint16_t red=0;std::memcpy(&red,pixels.data()+(10*W+20)*8,2);CHECK(std::abs(half_value(red)-.2f)<.002f);}
        return 0;
    };
    bool presented[2]{};
    for(UINT frame=0;frame<8;++frame){
        na::nr::native::Reset(game.list.Get());REQUIRE(SUCCEEDED(game.alloc->Reset()));REQUIRE(SUCCEEDED(game.list->Reset(game.alloc.Get(),nullptr)));
        auto* l=game.list.Get();l->OMSetRenderTargets(1,&a,FALSE,nullptr);l->SetGraphicsRootSignature(hud_root.Get());l->SetPipelineState(scene_pso.Get());
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&all);
        if(native){l->IASetVertexBuffers(0,1,&vb);l->IASetIndexBuffer(&ib);}
        const float different[4]={.4f,.1f,.2f,1.f};
        l->SetGraphicsRoot32BitConstants(0,4,frame==4?different:kScene,0);
        if(native)l->DrawIndexedInstanced(3,1,0,0,0);else l->DrawInstanced(3,1,0,0);
        if(frame==6)l->BeginQuery(query.Get(),D3D12_QUERY_TYPE_BINARY_OCCLUSION,0);
        l->SetPipelineState(frame==5?add_pso.Get():hud_pso.Get());l->RSSetScissorRects(1,&hud);l->SetGraphicsRoot32BitConstants(0,4,hc,0);
        if(frame==6){na::observe::DrawInfo info;REQUIRE(na::observe::ListDrawState(l,info));CHECK(info.query_active&&info.shader_readonly);}
        if(native)l->DrawIndexedInstanced(3,1,0,0,0);else l->DrawInstanced(3,1,0,0);
        if(frame==6){l->EndQuery(query.Get(),D3D12_QUERY_TYPE_BINARY_OCCLUSION,0);na::observe::DrawInfo info;REQUIRE(na::observe::ListDrawState(l,info));CHECK(!info.query_active);}
        auto t=test::Transition(upstream.Get(),R,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);l->ResourceBarrier(1,&t);
        if(native&&presented[frame==4?1:0]){t=test::Transition(frame==4?target2.Get():target.Get(),D3D12_RESOURCE_STATE_PRESENT,R);l->ResourceBarrier(1,&t);}
        l->OMSetRenderTargets(1,frame==4?&b2:&b,FALSE,nullptr);l->SetGraphicsRootSignature(transfer_root.Get());l->SetPipelineState(frame==4?process_pso.Get():copy_pso.Get());
        ID3D12DescriptorHeap* heaps[]={heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetGraphicsRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        l->RSSetViewports(1,&vp);l->RSSetScissorRects(1,&all);l->DrawInstanced(3,1,0,0);
        t=test::Transition(upstream.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,R);l->ResourceBarrier(1,&t);
        if(native){t=test::Transition(frame==4?target2.Get():target.Get(),R,D3D12_RESOURCE_STATE_PRESENT);l->ResourceBarrier(1,&t);presented[frame==4?1:0]=true;}
        if(native&&frame==7){t=test::Transition(vertices.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,D3D12_RESOURCE_STATE_COPY_DEST);l->ResourceBarrier(1,&t);}
        REQUIRE(SUCCEEDED(l->Close()));ID3D12CommandList* lists[]={l};{na::nr::native::Submission submitted(d.queue.Get(),1,lists);d.queue->ExecuteCommandLists(1,lists);}d.WaitIdle();
        if(native){
            // An unrelated batch cannot licence replay of old game buffers.
            test::List unrelated;REQUIRE(unrelated.Create(dev));REQUIRE(SUCCEEDED(unrelated.list->Close()));
            ID3D12CommandList* other[]={unrelated.list.Get()};const auto before=native_fixture_calls;
            nh::AfterSubmission(d.queue.Get(),1,other);CHECK(native_fixture_calls==before);
            nh::AfterSubmission(d.queue.Get(),1,lists);
        }
        if(frame==3)continue; // render-ahead: the next source reuses its resource before this Present
        if(frame==4){if(validate(target.Get(),3)||validate(target2.Get(),4))return 1;}
        else if(validate(target.Get(),frame))return 1;
    }
    if(native){CHECK(native_fixture_calls>=4);CHECK(nh::GetStats().native_reused>0);native_fixture=nullptr;nh::SetPreHudRendererForTests(nullptr);}
    return 0;
}

int end_to_end(test::Device& d)
{
    ID3D12Device* dev = d.device.Get();
    nh::Install(); // before the observer sees any recording (raster snapshots latch at Reset)
    REQUIRE(na::observe::InstallProducers(dev, d.queue.Get(), true));
    na_observe_config cfg{};
    cfg.size_bytes = sizeof cfg;
    REQUIRE(na_observe_start(&cfg, sink, nullptr) == NA_OK);
    constexpr uint32_t RW = 64, RH = 32, OW = 96, OH = 48;
    constexpr DXGI_FORMAT F = DXGI_FORMAT_R8G8B8A8_UNORM, DF = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    constexpr auto RTS = D3D12_RESOURCE_STATE_RENDER_TARGET, NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto proxy = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto other = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth = test::CreateTexture(dev, RW, RH, DF, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto ref = test::CreateTexture(dev, OW, OH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto picture = test::CreateTexture(dev, OW, OH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto out = test::CreateTexture(dev, OW, OH, F, UAV, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    REQUIRE(proxy && other && depth && ref && picture && out);
    nh::Layer layer;
    CHECK(!nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer)); // learns the proxy

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
    REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs))) && SUCCEEDED(dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&dsvs))));
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    ID3D12Resource* targets[4] = {proxy.Get(), other.Get(), ref.Get(), picture.Get()};
    for (int i = 0; i < 4; ++i) {
        rtv[i] = rtvs->GetCPUDescriptorHandleForHeapStart();
        rtv[i].ptr += SIZE_T(i) * inc;
        dev->CreateRenderTargetView(targets[i], nullptr, rtv[i]);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
    dev->CreateDepthStencilView(depth.Get(), nullptr, dsv);

    ComPtr<ID3DBlob> vs, ps, err, blob;
    REQUIRE(SUCCEEDED(D3DCompile(kDraw, sizeof kDraw - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)));
    REQUIRE(SUCCEEDED(D3DCompile(kDraw, sizeof kDraw - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err)));
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants = {0, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC rsd{1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    REQUIRE(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)));
    ComPtr<ID3D12RootSignature> root;
    REQUIRE(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))));
    ComPtr<ID3D12PipelineState> with_depth[3], plain[3];
    for (int b = 0; b < 3; ++b) {
        with_depth[b] = pipeline(dev, root.Get(), vs.Get(), ps.Get(), Blend(b), DF);
        plain[b] = pipeline(dev, root.Get(), vs.Get(), ps.Get(), Blend(b), DXGI_FORMAT_UNKNOWN);
        REQUIRE(with_depth[b] && plain[b]);
    }

    // The game's frame: scene into the proxy (no depth), HUD with its own depth target, then a
    // later pass on another target that relies on the game's viewport and scissor still being set.
    test::List game;
    REQUIRE(game.Create(dev));
    ID3D12GraphicsCommandList* l = game.list.Get();
    REQUIRE(SUCCEEDED(l->Close()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    const float black[4] = {0, 0, 0, 1};
    l->ClearRenderTargetView(rtv[1], black, 0, nullptr);
    l->SetGraphicsRootSignature(root.Get());
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT full{0, 0, float(RW), float(RH), 0, 1};
    const D3D12_RECT all{0, 0, LONG(RW), LONG(RH)};
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    l->RSSetViewports(1, &full);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, kScene, 0);
    l->DrawInstanced(3, 1, 0, 0); // the composite: stays in the proxy
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
    l->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv);
    for (const Hud& h : kHud) {
        const D3D12_RECT r{h.rect[0], h.rect[1], h.rect[2], h.rect[3]};
        l->RSSetScissorRects(1, &r);
        l->SetPipelineState(with_depth[int(h.blend)].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, h.color, 0);
        l->DrawInstanced(3, 1, 0, 0);
    }
    const float white[4] = {1, 1, 1, 1};
    l->OMSetRenderTargets(1, &rtv[1], FALSE, nullptr); // viewport/scissor: still the game's last ones
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, white, 0);
    l->DrawInstanced(3, 1, 0, 0);
    REQUIRE(SUCCEEDED(l->Close()));
    ID3D12CommandList* lists[] = {l};
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);

    const nh::Stats st = nh::GetStats(); // before TakeFrame: this frame's counts are in the totals
    const bool got = nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer);
    std::printf("native HUD: layer=%d %s\n", got ? 1 : 0, nh::StatusLine().c_str());
    CHECK(got);
    CHECK(st.scene_draws == 1);
    CHECK(st.redirected == 3);
    CHECK(st.forwarded == 0);
    CHECK(st.restore_failed == 0);
    REQUIRE(got && layer.n0 && layer.n1);

    std::vector<uint8_t> px;
    REQUIRE(readback(d, proxy.Get(), RTS, 4, px));
    const auto at = [&](const std::vector<uint8_t>& v, uint32_t w, uint32_t x, uint32_t y, int c) { return int(v[(size_t(y) * w + x) * 4 + c]); };
    // DLSS's input: the scene only, where every HUD draw would have been.
    CHECK(std::abs(at(px, RW, 20, 10, 0) - 51) <= 1 && std::abs(at(px, RW, 40, 20, 2) - 153) <= 1 && std::abs(at(px, RW, 5, 28, 0) - 51) <= 1);
    REQUIRE(readback(d, other.Get(), RTS, 4, px));
    CHECK(at(px, RW, 5, 28, 0) == 255); // the game's scissor came back (not our native-size one)
    CHECK(at(px, RW, 12, 28, 0) == 0);
    CHECK(at(px, RW, 5, 20, 0) == 0);

    // Reference: the same frame drawn directly at native size; picture: the scene alone
    // at native size (what DLSS would hand the composite).
    test::List rl;
    REQUIRE(rl.Create(dev));
    ID3D12GraphicsCommandList* r = rl.list.Get();
    r->ClearRenderTargetView(rtv[2], kScene, 0, nullptr);
    r->ClearRenderTargetView(rtv[3], kScene, 0, nullptr);
    r->SetGraphicsRootSignature(root.Get());
    r->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT nfull{0, 0, float(OW), float(OH), 0, 1};
    r->OMSetRenderTargets(1, &rtv[2], FALSE, nullptr);
    r->RSSetViewports(1, &nfull);
    for (const Hud& h : kHud) {
        int32_t s[4];
        nh::ScaleScissor(h.rect, float(OW) / RW, float(OH) / RH, OW, OH, s);
        const D3D12_RECT rr{s[0], s[1], s[2], s[3]};
        r->RSSetScissorRects(1, &rr);
        r->SetPipelineState(plain[int(h.blend)].Get());
        r->SetGraphicsRoot32BitConstants(0, 4, h.color, 0);
        r->DrawInstanced(3, 1, 0, 0);
    }
    D3D12_RESOURCE_BARRIER b[3] = {test::Transition(picture.Get(), RTS, NPSR), test::Transition(layer.n0, RTS, NPSR),
                                   test::Transition(layer.n1, RTS, NPSR)};
    r->ResourceBarrier(3, b);
    REQUIRE(na::j2::RecordHudLayerForTests(d.queue.Get(), r, picture.Get(), F, layer.n0, layer.n1, layer.view, layer.grey, layer.srgb,
                                           out.Get(), OW, OH));
    for (auto& t : b) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    r->ResourceBarrier(3, b);
    REQUIRE(SUCCEEDED(r->Close()));
    ID3D12CommandList* rls[] = {r};
    d.queue->ExecuteCommandLists(1, rls);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    std::vector<uint8_t> want, have;
    REQUIRE(readback(d, ref.Get(), RTS, 4, want) && readback(d, out.Get(), UAV, 4, have));
    int worst = 0;
    uint32_t wx = 0, wy = 0;
    for (uint32_t y = 0; y < OH; ++y)
        for (uint32_t x = 0; x < OW; ++x)
            for (int c = 0; c < 3; ++c) {
                const int e = std::abs(at(want, OW, x, y, c) - at(have, OW, x, y, c));
                if (e > worst) { worst = e; wx = x; wy = y; }
            }
    std::printf("native HUD composite vs direct native draw: worst %d/255 at %u,%u (over %d,%d,%d; add+over %d,%d,%d)\n", worst, wx, wy,
                at(have, OW, 20, 10, 0), at(have, OW, 20, 10, 1), at(have, OW, 20, 10, 2),
                at(have, OW, 45, 22, 0), at(have, OW, 45, 22, 1), at(have, OW, 45, 22, 2));
    CHECK(worst <= 4);

    // Second frame, Anno's pause menu: after a HUD draw, an opaque full-screen draw (the blurred
    // scene copy) covers everything; it must go to the proxy and restart the layers, and later
    // HUD draws still go native.
    const float back[4] = {0.f, 0.6f, 0.2f, 1.f};
    nh::SetCoverRule(true); // opt-in ([present] hud_cover=1): the rule under test
    REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    l->SetGraphicsRootSignature(root.Get());
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    l->RSSetViewports(1, &full);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, kScene, 0);
    l->DrawInstanced(3, 1, 0, 0); // composite
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
    l->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv);
    {
        const D3D12_RECT hr{kHud[0].rect[0], kHud[0].rect[1], kHud[0].rect[2], kHud[0].rect[3]};
        l->RSSetScissorRects(1, &hr);
        l->SetPipelineState(with_depth[int(kHud[0].blend)].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, kHud[0].color, 0);
        l->DrawInstanced(3, 1, 0, 0); // HUD: native
    }
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, back, 0);
    l->DrawInstanced(3, 1, 0, 0); // pause background: covers the screen
    l->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv);
    {
        const D3D12_RECT hr{kHud[2].rect[0], kHud[2].rect[1], kHud[2].rect[2], kHud[2].rect[3]};
        l->RSSetScissorRects(1, &hr);
        l->SetPipelineState(with_depth[int(kHud[2].blend)].Get());
        l->SetGraphicsRoot32BitConstants(0, 4, kHud[2].color, 0);
        l->DrawInstanced(3, 1, 0, 0); // menu panel: native
    }
    REQUIRE(SUCCEEDED(l->Close()));
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    const nh::Stats s2 = nh::GetStats();
    nh::Layer layer2;
    const bool got2 = nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer2);
    std::printf("native HUD pause frame: layer=%d %s\n", got2 ? 1 : 0, nh::StatusLine().c_str());
    CHECK(got2);
    CHECK(s2.scene_draws - st.scene_draws == 1);
    CHECK(s2.redirected - st.redirected == 2);
    CHECK(s2.covered - st.covered == 1);
    REQUIRE(got2 && layer2.n0);
    REQUIRE(readback(d, proxy.Get(), RTS, 4, px));
    CHECK(std::abs(at(px, RW, 20, 10, 1) - 153) <= 1 && at(px, RW, 20, 10, 0) == 0); // the background, in DLSS's input
    REQUIRE(readback(d, layer2.n0, RTS, 4, px));
    CHECK(at(px, OW, 30, 15, 0) == 0 && at(px, OW, 30, 15, 1) == 0); // the covered HUD draw is gone from the layer
    CHECK(std::abs(at(px, OW, 5, 44, 0) - 230) <= 1);                 // the later panel is there, at native size
    nh::SetCoverRule(false);

    // Third frame, Anno's menu text: a UI clipping mask (a depth/stencil-only draw, no colour
    // target) on the HUD's depth before the frame's first HUD draw, then text tested against it.
    // The mask must reach the native depths too, or the text fails the stencil test there.
    ComPtr<ID3D12PipelineState> mask_pso, masked_pso;
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = pd.DepthStencilState.StencilWriteMask = 0xFF;
        D3D12_DEPTH_STENCILOP_DESC write{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
        pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = write;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 0;
        pd.DSVFormat = DF;
        pd.SampleDesc.Count = 1;
        REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&mask_pso))));
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        auto& rt = pd.BlendState.RenderTarget[0];
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.BlendEnable = TRUE; // blended: HUD
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pd.DepthStencilState.StencilWriteMask = 0;
        D3D12_DEPTH_STENCILOP_DESC test_eq{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
        pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = test_eq;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = F;
        REQUIRE(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&masked_pso))));
    }
    const float text[4] = {0.8f, 0.4f, 0.f, 1.f};
    REQUIRE(SUCCEEDED(game.alloc->Reset()) && SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
    l->SetGraphicsRootSignature(root.Get());
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->OMSetRenderTargets(1, &rtv[0], FALSE, nullptr);
    l->RSSetViewports(1, &full);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(plain[0].Get());
    l->SetGraphicsRoot32BitConstants(0, 4, kScene, 0);
    l->DrawInstanced(3, 1, 0, 0); // composite
    l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
    l->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    l->OMSetStencilRef(1);
    {
        const D3D12_RECT mr{kHud[0].rect[0], kHud[0].rect[1], kHud[0].rect[2], kHud[0].rect[3]};
        l->RSSetScissorRects(1, &mr);
        l->SetPipelineState(mask_pso.Get());
        l->DrawInstanced(3, 1, 0, 0); // the mask: stencil 1 inside (8,4)-(40,20)
    }
    l->OMSetRenderTargets(1, &rtv[0], FALSE, &dsv);
    l->RSSetScissorRects(1, &all);
    l->SetPipelineState(masked_pso.Get());
    l->SetGraphicsRoot32BitConstants(0, 4, text, 0);
    l->DrawInstanced(3, 1, 0, 0); // the text: only where the mask is
    REQUIRE(SUCCEEDED(l->Close()));
    d.queue->ExecuteCommandLists(1, lists);
    d.WaitIdle();
    CHECK(dev->GetDeviceRemovedReason() == S_OK);
    const nh::Stats s3 = nh::GetStats();
    nh::Layer layer3;
    const bool got3 = nh::TakeFrame(proxy.Get(), RW, RH, OW, OH, &layer3);
    std::printf("native HUD mask frame: layer=%d %s\n", got3 ? 1 : 0, nh::StatusLine().c_str());
    CHECK(got3);
    CHECK(s3.masks - s2.masks == 1);
    CHECK(s3.redirected - s2.redirected == 1);
    CHECK(s3.restore_failed == s2.restore_failed);
    REQUIRE(got3 && layer3.n0);
    REQUIRE(readback(d, layer3.n0, RTS, 4, px));
    CHECK(std::abs(at(px, OW, 30, 15, 0) - 204) <= 1 && std::abs(at(px, OW, 30, 15, 1) - 102) <= 1); // inside the mask (native 12,6-60,30)
    CHECK(at(px, OW, 80, 40, 0) == 0 && at(px, OW, 5, 3, 0) == 0);                                   // outside: nothing
    REQUIRE(readback(d, proxy.Get(), RTS, 4, px));
    CHECK(std::abs(at(px, RW, 50, 25, 0) - 51) <= 1); // the proxy keeps the picture (the text went native)

    // H1b: the game composes its picture and HUD in an intermediate target X and hands it to the
    // back buffer with one CopyResource. X is learned from that structure (3 frames with no HUD
    // in the back buffer itself), then its HUD draws go native and the copy carries the scene only.
    {
        auto proxy2 = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        auto x = test::CreateTexture(dev, RW, RH, F, RTS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        REQUIRE(proxy2 && x);
        D3D12_DESCRIPTOR_HEAP_DESC xd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ComPtr<ID3D12DescriptorHeap> xh;
        REQUIRE(SUCCEEDED(dev->CreateDescriptorHeap(&xd, IID_PPV_ARGS(&xh))));
        const D3D12_CPU_DESCRIPTOR_HANDLE xrtv = xh->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(x.Get(), nullptr, xrtv);
        nh::Layer lx;
        CHECK(!nh::TakeFrame(proxy2.Get(), RW, RH, OW, OH, &lx)); // learns the back buffer
        const Hud& h = kHud[0]; // "over" blend, 8,4-40,20
        bool got_last = false;
        nh::Layer last{};
        for (int frame = 0; frame < 4; ++frame) {
            REQUIRE(SUCCEEDED(l->Reset(game.alloc.Get(), nullptr)));
            l->SetGraphicsRootSignature(root.Get());
            l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            l->OMSetRenderTargets(1, &xrtv, FALSE, nullptr);
            l->RSSetViewports(1, &full);
            l->RSSetScissorRects(1, &all);
            l->SetPipelineState(plain[0].Get());
            l->SetGraphicsRoot32BitConstants(0, 4, kScene, 0);
            l->DrawInstanced(3, 1, 0, 0); // scene composite into X
            const D3D12_RECT hr{h.rect[0], h.rect[1], h.rect[2], h.rect[3]};
            l->RSSetScissorRects(1, &hr);
            l->SetPipelineState(plain[int(h.blend)].Get());
            l->SetGraphicsRoot32BitConstants(0, 4, h.color, 0);
            l->DrawInstanced(3, 1, 0, 0); // HUD into X
            D3D12_RESOURCE_BARRIER tb[2]{};
            for (int i = 0; i < 2; ++i) {
                tb[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            tb[0].Transition.pResource = x.Get(); tb[0].Transition.StateBefore = RTS; tb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            tb[1].Transition.pResource = proxy2.Get(); tb[1].Transition.StateBefore = RTS; tb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            l->ResourceBarrier(2, tb);
            l->CopyResource(proxy2.Get(), x.Get()); // the hand-off
            for (auto& t : tb) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
            l->ResourceBarrier(2, tb);
            REQUIRE(SUCCEEDED(l->Close()));
            d.queue->ExecuteCommandLists(1, lists);
            d.WaitIdle();
            CHECK(dev->GetDeviceRemovedReason() == S_OK);
            const bool got_up = nh::TakeFrame(proxy2.Get(), RW, RH, OW, OH, &lx);
            std::printf("native HUD upstream frame %d: layer=%d %s\n", frame, got_up ? 1 : 0, nh::StatusLine().c_str());
            if (frame < 3) CHECK(!got_up);
            else { got_last = got_up; last = lx; }
        }
        const nh::Stats su = nh::GetStats();
        CHECK(su.upstream_learned == 1);
        CHECK(su.upstream_now == 1);
        CHECK(su.upstream_frames == 1);
        CHECK(got_last);
        REQUIRE(got_last && last.n0);
        REQUIRE(readback(d, last.n0, RTS, 4, px));
        CHECK(std::abs(at(px, OW, 36, 18, 0) - 153) <= 1);  // the HUD, native, over black (render 24,12)
        CHECK(at(px, OW, 80, 40, 0) == 0);                    // outside it: nothing
        REQUIRE(readback(d, proxy2.Get(), RTS, 4, px));
        CHECK(std::abs(at(px, RW, 24, 12, 0) - 51) <= 1);   // the back buffer got the scene only
    }
    if(source_lineage(d)||draw_handoff(d)||draw_handoff(d,true))return 1;
    for(int resize=0;resize<5;++resize)if(draw_handoff(d,true,true))return 1;
    CHECK(nh::GetStats().upstream_dropped>0); // obsolete canvas profiles cannot fill all eight discovery slots
    na_observe_stop();
    return 0;
}
} // namespace

int main()
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    helpers();
    char why[1024] = {};
    const bool compiled = na::j2::CompileHudShadersForTests(why, sizeof why);
    if (!compiled) std::printf("HUD shaders: %s\n", why);
    CHECK(compiled);
    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("j2_native_hud_test");
    ComPtr<ID3D12InfoQueue> info;d.device->QueryInterface(IID_PPV_ARGS(&info));
    if (jitter_evidence(d)) return 1;
    if (classify(d)) return 1;
    if (picture_paths(d)) return 1;
    if (native_picture_phases(d)) return 1;
    if (canvas_probe(d)) return 1;
    if (end_to_end(d)) return 1;
    if (native_capture(d)) return 1;
    if(info){UINT errors=0;for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
        SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<uint8_t> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if(SUCCEEDED(info->GetMessage(i,message,&bytes))&&message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::printf("DX12 ERROR: %s\n",message->pDescription);}
    }CHECK(errors==0);}
    // The native layers are observed objects held by statics: like the game host at exit,
    // retirement must not run against the observer's already-destroyed tables.
    na::observe::SetProcessExiting();
    return test::finish("j2_native_hud_test");
}
