# Shader helper commons

The general rule is to keep the helpers separated by responsibility and compose them in shaders that need more than one system.

- `shaders/common_pbr.hlsl` contains GGX/Schlick/Smith PBR helpers, default light helpers, shadow integration, simple IBL approximation, normal-map unpacking, and sRGB/linear conversion.
- `shaders/common_raymarch.hlsl` contains camera-ray reconstruction from `SceneCB.InvViewProj`/`SceneCB.ViewToWorld`, SDF primitives/boolean ops, repeat/rotation helpers, a generic SDF trace loop, normal estimation, soft shadows, ambient occlusion, and debug coloring.
- `shaders/common_atmosphere.hlsl` contains procedural sky, sun disk/glow, stars, ambient diffuse/specular approximations for PBR, height fog, aerial perspective, and ACES tonemapping.
- `shaders/examples/raymarch_pbr_atmosphere.hlsl` shows the three commons working together in a single fullscreen raymarch shader.

## Recommended include pattern

For regular mesh PBR:

```hlsl
#include "common_pbr.hlsl"
#include "common_atmosphere.hlsl" // optional, useful as procedural IBL/fog
```

Include paths are relative to the current shader file. From `shaders/examples/`, use `../common_pbr.hlsl`; from a shader directly under `shaders/`, use `common_pbr.hlsl`.

For raymarching with PBR and sky:

```hlsl
float scene_sdf(float3 p);
#define LT_RAYMARCH_SCENE(p) scene_sdf(p)

#include "common_pbr.hlsl"
#include "common_raymarch.hlsl"
#include "common_atmosphere.hlsl"

float scene_sdf(float3 p)
{
    return lt_sdf_sphere(p, 1.0);
}
```

`common_raymarch.hlsl` gives you camera rays directly from the engine camera:

```hlsl
LTRay ray = lt_raymarch_camera_ray(uv);
```

That uses `SceneCB.ViewToWorld` and `SceneCB.InvViewProj`, so the user does not need to manually pass FOV, aspect ratio, projection, near plane, or camera basis for common perspective-camera cases. `lt_raymarch_camera_ray_near_plane(uv)` is also available for shaders that prefer ray origins on the near plane.

## UserCB parameters that work well

A minimal raymarch/PBR material block:

```hlsl
cbuffer UserCB : register(b2)
{
    float4 BaseColorExposure; // rgb = base color, a = exposure
    float4 Material;          // x = roughness, y = metallic, z = AO, w = unused
    float4 Raymarch;          // x = max distance, y = epsilon, z = max steps, w = normal epsilon
    float4 Sky;               // x = sky intensity, y = sun intensity, z = horizon, w = stars
    float4 Fog;               // x = density, y = height falloff
};
```

Practical defaults:

| Parameter | Suggested start |
|---|---:|
| `roughness` | `0.45` |
| `metallic` | `0.0` |
| `AO` | `1.0` |
| `max distance` | `100.0` |
| `epsilon` | `0.001` |
| `max steps` | `96` to `160` |
| `normal epsilon` | `0.002` |
| `sky intensity` | `1.0` |
| `sun intensity` | `1.0` |
| `horizon intensity` | `1.0` |
| `fog density` | `0.01` to `0.03` |
| `fog height falloff` | `0.05` to `0.20` |

## How the modules fit together

`common_raymarch.hlsl` produces a hit position and normal. `common_pbr.hlsl` shades that hit using the engine light. `common_atmosphere.hlsl` can provide procedural diffuse/specular environment colors and then fog the final result.

The systems are intentionally not one giant file. Keeping them separate avoids pulling raymarch code into normal mesh materials, avoids forcing atmosphere into simple PBR materials, and makes the include dependency easy to understand.


## References / related reading

The helper files include short English comments with references. They are not meant to imply that code was copied verbatim; they document the model lineage and give users places to learn more.

### PBR

- Robert L. Cook and Kenneth E. Torrance, **"A Reflectance Model for Computer Graphics"** (1982). Classic microfacet reflectance model.
- Christophe Schlick, **"An Inexpensive BRDF Model for Physically-based Rendering"** (1994). Fast Fresnel approximation widely used in realtime rendering.
- Brent Burley, **"Physically-Based Shading at Disney"** (SIGGRAPH 2012). Artist-friendly material model and production PBR notes.
- Brian Karis, **"Real Shading in Unreal Engine 4"** (SIGGRAPH 2013). Practical realtime PBR choices around GGX, Smith/Schlick and IBL.

### Raymarching / SDF

- John C. Hart, **"Sphere Tracing: A Geometric Method for the Antialiased Ray Tracing of Implicit Surfaces"** (1996). Classic sphere tracing paper.
- Inigo Quilez, **"Distance Functions"** and **"Raymarching Distance Fields"**. Practical SDF primitives, operators and realtime shader patterns.
- Jamie Wong, **"Ray Marching and Signed Distance Functions"** (2016). Friendly intro for people learning SDFs.

### Atmosphere / tonemapping

- Preetham, Shirley and Smits, **"A Practical Analytic Model for Daylight"** (SIGGRAPH 1999). Analytic daylight, sky and aerial perspective.
- Bruneton and Neyret, **"Precomputed Atmospheric Scattering"** (2008). Realtime atmosphere with Rayleigh/Mie scattering and aerial perspective.
- Hoffman and Preetham, **"Rendering Outdoor Light Scattering in Real Time"** (GDC 2002). Pragmatic realtime atmosphere overview.
- Krzysztof Narkowicz, **"ACES Filmic Tone Mapping Curve"** (2016). Compact ACES-style tonemapping fit useful in games and shader demos.
