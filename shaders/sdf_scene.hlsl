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

cbuffer SdfCB:register(b1) { float TimeScale; float3 BallColor; float Smoothness; };

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
VSOut VSMain(VSIn v) { VSOut o; o.pos=float4(v.pos.xy,0,1); o.uv=v.uv; return o; }
float sd_sphere(float3 p,float r){return length(p)-r;} float sd_box(float3 p,float3 b){float3 q=abs(p)-b;return length(max(q,0))+min(max(q.x,max(q.y,q.z)),0);} float sd_torus(float3 p,float2 t){float2 q=float2(length(p.xz)-t.x,p.y);return length(q)-t.y;}
float smin(float a,float b,float k){float h=saturate(0.5+0.5*(b-a)/k);return lerp(b,a,h)-k*h*(1-h);}
float map_scene(float3 p){float ts=TimeScale>0.001?TimeScale:0.5; float t=TimeVec.x*ts; float sm=Smoothness>0.001?Smoothness:0.35; float wob=sin(t)*0.65; float d1=sd_sphere(p-float3(wob,0,0),0.9); float d2=sd_box(p-float3(-wob,0.05,0),float3(0.65,0.65,0.65)); float d=smin(d1,d2,sm); float tr=sd_torus(p-float3(0,-0.1,0),float2(1.35,0.045)); return min(d,tr);}
float3 normal_at(float3 p){float2 e=float2(0.001,0);return normalize(float3(map_scene(p+e.xyy)-map_scene(p-e.xyy),map_scene(p+e.yxy)-map_scene(p-e.yxy),map_scene(p+e.yyx)-map_scene(p-e.yyx)));}
float softshadow(float3 ro,float3 rd){float res=1,t=0.04; [loop] for(int i=0;i<32;i++){float h=map_scene(ro+rd*t); res=min(res,8*h/t); t+=clamp(h,0.02,0.25); if(res<0.02||t>8)break;} return saturate(res);}
float4 PSMain(VSOut i):SV_Target { float2 ndc=i.uv*float2(2,-2)+float2(-1,1); float4 farp=mul(InvViewProj,float4(ndc,1,1)); float3 ro=CamPos.xyz; float3 rd=normalize(farp.xyz/max(farp.w,0.0001)-ro); float t=0,hit=0; float3 p=ro; [loop] for(int s=0;s<80;++s){p=ro+rd*t; float d=map_scene(p); if(d<0.001){hit=1;break;} if(t>35)break; t+=d;} if(hit<0.5){float3 sky=lerp(float3(0.035,0.045,0.065),float3(0.18,0.22,0.30),saturate(rd.y*0.5+0.5)); return float4(sky,1);} float3 base=dot(BallColor,BallColor)>0.001?BallColor:float3(1,0.32,0.18); float3 n=normal_at(p); float3 l=normalize(-LightDir.xyz); float ndl=saturate(dot(n,l)); float sh=softshadow(p+n*0.02,l); float fres=pow(1-saturate(dot(n,-rd)),3); float3 col=base*(0.12+LightColor.rgb*LightDir.w*ndl*sh)+fres*0.45; return float4(col,1); }
