#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D InputTex : register(t0);

cbuffer UserCB : register(b2)
{
    float2 BlurDirection; // Direction and radius scale, e.g. (1.25,0) or (0,2.25).
};

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float2 texel = demo64k_texel_size(InputTex) * BlurDirection;
    float3 c = InputTex.SampleLevel(LinearSampler, uv, 0).rgb * 0.204164;
    c += InputTex.SampleLevel(LinearSampler, uv + texel * 1.0, 0).rgb * 0.180174;
    c += InputTex.SampleLevel(LinearSampler, uv - texel * 1.0, 0).rgb * 0.180174;
    c += InputTex.SampleLevel(LinearSampler, uv + texel * 2.0, 0).rgb * 0.123832;
    c += InputTex.SampleLevel(LinearSampler, uv - texel * 2.0, 0).rgb * 0.123832;
    c += InputTex.SampleLevel(LinearSampler, uv + texel * 3.0, 0).rgb * 0.066282;
    c += InputTex.SampleLevel(LinearSampler, uv - texel * 3.0, 0).rgb * 0.066282;
    c += InputTex.SampleLevel(LinearSampler, uv + texel * 4.0, 0).rgb * 0.027630;
    c += InputTex.SampleLevel(LinearSampler, uv - texel * 4.0, 0).rgb * 0.027630;
    return float4(max(c, 0.0), 1.0);
}
