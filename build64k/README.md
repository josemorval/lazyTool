# build64k exporter

`build64k` converts a `.lt` project into a single `out64k.c` D3D11 player. It is intended for the 64k/procedural path only: the generated C file must not depend on external assets.

## Supported 64k path

Use this exporter when the project is fully procedural:

- shader-only draws that build geometry from `SV_VertexID`
- built-in primitive meshes (`cube`, `quad`, `fullscreen triangle`, `tetra`, `sphere`)
- internal render targets and built-in scene/backbuffer resources
- embedded VS/PS shader code, render-target passes, clears and shadow passes
- `value` resources baked into command parameters
- UserCB values, command parameters and timeline tracks
- command/camera/dirlight scene sources supported by the current `.lt` format (`param_source` and `source` fields); `cmd_rot` is exported as a float4 quaternion in the 64k runtime

## Not supported by design

The exporter intentionally skips features that require runtime assets or a larger runtime:

- glTF/GLB meshes
- texture/HDR files
- gaussian splat `.ply` assets
- compute/UAV/indirect GPU pipelines
- arbitrary external SRV resources

When one of those appears, `build64k` should warn and export only the subset that can run in the tiny VS/PS player.

## Build and validation

From `build64k` on Windows:

```bat
build.bat ..\projects\procedural_spheres_pbr_post.lt
```

Useful options:

```bat
set "LT64K_SKIP_UPX=1"        rem keep the uncompressed exe for inspection
set "LT64K_CFLAGS=/DLT_DEBUG_FPS=1"  rem add debug FPS overlay define
```

The main regression scene is:

```text
projects/procedural_spheres_pbr_post.lt
```

That project is intentionally procedural and should generate an `out64k.c` with no references to `.gltf`, `.glb`, image files, HDRs, `.ply` splats or `assets/` paths.
