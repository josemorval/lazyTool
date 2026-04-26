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
cbuffer VisualizeCB:register(b1){float DisplayScale;};Texture2D<float2> Vel:register(t0);SamplerState SmpLinear:register(s0);
struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};
struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};VSOut VSMain(VSIn v){VSOut o;o.pos=float4(v.pos.xy,0,1);o.uv=v.uv;return o;}float4 PSMain(VSOut i):SV_Target{float2 v=Vel.Sample(SmpLinear,i.uv);float s=DisplayScale>0.001?DisplayScale:3.5;float mag=saturate(length(v)*s);float ang=atan2(v.y,v.x);float3 col=0.5+0.5*cos(ang+float3(0,2.094,4.188));col=lerp(float3(0.015,0.02,0.035),col,mag);return float4(col,1);}
