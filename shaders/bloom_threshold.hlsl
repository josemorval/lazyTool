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

cbuffer BloomCB : register(b1) { float Threshold; };
Texture2D<float4> Source:register(t0); RWTexture2D<float4> Dest:register(u0); SamplerState SmpLinear:register(s0);
[numthreads(8,8,1)] void CSMain(uint3 did:SV_DispatchThreadID) { uint w,h; Dest.GetDimensions(w,h); if(did.x>=w||did.y>=h) return; float thr=Threshold>0.001?Threshold:1.0; float2 uv=(float2(did.xy)+0.5)/float2(w,h); float3 c=Source.SampleLevel(SmpLinear,uv,0).rgb; float lum=dot(c,float3(0.2126,0.7152,0.0722)); float knee=saturate((lum-thr)/max(thr,0.001)); Dest[did.xy]=float4(c*knee,1); }
