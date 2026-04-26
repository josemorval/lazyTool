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
cbuffer FluidCB : register(b1) { float Viscosity; float Force; float Radius; float DisplayScale; };
Texture2D<float2> VelIn:register(t0); RWTexture2D<float2> VelOut:register(u0); SamplerState SmpLinear:register(s0); [numthreads(8,8,1)] void CSMain(uint3 did:SV_DispatchThreadID){uint w,h;VelOut.GetDimensions(w,h);if(did.x>=w||did.y>=h)return;float2 uv=(float2(did.xy)+0.5)/float2(w,h);float2 v=VelIn.SampleLevel(SmpLinear,uv,0);float dt=0.016;float2 prev=uv-v*dt;float visc=Viscosity>0.001?Viscosity:0.992;VelOut[did.xy]=VelIn.SampleLevel(SmpLinear,saturate(prev),0)*visc;}
