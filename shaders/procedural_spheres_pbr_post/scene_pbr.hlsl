// Main procedural PBR pass. It shades the checker ground and instanced spheres,
// reads the cascaded shadow atlas, and outputs HDR color into the scene target.
#include "scene_common.hlsl"

/*
    Procedural PBR scene shader.

    One shader renders both the checkerboard ground cube and the instanced
    sphere cluster.  The command parameter MaterialParams.w selects the mode:

      0 = regular mesh/object transform from ObjectCB.World
      1 = procedural instanced sphere placement driven by SV_InstanceID

    This keeps the scene compact: one sphere mesh primitive is drawn many times,
    and all per-sphere position/radius/color variation is generated from stable
    hash functions inside the vertex/pixel shader.  No texture files are read.
*/

struct SceneVSOut
{
    float4 pos        : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 worldNor   : TEXCOORD1;
    float2 uv         : TEXCOORD2;
    float  instId     : TEXCOORD3;
    float3 localNor   : TEXCOORD4;
};

SceneVSOut VSMain(VSIn v, uint instance_id : SV_InstanceID)
{
    SceneVSOut o;
    SceneSurfaceVertex s = build_scene_surface_vertex(v, instance_id);
    o.worldPos = s.worldPos;
    o.worldNor = s.worldNor;
    o.localNor = s.localNor;
    o.instId = s.instId;
    o.uv = s.uv;
    o.pos = mul(ViewProj, float4(s.worldPos, 1.0));
    return o;
}

float checker_mask(float2 p)
{
    float2 cell = floor(p);
    float raw = fmod(cell.x + cell.y, 2.0);

    // Cheap anti-aliased grout: fade tile contrast near cell borders using
    // derivative width.  This avoids crawling when the camera pulls back.
    float2 f = frac(p);
    float edge_distance = min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y));
    float aa = max(fwidth(p.x) + fwidth(p.y), 0.0015);
    float grout_blend = smoothstep(0.0, aa * 1.75, edge_distance);
    return lerp(0.5, raw, grout_blend);
}

float sphere_pattern(float3 local_n, float3 world_pos, float instance_id)
{
    float scale = max(PatternParams.y, 0.1);
    float phase = PatternParams.w + instance_id * 0.37;
    float base = fbm3(local_n * scale + world_pos * 0.21 + phase);
    float fine = fbm3(local_n * scale * 3.1 + phase * 1.7);
    float ridges = 1.0 - abs(base * 2.0 - 1.0);
    ridges = pow(saturate(ridges), max(PatternParams.z, 0.1));

    float veins = smoothstep(0.42, 0.74, fine + ridges * 0.35);
    float contrast = max(PatternParams.x, 0.1);
    return pow(saturate(lerp(base, veins, 0.48)), contrast);
}

float4 PSMain(SceneVSOut i) : SV_Target
{
    bool sphere_mode = MaterialParams.w > 0.5;
    float3 n = normalize(i.worldNor);
    float3 v = normalize(CamPos.xyz - i.worldPos);

    float metallic = saturate(MaterialParams.x);
    float roughness = clamp(MaterialParams.y, 0.045, 1.0);
    float3 albedo = 0.0;

    if (sphere_mode)
    {
        float mask = sphere_pattern(normalize(i.localNor), i.worldPos, i.instId);
        float3 palette = lerp(MaterialA.rgb, MaterialB.rgb, mask);

        // Add a second, softer octave as color marbling rather than geometry.
        float warm = fbm3(i.worldPos * 0.65 + i.instId * 1.31);
        albedo = palette * lerp(0.72, 1.22, warm);
        roughness = clamp(roughness * lerp(0.82, 1.28, mask), 0.05, 0.95);
    }
    else
    {
        float scale = max(MaterialParams.z, 0.05);
        float mask = checker_mask(i.worldPos.xz * scale);
        albedo = lerp(MaterialA.rgb, MaterialB.rgb, mask);

        // Subtle procedural dirt/grout variation, still purely analytic.
        float dirt = fbm3(float3(i.worldPos.xz * 0.9, 0.0));
        albedo *= lerp(0.88, 1.05, dirt);
    }

    float3 direct = evaluate_pbr_direct(i.worldPos, n, v, albedo, metallic, roughness);

    // The requested sky is black, so there is no environment map.  This tiny
    // ambient term represents camera exposure and local floor bounce; shadows
    // and SSAO still do the heavy grounding work.
    float horizon = saturate(n.y * 0.5 + 0.5);
    float3 ambient = albedo * lerp(0.010, 0.035, horizon) * (sphere_mode ? 0.9 : 1.15);
    float3 rim = pow(1.0 - saturate(dot(n, v)), 4.0) * LightColor.xyz * 0.025;

    return float4(max(direct + ambient + rim, 0.0), 1.0);
}
