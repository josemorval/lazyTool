// Half-res volume composite with an analytic physical-ish sky.
//
// The sky/floor/shadow code is authored for lazyTool. It is not copied from a repository;
// it uses the standard ideas behind analytic daylight models: Rayleigh/Mie
// scattering lobes, directional sun disk and horizon air-mass tint. For a
// deeper reference model, see Preetham et al., "A Practical Analytic Model
// for Daylight" and later Hosek-Wilkie sky work.
// The procedural cloud layer follows the same high-level recipe used by many
// real-time engines: low-frequency coverage noise + detail erosion + sunlit
// forward scattering. It is local shader code, not copied from a sample.
#define LT_NO_DEFAULT_SHADOWMAP
#include "nanovdb_common.hlsl"

Texture2D VolumeHistoryTex : register(t0);
StructuredBuffer<uint> VolumeNanoVDB : register(t1);
Texture2D CloudSkyTex : register(t2);
Texture2D FloorShadowTex : register(t3);
SamplerState LinearSampler : register(s0);

cbuffer UserCB : register(b2)
{
    float4 VolumeCompositeTuning; // rgb: sky tint/ground fallback, w: volume alpha scale.
    float4 VolumeSkyTuning;       // x: sky intensity, y: turbidity/airmass, z: sun disk intensity, w: sun disk sharpness.
    float4 VolumeFloorTuning;     // x: floor y, y: checker scale, z: rock detail, w: shadow absorption.
    float4 VolumeFloorShapeTuning; // x: half size, y: edge feather, z: roughness, w: shadow strength.
    float4 VolumeAtmosphereTuning; // x: fog density, y: fog height falloff, z: god-ray strength, w: god-ray sharpness.
    float4 VolumeCloudTuning;     // x: coverage, y: density, z: cloud base altitude, w: noise scale.
    float4 VolumeBoxMinStep;      // shared with raymarch shader.
    float4 VolumeBoxMaxDensity;   // shared with raymarch shader.
    float4 VolumeRenderTuning;    // z: grid byte offset.
    float4 VolumeOptimizationTuning; // y: shadow linear sample, z: mapping mode.
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
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-3);
    return (1.0 - g2) / max(4.0 * LT_PI * pow(denom, 1.5), 1e-3);
}

float3 sky_ray_from_uv(float2 uv)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    float4 world = mul(InvViewProj, clip);
    float3 pos = world.xyz / max(abs(world.w), 1e-5);
    return normalize(pos - CamPos.xyz);
}

float3 physical_sky(float3 rd)
{
    float3 sun_dir = normalize(-LightDir.xyz);
    float mu = dot(rd, sun_dir);
    float view_up = saturate(rd.y * 0.5 + 0.5);
    float air_mass = 1.0 / max(rd.y + 0.18, 0.06);
    float turbidity = max(VolumeSkyTuning.y, 0.1);

    float3 beta_r = float3(5.8e-3, 13.5e-3, 33.1e-3);
    float3 beta_m = 21.0e-3.xxx * turbidity;
    float phase_r = 3.0 / (16.0 * LT_PI) * (1.0 + mu * mu);
    float phase_m = hg_phase(mu, 0.76);
    float3 extinction = exp(-(beta_r + beta_m) * air_mass * 24.0);
    float3 inscatter = (beta_r * phase_r + beta_m * phase_m) *
                       (1.0 - extinction) / max(beta_r + beta_m, 1e-4.xxx);

    float horizon = pow(saturate(1.0 - abs(rd.y)), 4.0);
    float3 horizon_tint = float3(1.0, 0.55, 0.22) * horizon * 0.16 * turbidity;
    float sun_disk = pow(saturate(mu), max(VolumeSkyTuning.w, 16.0));
    float3 sun = LightColor.rgb * LightDir.w * VolumeSkyTuning.z * sun_disk;
    float3 ground = VolumeCompositeTuning.rgb * smoothstep(0.0, -0.08, rd.y);
    float3 sky = (inscatter * lerp(0.55, 1.0, view_up) + horizon_tint) *
                 max(VolumeSkyTuning.x, 0.0);
    return max(sky + sun + ground, 0.0);
}

float value_noise_2d(float2 p)
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

float fbm_2d(float2 p)
{
    float n = 0.0;
    float a = 0.5;
    float2 q = p;
    [unroll]
    for (int i = 0; i < 4; i++)
    {
        n += value_noise_2d(q) * a;
        q = q * 2.03 + 17.1;
        a *= 0.5;
    }
    return n;
}

float cellular_2d(float2 p)
{
    float n0 = value_noise_2d(p);
    float n1 = value_noise_2d(p * 2.37 + 11.4);
    float n = max(n0, n1 * 0.82);
    return 1.0 - smoothstep(0.58, 0.96, n);
}

