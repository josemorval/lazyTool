#ifndef LAZYTOOL_COMMON_PBR_HLSL
#define LAZYTOOL_COMMON_PBR_HLSL

#ifndef LAZYTOOL_COMMON_HLSL
#include "common.hlsl"
#endif

// -----------------------------------------------------------------------------
// lazyTool PBR helpers
// -----------------------------------------------------------------------------
// Conventions:
// - All colors are expected to be linear unless the function name says sRGB.
// - N, V, L and R are world-space unit vectors.
// - roughness and metallic are normalized 0..1 values.
// - LightDir.xyz follows the engine SceneCB convention. Use -LightDir.xyz as the
//   direction from a shaded point toward the directional light.
//
// Related reading / model lineage:
// - Cook & Torrance, "A Reflectance Model for Computer Graphics" (1982):
//   classic microfacet reflection model using distribution, Fresnel and geometry
//   terms. https://dl.acm.org/doi/10.1145/357290.357293
// - Schlick, "An Inexpensive BRDF Model for Physically-based Rendering" (1994):
//   low-cost Fresnel approximation used heavily in realtime renderers.
//   https://doi.org/10.1111/1467-8659.1330233
// - Brent Burley, "Physically-Based Shading at Disney" (SIGGRAPH 2012):
//   artist-friendly physically based material workflow and roughness conventions.
//   https://disneyanimation.com/publications/physically-based-shading-at-disney/
// - Brian Karis, "Real Shading in Unreal Engine 4" (SIGGRAPH 2013): practical
//   realtime GGX/Trowbridge-Reitz, Smith/Schlick geometry and split-sum IBL ideas.
//   https://blog.selfshadow.com/publications/s2013-shading-course/
//
// These helpers are educational lazyTool glue around common realtime PBR formulas;
// they are not copied verbatim from the references above.
// -----------------------------------------------------------------------------

struct LTPBRMaterial
{
    float3 base_color;
    float  roughness;
    float  metallic;
    float3 emissive;
    float  occlusion;
};

struct LTPBRLighting
{
    float3 diffuse_irradiance;
    float3 specular_radiance;
};

float3 lt_srgb_to_linear(float3 c)
{
    return pow(saturate(c), float3(2.2, 2.2, 2.2));
}

