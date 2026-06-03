#ifndef LAZYTOOL_COMMON_HLSL
#define LAZYTOOL_COMMON_HLSL

#define LT_MAX_SHADOW_CASCADES 4

// Per-frame constants written by lazyTool before each draw/dispatch that uses
// the standard scene buffer. Matrix names follow the transform they perform.
cbuffer SceneCB : register(b0)
{
    // Camera transforms.
    float4x4 WorldToView;
    float4x4 ViewToWorld;
    float4x4 ViewProj;
    float4x4 InvViewProj;

    // Previous-frame camera transforms, useful for velocity/motion vectors.
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;

    float4 TimeVec;       // x = time, y = dt, z = frame
    float4 CameraParams;  // x = 0 perspective, 1 orthographic; y = ortho height; z/w = near/far

    // Main editor light. Directional lights use LightDir; spot lights also use
    // LightPos and LightParams.y/z/w for cone and range information.
    float4 LightDir;      // xyz = directional light direction, w = intensity
    float4 LightColor;    // rgb = directional light color
    float4 LightPos;      // xyz = light world position
    float4 LightParams;   // x = 0 directional, 1 spot; y/z = spot inner/outer cos; w = range

    // Shadow data. Directional lights can use cascades; spot lights use index 0.
    float4 ShadowCascadeSplits;
    float4 ShadowParams;  // x = cascade count, y = camera near, z = camera far
    float4x4 ShadowViewProj;
    float4x4 PrevShadowViewProj;
    float4 ShadowCascadeRects[LT_MAX_SHADOW_CASCADES];      // reserved cascade UV rects
    float4x4 ShadowCascadeViewProj[LT_MAX_SHADOW_CASCADES];
};

// Per-object transform written by draw commands. Procedural draws still receive
// this buffer when the command has a transform.
cbuffer ObjectCB : register(b1)
{
    float4x4 LocalToWorld;
};

// Default shadow resources used by the convenience overloads below. Define
// LT_NO_DEFAULT_SHADOWMAP before including this file if you want to bind and
// pass a custom shadow map/sampler explicitly.
#ifndef LT_NO_DEFAULT_SHADOWMAP
Texture2DArray ShadowMap : register(t7);
SamplerComparisonState ShadowSampler : register(s1);
#endif

// General constants. LT_EPS is intentionally conservative for divide guards.
static const float LT_PI      = 3.14159265359;
static const float LT_TWO_PI  = 6.28318530718;
static const float LT_HALF_PI = 1.57079632679;
static const float LT_EPS     = 1e-5;

// Small scalar/vector helpers.
float lt_square(float x) { return x * x; }
float2 lt_square(float2 x) { return x * x; }
float3 lt_square(float3 x) { return x * x; }

// Normalize with a floor on length squared, avoiding NaNs for zero vectors.
float2 lt_safe_normalize(float2 v)
{
    return v * rsqrt(max(dot(v, v), 1e-8));
}

float3 lt_safe_normalize(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-8));
}

