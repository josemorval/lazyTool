#ifndef DEMO64K_SKY_COMMON_HLSL
#define DEMO64K_SKY_COMMON_HLSL

#include "../common.hlsl"

float demo64k_sky_hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

float demo64k_sky_starfield(float3 rd)
{
    // Very cheap procedural stars; stable in world direction and only visible in the upper sky.
    float gate = smoothstep(0.03, 0.32, rd.y);
    float3 cell = floor(rd * 420.0 + float3(17.0, 43.0, 91.0));
    float h = demo64k_sky_hash31(cell);
    float star = smoothstep(0.9965, 1.0, h);
    float sparkle = 0.65 + 0.35 * sin(TimeVec.x * 1.7 + h * 64.0);
    return star * sparkle * gate;
}

float3 demo64k_sky_radiance(float3 rd, float4 skyParams, float4 skyTint, float3 sunDir, float3 sunColor)
{
    rd = lt_safe_normalize(rd);
    sunDir = lt_safe_normalize(sunDir);

    float up = saturate(rd.y * 0.5 + 0.5);
    float horizon = exp2(-abs(rd.y) * 7.5);
    float down = saturate(-rd.y);

    float3 zenith = float3(0.010, 0.018, 0.040) * skyTint.rgb;
    float3 mid    = float3(0.020, 0.026, 0.045) * skyTint.rgb;
    float3 low    = float3(0.010, 0.008, 0.014) * skyTint.rgb;
    float3 sky = lerp(low, lerp(mid, zenith, pow(up, 1.6)), smoothstep(0.0, 1.0, up));

    float sunDot = saturate(dot(rd, sunDir));
    float diskPower = lerp(320.0, 1900.0, saturate(skyParams.y));
    float sunDisk = pow(sunDot, diskPower) * (2.0 + skyParams.y * 10.0);
    float sunGlow = pow(sunDot, 24.0) * 0.22 + pow(sunDot, 5.5) * 0.045;
    float3 warmHorizon = sunColor * horizon * skyParams.z * (0.10 + 0.32 * pow(sunDot, 3.0));

    float stars = demo64k_sky_starfield(rd) * skyParams.w;
    sky += warmHorizon;
    sky += sunColor * (sunDisk + sunGlow) * skyParams.y;
    sky += float3(0.55, 0.70, 1.0) * stars;
    sky *= lerp(1.0, 0.18, down);
    return max(sky * skyParams.x, 0.0);
}

float3 demo64k_sky_ambient_diffuse(float3 n, float4 skyParams, float4 skyTint, float3 sunDir, float3 sunColor)
{
    // Compact procedural irradiance approximation: three sky probes plus a warm horizon term.
    float hemi = saturate(n.y * 0.5 + 0.5);
    float3 upCol = demo64k_sky_radiance(float3(0.0, 1.0, 0.0), skyParams, skyTint, sunDir, sunColor);
    float3 sideCol = demo64k_sky_radiance(lt_safe_normalize(float3(n.x, 0.18, n.z)), skyParams, skyTint, sunDir, sunColor);
    float3 downCol = float3(0.010, 0.009, 0.008) * skyTint.rgb * skyParams.x;
    return lerp(downCol, lerp(sideCol, upCol, hemi), hemi) * 0.72;
}

float3 demo64k_sky_ambient_specular(float3 r, float roughness, float4 skyParams, float4 skyTint, float3 sunDir, float3 sunColor)
{
    // Roughness-aware approximation to prefiltered IBL without cubemaps/textures.
    float3 sharp = demo64k_sky_radiance(r, skyParams, skyTint, sunDir, sunColor);
    float3 soft  = demo64k_sky_ambient_diffuse(r, skyParams, skyTint, sunDir, sunColor);
    float gloss = saturate(1.0 - roughness);
    return lerp(soft, sharp, gloss * gloss);
}

#endif
