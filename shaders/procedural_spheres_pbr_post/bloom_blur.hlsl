#include "common.hlsl"

/*
    Separable bloom blur.

    The project runs this as a ping-pong pair inside a repeat node.  Repeating
    the pair widens the lobe while keeping each pass cheap and cache-friendly.
*/

Texture2D SourceTex : register(t0);

cbuffer UserCB : register(b2)
{
    float4 BlurParams; // x axis: 1 horizontal / 0 vertical, y radius scale, z unused, w unused.
};

PostVSOut VSMain(VSIn v)
{
    return make_fullscreen_vertex(v);
}

float3 sample_bloom(float2 uv)
{
    return SourceTex.SampleLevel(LinearSampler, uv, 0).rgb;
}

float4 PSMain(PostVSOut i) : SV_Target
{
    uint w = 0;
    uint h = 0;
    SourceTex.GetDimensions(w, h);
    float2 texel = 1.0 / float2((float)w, (float)h);
    float2 axis = (BlurParams.x > 0.5) ? float2(1.0, 0.0) : float2(0.0, 1.0);
    float2 step_uv = axis * texel * max(BlurParams.y, 0.25);

    // 9-tap Gaussian using optimized bilinear offsets.
    float3 c = sample_bloom(i.uv) * 0.2270270270;
    c += sample_bloom(i.uv + step_uv * 1.3846153846) * 0.3162162162;
    c += sample_bloom(i.uv - step_uv * 1.3846153846) * 0.3162162162;
    c += sample_bloom(i.uv + step_uv * 3.2307692308) * 0.0702702703;
    c += sample_bloom(i.uv - step_uv * 3.2307692308) * 0.0702702703;
    return float4(c, 1.0);
}
