// Procedural scene parameters shared by ground/sphere material passes. UserCB
// values drive material colors, roughness/metalness, patterns, and instancing.
#ifndef PROCEDURAL_SPHERES_PBR_POST_SCENE_COMMON_HLSL
#define PROCEDURAL_SPHERES_PBR_POST_SCENE_COMMON_HLSL

#include "common.hlsl"

cbuffer UserCB : register(b2)
{
    float4 MaterialA;       // Ground: dark tile. Spheres: low-noise palette color.
    float4 MaterialB;       // Ground: bright tile. Spheres: high-noise palette color.
    float4 MaterialParams;  // x metallic, y roughness, z pattern scale, w mode: 0 ground/object, 1 instanced spheres.
    float4 PatternParams;   // x contrast, y noise scale, z ridge amount, w palette phase.
    float4 InstanceGrid;    // x columns hint, y spacing, z base radius, w deterministic seed.
};

struct SceneSurfaceVertex
{
    float3 worldPos;
    float3 worldNor;
    float2 uv;
    float  instId;
    float3 localNor;
};

float3 procedural_sphere_center(uint instance_id, float radius)
{
    float id = (float)instance_id;
    float seed = InstanceGrid.w;
    float spacing = max(InstanceGrid.y, 0.1);

    // A sunflower/spiral layout gives an organic distribution with one compact
    // formula and no placement table. The square-root radius spreads instances
    // roughly evenly over the floor.
    float a = id * GOLDEN_ANGLE + seed;
    float ring = sqrt(id + 0.35);
    float2 jitter = float2(hash11(id * 9.17 + seed), hash11(id * 5.31 + seed + 4.0)) - 0.5;
    float2 xz = float2(cos(a), sin(a)) * ring * spacing * 0.72 + jitter * spacing * 0.16;

    // Lift each sphere so it rests on the top of the ground cube at y = 0.
    return float3(xz.x, radius + 0.012, xz.y);
}

float procedural_sphere_radius(uint instance_id)
{
    float seed = InstanceGrid.w;
    float base_radius = max(InstanceGrid.z, 0.05);
    float h = hash11((float)instance_id * 7.73 + seed * 2.0);
    return base_radius * lerp(0.72, 1.42, h);
}

SceneSurfaceVertex build_scene_surface_vertex(VSIn v, uint instance_id)
{
    SceneSurfaceVertex o;
    bool instanced_sphere_mode = MaterialParams.w > 0.5;

    if (instanced_sphere_mode)
    {
        float radius = procedural_sphere_radius(instance_id);
        float3 center = procedural_sphere_center(instance_id, radius);
        o.worldPos = center + v.pos * radius;
        o.worldNor = normalize(v.nor);
        o.localNor = normalize(v.nor);
        o.instId = (float)instance_id;
    }
    else
    {
        float4 world_pos = mul(World, float4(v.pos, 1.0));
        o.worldPos = world_pos.xyz;
        o.worldNor = normalize(mul(World, float4(v.nor, 0.0)).xyz);
        o.localNor = normalize(v.nor);
        o.instId = 0.0;
    }

    o.uv = v.uv;
    return o;
}

#endif
