#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D BloomTex : register(t0);
Texture2D HDRTex   : register(t1);

cbuffer UserCB : register(b2)
{
    float4 PostFlareParams; // x threshold, y intensity, z ghost spacing, w hex sharpness.
};

float demo64k_hex_mask(float2 p, float sharpness)
{
    p = abs(p);
    float h = max(p.x * 0.866025 + p.y * 0.5, p.y);
    return pow(saturate(1.0 - h), max(sharpness, 0.05));
}

float3 demo64k_threshold(float3 c, float threshold)
{
    float l = lt_luminance(c);
    return c * saturate((l - threshold) / max(l, 1e-4));
}

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float2 center = float2(0.5, 0.5);
    float2 fromCenter = uv - center;
    float threshold = max(PostFlareParams.x, 0.001);
    float intensity = max(PostFlareParams.y, 0.0);
    float spacing = PostFlareParams.z;
    float sharpness = PostFlareParams.w;

    float3 flare = 0.0.xxx;
    [unroll]
    for (int g = 0; g < 5; ++g) {
        float f = (g - 2.0) * spacing;
        float2 guv = center - fromCenter * f;
        float2 gp = (guv - center) * float2(1.0, 1.25);
        float hex = demo64k_hex_mask(gp * (2.1 + g * 0.42), sharpness);
        float3 tint = lerp(float3(0.65, 0.82, 1.0), float3(1.0, 0.62, 0.34), g / 4.0);
        float3 s = demo64k_threshold(BloomTex.SampleLevel(LinearSampler, saturate(guv), 0).rgb, threshold);
        flare += s * tint * hex * (0.25 / (1.0 + abs(f)));
    }

    float2 texel = demo64k_texel_size(BloomTex);
    float3 streak = 0.0.xxx;
    [unroll]
    for (int x = -8; x <= 8; ++x) {
        float w = exp(-abs((float)x) * 0.35);
        streak += demo64k_threshold(BloomTex.SampleLevel(LinearSampler, float2(saturate(uv.x + x * texel.x * 5.0), uv.y), 0).rgb, threshold) * w;
    }
    streak *= 0.018 * float3(0.58, 0.78, 1.0);

    float3 hdr_hot = demo64k_threshold(HDRTex.SampleLevel(LinearSampler, uv, 0).rgb, threshold * 1.55);
    float localHex = demo64k_hex_mask((uv - center) * float2(2.4, 2.9), sharpness) * 0.025;
    flare += hdr_hot * localHex;
    flare += streak;
    return float4(flare * intensity, 1.0);
}
