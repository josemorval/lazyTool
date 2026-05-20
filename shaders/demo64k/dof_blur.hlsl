#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D HDRSceneTex : register(t0);
Texture2D CoCTex      : register(t1);

cbuffer UserCB : register(b2)
{
    float4 PostDOFParams; // x focus distance, y aperture, z max blur pixels, w spare.
};

static const float2 kBokeh[24] = {
    float2( 0.000,  0.000),
    float2( 0.866,  0.500), float2( 0.500,  0.866), float2(-0.500,  0.866), float2(-0.866,  0.500), float2(-1.000,  0.000), float2(-0.866, -0.500), float2(-0.500, -0.866), float2( 0.500, -0.866), float2( 0.866, -0.500),
    float2( 1.732,  0.000), float2( 1.225,  1.225), float2( 0.000,  1.732), float2(-1.225,  1.225), float2(-1.732,  0.000), float2(-1.225, -1.225), float2( 0.000, -1.732), float2( 1.225, -1.225),
    float2( 0.360,  0.120), float2(-0.280,  0.320), float2( 0.180, -0.420), float2(-0.460, -0.100), float2( 0.620, -0.220), float2(-0.080,  0.640)
};

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float3 center = HDRSceneTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float3 cocPack = CoCTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float coc = cocPack.r;
    float nearCoc = cocPack.g;
    float2 texel = demo64k_texel_size(HDRSceneTex);
    float radius = coc * max(PostDOFParams.z, 0.0);

    float3 sum = center * 1.25;
    float wsum = 1.25;
    float nearBleed = 0.0;

    [unroll]
    for (int k = 1; k < 24; ++k) {
        float2 disk = kBokeh[k];
        float2 suv = saturate(uv + disk * texel * max(radius, 0.35));
        float3 scoc = CoCTex.SampleLevel(LinearSampler, suv, 0).rgb;
        float sampleCoc = scoc.r;
        float sampleNear = scoc.g;
        float tapRadius = length(disk) * 0.50;
        float bokehGate = smoothstep(tapRadius - 0.18, tapRadius + 0.42, max(coc, sampleCoc));
        float foreground = saturate(sampleNear - nearCoc * 0.35);
        float w = (0.30 + 0.90 * bokehGate + 0.75 * foreground) / (1.0 + tapRadius * 0.25);

        float3 s = HDRSceneTex.SampleLevel(LinearSampler, suv, 0).rgb;
        float highlight = saturate((lt_luminance(s) - 1.0) * 0.25) * sampleCoc;
        sum += s * w * (1.0 + highlight * 0.25);
        wsum += w;
        nearBleed = max(nearBleed, foreground);
    }

    float3 blurred = sum / max(wsum, 1e-4);
    float blurMix = smoothstep(0.025, 0.85, coc);
    blurMix = max(blurMix, nearBleed * 0.85);
    return float4(lerp(center, blurred, saturate(blurMix)), 1.0);
}
