#ifndef DEMO64K_PROCEDURAL_MATERIALS_HLSL
#define DEMO64K_PROCEDURAL_MATERIALS_HLSL

#include "pbr_common.hlsl"

float demo64k_hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

float demo64k_noise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = demo64k_hash13(i + float3(0,0,0));
    float n100 = demo64k_hash13(i + float3(1,0,0));
    float n010 = demo64k_hash13(i + float3(0,1,0));
    float n110 = demo64k_hash13(i + float3(1,1,0));
    float n001 = demo64k_hash13(i + float3(0,0,1));
    float n101 = demo64k_hash13(i + float3(1,0,1));
    float n011 = demo64k_hash13(i + float3(0,1,1));
    float n111 = demo64k_hash13(i + float3(1,1,1));

    float nx00 = lerp(n000, n100, f.x);
    float nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x);
    float nx11 = lerp(n011, n111, f.x);
    return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}

float demo64k_fbm(float3 p)
{
    float a = 0.5;
    float n = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        n += demo64k_noise3(p) * a;
        p = p * 2.13 + 17.1;
        a *= 0.5;
    }
    return n;
}

float demo64k_ridge(float3 p)
{
    float n = demo64k_fbm(p);
    return 1.0 - abs(n * 2.0 - 1.0);
}

float3 demo64k_cheap_bump(float3 normal_ws, float3 world_pos, float scale, float amount)
{
    float e = 0.032;
    float3 p = world_pos * scale;
    float3 g;
    g.x = demo64k_fbm(p + float3(e,0,0)) - demo64k_fbm(p - float3(e,0,0));
    g.y = demo64k_fbm(p + float3(0,e,0)) - demo64k_fbm(p - float3(0,e,0));
    g.z = demo64k_fbm(p + float3(0,0,e)) - demo64k_fbm(p - float3(0,0,e));
    return lt_safe_normalize(normal_ws - g * amount);
}

float demo64k_stripe(float3 p, float3 n, float freq)
{
    float3 an = pow(abs(n), 4.0);
    an /= max(an.x + an.y + an.z, 1e-4);
    float sx = 0.5 + 0.5 * sin((p.y + p.z * 0.23) * freq);
    float sy = 0.5 + 0.5 * sin((p.x + p.z * 0.17) * freq);
    float sz = 0.5 + 0.5 * sin((p.x + p.y * 0.19) * freq);
    return sx * an.x + sy * an.y + sz * an.z;
}

float demo64k_soft_panel_line(float3 p, float3 n)
{
    // Subtle triplanar construction seams, less repetitive than the previous broad floor stripes.
    float3 an = pow(abs(n), 5.0);
    an /= max(an.x + an.y + an.z, 1e-4);
    float2 gx = abs(frac(p.yz * 0.75) - 0.5);
    float2 gy = abs(frac(p.xz * 0.75) - 0.5);
    float2 gz = abs(frac(p.xy * 0.75) - 0.5);
    float lx = 1.0 - smoothstep(0.018, 0.065, min(gx.x, gx.y));
    float ly = 1.0 - smoothstep(0.018, 0.065, min(gy.x, gy.y));
    float lz = 1.0 - smoothstep(0.018, 0.065, min(gz.x, gz.y));
    return lx * an.x + ly * an.y + lz * an.z;
}

