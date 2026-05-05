/*
Sample 01: physically based shading for DamagedHelmet.glb

This shader is intentionally verbose. The aim is not to be the smallest PBR
implementation possible, but to show the moving parts clearly:

1. How glTF material textures are consumed inside lazyTool:
   - t0 = base color
   - t1 = metallic-roughness
   - t2 = normal
   - t3 = emissive
   - t4 = occlusion
   - t5 = environment map (bound manually by the command)
   - t7 = shadow map (bound manually by the command and also provided by the
          engine when shadow receiving is enabled)
2. How a classic Cook-Torrance BRDF is assembled.
3. How a single HDR texture can drive both the skybox and a lightweight IBL
   approximation in a self-contained sample.

References
----------
- glTF 2.0 metallic-roughness material model
- "Real Shading in Unreal Engine 4" (Brian Karis)
- "Moving Frostbite to Physically Based Rendering" (Lagarde, de Rousiers)
- "Microfacet Models for Refraction through Rough Surfaces" (Walter et al.)
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
    float4 BaseColorFactor;
    float4 SurfaceControls; // x = metallic scale, y = roughness scale, z = normal strength, w = emissive scale
};

Texture2D BaseColorTex : register(t0);
Texture2D MetalRoughTex : register(t1);
Texture2D NormalTex : register(t2);
Texture2D EmissiveTex : register(t3);
Texture2D OcclusionTex : register(t4);
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
    float2 uv       : TEXCOORD2;
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

bool texture_is_bound(Texture2D tex)
{
    uint w = 0;
    uint h = 0;
    tex.GetDimensions(w, h);
    return w > 0 && h > 0;
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
    float2 uv = direction_to_latlong_uv(dir);
    return EnvMap.SampleLevel(LinearSampler, uv, lod_level).rgb;
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

    // A tiny slope-aware bias is enough for this sample and keeps the function
    // readable without introducing PCF kernels or normal-offset bias code.
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

float3 decode_srgb(float3 c)
{
    return pow(saturate(c), 2.2);
}

float3 decode_normal_map(float3 encoded)
{
    return normalize(encoded * 2.0 - 1.0);
}

float3x3 cotangent_frame(float3 n, float3 world_pos, float2 uv)
{
    // Reconstruct tangent space from derivatives so the sample does not depend
    // on tangent attributes in the mesh.
    float3 dp1 = ddx(world_pos);
    float3 dp2 = ddy(world_pos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2_perp = cross(dp2, n);
    float3 dp1_perp = cross(n, dp1);
    float3 t = dp2_perp * duv1.x + dp1_perp * duv2.x;
    float3 b = dp2_perp * duv1.y + dp1_perp * duv2.y;

    float inv_max = rsqrt(max(dot(t, t), dot(b, b)));
    return float3x3(t * inv_max, b * inv_max, n);
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
    o.uv = v.uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 base_color = BaseColorFactor;
    if (texture_is_bound(BaseColorTex)) {
        float4 texel = BaseColorTex.Sample(LinearSampler, i.uv);
        base_color.rgb *= decode_srgb(texel.rgb);
        base_color.a *= texel.a;
    }

    float metallic = saturate(SurfaceControls.x);
    float roughness = saturate(SurfaceControls.y);
    if (texture_is_bound(MetalRoughTex)) {
        float4 mr = MetalRoughTex.Sample(LinearSampler, i.uv);
        metallic *= mr.b;
        roughness *= mr.g;
    }
    roughness = clamp(roughness, 0.045, 1.0);

    float3 n = normalize(i.worldNor);
    if (texture_is_bound(NormalTex) && SurfaceControls.z > 0.0) {
        float3 tangent_space_n = decode_normal_map(NormalTex.Sample(LinearSampler, i.uv).xyz);
        tangent_space_n.xy *= SurfaceControls.z;
        tangent_space_n = normalize(float3(tangent_space_n.xy, max(tangent_space_n.z, 1e-4)));
        float3x3 tbn = cotangent_frame(n, i.worldPos, i.uv);
        n = normalize(mul(tangent_space_n, tbn));
    }

    float occlusion = 1.0;
    if (texture_is_bound(OcclusionTex))
        occlusion = OcclusionTex.Sample(LinearSampler, i.uv).r;

    float3 emissive = 0.0;
    if (texture_is_bound(EmissiveTex))
        emissive = decode_srgb(EmissiveTex.Sample(LinearSampler, i.uv).rgb) * SurfaceControls.w;

    float3 v = normalize(CamPos.xyz - i.worldPos);
    float3 l = normalize(-LightDir.xyz);
    float3 h = normalize(v + l);
    float3 r = reflect(-v, n);

    float ndl = saturate(dot(n, l));
    float ndv = saturate(dot(n, v));
    float ndh = saturate(dot(n, h));
    float vdh = saturate(dot(v, h));

    float3 albedo = base_color.rgb;
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

    // A full production PBR pipeline would use irradiance convolution and a
    // prefiltered specular environment map. For a didactic sample we keep a
    // lighter approximation: diffuse from the normal direction and specular
    // from the reflection direction with a roughness-driven mip level.
    float3 ibl_diffuse = sample_environment(n, 6.0) * albedo;
    float3 ibl_specular = sample_environment(r, roughness * 6.0) * fresnel;
    float3 ambient = (kd * ibl_diffuse + ibl_specular) * occlusion * 0.6;

    float3 hdr = direct + ambient + emissive;
    float3 mapped = aces_fitted(hdr);
    float3 display = pow(mapped, 1.0 / 2.2);
    return float4(display, base_color.a);
}
