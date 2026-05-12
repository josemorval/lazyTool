/*
Sample 01: thin cube ground using the same lighting vocabulary as the helmet.

This shader is intentionally simpler than helmet_pbr.hlsl:

1. The material is fully parametric and does not consume glTF texture slots.
2. It still uses the same environment texture (t5) and the same shadow atlas
   (t7) so the sample reads like a cohesive mini-scene.
3. Keeping it separate avoids validation noise from "missing t0..t4" on a
   primitive floor mesh that does not carry any material textures.
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

cbuffer ObjectCB : register(b1)
{
    float4x4 World;
};

cbuffer UserCB : register(b2)
{
    float4 GroundColor;
    float4 GroundMaterial; // x = metallic, y = roughness, z = env diffuse scale, w = env specular scale
};

Texture2D EnvMap : register(t5);
Texture2D ShadowMap : register(t7);
SamplerState LinearSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 pos      : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 worldNor : TEXCOORD1;
};

static const float PI = 3.14159265359;
static const float TWO_PI = 6.28318530718;

float3 aces_fitted(float3 color)
{
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
    float longitude = atan2(dir.z, dir.x);
    float latitude = acos(clamp(dir.y, -1.0, 1.0));
    return float2(longitude / TWO_PI + 0.5, latitude / PI);
}

float3 sample_environment(float3 dir, float lod_level)
{
    return EnvMap.SampleLevel(LinearSampler, direction_to_latlong_uv(dir), lod_level).rgb;
}

float sample_shadow_cascade(int cascade_index, float3 world_pos, float ndl)
{
    uint shadow_w = 0;
    uint shadow_h = 0;
    ShadowMap.GetDimensions(shadow_w, shadow_h);
    if (shadow_w == 0 || shadow_h == 0)
        return 1.0;

    float4 shadow_clip = mul(ShadowCascadeViewProj[cascade_index], float4(world_pos, 1.0));
    if (abs(shadow_clip.w) < 1e-5)
        return 1.0;

    float3 shadow_ndc = shadow_clip.xyz / shadow_clip.w;
    float2 uv_local = float2(shadow_ndc.x * 0.5 + 0.5, shadow_ndc.y * -0.5 + 0.5);
    if (uv_local.x < 0.0 || uv_local.x > 1.0 || uv_local.y < 0.0 || uv_local.y > 1.0)
        return 1.0;
    if (shadow_ndc.z < 0.0 || shadow_ndc.z > 1.0)
        return 1.0;

    float4 rect = ShadowCascadeRects[cascade_index];
    float2 atlas_uv = uv_local * rect.xy + rect.zw;
    float bias = lerp(0.0025, 0.0003, saturate(ndl));
    return ShadowMap.SampleCmpLevelZero(ShadowSampler, atlas_uv, shadow_ndc.z - bias);
}

int select_shadow_cascade(float3 world_pos)
{
    int cascade_count = clamp((int)ShadowParams.x, 1, 4);
    float view_depth = dot(world_pos - CamPos.xyz, CamDir.xyz);
    int cascade_index = 0;
    [unroll]
    for (int i = 0; i < 3; i++) {
        if (i + 1 < cascade_count && view_depth > ShadowCascadeSplits[i])
            cascade_index = i + 1;
    }
    return cascade_index;
}

float distribution_ggx(float ndh, float roughness)
{
    float a = max(roughness * roughness, 1e-4);
    float a2 = a * a;
    float denom = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-5);
}

float visibility_smith_ggx(float ndl, float ndv, float roughness)
{
    float k = max((roughness + 1.0) * (roughness + 1.0) * 0.125, 1e-4);
    float gl = ndl / lerp(ndl, 1.0, k);
    float gv = ndv / lerp(ndv, 1.0, k);
    return gl * gv;
}

float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 world_pos = mul(World, float4(v.pos, 1.0));
    o.pos = mul(ViewProj, world_pos);
    o.worldPos = world_pos.xyz;
    o.worldNor = normalize(mul(World, float4(v.nor, 0.0)).xyz);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 n = normalize(i.worldNor);
    float3 v = normalize(CamPos.xyz - i.worldPos);
    float3 l = normalize(-LightDir.xyz);
    float3 h = normalize(v + l);
    float3 r = reflect(-v, n);

    float metallic = saturate(GroundMaterial.x);
    float roughness = clamp(GroundMaterial.y, 0.045, 1.0);
    float ndl = saturate(dot(n, l));
    float ndv = saturate(dot(n, v));
    float ndh = saturate(dot(n, h));
    float vdh = saturate(dot(v, h));

    float3 albedo = GroundColor.rgb;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 fresnel = fresnel_schlick(vdh, f0);
    float d = distribution_ggx(ndh, roughness);
    float g = visibility_smith_ggx(ndl, ndv, roughness);
    float3 specular = (d * g * fresnel) / max(4.0 * ndl * ndv, 1e-4);
    float3 kd = (1.0 - fresnel) * (1.0 - metallic);
    float3 diffuse = kd * albedo / PI;

    int cascade_index = select_shadow_cascade(i.worldPos);
    float shadow = sample_shadow_cascade(cascade_index, i.worldPos, ndl);
    float3 direct = (diffuse + specular) * (LightColor.xyz * LightDir.w) * ndl * shadow;

    float3 ambient = kd * sample_environment(n, 6.0) * GroundMaterial.z;
    ambient += sample_environment(r, roughness * 6.0) * fresnel * GroundMaterial.w;

    float3 mapped = aces_fitted(direct + ambient);
    float3 display = pow(mapped, 1.0 / 2.2);
    return float4(display, 1.0);
}
