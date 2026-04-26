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

cbuffer SsaoCB:register(b1) { float Radius; float Intensity; };
Texture2D<float> SceneDepth:register(t0); RWTexture2D<float> Dest:register(u0); SamplerState SmpLinear:register(s0);
float3 reconstruct_world(float2 uv,float depth) { float2 ndc=uv*float2(2,-2)+float2(-1,1); float4 clip=float4(ndc,depth,1); float4 w=mul(InvViewProj,clip); return w.xyz/max(w.w,0.0001); }
float hash12(float2 p) { p=frac(p*float2(123.34,456.21)); p+=dot(p,p+45.32); return frac(p.x*p.y); }
static const float3 KERNEL[12]={float3(0.5,0.5,0.5),float3(-0.5,0.5,0.5),float3(0.5,-0.5,0.5),float3(-0.5,-0.5,0.5),float3(0.9,0,0.25),float3(-0.9,0,0.25),float3(0,0.9,0.25),float3(0,-0.9,0.25),float3(0.35,0.15,0.9),float3(-0.2,0.4,0.75),float3(0.15,-0.35,0.8),float3(-0.45,-0.25,0.65)};
[numthreads(8,8,1)] void CSMain(uint3 did:SV_DispatchThreadID) { uint w,h; Dest.GetDimensions(w,h); if(did.x>=w||did.y>=h)return; float2 uv=(float2(did.xy)+0.5)/float2(w,h); float depth=SceneDepth.SampleLevel(SmpLinear,uv,0); if(depth>=0.99999){Dest[did.xy]=1;return;} float3 wpos=reconstruct_world(uv,depth); float radius=Radius>0.001?Radius:0.4; float jitter=hash12(float2(did.xy)+TimeVec.x); float occ=0; [unroll] for(int s=0;s<12;++s){ float3 sample_w=wpos+KERNEL[s]*radius*(0.35+0.65*frac(jitter+s*0.37)); float4 clip=mul(ViewProj,float4(sample_w,1)); clip/=max(clip.w,0.0001); float2 suv=clip.xy*float2(0.5,-0.5)+0.5; if(any(suv<0)||any(suv>1))continue; float refd=SceneDepth.SampleLevel(SmpLinear,suv,0); occ += refd < clip.z-0.0007 ? 1.0 : 0.0; } float ao=saturate(1.0-occ/12.0); Dest[did.xy]=ao; }
