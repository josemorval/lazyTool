// Equirectangular skybox shader. Drawn as background/environment using a texture
// sampled in direction space rather than mesh UVs.
/*
Sample 01: PBR helmet with an equirectangular skybox.

This shader draws the background from a regular HDR texture stored in
latitude/longitude (also called equirectangular) layout. The goal is to keep
the sample self-contained and readable:

1. No cubemap conversion step is required.
2. The shader only needs the shared SceneCB and the environment texture at t5.
3. The fullscreen triangle comes from a regular mesh resource, so the project
   file stays simple and easy to inspect.

References
----------
- "Moving Frostbite to Physically Based Rendering" (Lagarde, de Rousiers)
- "Real Shading in Unreal Engine 4" (Brian Karis)
- LearnOpenGL's environment mapping and HDR chapters for the equirectangular
  parameterization
*/

cbuffer SceneCB : register(b0)
{
    float4x4 ViewProj;
    float4 TimeVec;
    float4 LightDir;
    float4 LightColor;
    float4 CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4x4 PrevShadowViewProj;
    float4 CamDir;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

Texture2D EnvMap : register(t5);
SamplerState LinearSampler : register(s0);

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 pos      : SV_POSITION;
    float3 worldDir : TEXCOORD0;
};

static const float PI = 3.14159265359;
static const float TWO_PI = 6.28318530718;

float3 aces_fitted(float3 color)
{
    // ACES-like fit from Narkowicz is compact and works well for samples where
    // we want HDR data to remain visually plausible without adding a separate
    // post-process stack to the project.
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float2 direction_to_latlong_uv(float3 dir)
{
    dir = normalize(dir);

    // Longitude wraps around the horizon.
    float longitude = atan2(dir.z, dir.x);

    // Latitude runs from top (north pole) to bottom (south pole).
    float latitude = acos(clamp(dir.y, -1.0, 1.0));

    return float2(longitude / TWO_PI + 0.5, latitude / PI);
}

VSOut VSMain(VSIn v)
{
    VSOut o;

    // The fullscreen triangle mesh already stores clip-space positions.
    o.pos = float4(v.pos.xy, 0.9999, 1.0);

    // Reconstruct a point on the far plane, then derive the world-space ray
    // direction from the camera origin.
    float4 worldFar = mul(InvViewProj, float4(v.pos.xy, 1.0, 1.0));
    worldFar.xyz /= max(abs(worldFar.w), 1e-5);
    o.worldDir = normalize(worldFar.xyz - CamPos.xyz);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = direction_to_latlong_uv(i.worldDir);
    float3 hdr = EnvMap.SampleLevel(LinearSampler, uv, 0.0).rgb;

    // The scene render target is UNORM, so we tone map and manually encode for
    // display instead of relying on an sRGB swap chain.
    float3 mapped = aces_fitted(hdr);
    float3 display = pow(mapped, 1.0 / 2.2);
    return float4(display, 1.0);
}
