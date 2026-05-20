#include "../common.hlsl"
#include "fullscreen_common.hlsl"
#include "pbr_common.hlsl"
#include "sky_common.hlsl"

Texture2D SceneDepthTex      : register(t0);
Texture2D NormalRoughnessTex : register(t1);
Texture2D AlbedoMetalnessTex : register(t2);
Texture2D EmissiveTex        : register(t3);
Texture2D AOTex              : register(t4);

cbuffer UserCB : register(b2)
{
    float4 PostAOParams;     // x radius, y intensity, z bias, w contrast/power.
    float4 SunColorTuning;   // rgb directional-light tint multiplier, w ambient amount.
    float4 SkyParams;        // x sky intensity, y sun disk/glow, z horizon warmth, w stars.
    float4 SkyTint;          // rgb sky tint, w sky reflection strength.
};

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float depth01 = SceneDepthTex.SampleLevel(LinearSampler, uv, 0).r;
    float3 sunDir = lt_safe_normalize(-LightDir.xyz);
    float3 sunColor = LightColor.rgb * SunColorTuning.rgb * max(LightDir.w, 0.0);

    if (depth01 >= 0.99999) {
        float2 ndc = lt_viewport_uv_to_ndc(uv);
        float3 rd = lt_safe_normalize(lt_scene_depth_to_world(uv, 0.999) - CamPos.xyz);
        float3 sky = demo64k_sky_radiance(rd, SkyParams, SkyTint, sunDir, sunColor);
        // Slight cinematic bottom fade so the procedural sky sits behind the abstract set.
        sky *= 0.85 + 0.15 * smoothstep(-0.9, 0.9, ndc.y);
        return float4(sky, 1.0);
    }

    float3 world_pos = lt_scene_depth_to_world(uv, depth01);
    float4 nr = NormalRoughnessTex.SampleLevel(LinearSampler, uv, 0);
    float4 am = AlbedoMetalnessTex.SampleLevel(LinearSampler, uv, 0);
    float3 emissive = EmissiveTex.SampleLevel(LinearSampler, uv, 0).rgb;

    PBRMaterial m;
    m.albedo = saturate(am.rgb);
    m.metalness = saturate(am.a);
    m.roughness = saturate(nr.a);
    m.emissive = emissive;

    float3 N = lt_decode_normal_rgb(nr);
    float3 V = lt_vector_to_camera_ws(world_pos);
    float3 L = sunDir;
    float shadow = lt_sample_shadow_pcf3x3(world_pos, N, L);

    float ao = AOTex.SampleLevel(LinearSampler, uv, 0).r;
    ao = pow(saturate(ao), max(PostAOParams.w, 0.05));

    float3 direct = demo64k_pbr_direct(m, N, V, L, sunColor * shadow);

    float3 R = reflect(-V, N);
    float3 skyDiffuse = demo64k_sky_ambient_diffuse(N, SkyParams, SkyTint, sunDir, sunColor) * SunColorTuning.w;
    float3 skySpec = demo64k_sky_ambient_specular(R, m.roughness, SkyParams, SkyTint, sunDir, sunColor) * SkyTint.w;
    float3 ambient = demo64k_pbr_ibl(m, N, V, ao, skyDiffuse, skySpec);

    // Subtle contact-independent edge lift makes glossy dark objects read like a polished demo scene.
    float rim = pow(1.0 - saturate(dot(N, V)), 4.0) * 0.045 * (1.0 - m.roughness);
    float3 color = direct + ambient + rim * skySpec * (0.4 + 0.6 * m.metalness) + emissive;
    return float4(max(color, 0.0), 1.0);
}
