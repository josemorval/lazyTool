#ifndef LAZYTOOL_COMMON_RAYMARCH_HLSL
#define LAZYTOOL_COMMON_RAYMARCH_HLSL

#ifndef LAZYTOOL_COMMON_HLSL
#include "common.hlsl"
#endif

// -----------------------------------------------------------------------------
// lazyTool raymarch helpers
// -----------------------------------------------------------------------------
// Define LT_RAYMARCH_SCENE(p) before including this file to use the built-in
// trace/normal/AO helpers with your own SDF. If you do not define it, a unit
// sphere at the origin is used so new shaders compile immediately.
//
// Related reading / model lineage:
// - John C. Hart, "Sphere Tracing: A Geometric Method for the Antialiased Ray
//   Tracing of Implicit Surfaces" (1996): the classic distance-bound marching
//   method. https://graphics.stanford.edu/courses/cs348b-20-spring-content/uploads/hart.pdf
// - Inigo Quilez, "Distance Functions" and "Raymarching Distance Fields":
//   practical SDF primitive/operator formulas and realtime shader techniques.
//   https://iquilezles.org/articles/distfunctions/
//   https://iquilezles.org/articles/raymarchingdf/
// - Jamie Wong, "Ray Marching and Signed Distance Functions" (2016): friendly
//   introductory explanation of SDFs, normals and marching loops.
//   https://jamie-wong.com/2016/07/15/ray-marching-signed-distance-functions/
//
// These helpers are intentionally small and editor-friendly. They follow common
// SDF/raymarching formulas, but are written for lazyTool's SceneCB conventions.
// -----------------------------------------------------------------------------

struct LTRay
{
    float3 origin;
    float3 dir;
};

struct LTRaymarchParams
{
    float min_t;
    float max_t;
    float epsilon;
    int   max_steps;
    float normal_epsilon;
};

struct LTRaymarchHit
{
    int    hit;
    int    steps;
    float  t;
    float  distance;
    float3 position;
    float3 normal;
};

// Basic SDF primitives. The convention is: negative inside, zero on the surface,
// positive outside. Combining SDFs with min/max gives CSG-like modeling tools.
float lt_sdf_sphere(float3 p, float radius)
{
    return length(p) - radius;
}

