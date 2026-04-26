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

struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
VSOut VSMain(VSIn v) { VSOut o; float4 wpos=mul(World,float4(v.pos,1)); o.pos=mul(ViewProj,wpos); o.uv=v.uv; return o; }
float line_grid(float2 uv, float scale) { float2 g=abs(frac(uv*scale)-0.5); float2 fw=max(fwidth(uv*scale),0.001); float l=1.0-min(min(g.x/fw.x,g.y/fw.y),1.0); return l; }
float4 PSMain(VSOut i):SV_Target { float2 uv=frac(i.uv); float grid=line_grid(uv,8.0); float3 c=float3(uv,0.18); c=lerp(c,float3(1,1,1),grid*0.85); return float4(c,1); }
