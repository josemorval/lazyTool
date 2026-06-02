# build64k exporter

`build64k` is lazyTool's procedural tiny-player export path. It converts a compatible `.lt` project into a compact single-file C DirectX 11 player, then builds and compresses it into `lt64k.exe`.

This exporter is intentionally stricter than the normal packed EXE export. It is meant for projects that can rebuild their result from shaders, primitive meshes, internal render targets, command data, UserCB values, timeline clips, and built-in scene resources without external runtime assets.

---

## Quick start

From a **Developer Command Prompt for Visual Studio**:

```bat
cd build64k
build.bat ..\projects\your_procedural_project.lt
```

The project path should be passed explicitly.

The script:

1. builds `build64k.exe` from `build64k.cpp`;
2. converts the `.lt` project into `out64k.c`;
3. compiles `out64k.c` into `lt64k.exe` using a small `/NODEFAULTLIB` setup;
4. compresses the result with the bundled `upx.exe`.

Generated files:

| File | Meaning |
|---|---|
| `out64k.c` | Generated single-file C player source. |
| `lt64k.exe` | Final compressed tiny player. |
| `lt64k_unpacked.exe` | Optional uncompressed output when kept for testing or size comparison. |

---

## Good project shape

Use this exporter for procedural projects built from:

- VS/PS shader resources;
- shader-only procedural draws using `SV_VertexID`;
- built-in primitive meshes;
- internal render targets;
- scene color, scene depth, and shadow map built-ins;
- clear and draw commands;
- render target/depth bindings;
- command parameters and UserCB values;
- supported timeline tracks;
- light and shadow data;
- export settings such as VSync, Escape-to-close, timeline exit behavior, and FPS-in-title.

The exporter can inline shader includes and minify project/shader text where useful.

---

## Not supported by design

The tiny player intentionally skips features that require a larger runtime or external asset pack:

- external texture/HDR files;
- glTF/GLB meshes;
- Gaussian splat PLY assets;
- arbitrary external SRV resources;
- general compute/UAV/indirect GPU pipelines.

When unsupported content appears, the exporter should warn and keep only the subset that can run in the tiny VS/PS player.

Use the normal packed EXE exporter from the root editor when a project needs external files or the full runtime feature set.

---

## Practical notes

- Save the project from the editor before exporting so `export_settings`, `timeline_global`, `timeline_clip`, and per-key `timeline_key` interpolation data are current.
- The parser accepts the current `.lt` format only. It does not repair old light aliases, old camera records, or timeline clips with the former global interpolation field.
- A good 64k candidate should avoid asset paths and keep resources internal/procedural.
- Prefer primitive meshes, procedural vertex IDs, generated render targets, and UserCB/timeline data.
- Keep shaders self-contained or use includes that can be inlined by the exporter.
- The generated player is not intended to be feature-equivalent with `lazyPlayer.exe`; it is a compact procedural subset.
