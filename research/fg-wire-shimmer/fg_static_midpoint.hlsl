// Offline 2x FG trial for linear FP16 scene colour. Dispatch before HUD composition.
Texture2D<float4> older : register(t0);
Texture2D<float4> newer : register(t1);
Texture2D<float2> motion : register(t2);
RWTexture2D<float4> generated : register(u0);
cbuffer Frame : register(b0) { uint width, height; };
groupshared float diff[28*28];
groupshared float h5[28*24];
groupshared float v5[24*24];
groupshared float h9[24*16];
float proxyLuma(float3 color) {
    return dot(sign(color)*pow(abs(color),1.0/2.2), float3(.2126,.7152,.0722));
}
[numthreads(16,16,1)]
void main(uint3 tile:SV_GroupID, uint3 tid:SV_GroupThreadID, uint lane:SV_GroupIndex) {
    uint2 p=tile.xy*16+tid.xy;
    uint mw,mh;
    motion.GetDimensions(mw,mh);
    int2 edge=int2(width,height)-1;
    for(uint i=lane;i<28*28;i+=256) {
        int2 samplePos=clamp(int2(tile.xy*16)+int2(i%28,i/28)-6,int2(0,0),edge);
        diff[i]=proxyLuma(older[samplePos].rgb)-proxyLuma(newer[samplePos].rgb);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<28*24;i+=256) {
        uint x=i%24,y=i/24;
        float sum=0;
        [unroll] for(uint k=0;k<5;k++) sum+=diff[y*28+x+k];
        h5[i]=sum*.2;
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<24*24;i+=256) {
        float sum=0;
        [unroll] for(uint k=0;k<5;k++) sum+=h5[i+k*24];
        v5[i]=abs(sum*.2);
    }
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<24*16;i+=256) {
        uint x=i%16,y=i/16;
        float sum=0;
        [unroll] for(uint k=0;k<9;k++) sum+=v5[y*24+x+k];
        h9[i]=sum/9;
    }
    GroupMemoryBarrierWithGroupSync();
    if(p.x>=width || p.y>=height) return;
    float disagreement=0;
    [unroll] for(uint k=0;k<9;k++) disagreement+=h9[(tid.y+k)*16+tid.x];
    disagreement*=255.0/9;
    uint2 mSize=uint2(mw,mh), size=uint2(width,height);
    uint2 at=min(p*mSize/size,mSize-1);
    float2 scale=float2(size)/float2(mSize);
    // Include adjacent motion so a thin moving object can veto the still-background rule.
    float speed=0;
    [unroll] for(int yy=-1;yy<=1;yy++) {
        [unroll] for(int xx=-1;xx<=1;xx++) {
            float2 v=motion[clamp(int2(at)+int2(xx,yy),int2(0,0),int2(mSize)-1)]*scale;
            speed=max(speed,all(isfinite(v))?length(v):1e6);
        }
    }
    float weight=saturate((.85-speed)/.55)*saturate((2.3-disagreement)/1.4);
    if(weight>0) {
        float4 g=generated[p];
        float3 mid=.5*(older[p].rgb+newer[p].rgb);
        generated[p]=float4(lerp(g.rgb,mid,weight),g.a);
    }
}
