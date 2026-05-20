#ifndef DEMO64K_PBR_COMMON_HLSL
#define DEMO64K_PBR_COMMON_HLSL

#include "../common.hlsl"

struct PBRMaterial
{
    float3 albedo;
    float  roughness;
    float  metalness;
    float3 emissive;
};

float3 demo64k_fresnel_schlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 demo64k_fresnel_schlick_roughness(float cosTheta, float3 F0, float roughness)
{
    float oneMinusR = 1.0 - saturate(roughness);
    return F0 + (max(float3(oneMinusR, oneMinusR, oneMinusR), F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float demo64k_distribution_ggx(float3 N, float3 H, float roughness)
{
    float a = max(roughness * roughness, 0.002);
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(LT_PI * d * d, 1e-5);
}

float demo64k_geometry_schlick_ggx(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-5);
}

float demo64k_geometry_smith(float3 N, float3 V, float3 L, float roughness)
{
    return demo64k_geometry_schlick_ggx(saturate(dot(N, V)), roughness) *
           demo64k_geometry_schlick_ggx(saturate(dot(N, L)), roughness);
}

float3 demo64k_pbr_direct(PBRMaterial m, float3 N, float3 V, float3 L, float3 radiance)
{
    float3 H = lt_safe_normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float HdotV = saturate(dot(H, V));

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), m.albedo, m.metalness);
    float3 F = demo64k_fresnel_schlick(HdotV, F0);
    float  D = demo64k_distribution_ggx(N, H, m.roughness);
    float  G = demo64k_geometry_smith(N, V, L, m.roughness);

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - m.metalness);
    float3 diffuse = kd * m.albedo / LT_PI;
    return (diffuse + specular) * radiance * NdotL;
}

float3 demo64k_pbr_ibl(PBRMaterial m, float3 N, float3 V, float ao, float3 diffuseIrradiance, float3 specularRadiance)
{
    float NdotV = saturate(dot(N, V));
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), m.albedo, m.metalness);
    float3 F = demo64k_fresnel_schlick_roughness(NdotV, F0, m.roughness);
    float3 kd = (1.0 - F) * (1.0 - m.metalness);

    // A tiny two-term environment BRDF approximation keeps metals believable without a LUT.
    float specScale = lerp(0.22, 0.95, saturate(1.0 - m.roughness));
    float3 diffuse = kd * m.albedo * diffuseIrradiance;
    float3 specular = F * specularRadiance * specScale;
    return (diffuse * ao + specular * lerp(ao, 1.0, 0.65));
}

#endif
