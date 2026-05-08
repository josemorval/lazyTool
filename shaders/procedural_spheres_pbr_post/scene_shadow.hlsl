// Shadow-only pass for procedural primitives. It transforms geometry into the
// selected shadow cascade and writes depth without a material pixel shader.
#include "scene_common.hlsl"

/*
    Depth-only shadow vertex shader for the procedural scene.

    The regular scene VS projects with ViewProj for the camera. The shadow
    prepass needs the same procedural world placement but must project with
    ShadowViewProj, which the engine swaps per cascade before drawing.
*/

struct ShadowVSOut
{
    float4 pos : SV_POSITION;
};

ShadowVSOut VSMain(VSIn v, uint instance_id : SV_InstanceID)
{
    ShadowVSOut o;
    SceneSurfaceVertex s = build_scene_surface_vertex(v, instance_id);
    o.pos = mul(ShadowViewProj, float4(s.worldPos, 1.0));
    return o;
}

float4 PSMain(ShadowVSOut i) : SV_Target
{
    return 0.0;
}
