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

cbuffer GodraysApplyCB:register(b1) { float Density; };
Texture2D<float> Rays:register(t0); RWTexture2D<float4> Dest:register(u0); SamplerState SmpLinear:register(s0);
[numthreads(8,8,1)] void CSMain(uint3 did:SV_DispatchThreadID){uint w,h;Dest.GetDimensions(w,h);if(did.x>=w||did.y>=h)return;float2 uv=(float2(did.xy)+0.5)/float2(w,h);float r=Rays.SampleLevel(SmpLinear,uv,0);float4 c=Dest[did.xy];float3 tint=LightColor.rgb*float3(1.0,0.86,0.62);c.rgb+=tint*r*2.2;Dest[did.xy]=c;}
