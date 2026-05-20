#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D BloomHalfTex    : register(t0);
Texture2D BloomQuarterTex : register(t1);
Texture2D BloomEighthTex  : register(t2);

cbuffer UserCB : register(b2)
{
    float4 PostBloomParams; // w is used as wide bloom contribution.
};

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float wide = saturate(PostBloomParams.w);
    float3 h = BloomHalfTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float3 q = BloomQuarterTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float3 e = BloomEighthTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float3 c = h * 0.58 + q * (0.34 + 0.18 * wide) + e * (0.22 + 0.35 * wide);
    return float4(max(c, 0.0), 1.0);
}
