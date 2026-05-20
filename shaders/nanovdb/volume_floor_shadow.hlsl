// Floor-space NanoVDB shadow map.
//
// Each pixel represents one point on the finite rocky floor. The shader marches
// from that point toward the directional light through the normalized volume box
// and writes Beer-Lambert transmittance. The final composite only samples this
// texture, so the expensive NanoVDB visibility integration is isolated here.
#define LT_NO_DEFAULT_SHADOWMAP
#include "nanovdb_common.hlsl"

StructuredBuffer<uint> VolumeNanoVDB : register(t0);

cbuffer UserCB : register(b2)
{
    float4 VolumeFloorTuning;      // x: floor y, y: terrain scale, z: height amount, w: shadow absorption.
    float4 VolumeFloorShapeTuning; // x: half size, y: edge feather, z: roughness, w: shadow strength.
    float4 VolumeBoxMinStep;
    float4 VolumeBoxMaxDensity;
    float4 VolumeRenderTuning;
    float4 VolumeOptimizationTuning;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = float4(v.pos.xy, 0.0, 1.0);
    o.uv = v.uv;
    return o;
}

float value_noise_2d_shadow(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float a = lt_hash12(i);
    float b = lt_hash12(i + float2(1.0, 0.0));
    float c = lt_hash12(i + float2(0.0, 1.0));
    float d = lt_hash12(i + float2(1.0, 1.0));
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm_2d_shadow(float2 p)
{
    float n = 0.0;
    float a = 0.5;
    float2 q = p;
    [unroll]
    for (int i = 0; i < 4; i++)
    {
        n += value_noise_2d_shadow(q) * a;
        q = q * 2.03 + 17.1;
        a *= 0.5;
    }
    return n;
}

float cellular_2d_shadow(float2 p)
{
    float n0 = value_noise_2d_shadow(p);
    float n1 = value_noise_2d_shadow(p * 2.37 + 11.4);
    float n = max(n0, n1 * 0.82);
    return 1.0 - smoothstep(0.58, 0.96, n);
}

float shadow_rock_height_world(float2 xz)
{
    float scale = max(VolumeFloorTuning.y, 0.01);
    float2 uv = xz * scale;
    float2 warp = float2(fbm_2d_shadow(uv * 0.46 + 13.0),
                         fbm_2d_shadow(uv * 0.51 - 7.0)) - 0.5;
    uv += warp * 0.85;
    float broad = fbm_2d_shadow(uv * 0.58);
    float ridge = 1.0 - abs(fbm_2d_shadow(uv * 1.55 + 8.0) * 2.0 - 1.0);
    ridge *= ridge;
    float strata = sin((uv.x * 1.7 + uv.y * 0.65 + broad * 2.8) * 3.14159) * 0.5 + 0.5;
    float fine = fbm_2d_shadow(uv * 8.5 + 41.0);
    float grit = cellular_2d_shadow(uv * 11.0);
    float amp = max(VolumeFloorTuning.z, 0.0) * 0.36;
    return ((broad - 0.5) * 0.40 + ridge * 0.22 + (strata - 0.5) * 0.16 +
            (fine - 0.5) * 0.08 + grit * 0.10 - 0.10) * amp;
}

float3 shadow_volume_grid_pos(float3 p, float3 grid_min, float3 grid_max)
{
    float mode = VolumeOptimizationTuning.z;
    if (mode < 0.5)
        return p;
    if (mode < 1.5)
        return lt_fit_box_to_box_uniform(p, VolumeBoxMinStep.xyz, VolumeBoxMaxDensity.xyz,
                                         grid_min, grid_max);
    return lt_remap_box_to_box(p, VolumeBoxMinStep.xyz, VolumeBoxMaxDensity.xyz,
                               grid_min, grid_max);
}

float shadow_density(float3 p, uint grid_offset, float3 grid_min, float3 grid_max)
{
    float3 gp = shadow_volume_grid_pos(p, grid_min, grid_max);
    float d = lt_nvdb_sample_linear_float(VolumeNanoVDB, grid_offset, gp);
    return max(d, 0.0) * max(VolumeBoxMaxDensity.w, 0.0);
}

float trace_shadow(float3 floor_p)
{
    uint grid_offset = (uint)max(VolumeRenderTuning.z, 0.0);
    float3 grid_min = VolumeBoxMinStep.xyz;
    float3 grid_max = VolumeBoxMaxDensity.xyz;
    if (VolumeOptimizationTuning.z > 0.5)
        lt_nvdb_active_world_bounds(VolumeNanoVDB, grid_offset, grid_min, grid_max);

    float3 light_dir = normalize(-LightDir.xyz);
    if (light_dir.y <= 0.02)
        light_dir.y = abs(light_dir.y) + 0.02;
    light_dir = normalize(light_dir);

    float t0, t1;
    if (!lt_ray_box(floor_p + light_dir * 0.01, light_dir,
                    VolumeBoxMinStep.xyz, VolumeBoxMaxDensity.xyz, t0, t1))
        return 1.0;

    const int steps = 48;
    float segment = max(t1 - t0, 1e-4);
    float ds = segment / (float)steps;
    float optical = 0.0;
    float peak = 0.0;

    [loop]
    for (int i = 0; i < steps; i++)
    {
        float t = t0 + ((float)i + 0.5) * ds;
        float density = shadow_density(floor_p + light_dir * t, grid_offset, grid_min, grid_max);
        optical += density * ds;
        peak = max(peak, density);
    }

    float strength = max(VolumeFloorTuning.w, 0.0) * max(VolumeFloorShapeTuning.w, 0.0);
    return exp(-(optical + peak * 0.04) * strength);
}

float4 PSMain(VSOut i) : SV_Target
{
    float half_size = max(VolumeFloorShapeTuning.x, 0.2);
    float2 xz = (i.uv * 2.0 - 1.0) * half_size;
    float y = VolumeFloorTuning.x + shadow_rock_height_world(xz);
    float shadow = trace_shadow(float3(xz.x, y, xz.y));
    return float4(shadow.xxx, 1.0);
}