float3 lt_linear_to_srgb(float3 c)
{
    return pow(max(c, 0.0), float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
}

LTPBRMaterial lt_pbr_material(float3 base_color, float roughness, float metallic)
{
    LTPBRMaterial m;
    m.base_color = max(base_color, 0.0);
    m.roughness = clamp(roughness, 0.035, 1.0);
    m.metallic = saturate(metallic);
    m.emissive = float3(0.0, 0.0, 0.0);
    m.occlusion = 1.0;
    return m;
}

LTPBRMaterial lt_pbr_material(float3 base_color, float roughness, float metallic, float3 emissive, float occlusion)
{
    LTPBRMaterial m = lt_pbr_material(base_color, roughness, metallic);
    m.emissive = max(emissive, 0.0);
    m.occlusion = saturate(occlusion);
    return m;
}

float3 lt_pbr_f0(LTPBRMaterial m)
{
    return lerp(float3(0.04, 0.04, 0.04), m.base_color, m.metallic);
}

// Schlick Fresnel approximation: cheap, stable and good enough for most
// realtime dielectric/metallic workflows.
float3 lt_pbr_fresnel_schlick(float cos_theta, float3 f0)
{
    float f = pow(saturate(1.0 - cos_theta), 5.0);
    return f0 + (1.0 - f0) * f;
}

float3 lt_pbr_fresnel_schlick_roughness(float cos_theta, float3 f0, float roughness)
{
    float one_minus_roughness = saturate(1.0 - roughness);
    return f0 + (max(float3(one_minus_roughness, one_minus_roughness, one_minus_roughness), f0) - f0) * pow(saturate(1.0 - cos_theta), 5.0);
}

// GGX is the game-industry name commonly used for the Trowbridge-Reitz normal
// distribution. Its longer highlight tail tends to look more natural than
// older Blinn-Phong style lobes for rough materials.
float lt_pbr_distribution_ggx(float3 n, float3 h, float roughness)
{
    float a = max(roughness * roughness, 0.002);
    float a2 = a * a;
    float ndh = saturate(dot(n, h));
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(LT_PI * d * d, 1e-6);
}

// Schlick-GGX geometry term matched to Smith visibility, following the common
// UE4-style realtime approximation.
float lt_pbr_geometry_schlick_ggx(float ndv, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return ndv / max(ndv * (1.0 - k) + k, 1e-6);
}

float lt_pbr_geometry_smith(float3 n, float3 v, float3 l, float roughness)
{
    float ndv = saturate(dot(n, v));
    float ndl = saturate(dot(n, l));
    return lt_pbr_geometry_schlick_ggx(ndv, roughness) *
           lt_pbr_geometry_schlick_ggx(ndl, roughness);
}

float3 lt_pbr_brdf_direct(LTPBRMaterial m, float3 n, float3 v, float3 l, float3 radiance)
{
    n = lt_safe_normalize(n);
    v = lt_safe_normalize(v);
    l = lt_safe_normalize(l);

    float3 h = lt_safe_normalize(v + l);
    float ndl = saturate(dot(n, l));
    float ndv = saturate(dot(n, v));
    float hdv = saturate(dot(h, v));

    float3 f0 = lt_pbr_f0(m);
    float3 f = lt_pbr_fresnel_schlick(hdv, f0);
    float d = lt_pbr_distribution_ggx(n, h, m.roughness);
    float g = lt_pbr_geometry_smith(n, v, l, m.roughness);

    float3 specular = (d * g * f) / max(4.0 * ndv * ndl, 1e-5);
    float3 kd = (1.0 - f) * (1.0 - m.metallic);
    float3 diffuse = kd * m.base_color / LT_PI;
    return (diffuse + specular) * radiance * ndl;
}

// Texture-free IBL approximation. A production renderer would normally use
// a prefiltered environment map plus a BRDF integration LUT; this compact helper
// keeps examples useful even before an IBL asset pipeline exists.
float3 lt_pbr_ibl(LTPBRMaterial m, float3 n, float3 v,
                  float3 diffuse_irradiance, float3 specular_radiance)
{
    n = lt_safe_normalize(n);
    v = lt_safe_normalize(v);

    float ndv = saturate(dot(n, v));
    float3 f0 = lt_pbr_f0(m);
    float3 f = lt_pbr_fresnel_schlick_roughness(ndv, f0, m.roughness);
    float3 kd = (1.0 - f) * (1.0 - m.metallic);

    // Small BRDF-LUT-free approximation. It keeps the helper texture-free while
    // still making metal/roughness changes obvious in the editor.
    float gloss = saturate(1.0 - m.roughness);
    float specular_scale = lerp(0.22, 0.95, gloss * gloss);
    float3 diffuse = kd * m.base_color * diffuse_irradiance;
    float3 specular = f * specular_radiance * specular_scale;
    return diffuse * m.occlusion + specular * lerp(m.occlusion, 1.0, 0.65);
}

float3 lt_pbr_default_light_dir_ws()
{
    return lt_safe_normalize(-LightDir.xyz);
}

float3 lt_pbr_default_light_radiance()
{
    return max(LightColor.rgb, 0.0) * max(LightDir.w, 0.0);
}

float lt_pbr_default_shadow(float3 world_pos, float3 normal_ws, float3 light_dir_ws)
{
#ifndef LT_NO_DEFAULT_SHADOWMAP
    return lt_sample_shadow_pcf3x3(world_pos, normal_ws, light_dir_ws);
#else
    return 1.0;
#endif
}

float3 lt_pbr_default_directional(LTPBRMaterial m, float3 world_pos, float3 normal_ws, float3 view_dir_ws)
{
    float3 l = lt_pbr_default_light_dir_ws();
    float shadow = lt_pbr_default_shadow(world_pos, normal_ws, l);
    return lt_pbr_brdf_direct(m, normal_ws, view_dir_ws, l, lt_pbr_default_light_radiance() * shadow);
}

float3 lt_pbr_shade(LTPBRMaterial m, float3 world_pos, float3 normal_ws, float3 view_dir_ws,
                    float3 diffuse_irradiance, float3 specular_radiance)
{
    float3 direct = lt_pbr_default_directional(m, world_pos, normal_ws, view_dir_ws);
    float3 indirect = lt_pbr_ibl(m, normal_ws, view_dir_ws, diffuse_irradiance, specular_radiance);
    return direct + indirect + m.emissive;
}

float3 lt_pbr_unpack_normal(Texture2D normal_map, SamplerState samp, float2 uv,
                            float3 normal_ws, float3 tangent_ws, float tangent_sign)
{
    float3 n = lt_safe_normalize(normal_ws);
    float3 t = lt_safe_normalize(tangent_ws - n * dot(n, tangent_ws));
    float3 b = cross(n, t) * tangent_sign;
    float3 map_n = normal_map.Sample(samp, uv).xyz * 2.0 - 1.0;
    return lt_safe_normalize(t * map_n.x + b * map_n.y + n * map_n.z);
}

float3 lt_pbr_debug_material(LTPBRMaterial m)
{
    return float3(m.roughness, m.metallic, m.occlusion);
}

#endif
