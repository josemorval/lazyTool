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

struct VSOut { float4 pos:SV_POSITION; float3 nor:NORMAL; };
VSOut VSMain(VSIn v) { VSOut o; float4 wpos=mul(World,float4(v.pos,1)); o.pos=mul(ViewProj,wpos); o.nor=normalize(mul(World,float4(v.nor,0)).xyz); return o; }
float4 PSMain(VSOut i):SV_Target { return float4(0.5+0.5*normalize(i.nor),1); }
