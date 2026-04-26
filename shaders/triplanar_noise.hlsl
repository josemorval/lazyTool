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
cbuffer ProcCB:register(b1){float3 BaseColor;float NoiseScale;float3 AccentColor;float Contrast;float RimStrength;};Texture2D<float> ShadowMap:register(t7);SamplerComparisonState SmpShadow:register(s1);
struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};
struct VSOut{float4 pos:SV_POSITION;float3 nor:NORMAL;float3 wpos:TEXCOORD0;float4 spos:TEXCOORD1;};float hash(float3 p){p=frac(p*0.3183099+0.1);p*=17.0;return frac(p.x*p.y*p.z*(p.x+p.y+p.z));}float noise(float3 x){float3 i=floor(x),f=frac(x);f=f*f*(3-2*f);float n=lerp(lerp(lerp(hash(i+float3(0,0,0)),hash(i+float3(1,0,0)),f.x),lerp(hash(i+float3(0,1,0)),hash(i+float3(1,1,0)),f.x),f.y),lerp(lerp(hash(i+float3(0,0,1)),hash(i+float3(1,0,1)),f.x),lerp(hash(i+float3(0,1,1)),hash(i+float3(1,1,1)),f.x),f.y),f.z);return n;}float fbm(float3 p){float a=0.5,n=0;[unroll]for(int i=0;i<4;i++){n+=noise(p)*a;p=p*2.02+3.1;a*=0.5;}return n;}VSOut VSMain(VSIn v){VSOut o;float4 w=mul(World,float4(v.pos,1));o.wpos=w.xyz;o.pos=mul(ViewProj,w);o.nor=normalize(mul(World,float4(v.nor,0)).xyz);o.spos=mul(ShadowViewProj,w);return o;}float sh(float4 sp,float ndl){float3 p=sp.xyz/max(sp.w,0.0001);float2 uv=p.xy*float2(0.5,-0.5)+0.5;if(any(uv<0)||any(uv>1)||p.z<=0||p.z>=1)return 1;return ShadowMap.SampleCmpLevelZero(SmpShadow,uv,p.z-max(0.00025,0.0015*(1-ndl)));}float4 PSMain(VSOut i):SV_Target{float scale=NoiseScale>0.001?NoiseScale:3;float n=pow(saturate(fbm(i.wpos*scale+TimeVec.x*0.05)),Contrast>0.001?Contrast:1.4);float3 base=dot(BaseColor,BaseColor)>0.001?BaseColor:float3(0.12,0.34,0.72);float3 acc=dot(AccentColor,AccentColor)>0.001?AccentColor:float3(1,0.62,0.12);float3 color=lerp(base,acc,n);float3 nor=normalize(i.nor);float3 l=normalize(-LightDir.xyz);float3 v=normalize(CamPos.xyz-i.wpos);float ndl=saturate(dot(nor,l));float rim=pow(1-saturate(dot(nor,v)),3)*(RimStrength>0.001?RimStrength:0.6);float vis=sh(i.spos,ndl);color*=0.13+LightColor.rgb*LightDir.w*ndl*vis;color+=acc*rim;return float4(color,1);}
