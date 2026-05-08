// Depth-aware blur used to clean SSAO while preserving edges. The axis/radius
// are controlled by UserCB so one shader handles horizontal and vertical passes.
#include "common.hlsl"

/*
    Separable bilateral blur.

    Used for SSAO.  A regular blur would wash contact occlusion across sphere
    silhouettes and checkerboard depth edges.  This pass weights samples by
    both spatial distance and reconstructed view-depth similarity.
*/

Texture2D SourceTex : register(t0);
Texture2D DepthTex  : register(t1);

cbuffer UserCB : register(b2)
{
    float4 BlurParams; // x axis: 1 horizontal / 0 vertical, y radius, z depth falloff, w unused.
};

PostVSOut VSMain(VSIn v)
{
    return make_fullscreen_vertex(v);
}

float sample_view_depth(float2 uv)
{
    float d = DepthTex.SampleLevel(LinearSampler, uv, 0).r;
    if (d >= 0.99995)
        return 1e6;
    return view_depth_from_world(reconstruct_world_from_depth(uv, d));
}

float4 PSMain(PostVSOut i) : SV_Target
{
    uint w = 0;
    uint h = 0;
    SourceTex.GetDimensions(w, h);
    float2 texel = 1.0 / float2((float)w, (float)h);
    float2 axis = (BlurParams.x > 0.5) ? float2(1.0, 0.0) : float2(0.0, 1.0);
    float radius = max(BlurParams.y, 0.5);
    float depth_falloff = max(BlurParams.z, 0.001);

    float center_v = sample_view_depth(i.uv);
    float sum = 0.0;
    float weight_sum = 0.0;

    [unroll]
    for (int k = -4; k <= 4; k++)
    {
        float fk = (float)k;
        float2 uv = i.uv + axis * texel * fk * radius;
        float spatial = exp(-0.5 * fk * fk / 5.0);
        float sv = sample_view_depth(uv);
        float depth_w = exp(-abs(sv - center_v) * depth_falloff);
        float wgt = spatial * depth_w;
        sum += SourceTex.SampleLevel(LinearSampler, uv, 0).r * wgt;
        weight_sum += wgt;
    }

    float ao = sum / max(weight_sum, 1e-4);
    return float4(ao, ao, ao, 1.0);
}
