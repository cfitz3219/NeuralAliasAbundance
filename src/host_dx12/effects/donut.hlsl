// Adapted from NVIDIA Donut histogram/exposure/tone mapping and paired Gaussian
// Bloom. Copyright (c) 2014-2021 NVIDIA CORPORATION. MIT; see third_party/donut.
// Filmic alternative adapted from Falcor (c) 2015-23 NVIDIA, BSD-3-Clause;
// see third_party/falcor. DX12 compute, bounded subrects and encoding are ours.
Texture2D<float4> Source : register(t0);
Texture2D<float4> BlurSource : register(t1);
RWTexture2D<float4> Destination : register(u0);
RWBuffer<uint> HistogramData : register(u1);
RWBuffer<uint> ExposureData : register(u2);
SamplerState LinearClamp : register(s0);
cbuffer Parameters : register(b0) {
    uint2 size; uint2 origin;
    uint2 blurSize; uint2 outputOrigin;
    uint srgb; uint tone; uint bloom; uint curve;
    float amount; float exposureBias; float white; float seconds;
    float speedUp; float speedDown; float low; float high;
    float minLuma; float maxLuma; float bloomAmount; float sigma;
    float threshold; uint reset; uint horizontal; uint padding;
};
float3 decode(float3 c) { return srgb ? select(c<=.04045,c/12.92,pow(max((c+.055)/1.055,0),2.4)) : c; }
float3 encode(float3 c) { return srgb ? select(c<=.0031308,c*12.92,1.055*pow(max(c,0),1/2.4)-.055) : c; }
float3 clean(float3 c) { return select(isfinite(c),c,0); }
float luminance(float3 c) { return dot(max(clean(c),0),float3(.2126,.7152,.0722)); }
[numthreads(256,1,1)] void Clear(uint i:SV_DispatchThreadID) { HistogramData[i]=0; }
groupshared uint bins[256];
[numthreads(16,16,1)] void Histogram(uint i:SV_GroupIndex,uint2 p:SV_DispatchThreadID) {
    bins[i]=0; GroupMemoryBarrierWithGroupSync();
    // One sample per 4x4 area: bounded cost; histogram weights remain Donut's 6-bit fixed point.
    p=p*4+2;
    if(all(p<size)) {
        float lum=luminance(decode(Source[p+origin].rgb));
        float bin=saturate((log2(max(lum,exp2(-10.0)))+10)/14)*255;
        uint left=(uint)floor(bin), right=left+1, rw=(uint)(frac(bin)*64);
        InterlockedAdd(bins[left],64-rw);
        if(rw && right<256) InterlockedAdd(bins[right],rw);
    }
    GroupMemoryBarrierWithGroupSync();
    if(bins[i]) InterlockedAdd(HistogramData[i],bins[i]);
}
[numthreads(1,1,1)] void Exposure() {
    float count=0; for(uint i=0;i<256;++i) count+=HistogramData[i]/64.0;
    float lc=count*low,hc=count*high,cdf=0,weighted=0,total=0;
    for(uint j=0;j<256;++j) {
        float n=HistogramData[j]/64.0;
        if(lc<=cdf+n && cdf<=hc) { weighted+=exp2(j/256.0*14-10)*n;total+=n; }
        cdf+=n;
    }
    float target=clamp(total>0?weighted/total:0,minLuma,maxLuma);
    if(!reset) {
        float old=asfloat(ExposureData[0]);
        float diff=old-target;
        float speed=diff<0?speedUp:speedDown;
        if(isfinite(old)&&old>0&&speed>0) target+=diff*exp2(-seconds*speed);
    }
    ExposureData[0]=asuint(target);
}
[numthreads(8,8,1)] void Downsample(uint2 p:SV_DispatchThreadID) {
    if(any(p>=blurSize))return;
    float3 c=0;uint n=0;
    for(uint y=0;y<4;++y)for(uint x=0;x<4;++x) {
        uint2 q=p*4+uint2(x,y);if(any(q>=size))continue;
        float3 v=max(clean(decode(Source[q+origin].rgb)),0);
        // Bright extraction is our optional wrapper, before the original Donut blur.
        float l=luminance(v);c+=v*(max(l-threshold,0)/max(l,1e-6));++n;
    }
    Destination[p]=float4(c/max(n,1),1);
}
[numthreads(8,8,1)] void Blur(uint2 p:SV_DispatchThreadID) {
    if(any(p>=blurSize))return;
    float2 uv=(p+.5)/blurSize;
    float2 step=horizontal?float2(1.0/blurSize.x,0):float2(0,1.0/blurSize.y);
    float3 result=BlurSource[p].rgb;
    float norm=1;
    float arg=-1/(2*sigma*sigma);
    for(float x=1;x<round(sigma*4);x+=2) {
        float w1=exp(x*x*arg),w2=exp((x+1)*(x+1)*arg),w=w1+w2;
        float2 offset=step*(x+w2/w);
        result+=(BlurSource.SampleLevel(LinearClamp,uv+offset,0).rgb+BlurSource.SampleLevel(LinearClamp,uv-offset,0).rgb)*w;
        norm+=2*w;
    }
    // Normalize the finite kernel exactly: constant images remain constant.
    Destination[p]=float4(result/norm,1);
}
#include "../../../third_party/falcor/filmic.hlsli"
[numthreads(8,8,1)] void Compose(uint2 p:SV_DispatchThreadID) {
    if(any(p>=size))return;
    float4 original=Source[p+origin];float3 c=clean(decode(original.rgb));
    if(bloom)c+=BlurSource.SampleLevel(LinearClamp,(p+.5)/size,0).rgb*bloomAmount;
    if(tone) {
        float l=luminance(c), adapted=max(asfloat(ExposureData[0]),minLuma);
        float scale=exp2(exposureBias)/adapted;
        float sl=l*scale;
        float mapped=(sl*(1+sl/(white*white)))/(1+sl);
        float3 mappedColor=curve?toneMapAces(max(c,0)*scale):max(c,0)*(l>0?mapped/l:0);
        // Keep signed HDR components and alpha; no SDR clamp/output transfer is introduced.
        c=lerp(c,mappedColor+min(c,0),amount);
    }
    Destination[p+outputOrigin]=float4(encode(c),original.a);
}
