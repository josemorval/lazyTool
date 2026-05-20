#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D HDRSceneTex : register(t0);

cbuffer UserCB : register(b2)
{
    float4 PostBloomParams; // x threshold, y soft knee, z intensity, w wide-bloom blend.
};

float3 demo64k_bloom_curve(float3 c)
{
    float lum = lt_luminance(c);
    float threshold = max(PostBloomParams.x, 0.001);
    float knee = max(PostBloomParams.y, 0.001) * threshold;
    float soft = saturate((lum - threshold + knee) / max(2.0 * knee, 1e-4));
    soft = soft * soft * (3.0 - 2.0 * soft);
    float weight = saturate((lum - threshold + knee * soft) / max(lum, 1e-4));
    // Firefly guard keeps single HDR pixels from turning into hard blobs.
    c = min(c, 32.0.xxx);
    return max(c * weight, 0.0);
}

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float2 texel = demo64k_texel_size(HDRSceneTex);
    float3 c = HDRSceneTex.SampleLevel(LinearSampler, uv, 0).rgb * 0.50;
    c += HDRSceneTex.SampleLevel(LinearSampler, uv + texel * float2( 1.0, 0.0), 0).rgb * 0.125;
    c += HDRSceneTex.SampleLevel(LinearSampler, uv + texel * float2(-1.0, 0.0), 0).rgb * 0.125;
    c += HDRSceneTex.SampleLevel(LinearSampler, uv + texel * float2( 0.0, 1.0), 0).rgb * 0.125;
    c += HDRSceneTex.SampleLevel(LinearSampler, uv + texel * float2( 0.0,-1.0), 0).rgb * 0.125;
    return float4(demo64k_bloom_curve(c), 1.0);
}
