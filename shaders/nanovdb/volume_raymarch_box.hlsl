// NanoVDB volume raymarcher for lazyTool.
//
// What this shader does:
// 1. Rasterize a cube proxy only to get viewport pixels that can see the volume.
// 2. Intersect the camera ray against VolumeBoxMinStep.xyz/VolumeBoxMaxDensity.xyz.
// 3. March through that normalized scene box.
// 4. Remap each scene-space sample into the active NanoVDB grid bounds.
// 5. Accumulate single-scattering style color with Beer-Lambert opacity.
// 6. Output color/alpha plus a weighted mean world position used by temporal
//    reprojection in volume_temporal_resolve.hlsl.
//
// External/reference material:
// - The NanoVDB buffer access comes from OpenVDB's PNanoVDB.h.
//   https://www.openvdb.org/documentation/doxygen/PNanoVDB_8h_source.html
// - The phase function is the standard Henyey-Greenstein approximation used in
//   participating media. See Henyey and Greenstein, "Diffuse radiation in the
//   Galaxy" (1941). In graphics it is commonly used as a cheap anisotropic
//   scattering lobe for fog, clouds and smoke.
// - The small ambient sky term is an artistic approximation inspired by analytic
//   daylight models such as Preetham et al. (SIGGRAPH 1999). It is not a
//   physically complete sky probe; it lets the bunny pick up some blue/warm
//   atmosphere color instead of being lit only by a constant gray ambient.
// - This file does not copy code from a repo for the raymarch loop, lighting,
//   normalization, or stochastic jitter.
#include "nanovdb_common.hlsl"

StructuredBuffer<uint> VolumeNanoVDB : register(t0);

cbuffer UserCB : register(b2)
{
    float4 VolumeBoxMinStep;      // xyz: world-space bounds min, w: base step length.
    float4 VolumeBoxMaxDensity;   // xyz: world-space bounds max, w: density scale.
    float4 VolumeAlbedoEmission;  // rgb: scattering albedo/tint, w: emission amount.
    float4 VolumeRenderTuning;    // x: max steps, y: alpha cutoff, z: grid byte offset, w: jitter.
    float4 VolumeLightTuning;     // x: light absorption, y: phase forwardness, z: ambient, w: shadow steps.
    float4 VolumeSkyTuning;       // x: sky intensity, y: turbidity, z/w used by composite sky.
    float4 VolumeAtmosphereTuning; // x: haze density, y: height falloff, z: sky ambient strength, w: reserved.
    // x: shadow reuse stride, y: shadow linear sample, z: grid mapping mode, w: jitter mode.
    // z = 0 direct NanoVDB world coords, 1 normalized uniform fit, 2 normalized stretch.
    // w = 0 interleaved-gradient jitter, 1 temporal low-discrepancy stochastic jitter.
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
    float3 wpos : TEXCOORD0;
};

struct VolumePSOut
{
    float4 color   : SV_Target0; // rgb: unassociated volume color, a: opacity.
    float4 moments : SV_Target1; // xyz: alpha-weighted mean world position, a: opacity.
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 wpos = mul(World, float4(v.pos, 1.0));
    o.pos = mul(ViewProj, wpos);
    o.wpos = wpos.xyz;
    return o;
}

float volume_phase_hg(float cos_theta, float g)
{
    // Henyey-Greenstein returns a normalized angular lobe. g > 0 pushes more
    // light forward in the light direction; g = 0 is isotropic scattering.
    g = clamp(g, -0.85, 0.85);
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-3);
    return (1.0 - g2) / max(pow(denom, 1.5), 1e-3);
}

