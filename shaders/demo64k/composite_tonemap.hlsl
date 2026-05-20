#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D DOFSceneTex : register(t0);
Texture2D BloomTex    : register(t1);
Texture2D FlareTex    : register(t2);

cbuffer UserCB : register(b2)
{
    float4 PostBloomParams; // x threshold, y knee, z bloom intensity, w wide-bloom blend.
    float4 PostGradeParams; // x exposure, y contrast, z saturation, w vignette.
    float4 ColorGrade;      // rgb color grade multiplier, w chromatic aberration amount.
    float  GrainAmount;
};

float demo64k_hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 demo64k_apply_grade(float3 c, float2 uv)
{
    c *= exp2(PostGradeParams.x);
    c = max(c, 0.0);
    c = lt_aces_fitted(c);

    float lum = lt_luminance(c);
    c = lerp(float3(lum, lum, lum), c, max(PostGradeParams.z, 0.0));
    c = (c - 0.5) * max(PostGradeParams.y, 0.0) + 0.5;
    c *= ColorGrade.rgb;

    float2 v = uv * 2.0 - 1.0;
    float vig = smoothstep(1.20, 0.12, dot(v, v) * max(PostGradeParams.w, 0.0));
    c *= lerp(1.0, vig, saturate(PostGradeParams.w));

    float grain = demo64k_hash12(uv * 1733.0 + TimeVec.z * 19.13) - 0.5;
    c += grain * GrainAmount;
    return saturate(c);
}

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float2 radial = (uv - 0.5);
    float ca = ColorGrade.w * 0.00155;

    float3 base;
    base.r = DOFSceneTex.SampleLevel(LinearSampler, uv + radial * ca, 0).r;
    base.g = DOFSceneTex.SampleLevel(LinearSampler, uv, 0).g;
    base.b = DOFSceneTex.SampleLevel(LinearSampler, uv - radial * ca, 0).b;

    float3 bloom = BloomTex.SampleLevel(LinearSampler, uv, 0).rgb * PostBloomParams.z;
    float3 flare = FlareTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float3 color = base + bloom + flare;
    color = demo64k_apply_grade(color, uv);
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0);
}
