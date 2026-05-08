#include "common.hlsl"

/*
    Screen-space ambient occlusion.

    The shader reconstructs world positions from the editor depth buffer and
    compares nearby samples in a rotated spiral pattern.  It is not a full GTAO
    implementation, but it uses enough samples, depth-aware range checks and a
    later bilateral blur to give stable contact shadows under the spheres.
*/

Texture2D DepthTex : register(t0);

cbuffer UserCB : register(b2)
{
    float4 SSAOParams;   // x world radius, y strength, z bias, w sample scale.
    float4 SSAOParams2;  // x final power, y max screen radius, z noise amount, w unused.
};

PostVSOut VSMain(VSIn v)
{
    return make_fullscreen_vertex(v);
}

float4 PSMain(PostVSOut i) : SV_Target
{
    uint w = 0;
    uint h = 0;
    DepthTex.GetDimensions(w, h);
    float2 texel = 1.0 / float2((float)w, (float)h);

    float depth = DepthTex.SampleLevel(LinearSampler, i.uv, 0).r;
    if (depth >= 0.99995)
        return float4(1.0, 1.0, 1.0, 1.0);

    float3 p = reconstruct_world_from_depth(i.uv, depth);
    float center_view = max(view_depth_from_world(p), 0.05);

    float depth_x = DepthTex.SampleLevel(LinearSampler, i.uv + float2(texel.x, 0), 0).r;
    float depth_y = DepthTex.SampleLevel(LinearSampler, i.uv + float2(0, texel.y), 0).r;
    float3 px = reconstruct_world_from_depth(i.uv + float2(texel.x, 0), depth_x);
    float3 py = reconstruct_world_from_depth(i.uv + float2(0, texel.y), depth_y);
    float3 n = normalize(cross(py - p, px - p));
    if (dot(n, CamDir.xyz) > 0.0)
        n = -n;

    const int sample_count = 24;
    float radius = max(SSAOParams.x, 0.01);
    float strength = max(SSAOParams.y, 0.0);
    float bias = max(SSAOParams.z, 0.0);
    float screen_radius = saturate((radius / center_view) * max(SSAOParams.w, 0.1));
    screen_radius = min(screen_radius, max(SSAOParams2.y, 0.01));

    float rot = hash21(i.uv * float2((float)w, (float)h) + TimeVec.xx) * 6.2831853;
    float occ = 0.0;
    float weight_sum = 0.0;

    [unroll]
    for (int s = 0; s < sample_count; s++)
    {
        float u = ((float)s + 0.5) / (float)sample_count;
        float a = rot + (float)s * GOLDEN_ANGLE;
        float r = sqrt(u);
        float2 dir = float2(cos(a), sin(a));
        float2 suv = i.uv + dir * r * screen_radius;

        if (suv.x <= 0.0 || suv.x >= 1.0 || suv.y <= 0.0 || suv.y >= 1.0)
            continue;

        float sd = DepthTex.SampleLevel(LinearSampler, suv, 0).r;
        if (sd >= 0.99995)
            continue;

        float3 sp = reconstruct_world_from_depth(suv, sd);
        float sample_view = view_depth_from_world(sp);
        float dz = center_view - sample_view;
        float dist = length(sp - p);
        float range = saturate(1.0 - dist / radius);
        float facing = saturate(dot(n, normalize(sp - p)) * 0.5 + 0.5);
        float nearer = smoothstep(bias, radius * 0.42, dz);
        float wgt = range * lerp(0.65, 1.0, facing);
        occ += nearer * wgt;
        weight_sum += wgt;
    }

    occ = occ / max(weight_sum, 1e-4);
    float ao = pow(saturate(1.0 - occ * strength), max(SSAOParams2.x, 0.2));
    return float4(ao, ao, ao, 1.0);
}
