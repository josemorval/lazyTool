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

Texture2D<float>       ShadowMap : register(t7);
SamplerComparisonState SmpShadow : register(s1);

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut { float4 pos:SV_POSITION; float3 nor:NORMAL; float3 wpos:TEXCOORD0; float4 spos:TEXCOORD1; };
VSOut VSMain(VSIn v) { VSOut o; float4 wpos=mul(World,float4(v.pos,1)); o.wpos=wpos.xyz; o.pos=mul(ViewProj,wpos); o.nor=normalize(mul(World,float4(v.nor,0)).xyz); o.spos=mul(ShadowViewProj,wpos); return o; }
float shadow_vis(float4 spos, float ndl) { float3 p=spos.xyz/max(spos.w,0.0001); float2 uv=p.xy*float2(0.5,-0.5)+0.5; if(any(uv<0)||any(uv>1)||p.z<=0||p.z>=1) return 1; float bias=max(0.00025,0.0015*(1-ndl)); return ShadowMap.SampleCmpLevelZero(SmpShadow,uv,p.z-bias); }
float4 PSMain(VSOut i):SV_Target { float3 n=normalize(i.nor); float3 l=normalize(-LightDir.xyz); float ndl=saturate(dot(n,l)); float vis=shadow_vis(i.spos,ndl); float grid=(step(0.96,frac(i.wpos.x))+step(0.96,frac(i.wpos.z)))*0.035; float3 base=float3(0.30,0.32,0.36)+grid; float3 col=base*(LightColor.rgb*LightDir.w*ndl*vis+float3(0.12,0.13,0.16)); return float4(col,1); }
