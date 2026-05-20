#include "gs_common.hlsl"

// Pass 2: conservative camera culling and compact writing of visible splats.
//
// The output list is compact: SortPairs[0..visibleCount-1]. The allocation
// counter is explicit: it is the InstanceCount DWORD in the indirect draw
// argument buffer. It is just a normal integer updated with InterlockedAdd, so
// the state stays visible in the graph.
//
// Important: this pass must be more conservative than draw_splats.hlsl. The
// previous version culled from center + world radius and also rejected large
// raw radii. That missed real projected ellipses, which is exactly the kind of
// artifact that the no-cull reference exposes. This version estimates the same
// projected 2D Gaussian footprint used by the draw pass and only rejects splats
// whose full screen-space quad is outside the view.

StructuredBuffer<GaussianSplat> Splats : register(t0);
RWBuffer<uint> IndirectArgs : register(u0);
RWStructuredBuffer<SortPair> SortPairs : register(u1);

float2 gs_cull_safe_ndc_offset(float4 center_clip, float4 endpoint_clip)
{
    if (abs(endpoint_clip.w) <= 1e-6 || abs(center_clip.w) <= 1e-6)
        return float2(0.0, 0.0);

    return endpoint_clip.xy / endpoint_clip.w - center_clip.xy / center_clip.w;
}

void gs_cull_covariance_axes_ndc(GaussianSplat s, float3 center, float4 center_clip,
                                 out float2 axis0, out float2 axis1)
{
    float3 sc = abs(s.scale.xyz) * max(SplatTuning.x, 1e-5);

    float3 bx = gs_clamp_world_axis(gs_transform_vector(gs_quat_rotate(s.quat, float3(1.0, 0.0, 0.0)) * sc.x));
    float3 by = gs_clamp_world_axis(gs_transform_vector(gs_quat_rotate(s.quat, float3(0.0, 1.0, 0.0)) * sc.y));
    float3 bz = gs_clamp_world_axis(gs_transform_vector(gs_quat_rotate(s.quat, float3(0.0, 0.0, 1.0)) * sc.z));

    float2 px = gs_cull_safe_ndc_offset(center_clip, mul(ViewProj, float4(center + bx, 1.0)));
    float2 py = gs_cull_safe_ndc_offset(center_clip, mul(ViewProj, float4(center + by, 1.0)));
    float2 pz = gs_cull_safe_ndc_offset(center_clip, mul(ViewProj, float4(center + bz, 1.0)));

    float c00 = dot(float3(px.x, py.x, pz.x), float3(px.x, py.x, pz.x));
    float c01 = dot(float3(px.x, py.x, pz.x), float3(px.y, py.y, pz.y));
    float c11 = dot(float3(px.y, py.y, pz.y), float3(px.y, py.y, pz.y));

    float trace_half = 0.5 * (c00 + c11);
    float delta = sqrt(max((0.5 * (c00 - c11)) * (0.5 * (c00 - c11)) + c01 * c01, 0.0));
    float lambda0 = max(trace_half + delta, 1e-8);
    float lambda1 = max(trace_half - delta, 1e-8);

    float2 e0 = abs(c01) > 1e-7 ? normalize(float2(c01, lambda0 - c00)) : float2(1.0, 0.0);
    float2 e1 = float2(-e0.y, e0.x);

    float sigma_radius = max(SplatTuning.y, 0.5);
    float anisotropy = saturate(CullingTuning.w);
    float circular = sqrt(max(0.5 * (lambda0 + lambda1), 1e-8));
    float radius0 = lerp(circular, sqrt(lambda0), anisotropy);
    float radius1 = lerp(circular, sqrt(lambda1), anisotropy);

    axis0 = e0 * radius0 * sigma_radius;
    axis1 = e1 * radius1 * sigma_radius;
}

bool splat_survives_camera(GaussianSplat s, float3 world_pos)
{
    float4 clip = mul(ViewProj, float4(world_pos, 1.0));
    if (clip.w <= 1e-5)
        return false;

    float3 ndc = clip.xyz / clip.w;
    float2 axis0;
    float2 axis1;
    gs_cull_covariance_axes_ndc(s, world_pos, clip, axis0, axis1);

    float2 extent = abs(axis0) + abs(axis1);
    float pad = max(CullingTuning.x, 0.0);

    if (ndc.x + extent.x < -1.0 - pad || ndc.x - extent.x > 1.0 + pad)
        return false;
    if (ndc.y + extent.y < -1.0 - pad || ndc.y - extent.y > 1.0 + pad)
        return false;
    if (ndc.z < -pad || ndc.z > 1.0 + pad)
        return false;

    return true;
}

[numthreads(GS_THREADS, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint splat_count = 0u;
    uint stride = 0u;
    Splats.GetDimensions(splat_count, stride);
    if (id.x >= splat_count)
        return;

    GaussianSplat s = Splats[id.x];
    float3 world_pos = gs_world_pos(s);

    if (!splat_survives_camera(s, world_pos))
        return;

    uint pair_capacity = 0u;
    uint pair_stride = 0u;
    SortPairs.GetDimensions(pair_capacity, pair_stride);

    uint write_capacity = min(gs_clamp_capacity(MaxVisible), gs_clamp_capacity(SortCapacity));
    write_capacity = min(write_capacity, pair_capacity);

    uint slot = 0u;
    InterlockedAdd(IndirectArgs[GS_DRAW_INSTANCE_COUNT], 1u, slot);

    // Overflow is accounted for later in finalize_sort_args.hlsl. The counter
    // is allowed to exceed the writable list so the debug/stat value remains
    // meaningful, but writes are clamped to the actual pair buffer capacity.
    if (slot >= write_capacity)
        return;

    SortPair p;
    p.key = gs_depth_key(world_pos);
    p.index = id.x;
    SortPairs[slot] = p;
}
