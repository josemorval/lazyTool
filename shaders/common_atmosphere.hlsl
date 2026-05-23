#ifndef LAZYTOOL_COMMON_ATMOSPHERE_HLSL
#define LAZYTOOL_COMMON_ATMOSPHERE_HLSL

#ifndef LAZYTOOL_COMMON_HLSL
#include "common.hlsl"
#endif

// -----------------------------------------------------------------------------
// lazyTool procedural atmosphere helpers
// -----------------------------------------------------------------------------
// This is a fast artistic atmosphere/fog/sky model, not a physically integrated
// Bruneton-style atmosphere. It is intended to be cheap, parameter-friendly and
// useful in editor prototypes, raymarch scenes and PBR examples.
//
// Related reading / model lineage:
// - Preetham, Shirley & Smits, "A Practical Analytic Model for Daylight"
//   (SIGGRAPH 1999): analytic sky and aerial perspective for daylight scenes.
//   https://dl.acm.org/doi/10.1145/311535.311545
// - Bruneton & Neyret, "Precomputed Atmospheric Scattering" (2008): realtime
//   atmosphere with Rayleigh/Mie multiple scattering and aerial perspective.
//   https://inria.hal.science/inria-00288758
// - Naty Hoffman & Arcot J. Preetham, "Rendering Outdoor Light Scattering in
//   Real Time" (GDC 2002): pragmatic realtime atmospheric scattering overview.
//   https://renderwonk.com/publications/gdc-2002/
// - Krzysztof Narkowicz, "ACES Filmic Tone Mapping Curve" (2016): compact
//   game-friendly ACES curve fit for HDR-to-LDR display.
//   https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
//
// This file is deliberately artistic rather than physically integrated. The
// references above are included to guide users toward deeper, production-grade
// sky/atmosphere models if they need them.
// -----------------------------------------------------------------------------

struct LTAtmosphereParams
{
    float  sky_intensity;
    float  sun_intensity;
    float  horizon_intensity;
    float  star_intensity;
    float  fog_density;
    float  fog_height_falloff;
    float  planet_radius;
    float3 sky_tint;
    float3 ground_color;
};

LTAtmosphereParams lt_atmosphere_default_params()
{
    LTAtmosphereParams p;
    p.sky_intensity = 1.0;
    p.sun_intensity = 1.0;
    p.horizon_intensity = 1.0;
    p.star_intensity = 0.0;
    p.fog_density = 0.015;
    p.fog_height_falloff = 0.10;
    p.planet_radius = 6371000.0;
    p.sky_tint = float3(1.0, 1.0, 1.0);
    p.ground_color = float3(0.018, 0.016, 0.014);
    return p;
}

float3 lt_atmosphere_default_sun_dir_ws()
{
    return lt_safe_normalize(-LightDir.xyz);
}

float3 lt_atmosphere_default_sun_color()
{
    return max(LightColor.rgb, 0.0) * max(LightDir.w, 0.0);
}

float lt_atmosphere_hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

// Tiny procedural star field for quick previews. This is an artistic hash, not
// an astronomical catalog or physically based night-sky model.
float lt_atmosphere_stars(float3 rd, float amount)
{
    float gate = smoothstep(0.04, 0.35, rd.y) * saturate(amount);
    float3 cell = floor(rd * 520.0 + float3(17.0, 43.0, 91.0));
    float h = lt_atmosphere_hash31(cell);
    float star = smoothstep(0.9968, 1.0, h);
    float sparkle = 0.70 + 0.30 * sin(TimeVec.x * 1.9 + h * 70.0);
    return star * sparkle * gate;
}

// Fast sky gradient plus sun disk/glow. It borrows the vocabulary of atmosphere
// rendering, but avoids full optical-depth integration to stay cheap and tunable.
float3 lt_atmosphere_sky(float3 rd, LTAtmosphereParams p, float3 sun_dir, float3 sun_color)
{
    rd = lt_safe_normalize(rd);
    sun_dir = lt_safe_normalize(sun_dir);

    float up01 = saturate(rd.y * 0.5 + 0.5);
    float horizon = exp2(-abs(rd.y) * 7.0);
    float down = saturate(-rd.y);
    float sun_dot = saturate(dot(rd, sun_dir));

    float3 zenith = float3(0.010, 0.020, 0.055) * p.sky_tint;
    float3 mid = float3(0.030, 0.045, 0.085) * p.sky_tint;
    float3 low = float3(0.010, 0.012, 0.020) * p.sky_tint;
    float3 sky = lerp(low, lerp(mid, zenith, pow(up01, 1.35)), smoothstep(0.0, 1.0, up01));

    float sun_disk = pow(sun_dot, 1400.0) * (3.0 + 8.0 * p.sun_intensity);
    float sun_glow = pow(sun_dot, 18.0) * 0.22 + pow(sun_dot, 4.5) * 0.040;
    float3 warm_horizon = sun_color * horizon * p.horizon_intensity * (0.10 + 0.26 * pow(sun_dot, 2.5));

    float stars = lt_atmosphere_stars(rd, p.star_intensity);
    sky += warm_horizon;
    sky += sun_color * (sun_disk + sun_glow) * p.sun_intensity;
    sky += float3(0.55, 0.70, 1.0) * stars;
    sky = lerp(sky, p.ground_color, down * down);
    return max(sky * p.sky_intensity, 0.0);
}