float checker_value_aa(float2 uv)
{
    float parity = frac(floor(uv.x) + floor(uv.y)) < 0.5 ? 0.0 : 1.0;
    float2 fw = max(fwidth(uv), 1e-4.xx);
    float2 cell = abs(frac(uv) - 0.5);
    float2 blend = smoothstep(0.5 - fw * 1.5, 0.5 + fw * 1.5, cell);
    float edge_blend = max(blend.x, blend.y);
    return lerp(parity, 0.5, edge_blend * 0.65);
}

float volume_floor_shadow(float3 p)
{
    float half_size = max(VolumeFloorShapeTuning.x, 0.2);
    float2 suv = p.xz / (half_size * 2.0) + 0.5;
    return FloorShadowTex.SampleLevel(LinearSampler, suv, 0).r;
}

float rock_height_world(float2 xz)
{
    float scale = max(VolumeFloorTuning.y, 0.01);
    float2 uv = xz * scale;
    float2 warp = float2(fbm_2d(uv * 0.46 + 13.0), fbm_2d(uv * 0.51 - 7.0)) - 0.5;
    uv += warp * 0.85;
    float broad = fbm_2d(uv * 0.58);
    float ridge = 1.0 - abs(fbm_2d(uv * 1.55 + 8.0) * 2.0 - 1.0);
    ridge *= ridge;
    float strata = sin((uv.x * 1.7 + uv.y * 0.65 + broad * 2.8) * 3.14159) * 0.5 + 0.5;
    float fine = fbm_2d(uv * 8.5 + 41.0);
    float grit = cellular_2d(uv * 11.0);
    float amp = max(VolumeFloorTuning.z, 0.0) * 0.36;
    return ((broad - 0.5) * 0.40 + ridge * 0.22 + (strata - 0.5) * 0.16 +
            (fine - 0.5) * 0.08 + grit * 0.10 - 0.10) * amp;
}

bool floor_heightfield_intersect(float3 ro, float3 rd, float floor_y, out float3 p, out float t_hit)
{
    p = 0.0.xxx;
    t_hit = 0.0;
    if (rd.y >= -1e-4)
        return false;

    float amp = max(VolumeFloorTuning.z, 0.0) * 0.36;
    float top_y = floor_y + amp * 0.85 + 0.05;
    float bottom_y = floor_y - amp * 0.40 - 0.05;
    float ta = (top_y - ro.y) / rd.y;
    float tb = (bottom_y - ro.y) / rd.y;
    float t0 = max(min(ta, tb), 0.0);
    float t1 = max(ta, tb);
    if (t1 <= t0)
        return false;

    const int steps = 24;
    float prev_t = t0;
    float3 prev_p = ro + rd * prev_t;
    float prev_f = prev_p.y - (floor_y + rock_height_world(prev_p.xz));
    if (prev_f <= 0.0)
    {
        p = prev_p;
        t_hit = prev_t;
        return true;
    }

    [loop]
    for (int i = 1; i <= steps; i++)
    {
        float t = lerp(t0, t1, (float)i / (float)steps);
        float3 q = ro + rd * t;
        float f = q.y - (floor_y + rock_height_world(q.xz));
        if (f <= 0.0)
        {
            float lo = prev_t;
            float hi = t;
            [unroll]
            for (int b = 0; b < 4; b++)
            {
                float mid = (lo + hi) * 0.5;
                float3 m = ro + rd * mid;
                float mf = m.y - (floor_y + rock_height_world(m.xz));
                if (mf > 0.0) lo = mid; else hi = mid;
            }
            t_hit = hi;
            p = ro + rd * t_hit;
            return true;
        }
        prev_t = t;
        prev_f = f;
    }
    return false;
}

