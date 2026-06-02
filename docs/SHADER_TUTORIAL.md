# Shader Tutorial

This document is only about shader-side patterns in lazyTool. The examples are
self-contained on purpose: no `#include`, no hidden helper file, and no engine
abstractions beyond the resource bindings shown in each snippet.

The register choices are examples. Match them with the command inspector:

- `SceneCB` at `b0` is the built-in scene constant buffer.
- `ObjectCB` at `b1` is the built-in object constant buffer for draw commands.
- `b2` is a practical slot for your own UserCB/material constants.
- Texture/SRV/UAV slots must match the command resource bindings.

## Mini PBR Pipeline

This is a small deferred pipeline:

1. A draw pass writes material data into three render targets.
2. A compute pass reads those buffers plus scene depth and writes final color.

Create these render textures:

| Name | Suggested format | Bindings |
|---|---|---|
| `GBufferAlbedoMetal` | `RGBA16F` or `RGBA8` | RTV + SRV |
| `GBufferNormalRough` | `RGBA16F` | RTV + SRV |
| `GBufferEmissiveAO` | `RGBA16F` | RTV + SRV |
| `LightingOutput` | `RGBA16F` | UAV + SRV |

### G-buffer Draw Shader

Bind the three G-buffer textures as MRTs. This shader writes simple PBR-style
payloads: base color, metallic, normal, roughness, emissive, and occlusion.

```hlsl
cbuffer SceneCB : register(b0)
{
    float4x4 WorldToView;
    float4x4 ViewToWorld;
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4 TimeVec;
    float4 CameraParams;
    float4 LightDir;
    float4 LightColor;
    float4 LightPos;
    float4 LightParams;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4x4 ShadowViewProj;
    float4x4 PrevShadowViewProj;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

cbuffer ObjectCB : register(b1)
{
    float4x4 LocalToWorld;
};

cbuffer MaterialCB : register(b2)
{
    float4 BaseColor;          // rgb = albedo, a = alpha
    float4 RoughMetalAO;       // x = roughness, y = metallic, z = occlusion
    float4 EmissiveColor;      // rgb = emissive, a = intensity
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 pos       : SV_POSITION;
    float3 world_pos : TEXCOORD0;
    float3 normal_ws : TEXCOORD1;
    float2 uv        : TEXCOORD2;
};

struct GBufferOut
{
    float4 albedo_metal : SV_Target0;
    float4 normal_rough : SV_Target1;
    float4 emissive_ao  : SV_Target2;
};

float3 safe_normalize(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-8));
}

float3 encode_normal(float3 n)
{
    return safe_normalize(n) * 0.5 + 0.5;
}

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 world = mul(LocalToWorld, float4(v.pos, 1.0));
    o.pos = mul(ViewProj, world);
    o.world_pos = world.xyz;
    o.normal_ws = safe_normalize(mul(LocalToWorld, float4(v.nor, 0.0)).xyz);
    o.uv = v.uv;
    return o;
}

GBufferOut PSMain(VSOut i)
{
    GBufferOut o;
    float roughness = saturate(RoughMetalAO.x);
    float metallic = saturate(RoughMetalAO.y);
    float ao = saturate(RoughMetalAO.z);
    float3 emissive = EmissiveColor.rgb * EmissiveColor.a;

    o.albedo_metal = float4(BaseColor.rgb, metallic);
    o.normal_rough = float4(encode_normal(i.normal_ws), roughness);
    o.emissive_ao = float4(emissive, ao);
    return o;
}
```

### Compute Composition Shader

Bind:

- `GBufferAlbedoMetal` as `t0`
- `GBufferNormalRough` as `t1`
- `GBufferEmissiveAO` as `t2`
- scene depth as `t3`
- `LightingOutput` as `u0`

