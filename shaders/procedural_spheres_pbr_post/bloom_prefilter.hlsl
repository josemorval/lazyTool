// Bloom prefilter pass. It extracts bright HDR pixels with a soft knee before
// the separable blur chain.
#include "common.hlsl"

/*
    Bloom prefilter.

    This pass extracts bright HDR values and uses a soft knee around the
    threshold.  The soft knee prevents hard clipping and gives smoother bloom
    when highlights move across procedural sphere noise.
*/

Texture2D SourceTex : register(t0);

cbuffer UserCB : register(b2)
{
    float4 BloomParams; // x threshold, y knee, z prefilter gain, w clamp value.
};

PostVSOut VSMain(VSIn v)
{
    return make_fullscreen_vertex(v);
}

float4 PSMain(PostVSOut i) : SV_Target
{
    float3 c = SourceTex.SampleLevel(LinearSampler, i.uv, 0).rgb;
    c = min(c, BloomParams.www);

    float threshold = max(BloomParams.x, 0.001);
    float knee = max(BloomParams.y, 0.001);
    float brightness = max3(c);
    float soft = brightness - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / max(4.0 * knee, 1e-4);
    float contribution = max(soft, brightness - threshold) / max(brightness, 1e-4);

    return float4(c * contribution * BloomParams.z, 1.0);
}