float lt_sdf_box(float3 p, float3 half_extents)
{
    float3 q = abs(p) - half_extents;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float lt_sdf_round_box(float3 p, float3 half_extents, float radius)
{
    return lt_sdf_box(p, half_extents) - radius;
}

float lt_sdf_plane(float3 p, float3 normal_ws, float offset)
{
    return dot(p, lt_safe_normalize(normal_ws)) + offset;
}

float lt_sdf_torus(float3 p, float2 major_minor)
{
    float2 q = float2(length(p.xz) - major_minor.x, p.y);
    return length(q) - major_minor.y;
}

float lt_sdf_capsule(float3 p, float3 a, float3 b, float radius)
{
    float3 pa = p - a;
    float3 ba = b - a;
    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
    return length(pa - ba * h) - radius;
}

float lt_sdf_union(float a, float b)
{
    return min(a, b);
}

float lt_sdf_intersection(float a, float b)
{
    return max(a, b);
}

float lt_sdf_subtraction(float a, float b)
{
    return max(a, -b);
}

float lt_sdf_smooth_union(float a, float b, float k)
{
    float h = saturate(0.5 + 0.5 * (b - a) / max(k, 1e-6));
    return lerp(b, a, h) - k * h * (1.0 - h);
}

float3 lt_raymarch_repeat(float3 p, float3 cell_size)
{
    return p - cell_size * round(p / max(cell_size, float3(1e-6, 1e-6, 1e-6)));
}

float2 lt_rotate2d(float2 p, float radians_value)
{
    float s, c;
    sincos(radians_value, s, c);
    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

float lt_raymarch_default_scene(float3 p)
{
    return lt_sdf_sphere(p, 1.0);
}

#ifndef LT_RAYMARCH_SCENE
#define LT_RAYMARCH_SCENE(p) lt_raymarch_default_scene(p)
#endif

LTRaymarchParams lt_raymarch_default_params()
{
    LTRaymarchParams p;
    p.min_t = 0.01;
    p.max_t = max(ShadowParams.z, 100.0);
    p.epsilon = 0.001;
    p.max_steps = 96;
    p.normal_epsilon = 0.002;
    return p;
}

float3 lt_raymarch_world_from_uv_depth(float2 uv, float depth01)
{
    return lt_scene_depth_to_world(uv, depth01);
}

// Reconstruct a world-space camera ray from InvViewProj, so shader authors do
// not need to pass FOV, aspect ratio or camera basis manually.
LTRay lt_raymarch_camera_ray(float2 uv)
{
    float3 far_ws = lt_raymarch_world_from_uv_depth(uv, 1.0);
    LTRay ray;
    ray.origin = lt_camera_position_ws();
    ray.dir = lt_safe_normalize(far_ws - ray.origin);
    return ray;
}

LTRay lt_raymarch_camera_ray_near_plane(float2 uv)
{
    float3 near_ws = lt_raymarch_world_from_uv_depth(uv, 0.0);
    float3 far_ws = lt_raymarch_world_from_uv_depth(uv, 1.0);
    LTRay ray;
    ray.origin = near_ws;
    ray.dir = lt_safe_normalize(far_ws - near_ws);
    return ray;
}

// Central-difference normal estimation. This is simple and robust for editor
// examples; analytic normals are faster when available.
float3 lt_raymarch_estimate_normal(float3 p, float eps)
{
    float2 e = float2(max(eps, 1e-5), 0.0);
    float3 n = float3(
        LT_RAYMARCH_SCENE(p + e.xyy) - LT_RAYMARCH_SCENE(p - e.xyy),
        LT_RAYMARCH_SCENE(p + e.yxy) - LT_RAYMARCH_SCENE(p - e.yxy),
        LT_RAYMARCH_SCENE(p + e.yyx) - LT_RAYMARCH_SCENE(p - e.yyx));
    return lt_safe_normalize(n);
}

// Sphere tracing / distance marching. The step length is the current SDF value,
// assuming the scene function is a conservative distance bound. If the SDF
// underestimates distance badly, the ray can overstep thin features.
LTRaymarchHit lt_raymarch_trace(LTRay ray, LTRaymarchParams params)
{
    LTRaymarchHit h;
    h.hit = 0;
    h.steps = 0;
    h.t = params.min_t;
    h.distance = 1e20;
    h.position = ray.origin + ray.dir * h.t;
    h.normal = float3(0.0, 0.0, 0.0);

    int steps = clamp(params.max_steps, 1, 512);
    [loop]
    for (int i = 0; i < steps; ++i) {
        float3 p = ray.origin + ray.dir * h.t;
        float d = LT_RAYMARCH_SCENE(p);
        h.steps = i + 1;
        h.distance = d;
        h.position = p;
        if (d <= params.epsilon) {
            h.hit = 1;
            h.normal = lt_raymarch_estimate_normal(p, params.normal_epsilon);
            break;
        }
        h.t += max(d, params.epsilon * 0.5);
        if (h.t >= params.max_t)
            break;
    }
    return h;
}

// Soft shadow approximation often used in SDF demos: visibility is reduced by
// nearby distance samples along the light ray instead of tracing an area light.
float lt_raymarch_soft_shadow(float3 ro, float3 rd, float min_t, float max_t, float softness, LTRaymarchParams params)
{
    float result = 1.0;
    float t = min_t;
    int steps = min(clamp(params.max_steps, 1, 512), 128);
    [loop]
    for (int i = 0; i < steps; ++i) {
        float h = LT_RAYMARCH_SCENE(ro + rd * t);
        if (h < params.epsilon)
            return 0.0;
        result = min(result, softness * h / max(t, 1e-4));
        t += clamp(h, params.epsilon * 2.0, 0.25);
        if (t > max_t)
            break;
    }
    return saturate(result);
}

// Cheap horizon-style ambient occlusion: sample the SDF along the normal and
// reduce light when geometry appears close to the surface.
float lt_raymarch_ambient_occlusion(float3 p, float3 n, float step_size, int step_count)
{
    float occ = 0.0;
    float weight = 1.0;
    int count = clamp(step_count, 1, 16);
    [loop]
    for (int i = 1; i <= count; ++i) {
        float d = step_size * (float)i;
        float sample_d = LT_RAYMARCH_SCENE(p + n * d);
        occ += (d - sample_d) * weight;
        weight *= 0.55;
    }
    return saturate(1.0 - occ);
}

float3 lt_raymarch_debug_steps(LTRaymarchHit h, int max_steps)
{
    float t = (float)h.steps / max((float)max_steps, 1.0);
    return lerp(float3(0.05, 0.20, 0.90), float3(1.0, 0.25, 0.05), saturate(t));
}

#endif
