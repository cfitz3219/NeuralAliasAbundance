// Controlled layered scene, not an engine adapter. Motion is previous-current
// in render pixels, excluding jitter. Depth is normalized forward Z.
cbuffer Constants : register(b0) {
    uint width; uint height; uint frameIndex; uint cameraCut;
    float jitterX; float jitterY; float previousOffset; float offset;
};
RWTexture2D<float4> scene : register(u0);
RWTexture2D<float> depth : register(u1);
RWTexture2D<float2> motion : register(u2);
RWTexture2D<float4> composite : register(u3);
[numthreads(8,8,1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if(thread.x>=width || thread.y>=height) return;
    float2 p=float2(thread.xy)+0.5-float2(jitterX,jitterY);
    float2 uv=p/float2(width,height);
    bool object=abs(p.x-(width*0.5+offset))<width*0.125 && abs(p.y-height*0.5)<height*0.2;
    float checker=fmod(floor(p.x/8)+floor(p.y/8),2);
    float4 color=object?float4(0.9,0.15,0.08,1):float4(uv.x*0.3,0.15+checker*0.12,uv.y*0.4,1);
    scene[thread.xy]=color;
    depth[thread.xy]=object?0.25:0.9;
    motion[thread.xy]=object && !cameraCut?float2(previousOffset-offset,0):float2(0,0);
    // Deliberately separate HUD surface: white band must never enter clean scene.
    bool hud=thread.x<width/3 && thread.y<height/10;
    composite[thread.xy]=hud?float4(1,1,1,1):color;
}