float3 volume_atmosphere_ambient(float3 rd)
{
    float3 sun_dir = normalize(-LightDir.xyz);
    float up = saturate(rd.y * 0.5 + 0.5);
    float sun_wrap = saturate(dot(rd, sun_dir) * 0.5 + 0.5);
    float turbidity = max(VolumeSkyTuning.y, 0.05);

    // Two-color sky probe approximation. Up-facing/sky-facing rays receive a
    // cooler blue, while directions nearer the sun gain a warmer tint. This is
    // deliberately cheap: evaluating the full sky model at every volume sample
    // would multiply the raymarch cost.
    float3 zenith = float3(0.20, 0.30, 0.48) * max(VolumeSkyTuning.x, 0.0);
    float3 horizon = float3(0.74, 0.55, 0.36) * turbidity;
    float3 sky = lerp(horizon, zenith, up);
    float3 sun_bleed = LightColor.rgb * LightDir.w * (0.10 + 0.16 * sun_wrap);
    return (sky * 0.12 + sun_bleed) * max(VolumeAtmosphereTuning.z, 0.0);
}

float3 volume_grid_sample_pos(float3 p, float grid_mapping_mode, float3 grid_min, float3 grid_max)
{
    if (grid_mapping_mode < 0.5)
        return p;

    // Mode 1 keeps the NanoVDB proportions intact by fitting the active grid
    // bbox into the visible scene box using one uniform scale.
    if (grid_mapping_mode < 1.5)
        return lt_fit_box_to_box_uniform(p, VolumeBoxMinStep.xyz, VolumeBoxMaxDensity.xyz,
                                         grid_min, grid_max);

    // Mode 2 is useful as a debugging stretch: every axis fills the scene box.
    return lt_remap_box_to_box(p, VolumeBoxMinStep.xyz, VolumeBoxMaxDensity.xyz,
                               grid_min, grid_max);
}

float sample_density(float3 p, uint grid_offset, float grid_mapping_mode, float3 grid_min, float3 grid_max)
{
    float3 gp = volume_grid_sample_pos(p, grid_mapping_mode, grid_min, grid_max);
    // NanoVDB stores density in sparse tree nodes. `lt_nvdb_sample_linear_float`
    // performs eight tree reads and lerps them in shader code, because a
    // NanoVDB tree is not a hardware Texture3D. That is why sample count is the
    // central performance lever for this renderer.
    return max(lt_nvdb_sample_linear_float(VolumeNanoVDB, grid_offset, gp), 0.0) *
           max(VolumeBoxMaxDensity.w, 0.0);
}

float sample_shadow_density(float3 p, uint grid_offset, float grid_mapping_mode, float3 grid_min, float3 grid_max)
{
    float3 gp = volume_grid_sample_pos(p, grid_mapping_mode, grid_min, grid_max);
    // Shadow rays are many secondary samples. Nearest sampling is deliberately
    // allowed here because it is much cheaper and usually acceptable in preview.
    float d = VolumeOptimizationTuning.y > 0.5 ?
        lt_nvdb_sample_linear_float(VolumeNanoVDB, grid_offset, gp) :
        lt_nvdb_sample_nearest_float(VolumeNanoVDB, grid_offset, gp);
    return max(d, 0.0) * max(VolumeBoxMaxDensity.w, 0.0);
}

float light_transmittance(float3 p, float3 light_dir, uint grid_offset, float step_len,
                          float grid_mapping_mode, float3 grid_min, float3 grid_max)
{
    int steps = clamp((int)VolumeLightTuning.w, 0, 32);
    if (steps <= 0 || VolumeLightTuning.x <= 0.0)
        return 1.0;

    float3 bmin = VolumeBoxMinStep.xyz;
    float3 bmax = VolumeBoxMaxDensity.xyz;
    float lt0, lt1;
    if (!lt_ray_box(p + light_dir * step_len, light_dir, bmin, bmax, lt0, lt1))
        return 1.0;

    float ds = min(step_len * 3.0, lt1 / max((float)steps, 1.0));
    float t = ds;
    float optical = 0.0;
    [loop]
    for (int i = 0; i < steps; i++)
    {
        if (t >= lt1)
            break;
        optical += sample_shadow_density(p + light_dir * t, grid_offset, grid_mapping_mode, grid_min, grid_max) * ds;
        t += ds;
    }
    // Beer-Lambert transmittance. Larger VolumeLightTuning.x means a denser
    // volume blocks more direct sunlight along this short secondary ray.
    return exp(-optical * max(VolumeLightTuning.x, 0.0));
}

