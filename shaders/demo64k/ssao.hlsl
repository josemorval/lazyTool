#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D SceneDepthTex      : register(t0);
Texture2D NormalRoughnessTex : register(t1);

cbuffer UserCB : register(b2)
{
    float4 PostAOParams; // x radius, y intensity, z bias, w output power.
};

float demo64k_interleaved_gradient(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float2 demo64k_spiral_dir(int idx, float spin)
{
    float a = idx * 2.39996323 + spin;
    return float2(cos(a), sin(a));
}

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float depth01 = SceneDepthTex.SampleLevel(LinearSampler, uv, 0).r;
    if (depth01 >= 0.9999)
        return float4(1, 1, 1, 1);

    float3 p = lt_scene_depth_to_world(uv, depth01);
    float3 n = lt_decode_normal_rgb(NormalRoughnessTex.SampleLevel(LinearSampler, uv, 0));
    float view_depth = max(lt_view_depth_from_world(p), 0.25);

    float radius = max(PostAOParams.x, 0.03);
    float intensity = max(PostAOParams.y, 0.0);
    float bias = max(PostAOParams.z, 0.0);

    uint tw = 1, th = 1;
    SceneDepthTex.GetDimensions(tw, th);
    float2 pixel = uv * float2(tw, th);
    float spin = demo64k_interleaved_gradient(pixel) * LT_TWO_PI;
    float jitter = demo64k_interleaved_gradient(pixel.yx + 19.19);
    float uv_radius = saturate(radius / view_depth) * 0.62;

    float occ = 0.0;
    float wsum = 0.0;
    const int SAMPLE_COUNT = 20;
    [unroll]
    for (int s = 0; s < SAMPLE_COUNT; ++s) {
        float r = (s + 0.5 + jitter * 0.35) / SAMPLE_COUNT;
        r = r * r * (1.35 - 0.35 * r);
        float2 suv = uv + demo64k_spiral_dir(s, spin) * uv_radius * r;
        if (any(suv < 0.0) || any(suv > 1.0))
            continue;

        float sd = SceneDepthTex.SampleLevel(LinearSampler, suv, 0).r;
        if (sd >= 0.9999)
            continue;

        float3 sp = lt_scene_depth_to_world(suv, sd);
        float3 v = sp - p;
        float dist2 = dot(v, v);
        float dist = sqrt(max(dist2, 1e-5));
        float3 dir = v / dist;
        float range = saturate(1.0 - dist / radius);
        range = range * range * (3.0 - 2.0 * range);
        float facing = saturate(dot(n, dir) - bias);
        float falloff = range / (1.0 + dist2 * 1.2);
        occ += facing * falloff;
        wsum += falloff;
    }

    occ = occ / max(wsum, 1e-4);
    float ao = exp2(-occ * intensity * 2.8);
    ao = pow(saturate(ao), max(PostAOParams.w, 0.05));
    return float4(ao, ao, ao, 1.0);
}
