# Procedural Spheres PBR + Post

This shader set is designed for a compact, procedural DX11 scene.  It does not depend on model files, image textures, HDRIs, blue-noise textures, or LUTs.  The project draws editor primitives and generates all visible material detail analytically in HLSL.

## Files

- `common.hlsl` contains the shared engine constant-buffer declarations, math helpers, hash/noise functions, depth reconstruction, PBR BRDF functions, ACES tone mapping, and a 5x5 PCF shadow sampler for the editor cascade shadow atlas.
- `scene_common.hlsl` contains the shared scene `UserCB` and procedural sphere placement used by both camera rendering and shadow rendering.
- `scene_pbr.hlsl` renders both the cube floor and the sphere cluster.  The floor uses a derivative-softened checkerboard.  The spheres are drawn with one instanced draw call; position, radius, and marbled color variation are generated from `SV_InstanceID`.
- `scene_shadow.hlsl` is the depth-only shadow VS. It reuses the same procedural placement as the scene shader but projects with `ShadowViewProj`.
- `ssao.hlsl` reconstructs world positions from the depth buffer and estimates contact occlusion with a rotated spiral kernel.
- `bilateral_blur.hlsl` denoises SSAO without bleeding over depth discontinuities.
- `bloom_prefilter.hlsl` extracts HDR highlights with a soft knee.
- `bloom_blur.hlsl` performs a separable Gaussian blur.  The `.lt` project places horizontal and vertical blur commands inside a `repeat` node for a wider bloom lobe.
- `dof_coc.hlsl` converts depth into a signed circle-of-confusion mask and keeps clear sky at zero blur to avoid silhouette halos.
- `dof_blur.hlsl` gathers a golden-angle bokeh disk from the HDR scene with a lightweight depth gate.
- `lensflare.hlsl` builds ghosts, halo, and a small streak from bloom, HDR highlights, and the projected directional light.  There is no external lens dirt texture.
- `final_composite.hlsl` combines DoF, SSAO, bloom, and lens flare in HDR, then applies exposure, vignette, ACES, gamma, and a tiny procedural grain.

## Binding conventions

The editor owns the following buffers:

- `SceneCB` at `b0`
- `ObjectCB` at `b1`
- Per-command reflected `UserCB` at `b2`

The scene shader uses `MaterialParams.w` as a mode selector:

- `0`: regular object transform, used by the ground cube
- `1`: procedural instanced sphere placement, used by the sphere cluster

The shadow prepass uses `scene_shadow.hlsl`, so the instanced shadows match the procedural sphere placement while using the light/cascade `ShadowViewProj`.

## Post-process quality notes

The post chain favors stable, readable quality over minimal instruction count:

1. SSAO is full resolution, then bilateral-blurred in two passes.
2. Bloom runs at quarter resolution but uses an HDR soft-knee prefilter and two repeated ping-pong blur rounds.
3. DoF uses a separate signed CoC pass and a 56-sample golden-angle gather.
4. Lens flare is derived from bloom/HDR highlights plus the directional light position, so it remains visible even when the bloom buffer has little punctual energy.

This is still procedural and asset-free, so the quality can later be traded down for a stricter 64k build if needed.
