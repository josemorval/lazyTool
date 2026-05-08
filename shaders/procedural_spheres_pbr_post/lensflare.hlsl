// Procedural lens flare pass. It derives ghosts, halo and streaks directly from
// the bright scene image, avoiding any external dirt or flare textures.
#include "common.hlsl"

/*
    Procedural lens flare from bloom highlights.

    There is no external dirt texture. The pass combines thresholded scene
    highlights with a procedural sun response projected from the directional
    light, then the final composite adds it in HDR before tone mapping.
*/

Texture2D BloomTex : register(t0);
Texture2D SceneTex : register(t1);

cbuffer UserCB : register(b2)
{
    float4 LensParams; // x intensity, y ghost spacing, z halo size, w chromatic offset.
};

PostVSOut VSMain(VSIn v)
{
    return make_fullscreen_vertex(v);
}

float3 thresholded_source(float2 uv)
{
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0)
        return 0.0;

    float3 bloom = BloomTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float3 scene = SceneTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float b = max3(scene);
    float scene_gate = smoothstep(0.85, 2.25, b);
    return bloom * 1.35 + scene * scene_gate * 0.22;
}

float2 light_screen_uv(out float visibility)
{
    float3 sun_dir = -normalize(LightDir.xyz);
    float4 clip = mul(ViewProj, float4(CamPos.xyz + sun_dir * 55.0, 1.0));
    if (clip.w <= 1e-4)
    {
        visibility = 0.0;
        return 0.5;
    }

    float2 ndc = clip.xy / clip.w;
    float edge = max(abs(ndc.x), abs(ndc.y));
    visibility = saturate(1.35 - edge * 0.72) * saturate(LightDir.w);
    return float2(ndc.x * 0.5 + 0.5, ndc.y * -0.5 + 0.5);
}

float4 PSMain(PostVSOut i) : SV_Target
{
    uint w = 0;
    uint h = 0;
    SceneTex.GetDimensions(w, h);
    float2 aspect = float2((float)w / max((float)h, 1.0), 1.0);

    float2 center = float2(0.5, 0.5);
    float sun_visibility = 0.0;
    float2 sun_uv = light_screen_uv(sun_visibility);
    float2 sun_to_center = center - sun_uv;
    float dist_center = length((i.uv - center) * aspect);
    float falloff = saturate(1.10 - dist_center * 1.05);

    float3 flare = 0.0;
    const int ghosts = 6;

    [unroll]
    for (int sg = 0; sg < ghosts; sg++)
    {
        float t = ((float)sg + 1.0) / (float)ghosts;
        float2 ghost_uv = i.uv + (center - i.uv) * (t * LensParams.y + 0.18);
        float2 chroma = normalize(center - i.uv + 1e-4) * LensParams.w * (t - 0.5);
        float3 sample_rgb;
        sample_rgb.r = thresholded_source(ghost_uv + chroma).r;
        sample_rgb.g = thresholded_source(ghost_uv).g;
        sample_rgb.b = thresholded_source(ghost_uv - chroma).b;
        float ghost_weight = pow(1.0 - t * 0.72, 1.5) * falloff;
        flare += sample_rgb * ghost_weight;
    }

    float3 sun_color = LightColor.xyz * LightDir.w;
    [unroll]
    for (int g = 0; g < ghosts; g++)
    {
        float t = ((float)g + 1.0) / (float)ghosts;
        float2 ghost_pos = sun_uv + sun_to_center * (0.58 + t * LensParams.y);
        float size = lerp(0.030, 0.115, t) * max(LensParams.z, 0.1);
        float2 d = (i.uv - ghost_pos) * aspect;
        float sprite = exp(-dot(d, d) / max(size * size, 1e-5));
        float chroma_shift = LensParams.w * (t - 0.35);
        float3 tint = float3(1.0 + chroma_shift * 9.0, 0.84, 0.62 - chroma_shift * 6.0);
        flare += sun_color * tint * sprite * sun_visibility * pow(1.0 - t * 0.62, 1.7) * 0.18;
    }

    float2 sun_delta = (i.uv - sun_uv) * aspect;
    float sun_core = exp(-dot(sun_delta, sun_delta) / 0.0016);
    flare += sun_color * sun_core * sun_visibility * 0.28;

    float halo_radius = max(LensParams.z, 0.01);
    float2 halo_dir = normalize(i.uv - center + 1e-4);
    float2 halo_uv = center + halo_dir * halo_radius;
    float halo_shape = exp(-abs(dist_center - halo_radius) * 22.0) * falloff;
    flare += thresholded_source(halo_uv) * halo_shape * 0.18;
    flare += sun_color * halo_shape * sun_visibility * 0.045;

    // A tiny horizontal streak reads as glass response without needing a dirt map.
    float streak = exp(-abs(i.uv.y - 0.5) * 42.0) * falloff;
    flare += thresholded_source(float2(i.uv.x, 0.5)) * streak * 0.06;

    return float4(flare * LensParams.x, 1.0);
}
