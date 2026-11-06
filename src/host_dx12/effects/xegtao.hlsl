// NeuralAlias binding/composition glue for pinned MIT Intel XeGTAO.

#define VA_PI 3.14159265358979323846
#define VA_SATURATE saturate
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
#define XE_GTAO_FP32_DEPTHS 1
#define XE_GTAO_USE_DEFAULT_CONSTANTS 0
#include "XeGTAO.h"
cbuffer Constants:register(b0) {
    GTAOConstants c;
    uint2 colorBase; uint2 depthBase;
    float amount; float clearDepth; uint quality; uint srgb;
    uint2 outputBase; uint finalApply; uint padding;
    float temporalBlend; float2 jitterDelta; float depthTolerance;
};
#include "XeGTAO.hlsli"
SamplerState pointClamp:register(s0);
#if defined(PASS_EXTRACT)
Texture2D<float> inputDepth:register(t0);
RWTexture2D<float> rawDepth:register(u0);
[numthreads(8,8,1)] void Extract(uint2 p:SV_DispatchThreadID) {
    if(any(p>=uint2(c.ViewportSize)))return;
    rawDepth[p]=inputDepth.Load(int3(p+depthBase,0));
}
#elif defined(PASS_PREFILTER)
Texture2D<float> rawDepth:register(t0);
RWTexture2D<float> mip0:register(u0); RWTexture2D<float> mip1:register(u1);
RWTexture2D<float> mip2:register(u2); RWTexture2D<float> mip3:register(u3);
RWTexture2D<float> mip4:register(u4);
[numthreads(8,8,1)] void Prefilter(uint2 p:SV_DispatchThreadID,uint2 t:SV_GroupThreadID) {
    XeGTAO_PrefilterDepths16x16(p,t,c,rawDepth,pointClamp,mip0,mip1,mip2,mip3,mip4);
}
#elif defined(PASS_NORMALS)
Texture2D<float> rawDepth:register(t0);
RWTexture2D<uint> normals:register(u0);
[numthreads(8,8,1)] void Normals(uint2 p:SV_DispatchThreadID) {
    if(any(p>=uint2(c.ViewportSize)))return;
    // A clamped edge neighbour coincides with the centre and has no direction.
    // Use the nearest interior normal at the outermost pixel instead of NaNs.
    uint2 q=clamp(p,1,uint2(c.ViewportSize)-2);
    float3 n=XeGTAO_ComputeViewspaceNormal(q,c,rawDepth,pointClamp);
    normals[p]=XeGTAO_FLOAT3_to_R11G11B10_UNORM(n*.5+.5);
}
#elif defined(PASS_MAIN)
Texture2D<float> viewDepth:register(t0); Texture2D<uint> normals:register(t1);
RWTexture2D<uint> ao:register(u0); RWTexture2D<unorm float> edges:register(u1);
[numthreads(8,8,1)] void Main(uint2 p:SV_DispatchThreadID) {
    if(any(p>=uint2(c.ViewportSize)))return;
    uint index=HilbertIndex(p.x,p.y); // fixed spatial samples: no new temporal noise
    float2 noise=frac(.5+index*float2(.7548776662466927,.5698402909980532));
    float3 n=normalize(XeGTAO_R11G11B10_UNORM_to_FLOAT3(normals[p])*2-1);
    XeGTAO_MainPass(p,quality==0?1:quality==1?2:quality==2?3:9,quality<2?2:3,noise,n,c,viewDepth,pointClamp,ao,edges);
}
#elif defined(PASS_DENOISE)
Texture2D<uint> ao:register(t0); Texture2D<float> edges:register(t1);
RWTexture2D<uint> filtered:register(u0);
[numthreads(8,8,1)] void Denoise(uint2 p:SV_DispatchThreadID) {
    // Intel's denoiser writes two horizontally adjacent pixels per thread.
    if(p.x*2>=uint(c.ViewportSize.x)||p.y>=uint(c.ViewportSize.y))return;
    XeGTAO_Denoise(uint2(p.x*2,p.y),c,ao,edges,pointClamp,filtered,finalApply!=0);
}
#elif defined(PASS_TEMPORAL)
// Audit 2026-11-06 (Cort: AO shimmers along edges while panning). The AO
// sample pattern is fixed in screen space and the depth is jittered, so a
// world point sees a different AO each frame. Keep a reprojected history:
// previous = current + motion, corrected by the jitter change; history taps
// whose view depth differs are rejected (disocclusion); the result is
// clamped to this frame's widened 3x3 range so moving shadows do not ghost.
Texture2D<uint> ao:register(t0); Texture2D<float> history:register(t1);
Texture2D<float> historyDepth:register(t2); Texture2D<float> viewDepth:register(t3);
Texture2D<float2> motion:register(t4);
RWTexture2D<float> visibilityOut:register(u0); RWTexture2D<float> depthOut:register(u1);
[numthreads(8,8,1)] void Temporal(uint2 p:SV_DispatchThreadID) {
    if(any(p>=uint2(c.ViewportSize)))return;
    const int2 size=int2(c.ViewportSize);
    float current=float(ao[p])/255.0;
    float z=viewDepth.Load(int3(p,0));
    depthOut[p]=z;
    float result=current;
    if(temporalBlend<1.0) {
        // Texel-centre space of the previous frame: its sample at t+0.5-jp
        // equals this pixel's p+0.5-jc+mv.
        float2 q=float2(p)+motion.Load(int3(p,0))+jitterDelta;
        // Same-surface history within one pixel of the reprojected point:
        // jitter moves a silhouette by up to a pixel, so the matching surface
        // can sit next to it. Taps with another view depth are rejected.
        int2 centre=int2(floor(q+.5));
        float sum=0,total=0;
        [unroll] for(int dy=-1;dy<=1;++dy)[unroll] for(int dx=-1;dx<=1;++dx) {
            int2 t=centre+int2(dx,dy);
            if(any(t<0)||any(t>=size))continue;
            float hz=historyDepth.Load(int3(t,0));
            if(!(abs(hz-z)<=depthTolerance*max(abs(z),1e-4)))continue;
            float wgt=max(0.,1.5-length(float2(t)-q));
            sum+=wgt*history.Load(int3(t,0));total+=wgt;
        }
        if(total>1e-3) {
            float lo=current,hi=current;
            [unroll] for(int y=-1;y<=1;++y)[unroll] for(int x=-1;x<=1;++x) {
                float v=float(ao[clamp(int2(p)+int2(x,y),0,size-1)])/255.0;lo=min(lo,v);hi=max(hi,v);
            }
            // Widened by half the range each side: measured on Cort's pan
            // capture, edge flicker -11% and >=32 swings -23% versus the plain
            // 3x3 clamp, with 13% more lag on moving shadows (no clamp: 32%).
            const float pad=.5*(hi-lo);
            result=lerp(clamp(sum/total,lo-pad,hi+pad),current,temporalBlend);
        }
    }
    visibilityOut[p]=result;
}
#elif defined(PASS_COMPOSE)
Texture2D<float4> image:register(t0); Texture2D<float> visibilityIn:register(t1);
Texture2D<float> rawDepth:register(t2); RWTexture2D<float4> outputColor:register(u0);
float3 LinearToSrgb(float3 v) {
    return float3(v.x<=.0031308?12.92*v.x:1.055*pow(max(v.x,0),1/2.4)-.055,
                  v.y<=.0031308?12.92*v.y:1.055*pow(max(v.y,0),1/2.4)-.055,
                  v.z<=.0031308?12.92*v.z:1.055*pow(max(v.z,0),1/2.4)-.055);
}
[numthreads(8,8,1)] void Compose(uint2 p:SV_DispatchThreadID) {
    if(any(p>=uint2(c.ViewportSize)))return;
    float4 color=image.Load(int3(p+colorBase,0));
    float visibility=saturate(visibilityIn[p]);
    // Clear sky contains no occluder: retain colour and alpha, including HDR.
    float weight=abs(rawDepth[p]-clearDepth)<1e-7?0:amount;
    color.rgb*=lerp(1.0,visibility,weight);
    if(srgb!=0)color.rgb=LinearToSrgb(color.rgb);
    outputColor[p+outputBase]=color;
}
#endif
