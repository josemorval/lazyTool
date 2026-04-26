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

cbuffer ObjectCB : register(b2)
{
    float4x4 World;
};

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut { float4 pos:SV_POSITION; float3 wpos:TEXCOORD0; };
VSOut VSMain(VSIn v) { VSOut o; float4 wpos=mul(World,float4(v.pos,1)); o.wpos=wpos.xyz; o.pos=mul(ViewProj,wpos); return o; }
float4 PSMain(VSOut i):SV_Target { float3 bands=frac(i.wpos*0.5); float3 axes=0.5+0.5*sin(i.wpos*3.14159); return float4(lerp(bands,axes,0.35),1); }
