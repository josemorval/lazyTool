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
RWTexture2D<float2> Vel:register(u0); [numthreads(8,8,1)] void CSMain(uint3 did:SV_DispatchThreadID){uint w,h;Vel.GetDimensions(w,h);if(did.x>=w||did.y>=h)return;float2 uv=(float2(did.xy)+0.5)/float2(w,h);float2 c=0.5+0.18*float2(sin(TimeVec.x*0.7),cos(TimeVec.x*0.5));float2 d=uv-c;float r=Radius>0.001?Radius:0.18;float fall=saturate(1-length(d)/r);fall=fall*fall*(3-2*fall);float2 tangent=normalize(float2(-d.y,d.x)+0.001);float2 impulse=(tangent+float2(0.5,0.15))*fall*(Force>0.001?Force:1.4);Vel[did.xy]=Vel[did.xy]*0.995+impulse;}
