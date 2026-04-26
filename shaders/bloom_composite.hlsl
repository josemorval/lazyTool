cbuffer SceneCB : register(b0)
{
    float4x4 ViewProj;
    float4   TimeVec;
    float4   LightDir;
    float4   LightColor;
    float4   CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
};

cbuffer BloomCompositeCB : register(b1) { float Intensity; };
Texture2D<float4> Bloom:register(t0); RWTexture2D<float4> Dest:register(u0); SamplerState SmpLinear:register(s0);
float3 aces(float3 x) { return (x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14); }
[numthreads(8,8,1)] void CSMain(uint3 did:SV_DispatchThreadID) { uint w,h; Dest.GetDimensions(w,h); if(did.x>=w||did.y>=h) return; float intensity=Intensity>0.001?Intensity:0.6; float2 uv=(float2(did.xy)+0.5)/float2(w,h); float4 scene=Dest[did.xy]; float3 b=Bloom.SampleLevel(SmpLinear,uv,0).rgb; float3 col=scene.rgb+b*intensity; Dest[did.xy]=float4(col,scene.a); }
