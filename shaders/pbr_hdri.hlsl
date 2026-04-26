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

cbuffer PBRMaterialCB:register(b1) { float MetallicScale; float RoughnessScale; float EnvIntensity; float Exposure; float EnvRotation; float DiffuseMip; float SpecularMipScale; float NormalStrength; float3 BaseColorTint; float EmissiveStrength; };
Texture2D TexBaseColor:register(t0); Texture2D TexMetalRough:register(t1); Texture2D TexNormal:register(t2); Texture2D TexEmissive:register(t3); Texture2D TexOcclusion:register(t4); Texture2D EnvMap:register(t5); Texture2D<float> ShadowMap:register(t7); SamplerState SmpLinear:register(s0); SamplerComparisonState SmpShadow:register(s1);

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};
 struct VSOut{float4 pos:SV_POSITION;float3 nor:NORMAL;float2 uv:TEXCOORD0;float3 wpos:TEXCOORD1;float4 spos:TEXCOORD2;}; static const float PI=3.14159265; static const float INV_PI=0.31830988618; static const float INV_TWO_PI=0.15915494309;
float pd(float v,float d){return abs(v)>0.00001?v:d;} float3 pd3(float3 v,float3 d){return dot(v,v)>0.00001?v:d;} float3 ry(float3 v,float a){float s=sin(a),c=cos(a);return float3(v.x*c-v.z*s,v.y,v.x*s+v.z*c);} float2 latlong(float3 d){d=normalize(d);return float2(atan2(d.z,d.x)*INV_TWO_PI+0.5,0.5-asin(clamp(d.y,-1,1))*INV_PI);} float3 env(float3 d,float rot,float mip){return EnvMap.SampleLevel(SmpLinear,latlong(ry(d,rot)),max(mip,0)).rgb;}
float shadow_visibility(float4 spos,float ndl){float3 p=spos.xyz/max(spos.w,0.0001);float2 uv=p.xy*float2(0.5,-0.5)+0.5;if(any(uv<0)||any(uv>1)||p.z<=0||p.z>=1)return 1;uint w,h;ShadowMap.GetDimensions(w,h);float2 tx=1.0/float2(max(w,1),max(h,1));float bias=max(0.00025,0.0015*(1-ndl));float v=0;[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)v+=ShadowMap.SampleCmpLevelZero(SmpShadow,uv+float2(x,y)*tx,p.z-bias);return v/9.0;}
float3 FSchlick(float c,float3 f0){return f0+(1-f0)*pow(saturate(1-c),5);} float DGGX(float3 n,float3 h,float r){float a=r*r,a2=a*a;float nh=saturate(dot(n,h));float d=nh*nh*(a2-1)+1;return a2/max(PI*d*d,0.0001);} float G1(float nv,float r){float k=(r+1)*(r+1)/8;return nv/max(nv*(1-k)+k,0.0001);}
float3 nmap(float3 n,float3 wpos,float2 uv,float strength){float3 smp=TexNormal.Sample(SmpLinear,uv).xyz;if(dot(smp,smp)<0.0001)return normalize(n);float3 tn=normalize(smp*2-1);tn.xy*=strength;float3 dp1=ddx(wpos),dp2=ddy(wpos);float2 duv1=ddx(uv),duv2=ddy(uv);float3 t=normalize(dp1*duv2.y-dp2*duv1.y);float3 b=normalize(-dp1*duv2.x+dp2*duv1.x);return normalize(mul(tn,float3x3(t,b,normalize(n))));} float3 aces(float3 x){return (x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14);}
VSOut VSMain(VSIn v){VSOut o;float4 w=mul(World,float4(v.pos,1));o.wpos=w.xyz;o.pos=mul(ViewProj,w);o.nor=normalize(mul(World,float4(v.nor,0)).xyz);o.uv=v.uv;o.spos=mul(ShadowViewProj,w);return o;}
float4 PSMain(VSOut i):SV_Target{float metallic_scale=pd(MetallicScale,1), rough_scale=pd(RoughnessScale,1), envI=pd(EnvIntensity,1), exposure=pd(Exposure,1), nstr=pd(NormalStrength,1); float3 tint=pd3(BaseColorTint,float3(1,1,1)); float3 base=TexBaseColor.Sample(SmpLinear,i.uv).rgb;if(dot(base,base)<0.00001)base=float3(0.72,0.72,0.72);float3 albedo=pow(max(base,0.001),2.2)*tint;float4 orm=TexMetalRough.Sample(SmpLinear,i.uv);float metallic=saturate((dot(orm.rgb,orm.rgb)>0.00001?orm.b:0)*metallic_scale);float rough=saturate((dot(orm.rgb,orm.rgb)>0.00001?orm.g:0.65)*rough_scale);rough=clamp(rough,0.045,1);float occl=TexOcclusion.Sample(SmpLinear,i.uv).r;if(occl<=0.00001)occl=dot(orm.rgb,orm.rgb)>0.00001?orm.r:1;float3 emiss=TexEmissive.Sample(SmpLinear,i.uv).rgb;emiss=dot(emiss,emiss)>0.00001?pow(max(emiss,0.001),2.2)*pd(EmissiveStrength,1):0;float3 n=nmap(i.nor,i.wpos,i.uv,nstr);float3 v=normalize(CamPos.xyz-i.wpos);float3 l=normalize(-LightDir.xyz);float3 h=normalize(v+l);float ndl=saturate(dot(n,l)), ndv=saturate(dot(n,v));float sh=shadow_visibility(i.spos,ndl);float3 f0=lerp(float3(0.04,0.04,0.04),albedo,metallic);float3 F=FSchlick(saturate(dot(h,v)),f0);float D=DGGX(n,h,rough);float G=G1(ndv,rough)*G1(ndl,rough);float3 spec=(D*G*F)/max(4*ndv*ndl,0.001);float3 kd=(1-F)*(1-metallic);float3 direct=(kd*albedo/PI+spec)*LightColor.rgb*LightDir.w*ndl*sh;float diffMip=pd(DiffuseMip,5);float specMip=pd(SpecularMipScale,7)*rough;float3 ambient=(env(n,EnvRotation,diffMip)*albedo*kd + env(reflect(-v,n),EnvRotation,specMip)*FSchlick(ndv,f0))*occl*envI;float3 color=direct+ambient+emiss; color=aces(color*exposure);color=pow(saturate(color),1.0/2.2);return float4(color,1);}
