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

cbuffer SkyCB:register(b1) { float Exposure; float Rotation; float Intensity; float MipLevel; float3 Tint; float Saturation; };
Texture2D EnvMap:register(t0); SamplerState SmpLinear:register(s0);

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};
 struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;}; static const float INV_PI=0.31830988618; static const float INV_TWO_PI=0.15915494309;
float pd(float v,float d){return abs(v)>0.00001?v:d;} float3 pd3(float3 v,float3 d){return dot(v,v)>0.00001?v:d;} float3 ry(float3 v,float a){float s=sin(a),c=cos(a);return float3(v.x*c-v.z*s,v.y,v.x*s+v.z*c);} float2 latlong(float3 d){d=normalize(d);return float2(atan2(d.z,d.x)*INV_TWO_PI+0.5,0.5-asin(clamp(d.y,-1,1))*INV_PI);} float3 aces(float3 x){return (x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14);}
VSOut VSMain(VSIn v){VSOut o;o.pos=float4(v.pos.xy,0,1);o.uv=v.uv;return o;}
float4 PSMain(VSOut i):SV_Target{float2 ndc=float2(i.uv.x*2-1,(1-i.uv.y)*2-1); float4 w=mul(InvViewProj,float4(ndc,1,1)); float3 dir=normalize(w.xyz/max(w.w,0.0001)-CamPos.xyz); dir=ry(dir,Rotation); float3 tint=pd3(Tint,float3(1,1,1)); float exposure=pd(Exposure,1); float intensity=pd(Intensity,1); float sat=pd(Saturation,1); float3 color=EnvMap.SampleLevel(SmpLinear,latlong(dir),max(MipLevel,0)).rgb*tint*intensity; float luma=dot(color,float3(0.2126,0.7152,0.0722)); color=lerp(luma.xxx,color,sat); color=aces(color*exposure); color=pow(saturate(color),1.0/2.2); return float4(color,1);}
