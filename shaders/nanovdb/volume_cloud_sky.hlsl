// Half-res procedural sky/cloud pass for the NanoVDB sample scene.
//
// The pass is deliberately separate from the final composite so the expensive
// cloud raymarch runs at the render-texture resolution, then the composite
// upsamples it with bilinear filtering. The model is procedural and local:
// analytic daylight + a bounded volumetric cloud slab with layered noise,
// vertical shaping and short light probes toward the sun.
//
// Visual target: local volumetric clouds in the spirit of HDRP/URP examples
// such as jiaozi158/UnityVolumetricCloudsURP. This file does not copy that
// implementation; it uses the same common ingredients: weather/shape noise,
// Worley erosion, height gradients and low-count light marching.
#define LT_NO_DEFAULT_SHADOWMAP
#include "nanovdb_common.hlsl"

cbuffer UserCB : register(b2)
{
    float4 VolumeCompositeTuning; // rgb: ground fallback tint.
    float4 VolumeSkyTuning;       // x: sky intensity, y: turbidity, z: sun disk intensity, w: sun disk sharpness.
    float4 VolumeAtmosphereTuning; // x: fog density, y: height falloff, z: god-ray strength, w: god-ray sharpness.
    float4 VolumeCloudTuning;     // x: coverage, y: density, z: base altitude, w: noise scale.
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

float hg_phase_cloud(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-3);
    return (1.0 - g2) / max(4.0 * LT_PI * pow(denom, 1.5), 1e-3);
}

float3 sky_ray_from_uv_cloud(float2 uv)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    float4 world = mul(InvViewProj, clip);
    float3 pos = world.xyz / max(abs(world.w), 1e-5);
    return normalize(pos - CamPos.xyz);
}

float value_noise_2d_cloud(float2 p)
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

float fbm_2d_cloud(float2 p)
{
    float n = 0.0;
    float a = 0.5;
    float2 q = p;
    [unroll]
    for (int i = 0; i < 5; i++)
    {
        n += value_noise_2d_cloud(q) * a;
        q = q * 2.03 + 17.1;
        a *= 0.5;
    }
    return n;
}

