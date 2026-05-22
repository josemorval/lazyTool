// NanoVDB final composite: temporal volume over an analytic atmosphere.
//
// This shader is intentionally only the bunny/atmosphere composite: no extra
// scene geometry or weather layers are mixed into this project.
//
// References and provenance:
// - Rayleigh/Mie sky terms follow the common real-time analytic daylight
//   family introduced by Preetham et al., "A Practical Analytic Model for
//   Daylight" (SIGGRAPH 1999). This is not a full implementation of that paper;
//   it is a compact shader approximation tuned for this viewer.
// - The Mie lobe uses the Henyey-Greenstein phase function, a standard
//   participating-media approximation also used in the volume raymarch shader.
// - No code in this file is copied from external repositories.
#define LT_NO_DEFAULT_SHADOWMAP
#include "nanovdb_common.hlsl"

Texture2D VolumeHistoryTex : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer UserCB : register(b2)
{
    float4 VolumeCompositeTuning;  // rgb: lower-hemisphere tint, w: volume alpha scale.
    float4 VolumeSkyTuning;        // x: intensity, y: turbidity, z: sun disk intensity, w: sun disk sharpness.
    float4 VolumeAtmosphereTuning; // x: aerial haze density, y: height falloff, z: sun shafts, w: shaft sharpness.
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

float hg_phase(float cos_theta, float g)
{
    g = clamp(g, -0.85, 0.85);
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-3);
    return (1.0 - g2) / max(4.0 * LT_PI * pow(denom, 1.5), 1e-3);
}

float3 view_ray_from_uv(float2 uv)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    float4 world = mul(InvViewProj, clip);
    float3 far_pos = world.xyz / max(abs(world.w), 1e-5);
    return normalize(far_pos - CamPos.xyz);
}

float3 analytic_atmosphere(float3 rd)
{
    float3 sun_dir = normalize(-LightDir.xyz);
    float mu = dot(rd, sun_dir);
    float view_up = saturate(rd.y * 0.5 + 0.5);
    float turbidity = max(VolumeSkyTuning.y, 0.05);

    // These wavelength-dependent coefficients are the usual compact real-time
    // Rayleigh constants. They are scaled aggressively below because this is an
    // artistic single-pass sky, not a physically unit-correct atmosphere model.
    float3 beta_r = float3(5.8e-3, 13.5e-3, 33.1e-3);
    float3 beta_m = 21.0e-3.xxx * turbidity;

    // The airmass approximation makes low horizon rays travel through more
    // atmosphere, so the horizon desaturates and warms while zenith stays bluer.
    float air_mass = 1.0 / max(rd.y + 0.18, 0.06);
    float phase_r = 3.0 / (16.0 * LT_PI) * (1.0 + mu * mu);
    float phase_m = hg_phase(mu, 0.72);
    float3 extinction = exp(-(beta_r + beta_m) * air_mass * 22.0);
    float3 inscatter = (beta_r * phase_r + beta_m * phase_m) *
                       (1.0 - extinction) / max(beta_r + beta_m, 1e-4.xxx);

    float horizon = pow(saturate(1.0 - abs(rd.y)), 4.0);
    float3 horizon_tint = float3(1.0, 0.52, 0.23) * horizon * 0.11 * turbidity;

    // The sun disk is intentionally much smaller than the previous scene
    // default. Higher sharpness values make the disk compact without needing an
    // explicit angular radius branch.
    float sun_disk = pow(saturate(mu), max(VolumeSkyTuning.w, 900.0));
    float3 sun = LightColor.rgb * LightDir.w * VolumeSkyTuning.z * sun_disk;

    float3 lower_hemi = VolumeCompositeTuning.rgb * smoothstep(0.02, -0.12, rd.y);
    float3 sky = (inscatter * lerp(0.52, 1.0, view_up) + horizon_tint) *
                 max(VolumeSkyTuning.x, 0.0);
    return max(sky + sun + lower_hemi, 0.0);
}

float3 apply_aerial_perspective(float3 color, float3 rd)
{
    float density = max(VolumeAtmosphereTuning.x, 0.0);
    if (density <= 0.0)
        return color;

    float3 sun_dir = normalize(-LightDir.xyz);
    float height_term = exp(-max(CamPos.y, 0.0) * max(VolumeAtmosphereTuning.y, 0.001));
    float fog = 1.0 - exp(-density * 80.0 * height_term);
    float shaft = pow(saturate(dot(rd, sun_dir)), max(VolumeAtmosphereTuning.w, 1.0)) *
                  max(VolumeAtmosphereTuning.z, 0.0);
    float3 fog_col = lerp(float3(0.42, 0.48, 0.56), LightColor.rgb, 0.35 + shaft);
    return lerp(color, fog_col, saturate(fog + shaft * 0.25));
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float3 rd = view_ray_from_uv(uv);
    float3 bg = apply_aerial_perspective(analytic_atmosphere(rd), rd);

    float4 volume = VolumeHistoryTex.SampleLevel(LinearSampler, uv, 0);
    float alpha = saturate(volume.a * max(VolumeCompositeTuning.w, 0.0));
    return float4(lerp(bg, volume.rgb, alpha), 1.0);
}
