// Shared declarations for the self-contained full-GPU radix Gaussian splat viewer.
//
// CPU work stops at loading the PLY once. Every frame after that is:
// reset -> camera cull -> indirect args -> GPU radix sort -> indirect draw.
//
// Capacity is intentionally project/resource driven. The shaders query buffer
// dimensions and clamp to the allocated buffers, so changing PLY size should not
// require editing HLSL constants. The included project allocates enough scratch
// space for 8,000,000 visible splats.

#ifndef GS_RADIX_COMMON_HLSL
#define GS_RADIX_COMMON_HLSL

cbuffer SceneCB : register(b0)
{
    float4x4 ViewProj;
    float4 TimeVec;
    float4 LightDir;
    float4 LightColor;
    float4 CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4x4 PrevShadowViewProj;
    float4 CamDir;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;          // y = camera near, z = camera far in lazyTool.
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

cbuffer UserCB : register(b2)
{
    // Runtime clamp controlled from the editor. The actual hard ceiling is the
    // allocated size of gs_sort_pairs_a/b and the radix auxiliary buffers.
    int MaxVisible;

    // Runtime sort clamp controlled from the editor. Use this to temporarily
    // lower cost without resizing buffers. It is clamped to the real buffers.
    int SortCapacity;

    // xyz translates the PLY in world space, w scales it uniformly. This is
    // the cheap scene transform knob for trying different PLYs.
    float4 SceneOffsetScale;

    // xyz scales individual PLY axes before the uniform scale above.
    // Negative values flip an axis. w is reserved.
    float4 SceneAxisScale;

    // x = splat radius multiplier.
    // y = sigma radius drawn by the quad. 3 means the quad edge is 3 sigma.
    // z = alpha cutoff used by PS discard.
    // w = opacity multiplier.
    float4 SplatTuning;

    // x = NDC frustum padding.
    // y = minimum world-space radius after scale.
    // z = optional maximum world-space one-sigma radius used as draw clamp.
    // w = anisotropy amount: 0 circular/smooth, 1 full projected covariance.
    float4 CullingTuning;

    // xyz = color tint, w = exposure-like brightness multiplier.
    float4 ToneTuning;

    // x = close fade start distance in world units.
    // y = close fade end distance in world units.
    // z = projected NDC half-extent where per-pixel tail fade starts.
    // w = fade strength.
    float4 CloseTuning;
};

struct GaussianSplat
{
    float4 pos_opacity; // xyz = center, w = opacity after CPU sigmoid decode.
    float4 quat;        // xyzw quaternion.
    float4 scale;       // xyz = Gaussian stddev-like scale.
    float4 color;       // rgb = decoded DC color, a = opacity duplicate.
};

struct SortPair
{
    uint key;   // Sort ascending. Smaller key is farther from camera.
    uint index; // Index into the GaussianSplat buffer.
};

// Layout of gs_indirect_args. It is a StructuredBuffer resource flagged as
// Indirect Args so DX11 accepts it in DrawInstancedIndirect/DispatchIndirect.
#define GS_DRAW_VERTEX_COUNT        0u
#define GS_DRAW_INSTANCE_COUNT      1u
#define GS_DRAW_START_VERTEX        2u
#define GS_DRAW_START_INSTANCE      3u

// DispatchIndirect for per-sort-group passes: histogram and scatter.
// Offset 16 bytes => element 4.
#define GS_DISPATCH_SORT_GROUPS_X   4u
#define GS_DISPATCH_SORT_GROUPS_Y   5u
#define GS_DISPATCH_SORT_GROUPS_Z   6u

// DispatchIndirect for per-scan-block passes: group-count scan and add-offsets.
// Offset 28 bytes => element 7.
#define GS_DISPATCH_SCAN_BLOCKS_X   7u
#define GS_DISPATCH_SCAN_BLOCKS_Y   8u
#define GS_DISPATCH_SCAN_BLOCKS_Z   9u

// Layout of gs_sort_state. It is a normal RWStructuredBuffer<uint>.
#define GS_STATE_VISIBLE_COUNT      0u
#define GS_STATE_SORT_COUNT         1u
#define GS_STATE_RADIX_SHIFT        2u
#define GS_STATE_SORT_GROUPS        3u
#define GS_STATE_DONE               4u
#define GS_STATE_RAW_VISIBLE        5u
#define GS_STATE_OVERFLOW           6u
#define GS_STATE_SCAN_BLOCKS        7u

#define GS_THREADS                  128u
#define GS_RADIX_BITS               4u
#define GS_RADIX_BINS               16u

// Each scan block scans this many radix sort groups for one bin. This is not a
// scene-size constant; it is the fixed tile size of the hierarchical scan.
#define GS_RADIX_SCAN_BLOCK_SIZE    1024u

// The second scan pass scans scan-block totals in one workgroup per bin. 128
// blocks covers 128 * 1024 * 128 = 16,777,216 splats, above the 8M project cap.
#define GS_RADIX_MAX_SCAN_BLOCKS    128u

#define GS_INVALID_KEY              0xffffffffu
#define GS_INVALID_INDEX            0xffffffffu

#if ((GS_RADIX_SCAN_BLOCK_SIZE & (GS_RADIX_SCAN_BLOCK_SIZE - 1u)) != 0u)
#error GS_RADIX_SCAN_BLOCK_SIZE must be a power of two.
#endif

#if ((GS_RADIX_MAX_SCAN_BLOCKS & (GS_RADIX_MAX_SCAN_BLOCKS - 1u)) != 0u)
#error GS_RADIX_MAX_SCAN_BLOCKS must be a power of two.
#endif

uint gs_clamp_capacity(int value)
{
    return (uint)max(value, 1);
}

uint gs_div_ceil(uint a, uint b)
{
    return (a + b - 1u) / b;
}

SortPair gs_invalid_pair()
{
    SortPair p;
    p.key = GS_INVALID_KEY;
    p.index = GS_INVALID_INDEX;
    return p;
}

float3 gs_world_pos(GaussianSplat s)
{
    return (s.pos_opacity.xyz * SceneAxisScale.xyz) * SceneOffsetScale.w + SceneOffsetScale.xyz;
}

float3 gs_world_scale(GaussianSplat s)
{
    float scene_scale = max(abs(SceneOffsetScale.w), 1e-5);
    float radius_scale = max(SplatTuning.x, 1e-5);
    float axis_scale = max(abs(SceneAxisScale.x), max(abs(SceneAxisScale.y), abs(SceneAxisScale.z)));
    axis_scale = max(axis_scale, 1e-5);
    float3 sc = abs(s.scale.xyz) * scene_scale * axis_scale * radius_scale;
    sc = max(sc, CullingTuning.yyy);
    if (CullingTuning.z > 0.0)
        sc = min(sc, CullingTuning.zzz);
    return sc;
}

float3 gs_unclamped_world_scale(GaussianSplat s)
{
    float scene_scale = max(abs(SceneOffsetScale.w), 1e-5);
    float radius_scale = max(SplatTuning.x, 1e-5);
    float axis_scale = max(abs(SceneAxisScale.x), max(abs(SceneAxisScale.y), abs(SceneAxisScale.z)));
    axis_scale = max(axis_scale, 1e-5);
    return abs(s.scale.xyz) * scene_scale * axis_scale * radius_scale;
}

float3 gs_transform_vector(float3 v)
{
    return (v * SceneAxisScale.xyz) * SceneOffsetScale.w;
}

float3 gs_clamp_world_axis(float3 v)
{
    if (CullingTuning.z <= 0.0)
        return v;

    float len = length(v);
    if (len <= CullingTuning.z || len <= 1e-8)
        return v;

    return v * (CullingTuning.z / len);
}

float gs_opacity(GaussianSplat s)
{
    return saturate(s.pos_opacity.w * max(SplatTuning.w, 0.0));
}

uint gs_depth_key(float3 world_pos)
{
    // Sort by signed camera-forward distance. For alpha blending we want
    // back-to-front rendering. The radix path sorts ascending, so far splats
    // get small keys and near splats get large keys.
    float near_z = max(ShadowParams.y, 1e-5);
    float far_z = max(ShadowParams.z, near_z + 1e-4);
    float view_depth = dot(world_pos - CamPos.xyz, normalize(CamDir.xyz));
    float depth01 = saturate((view_depth - near_z) / (far_z - near_z));
    uint q = (uint)min(depth01 * 4294967295.0, 4294967294.0);
    return 0xfffffffeu - q;
}

uint gs_radix_bin(uint key, uint shift)
{
    return (key >> shift) & (GS_RADIX_BINS - 1u);
}

float3 gs_quat_rotate(float4 q, float3 v)
{
    q = normalize(q);
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

#endif
