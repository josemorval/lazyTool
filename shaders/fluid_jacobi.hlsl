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
Texture2D<float> Pressure:register(t0); Texture2D<float> Div:register(t1); RWTexture2D<float> PressureOut:register(u0); [numthreads(8,8,1)] void CSMain(uint3 did:SV_DispatchThreadID){uint w,h;PressureOut.GetDimensions(w,h);if(did.x>=w||did.y>=h)return;int2 p=int2(did.xy);int2 l=clamp(p+int2(-1,0),int2(0,0),int2(w-1,h-1));int2 r=clamp(p+int2(1,0),int2(0,0),int2(w-1,h-1));int2 b=clamp(p+int2(0,-1),int2(0,0),int2(w-1,h-1));int2 t=clamp(p+int2(0,1),int2(0,0),int2(w-1,h-1));float pr=Pressure.Load(int3(r,0))+Pressure.Load(int3(l,0))+Pressure.Load(int3(t,0))+Pressure.Load(int3(b,0));PressureOut[p]=(pr-Div.Load(int3(p,0)))*0.25;}