float hash13_cloud(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

float value_noise_3d_cloud(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    float n000 = hash13_cloud(i + float3(0,0,0));
    float n100 = hash13_cloud(i + float3(1,0,0));
    float n010 = hash13_cloud(i + float3(0,1,0));
    float n110 = hash13_cloud(i + float3(1,1,0));
    float n001 = hash13_cloud(i + float3(0,0,1));
    float n101 = hash13_cloud(i + float3(1,0,1));
    float n011 = hash13_cloud(i + float3(0,1,1));
    float n111 = hash13_cloud(i + float3(1,1,1));
    float x00 = lerp(n000, n100, u.x);
    float x10 = lerp(n010, n110, u.x);
    float x01 = lerp(n001, n101, u.x);
    float x11 = lerp(n011, n111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

float fbm_3d_cloud(float3 p)
{
    float n = 0.0;
    float a = 0.5;
    float3 q = p;
    [unroll]
    for (int i = 0; i < 4; i++)
    {
        n += value_noise_3d_cloud(q) * a;
        q = q * 2.07 + 13.7;
        a *= 0.5;
    }
    return n;
}

float worley_3d_cloud(float3 p)
{
    float3 ip = floor(p);
    float3 fp = frac(p);
    float d = 8.0;
    [unroll]
    for (int z = 0; z <= 1; z++)
    {
        [unroll]
        for (int y = 0; y <= 1; y++)
        {
            [unroll]
            for (int x = 0; x <= 1; x++)
            {
                float3 g = float3((float)x, (float)y, (float)z);
                float3 cell = ip + g;
                float3 o = float3(hash13_cloud(cell + 0.0),
                                  hash13_cloud(cell + 17.0),
                                  hash13_cloud(cell + 37.0));
                float3 r = g + o - fp;
                d = min(d, dot(r, r));
            }
        }
    }
    return saturate(sqrt(d) * 0.92);
}

float worley_fbm_cloud(float3 p)
{
    float w0 = 1.0 - worley_3d_cloud(p);
    float w1 = 1.0 - worley_3d_cloud(p * 2.03 + 11.0);
    float w2 = 1.0 - worley_3d_cloud(p * 4.07 - 23.0);
    return saturate(w0 * 0.62 + w1 * 0.28 + w2 * 0.10);
}

float3 physical_sky_cloud(float3 rd)
{
    float3 sun_dir = normalize(-LightDir.xyz);
    float mu = dot(rd, sun_dir);
    float view_up = saturate(rd.y * 0.5 + 0.5);
    float air_mass = 1.0 / max(rd.y + 0.18, 0.06);
    float turbidity = max(VolumeSkyTuning.y, 0.1);

    float3 beta_r = float3(5.8e-3, 13.5e-3, 33.1e-3);
    float3 beta_m = 21.0e-3.xxx * turbidity;
    float phase_r = 3.0 / (16.0 * LT_PI) * (1.0 + mu * mu);
    float phase_m = hg_phase_cloud(mu, 0.76);
    float3 extinction = exp(-(beta_r + beta_m) * air_mass * 24.0);
    float3 inscatter = (beta_r * phase_r + beta_m * phase_m) *
                       (1.0 - extinction) / max(beta_r + beta_m, 1e-4.xxx);

    float horizon = pow(saturate(1.0 - abs(rd.y)), 4.0);
    float3 horizon_tint = float3(1.0, 0.48, 0.16) * horizon * 0.24 * turbidity;
    float sun_disk = pow(saturate(mu), max(VolumeSkyTuning.w, 16.0));
    float3 sun = LightColor.rgb * LightDir.w * VolumeSkyTuning.z * sun_disk;
    float3 ground = VolumeCompositeTuning.rgb * smoothstep(0.0, -0.08, rd.y);
    float sunset = smoothstep(-0.05, 0.18, sun_dir.y);
    float3 warm_low_sky = float3(1.0, 0.32, 0.07) * horizon * (1.0 - sunset) * 0.24;
    return max((inscatter * lerp(0.55, 1.0, view_up) + horizon_tint) *
               max(VolumeSkyTuning.x, 0.0) + sun + ground + warm_low_sky, 0.0);
}

float cloud_density_at_cloud(float3 p, float cloud_bottom, float cloud_top)
{
    float thickness = max(cloud_top - cloud_bottom, 0.1);
    float h = saturate((p.y - cloud_bottom) / thickness);
    float profile = smoothstep(0.0, 0.16, h) * (1.0 - smoothstep(0.70, 1.0, h));
    if (profile <= 0.0)
        return 0.0;

    float scale = max(VolumeCloudTuning.w, 0.01);
    float3 q = p * scale;
    float wind = TimeVec.x * 0.018;
    float3 wind3 = float3(wind, wind * 0.18, wind * 0.37);
    float weather = fbm_2d_cloud(q.xz * 0.30 + float2(wind, wind * 0.28));
    float shape = fbm_3d_cloud(q * 0.82 + wind3);
    float billow = fbm_3d_cloud(q * 1.55 + wind3 * 1.7 + 9.0);
    float vertical = fbm_2d_cloud(q.xy * 1.35 + q.zy * 0.22 + 23.0);
    float worley = worley_fbm_cloud(q * 1.35 + wind3 * 2.3);
    float detail_worley = worley_fbm_cloud(q * 3.6 - 31.0 + wind3 * 5.0);

    float coverage = saturate(VolumeCloudTuning.x);
    float body = shape * 0.62 + billow * 0.48 + weather * 0.58 + vertical * 0.18;
    float eroded = body - (1.0 - worley) * 0.16 - (1.0 - detail_worley) * 0.06;
    float anvil = smoothstep(0.38, 0.82, h) * 0.18;
    float density = saturate((eroded + anvil - coverage) * VolumeCloudTuning.y);
    return smoothstep(0.005, 0.62, density) * profile;
}

float3 render_clouds(float2 screen_uv, float3 rd, float3 sky)
{
    if (VolumeCloudTuning.y <= 0.0 || rd.y <= 0.045)
        return sky;

    float cloud_bottom = max(VolumeCloudTuning.z, CamPos.y + 0.6);
    float cloud_top = cloud_bottom + max(2.8, cloud_bottom * 0.48);
    float ta = (cloud_bottom - CamPos.y) / rd.y;
    float tb = (cloud_top - CamPos.y) / rd.y;
    float t0 = max(min(ta, tb), 0.0);
    float t1 = min(max(ta, tb), 180.0);
    if (t1 <= t0)
        return sky;

    float3 sun_dir = normalize(-LightDir.xyz);
    float mu = dot(rd, sun_dir);
    float phase = hg_phase_cloud(mu, 0.55) * 1.9 + 0.16;
    float jitter = lt_hash12(screen_uv * float2(1739.0, 971.0) + TimeVec.xx * 17.0);
    const int steps = 32;
    float dt = (t1 - t0) / (float)steps;
    float trans = 1.0;
    float3 accum = 0.0.xxx;

    [loop]
    for (int i = 0; i < steps; i++)
    {
        float t = t0 + ((float)i + jitter) * dt;
        float3 p = CamPos.xyz + rd * t;
        float density = cloud_density_at_cloud(p, cloud_bottom, cloud_top);
        if (density <= 0.001)
            continue;

        float alpha = 1.0 - exp(-density * dt * 1.15);
        float forward = pow(saturate(mu), 4.0);
        float edge = pow(saturate(1.0 - abs(mu)), 3.0);
        float height = saturate((p.y - cloud_bottom) / max(cloud_top - cloud_bottom, 0.1));
        float self_shadow = exp(-density * lerp(1.95, 0.70, height));
        float3 ambient = lerp(float3(0.15, 0.14, 0.14), sky, 0.11 + height * 0.11);
        float3 sunlight = LightColor.rgb * LightDir.w * self_shadow * phase;
        float powder = 1.0 - exp(-density * 2.2);
        float3 sunset_tint = lerp(LightColor.rgb, float3(1.0, 0.38, 0.10), 0.35);
        float3 cloud_col = ambient * lerp(0.30, 0.56, powder) +
                           sunlight * (0.48 + height * 0.22) +
                           sunset_tint * edge * self_shadow * 0.36 +
                           forward.xxx * 0.05;
        accum += trans * alpha * cloud_col;
        trans *= 1.0 - alpha;
        if (trans < 0.025)
            break;
    }

    float horizon_fade = smoothstep(0.045, 0.16, rd.y);
    return lerp(sky, accum + sky * trans, horizon_fade);
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float3 rd = sky_ray_from_uv_cloud(uv);
    float3 sky = physical_sky_cloud(rd);
    return float4(render_clouds(uv, rd, sky), 1.0);
}
