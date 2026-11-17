cbuffer Colour : register(b0) { float4 colour; };
Texture2D<float4> image : register(t17);
RWByteAddressBuffer sideEffect : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),.5,1);}
float4 indexed(float2 p:POSITION):SV_Position{return float4(p,.5,1);}
float4 ps(float4 p:SV_Position):SV_Target{return colour;}
float4 copy(float4 p:SV_Position):SV_Target{return image.Load(int3(p.xy,0));}
float4 altered(float4 p:SV_Position):SV_Target{return image.Load(int3(p.xy,0))*.8;}
float4 uav(float4 p:SV_Position):SV_Target{sideEffect.Store(0,1);return colour;}
#if defined(HUD_DIRECT_HEAP)
float4 heap(float4 p:SV_Position):SV_Target{Texture2D<float4> t=ResourceDescriptorHeap[0];return t.Load(int3(p.xy,0));}

#endif