float3 lt_atmosphere_sky(float3 rd, LTAtmosphereParams p)
{
    return lt_atmosphere_sky(rd, p, lt_atmosphere_default_sun_dir_ws(), lt_atmosphere_default_sun_color());
}

// Hemispherical ambient approximation for PBR examples: use procedural sky
// colors as a lightweight diffuse irradiance substitute.
float3 lt_atmosphere_ambient_diffuse(float3 n, LTAtmosphereParams p, float3 sun_dir, float3 sun_color)
{
    n = lt_safe_normalize(n);
    float hemi = saturate(n.y * 0.5 + 0.5);
    float3 up_col = lt_atmosphere_sky(float3(0.0, 1.0, 0.0), p, sun_dir, sun_color);
    float3 side_col = lt_atmosphere_sky(lt_safe_normalize(float3(n.x, 0.18, n.z)), p, sun_dir, sun_color);
    float3 down_col = p.ground_color * p.sky_intensity;
    return lerp(down_col, lerp(side_col, up_col, hemi), hemi) * 0.72;
}

float3 lt_atmosphere_ambient_diffuse(float3 n, LTAtmosphereParams p)
{
    return lt_atmosphere_ambient_diffuse(n, p, lt_atmosphere_default_sun_dir_ws(), lt_atmosphere_default_sun_color());
}

// Specular environment approximation. Production IBL would prefilter a cubemap;
// here roughness simply blends between sharp sky lookup and soft ambient color.
float3 lt_atmosphere_ambient_specular(float3 r, float roughness, LTAtmosphereParams p, float3 sun_dir, float3 sun_color)
{
    float3 sharp = lt_atmosphere_sky(r, p, sun_dir, sun_color);
    float3 soft = lt_atmosphere_ambient_diffuse(r, p, sun_dir, sun_color);
    float gloss = saturate(1.0 - roughness);
    return lerp(soft, sharp, gloss * gloss);
}

float3 lt_atmosphere_ambient_specular(float3 r, float roughness, LTAtmosphereParams p)
{
    return lt_atmosphere_ambient_specular(r, roughness, p, lt_atmosphere_default_sun_dir_ws(), lt_atmosphere_default_sun_color());
}

// Height-based exponential fog / aerial-perspective helper. This gives users a
// single density and falloff control instead of requiring scattering coefficients.
float lt_atmosphere_fog_amount(float3 world_pos, float view_distance, LTAtmosphereParams p)
{
    float height = max(world_pos.y, 0.0);
    float height_term = exp2(-height * max(p.fog_height_falloff, 0.0));
    float density = max(p.fog_density, 0.0) * height_term;
    return saturate(1.0 - exp2(-density * max(view_distance, 0.0)));
}

float3 lt_atmosphere_fog_color(float3 ray_dir, LTAtmosphereParams p, float3 sun_dir, float3 sun_color)
{
    float3 sky = lt_atmosphere_sky(ray_dir, p, sun_dir, sun_color);
    float forward = pow(saturate(dot(lt_safe_normalize(ray_dir), lt_safe_normalize(sun_dir))), 8.0);
    return sky + sun_color * forward * 0.12 * p.sun_intensity;
}

float3 lt_atmosphere_apply_fog(float3 color, float3 world_pos, float3 ray_dir,
                               LTAtmosphereParams p, float3 sun_dir, float3 sun_color)
{
    float view_distance = length(world_pos - lt_camera_position_ws());
    float fog = lt_atmosphere_fog_amount(world_pos, view_distance, p);
    return lerp(color, lt_atmosphere_fog_color(ray_dir, p, sun_dir, sun_color), fog);
}

float3 lt_atmosphere_apply_fog(float3 color, float3 world_pos, float3 ray_dir, LTAtmosphereParams p)
{
    return lt_atmosphere_apply_fog(color, world_pos, ray_dir, p,
                                   lt_atmosphere_default_sun_dir_ws(),
                                   lt_atmosphere_default_sun_color());
}

// Applies the shared lazyTool ACES-style fitted curve after exposure. For strict
// color pipelines, replace this with your project's OCIO/ACES output transform.
float3 lt_atmosphere_tonemap(float3 hdr_color, float exposure)
{
    return lt_aces_fitted(hdr_color * max(exposure, 0.0));
}

#endif