void floor_material(float3 p, out float3 albedo, out float3 normal, out float roughness)
{
    float scale = max(VolumeFloorTuning.y, 0.01);
    float2 uv = p.xz * scale;
    float2 warp = float2(fbm_2d(uv * 0.45 + 2.0), fbm_2d(uv * 0.52 - 19.0)) - 0.5;
    uv += warp * 0.75;
    float strata = fbm_2d(uv * 0.64);
    float dirt = fbm_2d(uv * 2.6 + 13.0);
    float fine = fbm_2d(uv * 13.0 + 37.0);
    float pebbles = cellular_2d(uv * 11.0);
    float cracks = 1.0 - cellular_2d(uv * 3.7 + 4.0);
    float damp = smoothstep(0.50, 0.88, fbm_2d(uv * 0.48 - 17.0));

    float3 charcoal = float3(0.075, 0.070, 0.064);
    float3 ash = float3(0.40, 0.385, 0.34);
    float3 clay = float3(0.24, 0.17, 0.13);
    float3 dust = float3(0.62, 0.59, 0.51);
    albedo = lerp(charcoal, ash, strata);
    albedo = lerp(albedo, clay, dirt * 0.42);
    albedo = lerp(albedo, dust, smoothstep(0.58, 0.94, fine) * 0.30);
    albedo *= 0.62 + pebbles * 0.38 - damp * 0.16 - cracks * 0.18;
    albedo = max(albedo, 0.0);

    float eps = 0.035;
    float hx0 = rock_height_world(p.xz - float2(eps, 0.0));
    float hx1 = rock_height_world(p.xz + float2(eps, 0.0));
    float hz0 = rock_height_world(p.xz - float2(0.0, eps));
    float hz1 = rock_height_world(p.xz + float2(0.0, eps));
    normal = normalize(float3(-(hx1 - hx0) / (2.0 * eps), 1.0, -(hz1 - hz0) / (2.0 * eps)));
    roughness = saturate(VolumeFloorShapeTuning.z);
}

float3 apply_aerial_perspective(float3 color, float3 rd, float distance_hint, float volume_shadow_hint)
{
    float density = max(VolumeAtmosphereTuning.x, 0.0);
    if (density <= 0.0)
        return color;

    float height_falloff = max(VolumeAtmosphereTuning.y, 0.001);
    float height_term = exp(-max(CamPos.y, 0.0) * height_falloff);
    float fog = 1.0 - exp(-density * max(distance_hint, 0.0) * height_term);
    float3 sun_dir = normalize(-LightDir.xyz);
    float shaft = pow(saturate(dot(rd, sun_dir)), max(VolumeAtmosphereTuning.w, 1.0)) *
                  VolumeAtmosphereTuning.z * (1.0 - volume_shadow_hint);
    float3 fog_col = lerp(float3(0.45, 0.50, 0.55), LightColor.rgb, 0.45 + shaft);
    return lerp(color, fog_col, saturate(fog + shaft * 0.35));
}

float3 scene_background(float2 uv, float3 rd)
{
    float3 analytic_sky = physical_sky(rd);
    float3 sky_sample = CloudSkyTex.SampleLevel(LinearSampler, uv, 0).rgb;
    float3 sky = dot(sky_sample, 1.0.xxx) > 1e-5 ? sky_sample : analytic_sky;
    float distance_hint = 80.0;
    float shadow_hint = 0.0;
    float3 result = apply_aerial_perspective(sky, rd, distance_hint, shadow_hint);
    float floor_y = VolumeFloorTuning.x;
    float3 p;
    float t;
    if (!floor_heightfield_intersect(CamPos.xyz, rd, floor_y, p, t))
        return result;

    float half_size = max(VolumeFloorShapeTuning.x, 0.2);
    float2 dist_to_edge = half_size.xx - abs(p.xz);
    float inside = min(dist_to_edge.x, dist_to_edge.y);
    if (inside <= 0.0)
        return result;

    float edge = smoothstep(0.0, max(VolumeFloorShapeTuning.y, 0.01), inside);
    float3 n;
    float3 albedo;
    float roughness;
    floor_material(p, albedo, n, roughness);
    float3 light_dir = normalize(-LightDir.xyz);
    float ndl = saturate(dot(n, light_dir));
    float shadow = volume_floor_shadow(p);
    shadow_hint = 1.0 - shadow;
    distance_hint = t;
    float3 view_dir = normalize(CamPos.xyz - p);
    float3 half_vec = normalize(light_dir + view_dir);
    float spec_power = lerp(96.0, 18.0, roughness);
    float spec = pow(saturate(dot(n, half_vec)), spec_power) * (1.0 - roughness) * shadow;
    float3 ambient = albedo * lerp(0.045, 0.12, shadow);
    float3 diffuse = albedo * LightColor.rgb * LightDir.w * ndl * shadow;
    float3 floor_lit = ambient + diffuse + spec.xxx * LightColor.rgb * 0.35;
    float horizon_fade = saturate(t * 0.004);
    float3 finite_floor = lerp(sky, floor_lit, edge);
    result = apply_aerial_perspective(lerp(finite_floor, sky, horizon_fade), rd, t, shadow_hint * edge);
    return result;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float4 volume = VolumeHistoryTex.SampleLevel(LinearSampler, uv, 0);
    float alpha = saturate(volume.a * max(VolumeCompositeTuning.w, 0.0));
    float3 rd = sky_ray_from_uv(uv);
    float3 bg = scene_background(uv, rd);
    float3 color = lerp(bg, volume.rgb, alpha);
    return float4(color, 1.0);
}
