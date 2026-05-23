// Fullscreen raymarch + PBR + atmosphere example.
// Recommended command setup:
// - Command type: DrawMesh / procedural fullscreen triangle or quad.
// - Shader path: shaders/examples/raymarch_pbr_atmosphere.hlsl
// - No textures required.
//
// The shader defines LT_RAYMARCH_SCENE before including common_raymarch.hlsl so
// the generic trace/normal/AO helpers operate on this file's SDF scene.

float lt_example_scene_sdf(float3 p);
#define LT_RAYMARCH_SCENE(p) lt_example_scene_sdf(p)

#include "../common.hlsl"
#include "../common_pbr.hlsl"
#include "../common_raymarch.hlsl"
#include "../common_atmosphere.hlsl"

cbuffer UserCB : register(b2)
{
    // xyz = base color, w = exposure.
    float4 BaseColorExposure;

    // x = roughness, y = metallic, z = AO multiplier, w = unused.
    float4 Material;

    // xyz = sphere center, w = sphere radius.
    float4 Sphere;

    // x = max distance, y = epsilon, z = max steps, w = normal epsilon.
    float4 Raymarch;

    // x = sky intensity, y = sun intensity, z = horizon intensity, w = stars.
    float4 Sky;

    // x = fog density, y = fog height falloff, z/w = unused.
    float4 Fog;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vertex_id : SV_VertexID)
{
    uint i = min(vertex_id, 5u);
    float2 pos = float2(-1.0, -1.0);
    float2 uv = float2(0.0, 1.0);

    if (i == 1u) { pos = float2(-1.0,  1.0); uv = float2(0.0, 0.0); }
    if (i == 2u) { pos = float2( 1.0,  1.0); uv = float2(1.0, 0.0); }
    if (i == 3u) { pos = float2(-1.0, -1.0); uv = float2(0.0, 1.0); }
    if (i == 4u) { pos = float2( 1.0,  1.0); uv = float2(1.0, 0.0); }
    if (i == 5u) { pos = float2( 1.0, -1.0); uv = float2(1.0, 1.0); }

    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float lt_example_scene_sdf(float3 p)
{
    float sphere = lt_sdf_sphere(p - Sphere.xyz, (Sphere.w > 0.0 ? Sphere.w : 1.0));
    float floor_plane = lt_sdf_plane(p, float3(0.0, 1.0, 0.0), 1.15);
    return lt_sdf_smooth_union(sphere, floor_plane, 0.08);
}

LTAtmosphereParams lt_example_atmosphere()
{
    LTAtmosphereParams a = lt_atmosphere_default_params();
    a.sky_intensity = Sky.x > 0.0 ? Sky.x : 1.0;
    a.sun_intensity = Sky.y > 0.0 ? Sky.y : 1.0;
    a.horizon_intensity = Sky.z > 0.0 ? Sky.z : 1.0;
    a.star_intensity = max(Sky.w, 0.0);
    a.fog_density = Fog.x > 0.0 ? Fog.x : 0.015;
    a.fog_height_falloff = Fog.y > 0.0 ? Fog.y : 0.10;
    return a;
}

float4 PSMain(VSOut i) : SV_Target
{
    LTAtmosphereParams atmosphere = lt_example_atmosphere();
    float3 sun_dir = lt_atmosphere_default_sun_dir_ws();
    float3 sun_color = lt_atmosphere_default_sun_color();

    LTRay ray = lt_raymarch_camera_ray(i.uv);

    LTRaymarchParams params = lt_raymarch_default_params();
    params.max_t = Raymarch.x > 0.0 ? Raymarch.x : 120.0;
    params.epsilon = Raymarch.y > 0.0 ? Raymarch.y : 0.001;
    params.max_steps = Raymarch.z > 0.0 ? (int)Raymarch.z : 128;
    params.normal_epsilon = Raymarch.w > 0.0 ? Raymarch.w : 0.002;

    LTRaymarchHit hit = lt_raymarch_trace(ray, params);
    if (hit.hit == 0) {
        float3 sky = lt_atmosphere_sky(ray.dir, atmosphere, sun_dir, sun_color);
        float exposure = BaseColorExposure.w > 0.0 ? BaseColorExposure.w : 1.0;
        return float4(lt_atmosphere_tonemap(sky, exposure), 1.0);
    }

    float3 n = hit.normal;
    float3 v = lt_safe_normalize(lt_camera_position_ws() - hit.position);
    float3 r = reflect(-v, n);

    float ao_slider = Material.z > 0.0 ? Material.z : 1.0;
    float ao = lt_raymarch_ambient_occlusion(hit.position, n, 0.08, 5) * ao_slider;
    float soft_shadow = lt_raymarch_soft_shadow(hit.position + n * 0.015, sun_dir,
                                                0.03, 80.0, 18.0, params);

    float3 base_color = any(BaseColorExposure.rgb > 0.0) ? BaseColorExposure.rgb : float3(0.85, 0.18, 0.10);
    float roughness = Material.x > 0.0 ? saturate(Material.x) : 0.45;
    LTPBRMaterial mat = lt_pbr_material(base_color,
                                        roughness,
                                        saturate(Material.y),
                                        float3(0.0, 0.0, 0.0),
                                        saturate(ao));

    float3 diffuse_env = lt_atmosphere_ambient_diffuse(n, atmosphere, sun_dir, sun_color);
    float3 specular_env = lt_atmosphere_ambient_specular(r, mat.roughness, atmosphere, sun_dir, sun_color);
    float3 color = lt_pbr_brdf_direct(mat, n, v, sun_dir, sun_color * soft_shadow) +
                   lt_pbr_ibl(mat, n, v, diffuse_env, specular_env) + mat.emissive;

    color = lt_atmosphere_apply_fog(color, hit.position, ray.dir, atmosphere, sun_dir, sun_color);
    float exposure = BaseColorExposure.w > 0.0 ? BaseColorExposure.w : 1.0;
    return float4(lt_atmosphere_tonemap(color, exposure), 1.0);
}
