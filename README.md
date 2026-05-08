# lazyTool

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Windows](https://img.shields.io/badge/platform-Windows-0078D4)
![DirectX 11](https://img.shields.io/badge/graphics-DirectX%2011-lightgrey)
![Dear ImGui](https://img.shields.io/badge/UI-Dear%20ImGui-ff69b4)
![HLSL](https://img.shields.io/badge/shaders-HLSL%20SM5.0-orange)

**lazyTool** is an experimental 3D render-graph editor for building, inspecting, animating, and exporting real-time DirectX 11 rendering pipelines.

The editor is designed for fast shader iteration and explicit GPU experimentation: create resources, add render commands, bind targets/SRVs/UAVs, tweak HLSL parameters, animate values on a timeline, and export the result either as a normal packed executable or as a tiny procedural 64k-style build.

> [!NOTE]
> This project is intentionally experimental and practical. The code favors transparency, direct control, and quick iteration over hiding the rendering pipeline behind abstractions.

---

## At a glance

| Area | Current capability |
|---|---|
| Platform | Windows desktop |
| Graphics API | DirectX 11 |
| UI | Dear ImGui editor |
| Shader workflow | Runtime HLSL compilation, fallback shaders, error reporting, reflection, hot recompilation |
| Pipeline model | Ordered command list / tree acting as a render graph |
| Resources | Textures, render textures, buffers, meshes, primitive meshes, shaders, scalar/vector values |
| Mesh import | Built-in primitives and glTF/GLB meshes |
| Draw path | Mesh draws, procedural draws, instancing, MRTs, pixel UAV outputs, indirect draw |
| Compute path | Compute dispatch, SRV/UAV bindings, repeat loops, indirect dispatch |
| Lighting | Directional light, cascaded-style shadow data, shadow map workflow |
| Animation | Timeline tracks for UserCB values, command transforms, command enabled state, camera, and directional light |
| Debugging | Integrated log, shader errors, resource previews, binding inspection |
| Profiling | DX11 timestamp profiler with frame and per-command GPU timings |
| Persistence | Plain-text `.lt` project files |
| Normal export | `lazyPlayer.exe` packed with the current project, shaders, and referenced assets |
| 64k export | `build64k/` procedural-only generator that emits a very small single-file C player |

---

## Contents

- [What lazyTool is](#what-lazytool-is)
- [Repository layout](#repository-layout)
- [Requirements](#requirements)
- [Build and run the editor](#build-and-run-the-editor)
- [Core workflow](#core-workflow)
- [Editor interface](#editor-interface)
- [Resources](#resources)
- [Commands](#commands)
- [Draw commands](#draw-commands)
- [Compute commands](#compute-commands)
- [UserCB and shader reflection](#usercb-and-shader-reflection)
- [Timeline animation](#timeline-animation)
- [Lighting and shadows](#lighting-and-shadows)
- [Camera and viewport](#camera-and-viewport)
- [Profiling and logs](#profiling-and-logs)
- [Project files](#project-files)
- [Normal standalone EXE export](#normal-standalone-exe-export)
- [64k procedural exporter](#64k-procedural-exporter)
- [Procedural PBR/post sample](#procedural-pbrpost-sample)
- [Binding conventions](#binding-conventions)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Internal limits](#internal-limits)
- [Current limitations](#current-limitations)
- [Development notes](#development-notes)

---

## What lazyTool is

lazyTool is both an editor and a small runtime for building frame pipelines visually but explicitly.

A project is made from:

- **Resources**: GPU data and editable values.
- **Commands**: ordered operations that clear, draw, dispatch compute, or group/repeat work.
- **Bindings**: render targets, depth targets, SRVs, UAVs, shaders, mesh sources, and constant-buffer parameters.
- **Timeline tracks**: keyframed animation data applied to parameters, transforms, camera, and lighting.

The core idea is simple: first create the resources, then add commands that read and write those resources, then iterate while looking directly at the result in the viewport.

---

## Repository layout

```text
lazyTool/
├─ src/                    # C++17 editor, DirectX 11 runtime, packed player, exporter code
├─ shaders/                # HLSL shaders and example pipelines
├─ projects/               # .lt sample projects
├─ build64k/               # procedural-only tiny player generator
├─ build.bat               # normal MSVC build script for editor + player
└─ README.md               # this document
```

Main source files:

| File | Responsibility |
|---|---|
| `src/main.cpp` | Win32 entry point, frame loop, editor/player mode selection, camera/time updates. |
| `src/dx11_ctx.cpp` | Shared DirectX 11 device, swap chain, scene targets, samplers, raster/depth/blend states, shadow resources. |
| `src/resources.cpp` | Resource allocation, release, texture loading, render texture creation, primitive mesh generation, glTF import. |
| `src/commands.cpp` | Command validation, execution, bindings, draw/dispatch logic, repeat/group traversal, GPU profiler. |
| `src/shader.cpp` | HLSL compilation, fallback shaders, shader reflection for `UserCB` and `ObjectCB`. |
| `src/user_cb.cpp` | Global user constant buffer, UserCB variable packing, resource-to-parameter synchronization. |
| `src/timeline.cpp` | Timeline data model, keyframe evaluation, target application. |
| `src/project.cpp` | Plain-text `.lt` save/load, resource and command reference resolution. |
| `src/embedded_pack.cpp` | Normal standalone executable export by appending a packed project/assets payload. |
| `src/ui.cpp` | Dear ImGui shell: resource panel, command panel, inspector, viewport, timeline, logs, top bar. |
| `src/app_settings.cpp` | Editor-wide settings such as VSync, profiler, validation, UI scale, and camera controls. |
| `src/log.cpp` | In-memory log ring used by the editor and runtime. |
| `build64k/build64k.cpp` | Procedural-only `.lt` to single-file C exporter for tiny executable builds. |

Some full repository checkouts may also contain `assets/`, `external/`, application resources, and icon files used by the normal editor build. Keep those in the repo when building the desktop editor.

---

## Requirements

- Windows.
- Visual Studio / MSVC with a Developer Command Prompt.
- Windows SDK with Direct3D 11 headers and libraries.
- DirectX 11-capable GPU.
- The normal editor build expects the vendored dependencies referenced by `build.bat`, including Dear ImGui, stb_image, cgltf, and NanoSVG.

The code targets C++17 for the editor and C17 for the generated 64k player source.

---

## Build and run the editor

Run from a **Developer Command Prompt for Visual Studio**:

```bat
build.bat
```

Build and immediately launch the editor:

```bat
build.bat run
```

Copy runtime folders to `bin/` without rebuilding:

```bat
build.bat copy
```

The normal build produces two executables:

| Output | Purpose |
|---|---|
| `bin/lazyTool.exe` | Full editor plus normal exporter. |
| `bin/lazyPlayer.exe` | Smaller player used by the normal packed EXE export path. |

---

## Core workflow

1. Open `lazyTool.exe`.
2. Create resources in the **Resources** panel.
3. Add commands in the **Command Pipeline** panel.
4. Select a command and configure shaders, render targets, depth targets, bindings, params, state, and transforms in the **Inspector**.
5. Compile or recompile shaders with `F5`.
6. Use the viewport, log, resource previews, and profiler to iterate.
7. Save the project as a `.lt` file.
8. Export either with the normal packed player or the procedural `build64k` path, depending on the project.

---

## Editor interface

The editor is split into a few persistent panels:

| Panel | Purpose |
|---|---|
| Resources | Create, select, edit, reload, and preview GPU resources. |
| Command Pipeline | Add, reorder, group, enable/disable, and select frame commands. |
| Inspector | Context-sensitive editor for the selected resource or command. |
| Scene View | Live rendered viewport with camera controls and optional grid. |
| Timeline | Keyframe editor for parameters, transforms, camera, and light. |
| Log | Runtime messages, warnings, shader compilation errors, export messages. |
| General / top bar | Project operations, playback controls, export actions, profiler and runtime toggles. |

Most UI state is immediate-mode interaction state. The persistent scene data lives in the resource, command, timeline, UserCB, and project systems.

---

## Resources

lazyTool resources are one-based handles stored in a fixed resource table. This makes project serialization simple and keeps editor references stable.

Supported resource kinds include:

| Resource kind | Notes |
|---|---|
| `texture2d` | Image file loaded through stb_image, typically used as SRV. |
| `texture3d` | 3D texture resource path / placeholder path. |
| `render_texture` | GPU-created render target, optional SRV/UAV/DSV usage depending on flags. |
| `buffer` | Structured/raw data buffer path or generated data buffer. |
| `mesh` / `mesh_gltf` | Imported glTF/GLB mesh data with parts/materials. |
| `mesh_primitive` | Built-in procedural primitive generated by the runtime. |
| `shader_vsps` | HLSL file compiled as `VSMain` + `PSMain`. |
| `shader_cs` | HLSL file compiled as `CSMain`. |
| `float`, `float2`, `float3`, `float4` | Editable values that can feed params, clears, or UserCB variables. |

Built-in resources expose engine state:

| Built-in | Purpose |
|---|---|
| `time` | Time/delta/frame data for shaders or value bindings. |
| `scene_color` | Main scene color target. |
| `scene_depth` | Main depth target. |
| `shadow_map` | Directional shadow atlas/depth resource. |
| `dirlight` | Directional light state. |

Render textures can be fixed-size or scene-scaled. They may be used as color targets, depth targets, SRVs, and UAVs depending on their flags and format.

---

## Commands

The command list is the runtime frame graph. Every enabled command is validated and then executed in order.

Supported command types:

| Command | Purpose |
|---|---|
| `clear` | Clear a color/depth target. Clear values can also come from compatible resources/UserCB-linked values. |
| `draw` / `draw_mesh` | Draw a mesh, primitive, or procedural source with a VS/PS shader. |
| `dispatch` | Run a compute shader with SRVs/UAVs and thread group counts. |
| `repeat` | Repeat child dispatch commands for iterative compute workflows. |
| `group` | Organize commands in the editor tree without directly issuing GPU work. |

Commands store their own bindings, draw state, shader params, transform, shadow options, profiler data, and optional parent/group relationship.

---

## Draw commands

Draw commands support:

- Mesh resources and built-in primitive meshes.
- Procedural draw sources where the shader synthesizes vertices from `SV_VertexID`.
- Per-command transform through `ObjectCB` when the shader declares it.
- Instancing.
- Multiple render targets.
- Optional depth target.
- Pixel shader SRVs.
- Pixel shader UAV outputs through the DX11 output-merger UAV path.
- Indirect draw argument buffers.
- Backface/cull/depth/blend state controls.
- Shadow casting and shadow receiving flags.

When drawing glTF meshes, material textures can be bound automatically to conventional pixel shader texture slots. Manual bindings can override those slots.

---

## Compute commands

Dispatch commands support:

- Compute shader resources compiled as `CSMain`.
- SRVs bound to `t0..t7`.
- UAVs bound to `u0..u7`.
- Editable/reflected UserCB params.
- Direct thread group counts.
- Optional indirect dispatch buffer and offset.
- Repeat blocks for iterative compute passes.

The normal editor/runtime path supports compute. The current 64k procedural player is intentionally VS/PS draw-focused and skips compute dispatches during export.

---

## UserCB and shader reflection

lazyTool has two related constant-buffer systems:

1. **`ObjectCB`**: engine-owned per-command object data, currently used for the world matrix.
2. **`UserCB`**: user-editable scalar/vector parameters reflected from shaders and exposed in the inspector/timeline.

Recommended register convention:

| Register | Owner | Purpose |
|---|---|---|
| `b0` | Engine | `SceneCB`: camera, time, light, shadow, and frame data. |
| `b1` | Engine | `ObjectCB`: per-command world transform for draw shaders. |
| `b2` | User shader | `UserCB`: editable shader parameters. |

Use this pattern for user parameters:

```hlsl
cbuffer UserCB : register(b2)
{
    float4 Color;
    float  Roughness;
    float  Metallic;
    float2 Tiling;
};
```

Important details:

- The cbuffer must be named exactly `UserCB` to be reflected as editable user parameters.
- `register(b2)` is the recommended slot because `b1` is reserved for `ObjectCB`.
- The reflection code records the actual bind slot from the compiled shader, so older shaders can still work if they put `UserCB` somewhere else.
- The inspector exposes simple scalar/vector values: `float`, `float2`, `float3`, and `float4`.
- Matrices, arrays, and complex structs are intentionally not exposed as editable UserCB variables.
- UserCB values can be edited per command, linked to global value resources, and animated on the timeline.

### Global UserCB panel

The **User CB** panel lets you create global shader variables backed by resources. This is useful when several commands should share the same animated or editable value.

Typical flow:

1. Create a `float`, `float2`, `float3`, or `float4` resource.
2. Add a matching variable in the **User CB** panel.
3. Link that variable to the resource.
4. Use the same variable name in shader `UserCB` declarations.
5. Animate the resource/variable on the timeline if needed.

The packed layout uses 16-byte-friendly slots so scalar/vector values remain predictable across VS, PS, and CS use.

---

## Timeline animation

The timeline is stored in `.lt` project files and evaluated at runtime before command execution.

It can animate:

| Track target | Animated data |
|---|---|
| UserCB variable / value resource | `float`, `float2`, `float3`, `float4` values. |
| Command transform | Position, rotation, scale. |
| Command enabled state | On/off animation for pipeline steps. |
| Camera | Position, direction/FOV-related state. |
| Directional light | Light position/target/color/intensity and shadow-related setup. |

Timeline settings include FPS, length, play/pause, looping, forward/backward playback, and interpolation.

The timeline is useful in both export paths:

- The normal packed player preserves the full editor runtime behavior.
- The 64k exporter includes supported timeline tracks for procedural projects.

---

## Lighting and shadows

The editor provides directional light state and a shadow-map workflow.

Shader-side scene data comes through `SceneCB` at `b0`, including camera matrices, light direction/color, shadow matrices, cascade data, and camera vectors.

Draw commands can opt into:

- **Shadow casting**: command contributes to the shadow map.
- **Shadow receiving**: command samples the shadow map during its visible pass.
- **Explicit shadow shader**: a shader used for the shadow pass.
- **Fallback/built-in primitive shadow path**: used where supported for primitive meshes, especially in the 64k path.

The shadow map is also exposed as a built-in resource, conventionally sampled from pixel shader slot `t7` when shadow receiving is enabled.

---

## Camera and viewport

The scene is rendered into an off-screen texture and displayed in the ImGui viewport.

Common viewport controls:

- Right mouse button: mouse look.
- `WASD`: move.
- `Q` / `E`: down/up.
- `Shift`: faster movement.
- `Ctrl`: slower movement.
- `L`: orbit the directional light.
- `F11`: fullscreen viewport.

The grid is an editor overlay and can be toggled from settings.

---

## Profiling and logs

The GPU profiler uses Direct3D 11 timestamp queries.

It reports:

- Total frame GPU time.
- Per-command GPU time.
- Whether the latest profile frame is ready.
- Basic scene/runtime status.

The log panel records:

- Shader compilation errors.
- Resource load failures.
- Validation warnings.
- Export messages.
- Runtime warnings.
- Scene restart/resize events.

---

## Project files

Projects are saved as plain-text `.lt` files.

Saved data includes:

- Camera and viewport-relevant state.
- Directional light and shadow settings.
- Resource list and resource properties.
- Command list, command hierarchy, bindings, params, states, and transforms.
- UserCB definitions and linked resources.
- Timeline settings, tracks, and keys.
- Mesh part/material enable state where applicable.

The format is intentionally line-oriented and diff-friendly. Save is strict; load is more permissive so older projects can survive new fields.

---

## Normal standalone EXE export

The normal export path is for full projects that may use external assets such as textures, glTF meshes, buffers, and shaders.

Build first:

```bat
build.bat
```

Then export from the editor with **Export EXE**, or use the CLI:

```bat
bin\lazyTool.exe --export projects\scene.lt bin\scene.exe
```

How it works:

1. `build.bat` creates `bin/lazyPlayer.exe`.
2. The exporter loads the `.lt` project.
3. Referenced shaders/assets are collected.
4. The project and assets are packed and appended to the player executable.
5. At runtime the player reads the embedded pack first, then falls back to disk paths where appropriate.

This is the right path for asset-heavy editor scenes.

---

## 64k procedural exporter

`build64k/` is a separate exporter for small procedural demos. It reads a normal lazyTool `.lt` project and emits a compact single-file C player.

This path is intentionally stricter than the normal player: it does **not** pack external asset files. The project should be procedural and recreate everything from shaders, primitive resources, render textures, and command data.

Build from a Developer Command Prompt:

```bat
cd build64k
build.bat ..\projects\procedural_spheres_pbr_post.lt
```

The script performs these steps:

1. Compile `build64k.cpp` into `build64k.exe`.
2. Run `build64k.exe` to generate `out64k.c` from the `.lt` project.
3. Compile `out64k.c` into `lt64k.exe` with a tiny `/NODEFAULTLIB` setup.
4. Copy the uncompressed executable to `lt64k_unpacked.exe`.
5. Optionally compress `lt64k.exe` with `upx.exe` from the same folder.

Generated/output files:

| File | Meaning |
|---|---|
| `build64k.exe` | Exporter executable. |
| `out64k.c` | Generated tiny runtime source. |
| `lt64k_unpacked.exe` | Unpacked tiny player, useful for testing and size comparison. |
| `lt64k.exe` | Final packed executable, usually UPX-compressed. |

Optional switches:

```bat
set "LT64K_SKIP_UPX=1"
build.bat ..\projects\procedural_spheres_pbr_post.lt
```

```bat
set "LT64K_CFLAGS=/DLT_DEBUG_FPS=1 /DLT_VSYNC=1"
build.bat ..\projects\procedural_spheres_pbr_post.lt
```

Supported in the current 64k path:

- VS/PS shader resources with source embedded into the generated C file.
- HLSL include inlining and optional shader-source minification.
- Primitive meshes and procedural draw sources.
- Render textures, scene color, scene depth, and shadow map built-ins.
- Command params and UserCB values.
- Supported timeline tracks.
- Directional light and shadow data.
- Draw commands, clears, render target/depth binding, and procedural shadow fallbacks where available.

Not supported in the current 64k path:

- External texture files.
- glTF/GLB mesh assets.
- External binary buffers.
- General compute/UAV dispatch execution in the tiny player.
- Arbitrary editor-only UI/runtime features.

Use the normal standalone export for asset-heavy scenes. Use `build64k/` for fully procedural demos where small executable size matters.

---

## Procedural PBR/post sample

The main procedural 64k-oriented sample is:

```text
projects/procedural_spheres_pbr_post.lt
shaders/procedural_spheres_pbr_post/
```

It demonstrates a fully procedural pipeline with no external mesh or texture assets.

The shader folder contains stages for:

- Shared scene/PBR helpers.
- Procedural sphere scene rendering.
- Shadow pass support.
- SSAO.
- Bilateral blur.
- Bloom prefilter and blur.
- Depth of field / circle of confusion.
- Lens flare.
- Final composite.

This project is the best starting point for the 64k exporter because it is designed around procedural draw data and generated render targets instead of external files.

---

## Binding conventions

### Global constant buffers

| Register | Name | Bound by | Notes |
|---|---|---|---|
| `b0` | `SceneCB` | Engine | Camera, time, light, shadow, and frame matrices. |
| `b1` | `ObjectCB` | Engine | Per-command world matrix for draw shaders that declare it. |
| `b2` recommended | `UserCB` | Engine from reflected user data | Editable scalar/vector shader parameters. The actual reflected slot is used. |

### Draw shader resource slots

| Binding | Shader stage | Slots |
|---|---|---|
| Material/user textures | Pixel Shader | `t0..t7` |
| Vertex SRVs | Vertex Shader | `t0..t7` |
| Pixel UAVs | Pixel Shader / output merger | `u0..u7`, following DX11 OM UAV rules |

Common pixel texture convention:

| Slot | Common use |
|---:|---|
| `t0` | Base color / albedo. |
| `t1` | Metallic/roughness. |
| `t2` | Normal map. |
| `t3` | Emissive. |
| `t4` | Occlusion. |
| `t5` | Environment map / HDRI. |
| `t6` | Free user-defined slot. |
| `t7` | Shadow map when shadow receiving is enabled. |

### Compute shader slots

| Binding | Shader stage | Slots |
|---|---|---|
| SRVs | Compute Shader | `t0..t7` |
| UAVs | Compute Shader | `u0..u7` |
| UserCB parameters | Compute Shader | Reflected `UserCB` register, recommended `b2` |

---

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `Space` | Pause/resume scene execution. |
| `F6` | Restart scene from frame 0. |
| `F11` | Toggle viewport fullscreen. |
| `F5` | Recompile shaders. |
| `Ctrl + S` | Save project. |
| `F1` | Show/hide shortcuts panel. |
| Arrow keys | Navigate resources/commands. |
| `Enter` | Select focused item. |
| `F2` | Rename selected resource or command. |
| `Delete` | Delete selection. |
| `X` | Enable/disable selected command. |
| Right mouse button | Mouse look in viewport. |
| `WASD` | Move camera. |
| `Q` / `E` | Move camera down/up. |
| `Shift` | Fast movement. |
| `Ctrl` | Slow movement. |
| `L` | Orbit directional light. |

---

## Internal limits

These are fixed-size limits from the current codebase:

| Limit | Value |
|---|---:|
| Maximum resources | 256 |
| Maximum commands | 256 |
| Maximum name length | 64 |
| Maximum path length | 256 |
| Texture slots | 8 |
| SRV slots | 8 |
| UAV slots | 8 |
| Render targets per draw | 4 |
| Textures per mesh material | 5 |
| Parts per mesh | 128 |
| Materials per mesh | 64 |
| UserCB variables | 64 |
| Reflected shader CB variables | 32 |
| Params per command | 32 |

---

## Current limitations

- Windows / DirectX 11 only.
- Shader Model 5.0.
- VS/PS shaders are expected to expose `VSMain` and `PSMain`.
- Compute shaders are expected to expose `CSMain`.
- The default mesh input layout is `POSITION`, `NORMAL`, and `TEXCOORD0`.
- Editable reflected shader parameters must live in a cbuffer named `UserCB`.
- Reflected `UserCB` editing supports simple scalar/vector values only, not matrices, arrays, or complex structs.
- `Repeat` is intended for compute iteration in the normal runtime, not repeated draw submission.
- glTF support is focused on triangle primitives.
- Some paths or names with spaces may be problematic because the `.lt` parser is token-based.
- glTF textures embedded as data URIs are not supported yet.
- There is no node graph; the pipeline is edited as a command list/tree.
- The 64k path is procedural-only and intentionally skips unsupported asset/compute features.

---

## Development notes

- Comments in the codebase focus on ownership, data flow, and places where editor/runtime/export behavior can diverge.
- The normal editor/player code path lives in `src/`.
- The compact procedural export path lives in `build64k/` and should be treated as a separate target with a deliberately smaller feature set.
- When changing shader parameter layout, keep `SceneCB`, `ObjectCB`, and `UserCB` ownership separate.
- When adding new serializable fields, update both save and load paths in `project.cpp`, and consider whether `build64k.cpp` also needs to parse them.
- When adding render features to the editor, decide explicitly whether they belong in the normal player only or also in the procedural 64k player.

---

## Philosophy

lazyTool prioritizes:

- Fast iteration.
- Explicit pipeline control.
- Inspectable GPU state.
- Practical shader experimentation.
- Small, understandable data structures.
- Export paths that match different goals: full assets for normal projects, tiny procedural output for 64k-style demos.

It intentionally does not hide the render pipeline. The value of the tool is that targets, SRVs, UAVs, shaders, params, command state, and timeline data remain visible and directly editable.