// Rec.709 luminance weights for linear RGB values.
float lt_luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Compact ACES fitted curve for preview/final color tonemapping.
float3 lt_aces_fitted(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

// Camera helpers. Suffixes:
//   _ws = world space
//   view = camera/view space
//   clip = homogeneous clip space
float3 lt_camera_position_ws()
{
    return mul(ViewToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz;
}

// Camera forward in world space. lazyTool/D3D view space looks down -Z.
float3 lt_camera_forward_ws()
{
    return lt_safe_normalize(mul(ViewToWorld, float4(0.0, 0.0, -1.0, 0.0)).xyz);
}

// Unit vector from a world-space point back to the camera position.
float3 lt_vector_to_camera_ws(float3 world_pos)
{
    return lt_safe_normalize(lt_camera_position_ws() - world_pos);
}

// Unit ray direction from the camera through a world-space point.
float3 lt_ray_from_camera_ws(float3 world_pos)
{
    return lt_safe_normalize(world_pos - lt_camera_position_ws());
}

// Object/local position to world homogeneous position.
float4 lt_object_to_world(float3 object_pos)
{
    return mul(LocalToWorld, float4(object_pos, 1.0));
}

// Object/local normal to world normal. Assumes LocalToWorld has no non-uniform
// scale that would require an inverse-transpose normal matrix.
float3 lt_object_normal_to_world(float3 object_normal)
{
    return lt_safe_normalize(mul(LocalToWorld, float4(object_normal, 0.0)).xyz);
}

// World position to clip space for SV_POSITION.
float4 lt_world_to_clip(float3 world_pos)
{
    return mul(ViewProj, float4(world_pos, 1.0));
}

// World position to camera/view space.
float4 lt_world_to_view(float3 world_pos)
{
    return mul(WorldToView, float4(world_pos, 1.0));
}

// Camera/view position back to world space.
float4 lt_view_to_world(float3 view_pos)
{
    return mul(ViewToWorld, float4(view_pos, 1.0));
}

// Homogeneous clip to normalized device coordinates. The w guard prevents
// explosive values when a point is on or extremely close to the camera plane.
float3 lt_clip_to_ndc(float4 clip_pos)
{
    return clip_pos.xyz / max(abs(clip_pos.w), LT_EPS);
}

// D3D NDC [-1, 1] to texture UV [0, 1], flipping Y for screen-space sampling.
float2 lt_ndc_to_uv(float2 ndc)
{
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// Clip-space position directly to UV.
float2 lt_clip_to_uv(float4 clip_pos)
{
    return lt_ndc_to_uv(lt_clip_to_ndc(clip_pos).xy);
}

// Screen UV + hardware depth to clip coordinates for inverse projection.
float4 lt_uv_depth_to_clip(float2 uv, float depth01)
{
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth01, 1.0);
}

// Reconstruct world position from screen UV and hardware depth.
float3 lt_scene_depth_to_world(float2 uv, float depth01)
{
    float4 world = mul(InvViewProj, lt_uv_depth_to_clip(uv, depth01));
    return world.xyz / max(abs(world.w), LT_EPS);
}

// Linear camera-forward depth for a world position. Positive values are in
// front of the camera because view space forward is -Z.
float lt_view_depth_from_world(float3 world_pos)
{
    float3 view_pos = mul(WorldToView, float4(world_pos, 1.0)).xyz;
    return -view_pos.z;
}

// Hardware depth sample to linear camera-forward depth.
float lt_scene_depth_to_view_depth(float2 uv, float depth01)
{
    return lt_view_depth_from_world(lt_scene_depth_to_world(uv, depth01));
}

// Hardware depth [0, 1] to linear view depth using the active camera mode.
float lt_depth01_to_view_depth(float depth01)
{
    float near_z = max(ShadowParams.y, LT_EPS);
    float far_z = max(ShadowParams.z, near_z + LT_EPS);
    if (CameraParams.x >= 0.5)
        return lerp(near_z, far_z, saturate(depth01));
    return (near_z * far_z) / max(far_z - depth01 * (far_z - near_z), LT_EPS);
}

// Linear view depth back to hardware depth [0, 1].
float lt_view_depth_to_depth01(float view_depth)
{
    float near_z = max(ShadowParams.y, LT_EPS);
    float far_z = max(ShadowParams.z, near_z + LT_EPS);
    if (CameraParams.x >= 0.5)
        return saturate((view_depth - near_z) / max(far_z - near_z, LT_EPS));
    return saturate((far_z * (view_depth - near_z)) / max(view_depth * (far_z - near_z), LT_EPS));
}

// Pixel shader SV_POSITION.xy to UV. Pass the current render target size.
float2 lt_sv_position_to_uv(float4 sv_position, float2 render_size)
{
    return sv_position.xy / max(render_size, float2(1.0, 1.0));
}

// UV to pixel coordinates for the supplied render size.
float2 lt_uv_to_pixel(float2 uv, float2 render_size)
{
    return uv * render_size;
}

// Pixel coordinates to UV for the supplied render size.
float2 lt_pixel_to_uv(float2 pixel, float2 render_size)
{
    return pixel / max(render_size, float2(1.0, 1.0));
}

// Viewport UV [0, 1] to D3D NDC [-1, 1].
float2 lt_viewport_uv_to_ndc(float2 uv)
{
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

// Decode a normal stored in RGB as [0, 1] to normalized [-1, 1].
float3 lt_decode_normal_rgb(float4 enc)
{
    return lt_safe_normalize(enc.xyz * 2.0 - 1.0);
}

// Previous-frame clip space for temporal effects.
float4 lt_prev_clip_from_world(float3 world_pos)
{
    return mul(PrevViewProj, float4(world_pos, 1.0));
}

// Current-minus-previous UV motion vector for a world-space point.
float2 lt_motion_vector_uv(float3 world_pos)
{
    float2 cur_uv = lt_clip_to_uv(lt_world_to_clip(world_pos));
    float2 prev_uv = lt_clip_to_uv(lt_prev_clip_from_world(world_pos));
    return cur_uv - prev_uv;
}

// Shadow helpers. Directional lights use cascades selected by view depth.
// Spot lights use cascade/index 0.
int lt_shadow_cascade_count()
{
    return clamp((int)ShadowParams.x, 1, LT_MAX_SHADOW_CASCADES);
}

// Select the shadow cascade that contains world_pos.
int lt_select_shadow_cascade(float3 world_pos)
{
    if (LightParams.x >= 0.5)
        return 0;
    int cascade_count = lt_shadow_cascade_count();
    float view_depth = lt_view_depth_from_world(world_pos);
    int cascade_index = 0;
    [unroll]
    for (int i = 0; i < LT_MAX_SHADOW_CASCADES - 1; ++i) {
        if (i + 1 < cascade_count && view_depth > ShadowCascadeSplits[i])
            cascade_index = i + 1;
    }
    return cascade_index;
}

// World position to shadow clip space for one cascade.
float4 lt_shadow_clip(int cascade_index, float3 world_pos)
{
    return mul(ShadowCascadeViewProj[cascade_index], float4(world_pos, 1.0));
}

// World position to shadow NDC for one cascade.
float3 lt_shadow_ndc(int cascade_index, float3 world_pos)
{
    return lt_clip_to_ndc(lt_shadow_clip(cascade_index, world_pos));
}

// Shadow NDC xy to local cascade UV.
float2 lt_shadow_local_uv_from_ndc(float3 shadow_ndc)
{
    return float2(shadow_ndc.x * 0.5 + 0.5, 0.5 - shadow_ndc.y * 0.5);
}

// Texture2DArray coordinate for the selected cascade layer.
float3 lt_shadow_array_uv(int cascade_index, float2 local_uv)
{
    return float3(local_uv, (float)cascade_index);
}

// True when the shadow lookup lies inside the light frustum/cascade.
bool lt_shadow_inside(float3 shadow_ndc, float2 local_uv)
{
    return all(local_uv >= 0.0) && all(local_uv <= 1.0) &&
           shadow_ndc.z >= 0.0 && shadow_ndc.z <= 1.0;
}

// Bias tuned for the built-in shadow map. Directional lights need a larger
// slope-scaled bias than spot lights in the default setup.
float lt_shadow_bias(float ndl)
{
    if (LightParams.x >= 0.5)
        return lerp(0.00008, 0.000015, saturate(ndl));
    float bias = lerp(0.0032, 0.00045, saturate(ndl));
    return bias;
}

// Normal offset used before sampling shadows to reduce self-shadowing.
float lt_shadow_normal_offset()
{
    if (LightParams.x >= 0.5)
        return clamp(LightParams.w * 0.00008, 0.00025, 0.006);
    return 0.012;
}

// Manual shadow-map overload. Use this when binding a custom shadow map/sampler
// or when LT_NO_DEFAULT_SHADOWMAP is enabled.
float lt_sample_shadow_cascade_pcf3x3(Texture2DArray shadow_map,
                                      SamplerComparisonState shadow_sampler,
                                      int cascade_index,
                                      float3 world_pos,
                                      float ndl)
{
    uint shadow_w = 0;
    uint shadow_h = 0;
    uint shadow_layers = 0;
    shadow_map.GetDimensions(shadow_w, shadow_h, shadow_layers);
    if (shadow_w == 0 || shadow_h == 0 || shadow_layers == 0 || cascade_index >= (int)shadow_layers)
        return 1.0;

    float3 shadow_ndc = lt_shadow_ndc(cascade_index, world_pos);
    float2 local_uv = lt_shadow_local_uv_from_ndc(shadow_ndc);
    if (!lt_shadow_inside(shadow_ndc, local_uv))
        return 1.0;

    float2 texel = 1.0 / float2(max(shadow_w, 1), max(shadow_h, 1));
    float z = shadow_ndc.z - lt_shadow_bias(ndl);
    float sum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x)
            sum += shadow_map.SampleCmpLevelZero(shadow_sampler, lt_shadow_array_uv(cascade_index, local_uv + texel * float2(x, y)), z);
    }
    return sum / 9.0;
}

#ifndef LT_NO_DEFAULT_SHADOWMAP
// Convenience overload using ShadowMap t7 and ShadowSampler s1.
float lt_sample_shadow_cascade_pcf3x3(int cascade_index,
                                      float3 world_pos,
                                      float ndl)
{
    return lt_sample_shadow_cascade_pcf3x3(ShadowMap, ShadowSampler, cascade_index, world_pos, ndl);
}
#endif

// Selects the cascade, applies normal offset and samples a 3x3 PCF kernel.
float lt_sample_shadow_pcf3x3(Texture2DArray shadow_map,
                              SamplerComparisonState shadow_sampler,
                              float3 world_pos,
                              float3 normal_ws,
                              float3 light_dir_ws)
{
    float ndl = saturate(dot(normal_ws, light_dir_ws));
    int cascade_index = lt_select_shadow_cascade(world_pos);
    return lt_sample_shadow_cascade_pcf3x3(shadow_map, shadow_sampler, cascade_index,
                                           world_pos + normal_ws * lt_shadow_normal_offset(), ndl);
}

#ifndef LT_NO_DEFAULT_SHADOWMAP
// Convenience overload using the default lazyTool shadow bindings.
float lt_sample_shadow_pcf3x3(float3 world_pos,
                              float3 normal_ws,
                              float3 light_dir_ws)
{
    return lt_sample_shadow_pcf3x3(ShadowMap, ShadowSampler, world_pos, normal_ws, light_dir_ws);
}
#endif

#endif