```hlsl
cbuffer SceneCB : register(b0)
{
    float4x4 WorldToView;
    float4x4 ViewToWorld;
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4 TimeVec;
    float4 CameraParams;
    float4 LightDir;
    float4 LightColor;
    float4 LightPos;
    float4 LightParams;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4x4 ShadowViewProj;
    float4x4 PrevShadowViewProj;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

Texture2D<float4> GBufferAlbedoMetal : register(t0);
Texture2D<float4> GBufferNormalRough : register(t1);
Texture2D<float4> GBufferEmissiveAO  : register(t2);
Texture2D<float>  SceneDepth         : register(t3);
RWTexture2D<float4> OutputColor      : register(u0);

float3 safe_normalize(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-8));
}

float3 decode_normal(float3 enc)
{
    return safe_normalize(enc * 2.0 - 1.0);
}

float4 uv_depth_to_clip(float2 uv, float depth01)
{
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth01, 1.0);
}

float3 scene_depth_to_world(float2 uv, float depth01)
{
    float4 world = mul(InvViewProj, uv_depth_to_clip(uv, depth01));
    return world.xyz / max(abs(world.w), 1e-5);
}

float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cos_theta), 5.0);
}

float distribution_ggx(float3 n, float3 h, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float ndh = saturate(dot(n, h));
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-5);
}

float geometry_schlick_ggx(float ndv, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return ndv / max(ndv * (1.0 - k) + k, 1e-5);
}

float geometry_smith(float3 n, float3 v, float3 l, float roughness)
{
    return geometry_schlick_ggx(saturate(dot(n, v)), roughness) *
           geometry_schlick_ggx(saturate(dot(n, l)), roughness);
}

float3 pbr_direct(float3 albedo, float metallic, float roughness,
                  float3 n, float3 v, float3 l, float3 radiance)
{
    float3 h = safe_normalize(v + l);
    float ndl = saturate(dot(n, l));
    float ndv = saturate(dot(n, v));
    float hdv = saturate(dot(h, v));

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 f = fresnel_schlick(hdv, f0);
    float d = distribution_ggx(n, h, roughness);
    float g = geometry_smith(n, v, l, roughness);
    float3 spec = (d * g * f) / max(4.0 * ndv * ndl, 1e-5);

    float3 kd = (1.0 - f) * (1.0 - metallic);
    float3 diffuse = kd * albedo / 3.14159265;
    return (diffuse + spec) * radiance * ndl;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    OutputColor.GetDimensions(w, h);
    if (id.x >= w || id.y >= h)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float depth01 = SceneDepth.Load(int3(id.xy, 0));

    float4 am = GBufferAlbedoMetal.Load(int3(id.xy, 0));
    float4 nr = GBufferNormalRough.Load(int3(id.xy, 0));
    float4 ea = GBufferEmissiveAO.Load(int3(id.xy, 0));

    float3 albedo = am.rgb;
    float metallic = saturate(am.a);
    float roughness = max(nr.a, 0.045);
    float3 n = decode_normal(nr.rgb);
    float ao = saturate(ea.a);

    float3 world_pos = scene_depth_to_world(uv, depth01);
    float3 v = safe_normalize(mul(ViewToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz - world_pos);
    float3 l = LightParams.x >= 0.5 ? safe_normalize(LightPos.xyz - world_pos)
                                    : safe_normalize(-LightDir.xyz);

    float attenuation = 1.0;
    if (LightParams.x >= 0.5) {
        float cone = dot(safe_normalize(world_pos - LightPos.xyz), safe_normalize(LightDir.xyz));
        attenuation = saturate((cone - LightParams.z) / max(LightParams.y - LightParams.z, 1e-5));
    }

    float3 radiance = LightColor.rgb * LightDir.w * attenuation;
    float3 ambient = albedo * 0.035 * ao;
    float3 color = ambient + pbr_direct(albedo, metallic, roughness, n, v, l, radiance) + ea.rgb;
    OutputColor[id.xy] = float4(color, 1.0);
}
```

## GPU Particles

Create a structured buffer named `Particles` with this layout:

```hlsl
struct Particle
{
    float3 pos;
    float life;
    float3 vel;
    float size;
    float4 color;
};
```

Use one compute command to update it, then one procedural draw command to render
six vertices per particle.

### Particle Update Compute

Bind `Particles` as UAV `u0`.

```hlsl
struct Particle
{
    float3 pos;
    float life;
    float3 vel;
    float size;
    float4 color;
};

RWStructuredBuffer<Particle> Particles : register(u0);

cbuffer ParticleCB : register(b2)
{
    float4 Emitter;       // xyz = spawn position, w = spawn radius
    float4 TimeParams;    // x = time, y = dt
    uint MaxParticles;
    float Gravity;
    float BaseSize;
    float Pad0;
};

float hash11(float n)
{
    return frac(sin(n) * 43758.5453123);
}

float3 hash31(float n)
{
    return frac(sin(float3(n, n + 13.1, n + 27.7)) * 43758.5453);
}

Particle spawn_particle(uint id, float time)
{
    float3 r = hash31((float)id * 17.0 + time * 11.0) * 2.0 - 1.0;
    r = normalize(r + float3(0.001, 0.25, 0.003));

    Particle p;
    p.pos = Emitter.xyz + r * Emitter.w * hash11((float)id + time);
    p.vel = r * lerp(0.6, 2.0, hash11((float)id * 3.7 + time));
    p.life = lerp(0.7, 2.4, hash11((float)id * 9.1 + time));
    p.size = BaseSize * lerp(0.6, 1.8, hash11((float)id * 5.3 + time));
    p.color = float4(1.0, 0.55 + 0.35 * hash11((float)id), 0.18, 1.0);
    return p;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= MaxParticles)
        return;

    Particle p = Particles[id.x];
    float dt = TimeParams.y;
    p.life -= dt;

    if (p.life <= 0.0) {
        p = spawn_particle(id.x, TimeParams.x);
    } else {
        p.vel.y -= Gravity * dt;
        p.pos += p.vel * dt;
        p.color.a = saturate(p.life);
    }

    Particles[id.x] = p;
}
```

### Particle Procedural Draw

Bind `Particles` as SRV `t0`. Set vertex count to `particle_count * 6`.

