#ifndef PROCEDURAL_SPHERES_PBR_POST_COMMON_HLSL
#define PROCEDURAL_SPHERES_PBR_POST_COMMON_HLSL

/*
    Shared shader utilities for the Procedural Spheres PBR + Post project.

    The project is intentionally asset-free: geometry comes from editor
    primitives, material detail comes from analytic checker/noise functions,
    and the post-process chain only consumes render targets produced inside
    the frame.  This makes the scene easier to reason about and keeps it close
    to a future size-constrained / build64k style workflow.

    Coordinate conventions used by the editor:
    - SceneCB is bound by the engine at b0.
    - ObjectCB is bound by the engine at b1 for mesh/object transforms.
    - UserCB is reflected per command and should be placed at b2.
    - Matrices are used with mul(Matrix, vector), matching the existing sample
      shaders shipped with the editor.
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

SamplerState LinearSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
Texture2D ShadowMap : register(t7);

static const float PI = 3.14159265358979323846;
static const float GOLDEN_ANGLE = 2.39996322972865332;

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct PostVSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

PostVSOut make_fullscreen_vertex(VSIn v)
{
    PostVSOut o;
    o.pos = float4(v.pos.xy, 0.0, 1.0);
    o.uv = v.uv;
    return o;
}

float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float max3(float3 c)
{
    return max(max(c.r, c.g), c.b);
}

float3 aces_fitted(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float hash11(float p)
{
    p = frac(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return frac(p);
}

float hash21(float2 p)
{
    float3 p3 = frac(float3(p.x, p.y, p.x) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float hash31(float3 p3)
{
    p3 = frac(p3 * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float noise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = hash31(i + float3(0, 0, 0));
    float n100 = hash31(i + float3(1, 0, 0));
    float n010 = hash31(i + float3(0, 1, 0));
    float n110 = hash31(i + float3(1, 1, 0));
    float n001 = hash31(i + float3(0, 0, 1));
    float n101 = hash31(i + float3(1, 0, 1));
    float n011 = hash31(i + float3(0, 1, 1));
    float n111 = hash31(i + float3(1, 1, 1));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);
    float nxy0 = lerp(nx00, nx10, f.y);
    float nxy1 = lerp(nx01, nx11, f.y);
    return lerp(nxy0, nxy1, f.z);
}

float fbm3(float3 p)
{
    float value = 0.0;
    float amp = 0.5;
    [unroll]
    for (int i = 0; i < 5; i++)
    {
        value += noise3(p) * amp;
        p = p * 2.03 + 17.17;
        amp *= 0.5;
    }
    return value;
}

float3 reconstruct_world_from_depth(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(InvViewProj, float4(ndc, depth, 1.0));
    return world.xyz / max(abs(world.w), 1e-5);
}

float view_depth_from_world(float3 world_pos)
{
    return dot(world_pos - CamPos.xyz, normalize(CamDir.xyz));
}

int select_shadow_cascade(float3 world_pos)
{
    int cascade_count = clamp((int)ShadowParams.x, 1, 4);
    float view_depth = view_depth_from_world(world_pos);
    int cascade_index = 0;

    [unroll]
    for (int i = 0; i < 3; i++)
    {
        if (i + 1 < cascade_count && view_depth > ShadowCascadeSplits[i])
            cascade_index = i + 1;
    }

    return cascade_index;
}

float sample_shadow_cascade_pcf(int cascade_index, float3 world_pos, float ndl)
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
    float2 atlas_texel = 1.0 / float2((float)shadow_w, (float)shadow_h);
    float2 local_texel = atlas_texel / max(rect.xy, atlas_texel);

    float edge_dist = min(min(uv_local.x, 1.0 - uv_local.x), min(uv_local.y, 1.0 - uv_local.y));
    float edge_fade = smoothstep(0.0, max(local_texel.x, local_texel.y) * 4.0, edge_dist);
    float2 atlas_min = rect.zw + atlas_texel * 2.5;
    float2 atlas_max = rect.zw + rect.xy - atlas_texel * 2.5;

    // Slope-scaled bias: shallow angles need more bias, lit-facing surfaces
    // keep it small to avoid detached contact shadows.
    float bias = lerp(0.0032, 0.00045, saturate(ndl));

    float sum = 0.0;
    float weight_sum = 0.0;
    [unroll]
    for (int y = -2; y <= 2; y++)
    {
        [unroll]
        for (int x = -2; x <= 2; x++)
        {
            float2 o = float2((float)x, (float)y);
            float w = exp(-dot(o, o) * 0.22);
            float2 sample_uv = clamp(atlas_uv + o * atlas_texel, atlas_min, atlas_max);
            sum += ShadowMap.SampleCmpLevelZero(ShadowSampler, sample_uv, shadow_ndc.z - bias) * w;
            weight_sum += w;
        }
    }

    float shadow = sum / max(weight_sum, 1e-4);
    return lerp(1.0, shadow, edge_fade);
}

float distribution_ggx(float ndh, float roughness)
{
    float a = max(roughness * roughness, 0.001);
    float a2 = a * a;
    float denom = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-5);
}

float visibility_smith_ggx(float ndl, float ndv, float roughness)
{
    float r = roughness + 1.0;
    float k = max((r * r) * 0.125, 1e-4);
    float gl = ndl / max(ndl * (1.0 - k) + k, 1e-4);
    float gv = ndv / max(ndv * (1.0 - k) + k, 1e-4);
    return gl * gv;
}

float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

float3 evaluate_pbr_direct(float3 world_pos, float3 n, float3 v, float3 albedo, float metallic, float roughness)
{
    float3 l = normalize(-LightDir.xyz);
    float3 h = normalize(v + l);

    float ndl = saturate(dot(n, l));
    float ndv = saturate(dot(n, v));
    float ndh = saturate(dot(n, h));
    float vdh = saturate(dot(v, h));

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 f = fresnel_schlick(vdh, f0);
    float d = distribution_ggx(ndh, roughness);
    float g = visibility_smith_ggx(ndl, ndv, roughness);
    float3 spec = (d * g * f) / max(4.0 * ndl * ndv, 1e-4);
    float3 kd = (1.0 - f) * (1.0 - metallic);
    float3 diffuse = kd * albedo / PI;

    int cascade_index = select_shadow_cascade(world_pos);
    float shadow = sample_shadow_cascade_pcf(cascade_index, world_pos, ndl);
    float3 radiance = LightColor.xyz * LightDir.w;

    return (diffuse + spec) * radiance * ndl * shadow;
}

#endif
