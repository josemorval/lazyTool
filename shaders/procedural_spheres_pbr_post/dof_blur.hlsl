// Depth-of-field gather pass. Uses a golden-angle sample pattern to approximate
// bokeh while rejecting depth-discontinuous samples.
#include "common.hlsl"

/*
    Depth of field gather blur.

    A golden-angle kernel approximates a circular bokeh disk. Signed CoC and a
    lightweight depth gate keep silhouettes from bleeding into the sky/floor
    discontinuities too aggressively.
*/

Texture2D SourceTex : register(t0);
Texture2D CoCTex    : register(t1);
Texture2D DepthTex  : register(t2);

cbuffer UserCB : register(b2)
{
    float4 DoFParams; // x max radius in pixels, y bokeh bias, z blend gain, w unused.
};

PostVSOut VSMain(VSIn v)
{
    return make_fullscreen_vertex(v);
}

float4 PSMain(PostVSOut i) : SV_Target
{
    uint w = 0;
    uint h = 0;
    SourceTex.GetDimensions(w, h);
    float2 texel = 1.0 / float2((float)w, (float)h);

    float3 sharp = SourceTex.SampleLevel(LinearSampler, i.uv, 0).rgb;
    float center_signed_coc = CoCTex.SampleLevel(LinearSampler, i.uv, 0).r;
    float center_coc = saturate(abs(center_signed_coc));
    float radius_px = center_coc * max(DoFParams.x, 0.0);
    if (radius_px < 0.35)
        return float4(sharp, 1.0);

    float center_depth = DepthTex.SampleLevel(LinearSampler, i.uv, 0).r;
    float center_view = 1e6;
    if (center_depth < 0.99995)
        center_view = view_depth_from_world(reconstruct_world_from_depth(i.uv, center_depth));

    const int sample_count = 56;
    float3 sum = sharp * 1.35;
    float weight_sum = 1.35;
    float rotation = hash21(floor(i.uv * float2((float)w, (float)h)) * 0.125) * 6.2831853;

    [unroll]
    for (int s = 0; s < sample_count; s++)
    {
        float u = ((float)s + 0.5) / (float)sample_count;
        float r = sqrt(u) * radius_px;
        float a = rotation + (float)s * GOLDEN_ANGLE;
        float2 offset = float2(cos(a), sin(a)) * r * texel;
        float2 uv = i.uv + offset;

        float sample_signed_coc = CoCTex.SampleLevel(LinearSampler, uv, 0).r;
        float sample_coc = saturate(abs(sample_signed_coc));
        float3 c = SourceTex.SampleLevel(LinearSampler, uv, 0).rgb;
        float aperture_weight = pow(u, max(DoFParams.y, 0.2));
        float coc_weight = lerp(0.25, 1.0, max(center_coc, sample_coc));
        float side_weight = (center_signed_coc * sample_signed_coc < -0.0001) ? 0.45 : 1.0;

        float sample_depth = DepthTex.SampleLevel(LinearSampler, uv, 0).r;
        float depth_weight = 1.0;
        if (sample_depth < 0.99995 && center_depth < 0.99995)
        {
            float sample_view = view_depth_from_world(reconstruct_world_from_depth(uv, sample_depth));
            float view_delta = sample_view - center_view;

            // Background blur should not pull nearer foreground silhouettes
            // far into the background. Foreground blur can still gather from
            // behind, but with a softer contribution.
            if (center_signed_coc > 0.0)
                depth_weight = smoothstep(-0.10, 0.32, view_delta);
            else if (center_signed_coc < 0.0)
                depth_weight = lerp(0.55, 1.0, smoothstep(-0.30, 0.80, view_delta));
        }

        float wgt = aperture_weight * coc_weight;
        wgt *= side_weight * depth_weight;
        sum += c * wgt;
        weight_sum += wgt;
    }

    float3 blurred = sum / max(weight_sum, 1e-4);
    float blend = smoothstep(0.02, 1.0, center_coc * max(DoFParams.z, 0.0));
    return float4(lerp(sharp, blurred, blend), 1.0);
}
