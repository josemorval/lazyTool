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

cbuffer ParticleDrawCB:register(b1) { float ParticleScale; float Brightness; };
struct Particle { float3 pos; float life; float3 vel; float seed; }; StructuredBuffer<Particle> Particles:register(t0);

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};
 struct VSOut{float4 pos:SV_POSITION;float4 color:COLOR0;};
VSOut VSMain(VSIn v,uint iid:SV_InstanceID){VSOut o;Particle p=Particles[iid];float scale=ParticleScale>0.0001?ParticleScale:0.035;float alive=saturate(p.life);float3 wpos=p.pos+v.pos*scale*(0.5+alive);o.pos=mul(ViewProj,float4(wpos,1));float heat=saturate(alive*0.35+length(p.vel)*0.12);float b=Brightness>0.001?Brightness:1.2;o.color=float4(lerp(float3(1,0.2,0.06),float3(0.25,0.55,1),heat)*b,saturate(alive));return o;}
float4 PSMain(VSOut i):SV_Target{return i.color;}