PBRMaterial demo64k_make_material(int material_id, float3 world_pos, float3 normal_ws, float4 tuning)
{
    PBRMaterial m;
    m.albedo = float3(0.5, 0.5, 0.5);
    m.roughness = 0.5;
    m.metalness = 0.0;
    m.emissive = float3(0.0, 0.0, 0.0);

    float detail_strength = max(tuning.z, 0.0);
    float n1 = demo64k_fbm(world_pos * 1.45);
    float n2 = demo64k_fbm(world_pos * 6.8 + normal_ws * 2.0);
    float nFine = demo64k_fbm(world_pos * 28.0 + 4.0 * n1);
    float stripe = demo64k_stripe(world_pos, normal_ws, 26.0);

    if (material_id == 1) {
        // Brushed gold: metal base with fine anisotropic bands and controlled roughness.
        float brushed = 0.78 + 0.22 * demo64k_stripe(world_pos * float3(1.2, 0.28, 1.0), normal_ws, 64.0);
        m.albedo = float3(1.0, 0.68, 0.26) * brushed;
        m.metalness = 1.0;
        m.roughness = lerp(0.13, 0.27, n2) + 0.035 * stripe;
    } else if (material_id == 2) {
        // Dark obsidian: smoky polished stone, readable under sky IBL.
        float veins = smoothstep(0.60, 0.96, demo64k_ridge(world_pos * 3.0 + 9.0));
        m.albedo = lerp(float3(0.010, 0.009, 0.015), float3(0.070, 0.055, 0.085), n1);
        m.albedo += veins * float3(0.030, 0.020, 0.045);
        m.metalness = 0.0;
        m.roughness = lerp(0.10, 0.24, n2);
    } else if (material_id == 3) {
        // White ceramic: warm glaze with tiny roughness waves.
        float glaze = 0.92 + 0.08 * nFine;
        m.albedo = lerp(float3(0.76, 0.73, 0.67), float3(1.0, 0.96, 0.88), n1) * glaze;
        m.metalness = 0.0;
        m.roughness = lerp(0.20, 0.34, n2);
    } else if (material_id == 4) {
        // Oxidized copper: patina follows both noise and soft triplanar bands.
        float patina = smoothstep(0.43, 0.84, n1 + 0.18 * stripe + 0.10 * demo64k_ridge(world_pos * 4.0));
        float3 copper = float3(0.94, 0.45, 0.22);
        float3 oxide = float3(0.04, 0.44, 0.37);
        m.albedo = lerp(copper, oxide, patina);
        m.metalness = lerp(1.0, 0.10, patina);
        m.roughness = lerp(0.18, 0.70, patina);
    } else if (material_id == 5) {
        // Procedural concrete: layered grain, panel seams and chipped aggregate.
        float grain = demo64k_fbm(world_pos * 2.6) * 0.55 + demo64k_fbm(world_pos * 18.0) * 0.45;
        float chips = smoothstep(0.72, 0.92, demo64k_ridge(world_pos * 9.0 + 2.0));
        float seams = demo64k_soft_panel_line(world_pos, normal_ws);
        float shade = 0.72 + 0.42 * grain - 0.14 * chips - 0.10 * seams;
        m.albedo = float3(0.40, 0.39, 0.36) * shade + seams * float3(0.045, 0.040, 0.035);
        m.metalness = 0.0;
        m.roughness = lerp(0.68, 0.93, grain) + seams * 0.04;
    } else if (material_id == 6) {
        // Neon emitter: small procedural source for bloom and lens flare.
        float pulse = 0.88 + 0.12 * sin(TimeVec.x * 2.2 + world_pos.x * 1.7);
        float3 neon = lerp(float3(0.05, 0.85, 1.0), float3(1.0, 0.20, 0.95), saturate(world_pos.y * 0.15 + 0.35));
        m.albedo = neon * 0.12;
        m.metalness = 0.0;
        m.roughness = 0.16;
        m.emissive = neon * (5.8 * max(tuning.w, 0.0)) * pulse;
    }

    m.roughness = saturate(m.roughness * max(tuning.x, 0.05));
    m.metalness = saturate(m.metalness * max(tuning.y, 0.0));
    m.albedo = saturate(m.albedo * (1.0 + detail_strength * (nFine - 0.5) * 0.09));
    return m;
}

float3 demo64k_material_normal(int material_id, float3 normal_ws, float3 world_pos, float4 tuning)
{
    float detail_strength = max(tuning.z, 0.0);
    float scale = (material_id == 5) ? 9.0 : 18.0;
    float amount = (material_id == 5) ? 0.42 : 0.16;
    if (material_id == 1) amount = 0.10;
    if (material_id == 6) amount = 0.04;
    return demo64k_cheap_bump(normal_ws, world_pos, scale, amount * detail_strength);
}

#endif
