#include "common.hlsl"

/*
    Final HDR composite.

    Order of operations:
    1. Start from the DoF-resolved HDR scene.
    2. Apply blurred SSAO as a multiplicative grounding term.
    3. Add bloom and lens flare in HDR.
    4. Apply exposure, subtle vignette, ACES tone mapping and display gamma.
*/

Texture2D SceneTex : register(t0);
Texture2D BloomTex : register(t1);
Texture2D AOTex    : register(t2);
Texture2D FlareTex : register(t3);

cbuffer UserCB : register(b2)
{
    float4 CompositeParams; // x exposure, y bloom intensity, z lens flare intensity, w AO strength.
};

PostVSOut VSMain(VSIn v)
{
    return make_fullscreen_vertex(v);
}

float4 PSMain(PostVSOut i) : SV_Target
{
    float3 scene = SceneTex.SampleLevel(LinearSampler, i.uv, 0).rgb;
    float3 bloom = BloomTex.SampleLevel(LinearSampler, i.uv, 0).rgb * CompositeParams.y;
    float ao = AOTex.SampleLevel(LinearSampler, i.uv, 0).r;
    float3 flare = FlareTex.SampleLevel(LinearSampler, i.uv, 0).rgb * CompositeParams.z;

    float ao_term = lerp(1.0, ao, saturate(CompositeParams.w));
    float3 hdr = scene * ao_term + bloom + flare;

    float2 q = i.uv - 0.5;
    float vignette = 1.0 - smoothstep(0.20, 0.86, dot(q, q));
    hdr *= lerp(0.82, 1.0, vignette);
    hdr *= max(CompositeParams.x, 0.0);

    float3 mapped = aces_fitted(hdr);
    float3 display = pow(saturate(mapped), 1.0 / 2.2);

    // Very subtle procedural grain breaks up banding after tone mapping.
    uint w = 0;
    uint h = 0;
    SceneTex.GetDimensions(w, h);
    float grain = (hash21(i.uv * float2((float)w, (float)h) + TimeVec.xx * 61.0) - 0.5) * 0.0035;
    return float4(saturate(display + grain), 1.0);
}
