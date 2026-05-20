#include "gs_common.hlsl"

// Final pass: procedural indirect draw.
//
// DrawInstancedIndirect supplies one instance per visible splat. The VS expands
// each instance into a camera-facing quad and the PS evaluates a Gaussian alpha
// falloff. The sorted pair list is already back-to-front, so normal alpha
// blending can composite the splats without any CPU intervention.

StructuredBuffer<GaussianSplat> Splats : register(t0);
StructuredBuffer<SortPair> SortPairs : register(t1);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 sigma_uv : TEXCOORD0;
    float4 color_opacity : TEXCOORD1;
    float2 close_info : TEXCOORD2; // x = camera-forward depth, y = projected NDC half-extent.
};

float2 quad_corner(uint vertex_id)
{
    // Two triangles: (-1,-1) (1,-1) (1,1), (-1,-1) (1,1) (-1,1).
    static const float2 kCorners[6] =
    {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0,  1.0)
    };
    return kCorners[vertex_id % 6u];
}

float2 safe_ndc_offset(float4 center_clip, float4 endpoint_clip)
{
    if (abs(endpoint_clip.w) <= 1e-6 || abs(center_clip.w) <= 1e-6)
        return float2(0.0, 0.0);

    return endpoint_clip.xy / endpoint_clip.w - center_clip.xy / center_clip.w;
}

void covariance_axes_ndc(GaussianSplat s, float3 center, float4 center_clip, out float2 axis0, out float2 axis1)
{
    float3 sc = abs(s.scale.xyz) * max(SplatTuning.x, 1e-5);

    // Rotate the three Gaussian basis vectors into world space, project those
    // endpoints, then build the 2D covariance directly in NDC. Keeping the
    // final quad screen-space avoids perspective/shear artifacts on large or
    // strongly anisotropic splats.
    float3 bx = gs_clamp_world_axis(gs_transform_vector(gs_quat_rotate(s.quat, float3(1.0, 0.0, 0.0)) * sc.x));
    float3 by = gs_clamp_world_axis(gs_transform_vector(gs_quat_rotate(s.quat, float3(0.0, 1.0, 0.0)) * sc.y));
    float3 bz = gs_clamp_world_axis(gs_transform_vector(gs_quat_rotate(s.quat, float3(0.0, 0.0, 1.0)) * sc.z));

    float2 px = safe_ndc_offset(center_clip, mul(ViewProj, float4(center + bx, 1.0)));
    float2 py = safe_ndc_offset(center_clip, mul(ViewProj, float4(center + by, 1.0)));
    float2 pz = safe_ndc_offset(center_clip, mul(ViewProj, float4(center + bz, 1.0)));

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

VSOut VSMain(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    SortPair pair = SortPairs[instance_id];
    GaussianSplat s = Splats[pair.index];

    float3 center = gs_world_pos(s);
    float4 center_clip = mul(ViewProj, float4(center, 1.0));
    float view_depth = dot(center - CamPos.xyz, normalize(CamDir.xyz));
    float2 axis0;
    float2 axis1;
    covariance_axes_ndc(s, center, center_clip, axis0, axis1);
    float2 extent = abs(axis0) + abs(axis1);

    float2 corner = quad_corner(vertex_id);
    float2 ndc_offset = axis0 * corner.x + axis1 * corner.y;

    VSOut o;
    o.pos = center_clip + float4(ndc_offset * center_clip.w, 0.0, 0.0);
    o.sigma_uv = corner * max(SplatTuning.y, 0.5);
    o.color_opacity = float4(s.color.rgb * ToneTuning.rgb * max(ToneTuning.w, 0.0), gs_opacity(s));
    o.close_info = float2(view_depth, max(extent.x, extent.y));
    return o;
}

float pixel_close_fade(float2 sigma_uv, float2 close_info, out float alpha_scale)
{
    alpha_scale = 1.0;
    float near_z = max(ShadowParams.y, 1e-5);
    if (close_info.x <= near_z)
    {
        alpha_scale = 0.0;
        return 0.0;
    }

    float fade_start = CloseTuning.x;
    float fade_end = CloseTuning.y;
    float extent_threshold = CloseTuning.z;
    float strength = saturate(CloseTuning.w);
    if (fade_end <= fade_start + 1e-5 || extent_threshold <= 0.0 || strength <= 0.0)
        return 1.0;

    float view_depth = close_info.x;
    float projected_extent = close_info.y;
    float close_amount = 1.0 - smoothstep(fade_start, fade_end, view_depth);
    float large_amount = saturate((projected_extent - extent_threshold) / max(extent_threshold, 1e-4));
    float amount = close_amount * large_amount * strength;
    if (amount <= 0.0)
        return 1.0;

    // Huge near-camera splats are the source of the white/beige washout: their
    // tails cover most pixels and many alpha-blended layers accumulate. Scale
    // their whole per-pixel contribution by projected size, but smoothly and
    // only in the close range. This avoids changing the quad geometry.
    float extent_scale = saturate(extent_threshold / max(projected_extent, extent_threshold));
    float area_scale = extent_scale * extent_scale;
    alpha_scale = lerp(1.0, area_scale, amount);

    // Fade the Gaussian tail, not the quad footprint. The center stays stable,
    // so this avoids the visible holes caused by vertex-level close clipping.
    float sigma_radius = max(SplatTuning.y, 0.5);
    float r = length(sigma_uv);
    float edge_start = lerp(sigma_radius * 0.92, sigma_radius * 0.45, amount);
    float tail = smoothstep(edge_start, sigma_radius, r);
    return lerp(1.0, 1.0 - tail, amount);
}

float4 PSMain(VSOut i) : SV_Target
{
    float r2 = dot(i.sigma_uv, i.sigma_uv);
    float close_alpha_scale = 1.0;
    float tail_fade = pixel_close_fade(i.sigma_uv, i.close_info, close_alpha_scale);
    float alpha = i.color_opacity.a * exp(-0.5 * r2) * close_alpha_scale * tail_fade;
    if (alpha <= max(SplatTuning.z, 0.0))
        discard;

    return float4(saturate(i.color_opacity.rgb), saturate(alpha));
}