VolumePSOut PSMain(VSOut i)
{
    float3 ro = CamPos.xyz;
    float3 rd = normalize(i.wpos - ro);
    float t0, t1;
    if (!lt_ray_box(ro, rd, VolumeBoxMinStep.xyz, VolumeBoxMaxDensity.xyz, t0, t1))
        discard;

    uint grid_offset = (uint)max(VolumeRenderTuning.z, 0.0);
    float step_len = max(VolumeBoxMinStep.w, 1e-4);
    int max_steps = clamp((int)VolumeRenderTuning.x, 8, 4096);
    int shadow_stride = clamp((int)max(VolumeOptimizationTuning.x, 1.0), 1, 16);
    float grid_mapping_mode = VolumeOptimizationTuning.z;
    float3 grid_min = VolumeBoxMinStep.xyz;
    float3 grid_max = VolumeBoxMaxDensity.xyz;
    if (grid_mapping_mode > 0.5)
        lt_nvdb_active_world_bounds(VolumeNanoVDB, grid_offset, grid_min, grid_max);

    // Stochastic start offset turns banding from a fixed step into temporally
    // distributed noise. It becomes most useful once a temporal history pass is
    // added, but it already helps half-res preview avoid obvious slice bands.
    float jitter = lt_stochastic_volume_jitter(i.pos.xy, (uint)TimeVec.z,
                                               VolumeOptimizationTuning.w) *
                   saturate(VolumeRenderTuning.w);
    float t = t0 + step_len * jitter;

    float3 light_dir = normalize(-LightDir.xyz);
    float phase = volume_phase_hg(dot(rd, light_dir), VolumeLightTuning.y);
    float3 atmosphere_ambient = volume_atmosphere_ambient(rd);
    float3 albedo = max(VolumeAlbedoEmission.rgb, 0.0);
    float3 accum = 0.0;
    float3 accum_pos = 0.0;
    float trans = 1.0;
    float last_light_visibility = 1.0;
    bool light_visibility_valid = false;
    int shadow_countdown = 0;

    [loop]
    for (int s = 0; s < max_steps; s++)
    {
        if (t > t1 || trans < max(VolumeRenderTuning.y, 0.001))
            break;

        float3 p = ro + rd * t;
        float density = sample_density(p, grid_offset, grid_mapping_mode, grid_min, grid_max);
        if (density > 1e-5)
        {
            float optical = density * step_len;
            float alpha = 1.0 - exp(-optical);
            if (!light_visibility_valid || shadow_countdown <= 0)
            {
                last_light_visibility = light_transmittance(p, light_dir, grid_offset, step_len,
                                                            grid_mapping_mode, grid_min, grid_max);
                light_visibility_valid = true;
                shadow_countdown = shadow_stride;
            }
            float3 sun = LightColor.rgb * LightDir.w * last_light_visibility * phase;
            float3 ambient = VolumeLightTuning.z.xxx + atmosphere_ambient;
            float3 radiance = albedo * (ambient + sun) + albedo * VolumeAlbedoEmission.w;
            float sample_weight = trans * alpha;
            accum += sample_weight * radiance;
            accum_pos += sample_weight * p;
            trans *= 1.0 - alpha;
        }
        shadow_countdown--;
        t += step_len;
    }

    float alpha_out = saturate(1.0 - trans);
    if (alpha_out <= 1e-4)
        discard;

    VolumePSOut o;
    o.color = float4(accum / max(alpha_out, 1e-4), alpha_out);
    o.moments = float4(accum_pos / max(alpha_out, 1e-4), alpha_out);
    return o;
}
