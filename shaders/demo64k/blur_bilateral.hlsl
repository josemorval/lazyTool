#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D InputAOTex         : register(t0);
Texture2D SceneDepthTex      : register(t1);
Texture2D NormalRoughnessTex : register(t2);

cbuffer UserCB : register(b2)
{
    float4 PostAOParams; // z bias also acts as a small depth tolerance helper.
    float2 BlurDirection; // (1,0) then (0,1) for separable AO denoise.
};

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float2 texel = demo64k_texel_size(InputAOTex) * BlurDirection;
    float center_depth01 = SceneDepthTex.SampleLevel(LinearSampler, uv, 0).r;
    if (center_depth01 >= 0.9999)
        return float4(1, 1, 1, 1);

    float3 center_p = lt_scene_depth_to_world(uv, center_depth01);
    float center_vd = lt_view_depth_from_world(center_p);
    float3 center_n = lt_decode_normal_rgb(NormalRoughnessTex.SampleLevel(LinearSampler, uv, 0));

    float sum = 0.0;
    float wsum = 0.0;
    [unroll]
    for (int k = -4; k <= 4; ++k) {
        float2 suv = saturate(uv + texel * k);
        float ao = InputAOTex.SampleLevel(LinearSampler, suv, 0).r;
        float d01 = SceneDepthTex.SampleLevel(LinearSampler, suv, 0).r;
        float3 n = lt_decode_normal_rgb(NormalRoughnessTex.SampleLevel(LinearSampler, suv, 0));

        float sample_vd = center_vd;
        if (d01 < 0.9999)
            sample_vd = lt_scene_depth_to_view_depth(suv, d01);

        float spatial = exp(-float(k * k) * 0.42);
        float normal_w = pow(saturate(dot(center_n, n)), 10.0);
        float depth_scale = lerp(5.0, 0.85, saturate(center_vd / 24.0));
        float depth_w = exp(-abs(sample_vd - center_vd) * depth_scale);
        float w = spatial * normal_w * depth_w;
        sum += ao * w;
        wsum += w;
    }

    float out_ao = sum / max(wsum, 1e-4);
    return float4(out_ao, out_ao, out_ao, 1.0);
}
