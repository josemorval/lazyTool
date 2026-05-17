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
- multiple sequential timelines; disabled timelines are skipped and the loop point is the end of the last enabled timeline
- project export settings from the editor's **Exporter / Standalone** section: VSync, Escape-to-close, timeline exit behavior and FPS-in-title are copied into `out64k.c`
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

The batch script is intentionally fixed: it builds the exporter, generates `out64k.c`, compiles `lt64k.exe`, packs it with `upx.exe`, and deletes intermediates.

The main regression scene is:

```text
projects/procedural_spheres_pbr_post.lt
```

That project is intentionally procedural and should generate an `out64k.c` with no references to `.gltf`, `.glb`, image files, HDRs, `.ply` splats or `assets/` paths.

Timeline export accepts the current `timeline_global` / `timeline_clip` blocks written by the editor. Each exported track is tagged with its source timeline so playback applies only the active clip at that moment.

The `.lt` file should include the current `export_settings` line written by the editor before `build64k` exports it.
