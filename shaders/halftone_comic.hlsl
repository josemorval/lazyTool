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
cbuffer HalfToneCB:register(b1){float DotScale;float InkStrength;float Posterize;};Texture2D<float4> Source:register(t0);SamplerState SmpLinear:register(s0);
struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};
struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};VSOut VSMain(VSIn v){VSOut o;o.pos=float4(v.pos.xy,0,1);o.uv=v.uv;return o;}float4 PSMain(VSOut i):SV_Target{float scale=DotScale>1?DotScale:90;float ink=saturate(InkStrength>0.001?InkStrength:0.75);float steps=Posterize>1?Posterize:5;float3 c=Source.Sample(SmpLinear,i.uv).rgb;float l=dot(c,float3(0.2126,0.7152,0.0722));float2 cell=frac(i.uv*scale)-0.5;float dots=smoothstep(0.32,0.28,length(cell));float shade=round(l*steps)/steps;float line=smoothstep(0.018,0.0,min(abs(frac(i.uv.x*24)-0.5),abs(frac(i.uv.y*18)-0.5)));float3 poster=floor(c*steps)/steps;float3 inked=lerp(poster,poster*(shade+dots*0.35),ink);inked*=1-line*0.18;return float4(saturate(inked),1);}
