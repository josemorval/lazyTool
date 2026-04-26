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

cbuffer BlurCB : register(b1) { float BlurRadius; };
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Dest : register(u0);
SamplerState SmpLinear : register(s0);
static const float WEIGHTS[5] = { 0.227027, 0.194594, 0.121622, 0.054054, 0.016216 };
[numthreads(8,8,1)]
void CSMain(uint3 did:SV_DispatchThreadID)
{
    uint w,h; Dest.GetDimensions(w,h); if(did.x>=w||did.y>=h) return;
    uint sw,sh; Source.GetDimensions(sw,sh);
    float radius = max(1.0, abs(BlurRadius) > 0.001 ? BlurRadius : 3.0);
    float2 uv=(float2(did.xy)+0.5)/float2(w,h);
    float2 min_uv=0.5/float2(max(sw,1),max(sh,1));
    float2 max_uv=1.0-min_uv;
    float4 acc=Source.SampleLevel(SmpLinear,clamp(uv,min_uv,max_uv),0)*WEIGHTS[0];
    [unroll] for(int i=1;i<5;++i){
        float2 o=float2(0.0, float(i) * radius / max(sh, 1));
        acc += Source.SampleLevel(SmpLinear,clamp(uv+o,min_uv,max_uv),0)*WEIGHTS[i];
        acc += Source.SampleLevel(SmpLinear,clamp(uv-o,min_uv,max_uv),0)*WEIGHTS[i];
    }
    Dest[did.xy]=acc;
}