```hlsl
cbuffer SceneCB : register(b0)
{
    float4x4 WorldToView;
    float4x4 ViewToWorld;
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4 TimeVec;
    float4 CameraParams;
    float4 LightDir;
    float4 LightColor;
    float4 LightPos;
    float4 LightParams;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4x4 ShadowViewProj;
    float4x4 PrevShadowViewProj;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

struct Particle
{
    float3 pos;
    float life;
    float3 vel;
    float size;
    float4 color;
};

StructuredBuffer<Particle> Particles : register(t0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

float3 safe_normalize(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-8));
}

float2 quad_corner(uint vertex_in_quad)
{
    float2 corners[6] = {
        float2(-1, -1), float2( 1, -1), float2( 1,  1),
        float2(-1, -1), float2( 1,  1), float2(-1,  1)
    };
    return corners[vertex_in_quad];
}

VSOut VSMain(uint vertex_id : SV_VertexID)
{
    uint particle_id = vertex_id / 6;
    uint vertex_in_quad = vertex_id - particle_id * 6;

    Particle p = Particles[particle_id];
    float2 q = quad_corner(vertex_in_quad);

    float3 forward = safe_normalize(mul(ViewToWorld, float4(0.0, 0.0, -1.0, 0.0)).xyz);
    float3 right = safe_normalize(cross(float3(0, 1, 0), forward));
    float3 up = safe_normalize(cross(forward, right));
    float3 world_pos = p.pos + (right * q.x + up * q.y) * p.size;

    VSOut o;
    o.pos = mul(ViewProj, float4(world_pos, 1.0));
    o.uv = q * 0.5 + 0.5;
    o.col = p.color;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 d = i.uv * 2.0 - 1.0;
    float alpha = saturate(1.0 - dot(d, d));
    alpha = alpha * alpha * i.col.a;
    return float4(i.col.rgb * alpha, alpha);
}
```

## Indirect Procedural Triangles From Points

This pattern uses a compute shader to fill a point buffer and write indirect
draw arguments. The draw pass reads each point and expands it into a triangle.

D3D11 `DrawInstancedIndirect` arguments are four uints:

```text
uint VertexCountPerInstance;
uint InstanceCount;
uint StartVertexLocation;
uint StartInstanceLocation;
```

Create:

- `Points`: structured buffer, SRV + UAV, element `float4`
- `IndirectArgs`: raw/uint buffer with indirect args enabled, UAV + indirect

### Build Points And Args Compute

Bind `Points` as `u0` and `IndirectArgs` as `u1`.

```hlsl
RWStructuredBuffer<float4> Points : register(u0);
RWByteAddressBuffer IndirectArgs  : register(u1);

cbuffer GridCB : register(b2)
{
    uint GridWidth;
    uint GridHeight;
    float CellSize;
    float Time;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint count = GridWidth * GridHeight;

    if (id.x == 0 && id.y == 0) {
        IndirectArgs.Store(0, 3);      // three vertices per triangle
        IndirectArgs.Store(4, count);  // one triangle instance per point
        IndirectArgs.Store(8, 0);
        IndirectArgs.Store(12, 0);
    }

    if (id.x >= GridWidth || id.y >= GridHeight)
        return;

    uint index = id.y * GridWidth + id.x;
    float2 centered = (float2(id.xy) - float2(GridWidth, GridHeight) * 0.5) * CellSize;
    float height = sin(centered.x * 0.7 + Time) * cos(centered.y * 0.6 + Time) * 0.35;
    Points[index] = float4(centered.x, height, centered.y, 1.0);
}
```

### Indirect Triangle Draw Shader

Bind `Points` as SRV `t0`. Use the indirect args buffer in the draw command.

```hlsl
cbuffer SceneCB : register(b0)
{
    float4x4 WorldToView;
    float4x4 ViewToWorld;
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4 TimeVec;
    float4 CameraParams;
    float4 LightDir;
    float4 LightColor;
    float4 LightPos;
    float4 LightParams;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4x4 ShadowViewProj;
    float4x4 PrevShadowViewProj;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

StructuredBuffer<float4> Points : register(t0);

cbuffer TriangleCB : register(b2)
{
    float TriangleSize;
    float3 Color;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 col : COLOR0;
};

VSOut VSMain(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    float3 p = Points[instance_id].xyz;

    float2 tri[3] = {
        float2(0.0, 1.0),
        float2(-0.866, -0.5),
        float2(0.866, -0.5)
    };

    float2 q = tri[vertex_id] * TriangleSize;
    float3 world_pos = p + float3(q.x, 0.0, q.y);

    VSOut o;
    o.pos = mul(ViewProj, float4(world_pos, 1.0));
    o.col = Color * (0.65 + 0.35 * saturate(p.y + 0.5));
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(i.col, 1.0);
}
```

The useful part is the split of responsibilities:

- Compute decides how many points exist and writes the indirect args.
- Draw expands each point into visible geometry using `SV_InstanceID`.
- No CPU-side count update is needed once the command bindings are set.
