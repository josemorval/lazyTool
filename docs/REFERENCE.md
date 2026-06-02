# lazyTool technical reference

This is the longer technical reference for the editor/runtime. The root `README.md` is intentionally shorter and optimized as a GitHub landing page.

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Windows](https://img.shields.io/badge/platform-Windows-0078D4)
![DirectX 11](https://img.shields.io/badge/graphics-DirectX%2011-lightgrey)
![Dear ImGui](https://img.shields.io/badge/UI-Dear%20ImGui-ff69b4)
![HLSL SM5](https://img.shields.io/badge/shaders-HLSL%20SM5.0-orange)

**lazyTool** is an experimental DirectX 11 render-pipeline editor for building, inspecting, animating, and exporting real-time graphics scenes.

It is closer to a low-level graphics workbench than to a traditional game editor: resources, passes, render targets, SRVs, UAVs, shader parameters, timing, and export settings stay visible and directly editable.

> This project intentionally favors explicit GPU control, small data structures, and fast iteration over hiding the rendering pipeline behind higher-level abstractions.

---

## Contents

- [Current scope](#current-scope)
- [Default scene](#default-scene)
- [Repository layout](#repository-layout)
- [Requirements](#requirements)
- [Build and run](#build-and-run)
- [Creating a project](#creating-a-project)
- [Editor overview](#editor-overview)
- [Resources](#resources)
- [Commands](#commands)
- [Inspector and bindings](#inspector-and-bindings)
- [Shader workflow](#shader-workflow)
- [UserCB and shader parameters](#usercb-and-shader-parameters)
- [Timeline animation](#timeline-animation)
- [Render Graph view](#render-graph-view)
- [Viewport and camera](#viewport-and-camera)
- [Lighting and shadows](#lighting-and-shadows)
- [Profiling and diagnostics](#profiling-and-diagnostics)
- [Project files](#project-files)
- [Exporting](#exporting)
- [64k procedural exporter](#64k-procedural-exporter)
- [Binding conventions](#binding-conventions)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Internal limits](#internal-limits)
- [Known limitations](#known-limitations)
- [Development notes](#development-notes)

---

## Current scope

| Area | Capability |
|---|---|
| Platform | Windows desktop |
| Graphics API | DirectX 11 |
| UI | Dear ImGui editor with docking, custom toolbar, inspector, timeline, shader editor, and render-graph view |
| Shader model | HLSL Shader Model 5.0 |
| Shader workflow | Runtime compilation, fallback shaders, compile errors, reflection, include tracking, source editor, and hot recompilation |
| Pipeline model | Ordered command tree: clear, draw, dispatch, indirect work, groups, and repeat containers |
| Resources | Numeric values, textures, render textures, 3D render textures, structured buffers, meshes, primitives, shaders, Gaussian splat buffers, and built-ins |
| Draw path | Mesh/procedural draws, instancing, indirect draw, MRTs, PS SRVs, OM UAVs, render state toggles, shadows |
| Compute path | Direct and indirect dispatch, SRV/UAV binding, dispatch size from source resource, optional reset-only dispatch |
| Parameters | Reflected `UserCB`, global User CB panel, command-local parameters, source-driven variables, timeline animation |
| Animation | Multiple timeline clips, sparse keys, interpolation, UserCB/value tracks, command transform/enabled tracks, camera and light tracks |
| Debugging | Log panel, shader diagnostics, D3D11 validation toggle, binding warnings, resource previews, debug bounds |
| Profiling | CPU frame breakdown, GPU timestamp profiler, per-command timing, shadow-pass timing, memory readouts |
| Persistence | Plain-text `.lt` project files |
| Export | Full packed EXE export and a stricter procedural 64k-style C exporter |

---

## Default scene

A new project opens with a minimal valid pipeline:

```text
Resources
  normal_cube      mesh primitive: cube
  normal_color     shader: shaders/default.hlsl

Commands
  clear_scene       clears scene color/depth
  draw_normal_cube  draws the cube to scene color/depth
```

The default shader is intentionally small and useful as a reference. It uses:

- `SceneCB` at `b0` for camera, time, light, and shadow data.
- `ObjectCB` at `b1` for the command `LocalToWorld` matrix.
- the built-in shadow map on pixel slot `t7`.
- cascade data from `SceneCB`.

This makes the first scene useful for checking the viewport, command inspector, shadow setup, shader compilation, and project serialization without needing external assets.

---

## Repository layout

```text
lazyTool/
├─ src/                    # C++17 editor/runtime/player/export code
├─ shaders/                # HLSL files used by projects; includes the default shader
├─ projects/               # .lt project files
├─ build64k/               # procedural-only tiny C player generator
├─ build.bat               # MSVC build script for editor + player
└─ README.md               # this document
```

Main source files:

| File | Responsibility |
|---|---|
| `src/main.cpp` | Win32 entry point, editor/player mode, frame loop, camera input, timeline playback, CLI export/play options. |
| `src/dx11_ctx.cpp` | D3D11 device, swap chain, scene targets, samplers, render states, shadow resources, scene constant buffer. |
| `src/resources.cpp` | Resource creation/reload/release, render textures, structured buffers, image loading, glTF import, primitives, Gaussian splats. |
| `src/commands.cpp` | Command execution, validation, draw/dispatch/indirect paths, groups/repeat, shadow prepass, GPU profiling. |
| `src/shader.cpp` | HLSL compilation, fallback shaders, shader reflection for `SceneCB`, `ObjectCB`, `UserCB`, SRVs, UAVs, samplers. |
| `src/user_cb.cpp` | Global and command-local UserCB packing, source-driven variables, reflected parameter synchronization. |
| `src/timeline.cpp` | Timeline clips, tracks, keys, interpolation, runtime application. |
| `src/project.cpp` | Strict current-format `.lt` save/load, default scene creation, and export settings. |
| `src/embedded_pack.cpp` | Normal standalone EXE export by appending project/assets/shaders to the player executable. |
| `src/ui.cpp` | Dear ImGui shell: toolbar, panels, inspector, viewport, shader editor, render graph, timeline, logs. |
| `src/app_settings.cpp` | Editor preferences stored outside project data: UI scale, profiler, validation, camera controls, grid. |
| `build64k/build64k.cpp` | Strict procedural `.lt` to single-file C exporter for tiny executables. |

A full build checkout also needs the external dependencies and application resources referenced by `build.bat`.

---

## Requirements

- Windows.
- Visual Studio / MSVC, preferably from a **Developer Command Prompt**.
- Windows SDK with Direct3D 11 headers and libraries.
- DirectX 11-capable GPU.
- Vendored dependencies expected by `build.bat`, including Dear ImGui, stb_image, cgltf, and NanoSVG.

The editor/runtime is C++17. The generated 64k player source is C17.

---

## Build and run

From a Developer Command Prompt:

```bat
build.bat
```

The first argument selects the build profile:

```bat
build.bat fast
build.bat profile
build.bat release
```

Outputs:

| Output | Purpose |
|---|---|
| `bin/lazyTool.exe` | Full editor and normal exporter. |
| `bin/lazyPlayer.exe` | Player stub used by packed standalone exports. |

The script cleans `bin/`, creates a generated build-info header, builds both executables, copies runtime folders when present, and launches the editor unless `norun` is passed.

Every successful build increments `build/build_number.txt`. The visible build code folds local `yyMMddHHmm` plus the counter into base36 and appears after the workspace name in the top bar.

Release builds also remove intermediate files from `bin/` and create `dist/lazyTool_build_<build-code>.zip`.

---

## Creating a project

A typical workflow is:

1. Create or load resources from the **Resources** panel.
2. Create commands from the **Command Pipeline** panel.
3. Select a command and configure targets, shader, mesh/procedural source, bindings, params, state, transform, and shadow options in the **Inspector**.
4. Use `F5` to compile all shaders, or `Ctrl+D` for the edited/selected shader.
5. Use the viewport, log, resource previews, render graph, and profiler to inspect the result.
6. Save the project as a `.lt` file.
7. Export either as a normal packed EXE or through the procedural 64k path.

A simple project can be made from only built-in primitives, render textures, generated buffers, and HLSL. An asset-heavy project can also reference external textures, glTF/GLB meshes, Gaussian splat PLY files, and shader includes.

---

## Editor overview

| Panel / window | Purpose |
|---|---|
| **Resources** | Create, filter, edit, reload, preview, and delete resources. User and built-in resources are shown separately. |
| **Command Pipeline** | Add, reorder, parent, copy/paste, enable/disable, and select frame commands. |
| **Inspector** | Context-sensitive editor for the selected resource or command. |
| **Bindings** | Readable summary of the currently selected command's shader/resource bindings. |
| **State** | Render/runtime state view for selected commands. |
| **Scene** | Live viewport rendered to an off-screen scene surface. |
| **Timeline** | Keyframe editor for parameters, command state, camera, and light. |
| **Shader Editor** | Built-in HLSL editor, optionally floating. |
| **Render Graph** | Visual read/write graph of commands and resources. |
| **User CB** | Global shader variable panel for shared and source-driven values. |
| **General** | Editor preferences, export settings, diagnostics, profiler, viewport, and camera controls. |
| **Log** | Runtime messages, shader errors, validation warnings, export messages, and scene events. |

The top toolbar exposes project operations, shader compilation, timeline, shader editor, render graph, export, shortcuts, current project name, and a compact status/profiler readout.

---

## Resources

Resources are one-based handles stored in a fixed table. This keeps serialization simple and references stable across editor operations.

| Resource kind | Notes |
|---|---|
| `int`, `int2`, `int3` | Editable integer values; can drive UserCB variables and resource-size data. |
| `float`, `float2`, `float3`, `float4` | Editable numeric values; can feed UserCB variables, command params, clear values, and timeline tracks. |
| `texture2d` | File-backed image texture loaded through stb_image; LDR and HDR paths are supported. |
| `render_texture2d` | Internal GPU texture with configurable RTV/SRV/UAV/DSV flags and optional scene-relative sizing. |
| `render_texture3d` | Internal 3D texture with SRV/UAV support and slice preview. |
| `structured_buffer` | GPU buffer with SRV/UAV flags and optional indirect-argument layout. |
| `mesh` | Imported glTF/GLB mesh data with parts, material slots, bounds, and material texture references. |
| `mesh_primitive` | Built-in cube, quad, tetrahedron, sphere, and fullscreen triangle meshes. |
| `gaussian_splat` | PLY splat buffer uploaded as a structured SRV with bounds and SH metadata. |
| `shader` | VS/PS or CS HLSL program with compile status, fallback state, reflection data, and source editor support. |

Built-ins are available as resources too:

| Built-in | Purpose |
|---|---|
| `time` | Runtime time/delta/frame data. |
| `scene_color` | Main editor/player scene color target. |
| `scene_depth` | Main scene depth target. |
| `shadow_map` | Main-light shadow `Texture2DArray` depth resource. |
| `light` | Main light and shadow settings. |

Some resources generate helper resources. For example, resources with dimensions/counts can expose implicit size values that may be linked into UserCB variables.

---

## Commands

The command list is the frame pipeline. Enabled commands are validated and executed in order, with groups and repeat containers providing hierarchy.

| Command | Purpose |
|---|---|
| `Clear` | Clear a color target and/or depth target. Clear color/depth may come from hardcoded values, value resources, or UserCB entries. |
| `Group` | Editor/runtime container for organizing child commands. |
| `Repeat` | Execute child commands multiple times; primarily useful for iterative compute workflows. |
| `DrawMesh` | Draw a mesh, primitive, or procedural vertex source with a VS/PS shader. |
| `DrawInstanced` | Draw mesh/procedural geometry with an instance count. |
| `IndirectDraw` | Draw through a D3D11 indirect argument buffer. |
| `Dispatch` | Execute a compute shader with explicit or source-derived group counts. |
| `IndirectDispatch` | Dispatch through an indirect argument buffer. |

Draw commands can use:

- mesh or procedural vertex sources;
- triangle-list or point-list topology;
- multiple render targets;
- depth target/test/write options;
- pixel SRVs and OM UAV outputs;
- vertex-stage SRVs;
- instancing;
- indirect draw arguments;
- color write, alpha blend, culling, depth test/write toggles;
- shadow casting and receiving.

Dispatch commands can use:

- compute shaders compiled as `CSMain`;
- SRVs at `t0..t7`;
- UAVs at `u0..u7`;
- reflected UserCB params;
- explicit group counts;
- group counts derived from a resource size;
- optional indirect dispatch;
- reset-only execution for setup passes.

---

## Inspector and bindings

The inspector is deliberately explicit. For a selected command it exposes the pieces that affect the D3D11 call:

- command enabled state and hierarchy data;
- clear targets and clear sources;
- render targets, depth target, and extra MRT slots;
- mesh/procedural source, shader, topology, vertex/instance counts;
- transform and computed world bounds;
- SRV and UAV binding arrays;
- reflected shader parameters;
- render-state toggles;
- shadow caster/receiver/shadow-shader options;
- notes, when inspector notes are enabled.

For a selected resource it exposes type-specific controls: file path/reload, render texture flags, buffer shape, mesh parts/materials/bounds, primitive switching, light and shadow setup, shader source, shader reflection, previews, and estimated memory.

The **Bindings** panel summarizes what the selected command is currently expected to bind, which is useful when debugging shader reflection or manual overrides.

---

## Shader workflow

Shader resources are either VS/PS programs or CS programs.

Conventions:

| Program | Expected entries |
|---|---|
| VS/PS shader | `VSMain` and `PSMain` |
| Compute shader | `CSMain` |

The editor supports:

- runtime HLSL compilation;
- fallback shaders when compilation fails;
- compile errors in the log and shader resource inspector;
- reflection of `ObjectCB`, `UserCB`, SRVs, UAVs, and samplers;
- compile-all with `F5`;
- compile selected/edited shader with `Ctrl+D`;
- source save with `Ctrl+S`;
- inline shader editor with syntax coloring, undo/redo, selection, cursor status, and autocomplete;
- include navigation/back stack from the shader editor;
- a floating shader editor window;
- starter shader template creation when a referenced `.hlsl` path does not exist.
- optional shared helper includes for PBR, raymarching, and atmospheric lighting; see `docs/SHADER_HELPERS.md`.

Template buttons are available for common VS/PS and compute starting points. They are meant to create a readable first file at the resource path, not to impose a sample project structure.

---

## UserCB and shader parameters

lazyTool separates engine-owned constant buffers from user-editable shader data.

| Register | Owner | Purpose |
|---|---|---|
| `b0` | Engine | `SceneCB`: camera, time, light, shadow, previous-frame matrices, and frame data. |
| `b1` | Engine | `ObjectCB`: per-command `LocalToWorld` matrix for draw shaders. |
| `b2` recommended | User shader | `UserCB`: editable scalar/vector parameters. The actual reflected slot is used at runtime. |

Recommended pattern:

```hlsl
cbuffer UserCB : register(b2)
{
    float4 Color;
    float  Roughness;
    float2 Tiling;
};
```

Important details:

- The cbuffer must be named exactly `UserCB` to be reflected as editable user data.
- `register(b2)` is the preferred convention, but the runtime uses the reflected bind slot.
- Supported reflected types are `int`, `int2`, `int3`, `float`, `float2`, `float3`, and `float4`.
- Matrices, arrays, and nested structs are intentionally not exposed in the UI.
- Global User CB variables are packed into 16-byte slots and the panel shows a matching HLSL snippet using `packoffset`.

Value editing is intentionally lightweight:

| Type | Default editor |
|---|---|
| `float` | `0..1` slider |
| `float2` | `0..1` two-component slider |
| `float3` | ImGui `ColorEdit3` in float display mode by default |
| `float4` | ImGui `ColorEdit4` in float display mode by default |
| `int`, `int2`, `int3` | integer inputs |

For `float3` and `float4`, the native ImGui color editor is used because it gives the useful right-click options menu. The editor default is `Float` display mode, so values start as `0.0..1.0` floats instead of an RGB-style integer view.

UserCB variables can be:

- hardcoded in the User CB panel;
- linked to compatible value resources;
- driven by command transform data;
- driven by camera position/rotation;
- driven by light position/target;
- animated on the timeline.

Command-local reflected parameters can also be hardcoded or source-driven.

---

## Timeline animation

The timeline system stores sparse keys but displays them as frame slots.

Supported timeline tracks:

| Track | Data |
|---|---|
| User variable / value parameter | `int`, `int2`, `int3`, `float`, `float2`, `float3`, `float4` values. |
| Command transform | Position, rotation quaternion, and scale. |
| Command enabled | On/off state for a pipeline step. |
| Camera | Camera position/orientation/FOV-related state. |
| Light | Light position, target, color, intensity, type, spot settings, and shadow-related setup. |

Timeline features:

- multiple timeline clips per project;
- enable/disable individual clips;
- sequential playback/export of enabled clips;
- FPS and frame-count controls;
- play direction and looping;
- per-keyframe interpolation modes: Step/Flat, Linear, Quadratic, Cubic;
- per-keyframe cubic tangent scale;
- keyboard editing for insert/update/delete/copy/cut/paste keys;
- horizontal scrolling and zoom;
- automatic key capture when editing tracked values.

Exported players reset to the beginning of the enabled timeline sequence rather than inheriting the editor's currently scrubbed frame.

---

## Render Graph view

The Render Graph window is a visual debugging view of the current command/resource relationships.

It shows:

- command order;
- group/repeat hierarchy;
- resource reads, writes, and read/write usages;
- target, depth, SRV, UAV, parameter-source, clear-source, indirect-argument, and shadow-map relationships;
- enabled/disabled command state.

The view supports pan/zoom and node selection, and is meant as an inspection tool rather than a node editor. The actual project pipeline is still edited through the command tree and inspector.

---

## Viewport and camera

The scene is rendered into an off-screen target and displayed in the ImGui viewport.

Viewport/editor aids include:

- horizon-locked and free camera modes;
- orange grid overlay with distance fade;
- optional camera orientation gizmo;
- optional debug draw bounds;
- transform gizmos for selected commands;
- fullscreen viewport mode;
- frame/orbit selected bounds.

Common controls:

| Input | Action |
|---|---|
| RMB | Mouse look. |
| `WASD` | Move along camera forward/right axes. |
| `R` / `T` | Move up/down on camera up axis. |
| `Q` / `E` | Roll left/right. |
| `Shift` | Faster movement. |
| `Ctrl` | Slower movement. |
| `Alt + LMB` | Orbit selected bounds, or scene bounds if nothing specific is selected. |
| `F` | Frame selected bounds, or scene bounds. |
| `L` | Orbit light. |
| `1` / `2` / `3` | Move / rotate / scale gizmo while hovering the viewport. |
| `Esc` | Disable active viewport gizmo. |

---

## Lighting and shadows

The built-in main light is exposed as the `light` resource. It can be directional or spot depending on its `Light Type`.

It stores:

- light position and target;
- color and intensity;
- shadow-map resolution;
- shadow near/far and orthographic extent;
- cascade count, split distance/lambda, and per-cascade data.

Draw commands can opt into:

- shadow casting;
- shadow receiving;
- explicit shadow shader;
- built-in primitive shadow fallback where supported.

`SceneCB` exposes both the single shadow view-projection and per-layer cascade data. The built-in shadow map is a `Texture2DArray` conventionally bound to pixel shader slot `t7` when shadow receiving is enabled.

---

## Profiling and diagnostics

Diagnostics are split between the log, validation, and profiler systems.

The **Log** panel records:

- shader compile errors;
- resource load/reload errors;
- validation warnings;
- export messages;
- scene resize/restart events;
- runtime warnings.

The **General** panel exposes:

- VSync;
- editor frame cap;
- D3D11 runtime validation toggle;
- shader binding warnings;
- D3D11 message flush;
- GPU profiler toggle;
- CPU frame breakdown and memory readouts.

The GPU profiler uses D3D11 timestamp queries and can report total GPU time, command time, and shadow-pass time.

---

## Project files

Projects are saved as plain-text `.lt` files.

Saved data includes:

- camera state;
- light and shadow setup;
- export settings;
- resources and resource notes;
- mesh part/material enable state;
- User CB variables and sources;
- command hierarchy, state, targets, bindings, params, transforms, notes, and indirect data;
- timeline clips, tracks, keys, and playback settings.

The format is line-oriented and easy to diff. Save writes the current format strictly; load is more permissive for older fields where possible.

File paths are normalized to forward slashes in saved project text.

---

## Exporting

lazyTool has two export paths with different goals.

### Normal packed EXE

Use this when the project uses normal assets such as textures, glTF/GLB meshes, Gaussian splat files, buffers, and shader includes.

Build first:

```bat
build.bat release
```

Then export from the toolbar with **Export EXE**, or from the command line:

```bat
bin\lazyTool.exe --export projects\your_project.lt bin\your_project.exe
```

The exporter:

1. uses `bin/lazyPlayer.exe` as the base executable;
2. reads the `.lt` project;
3. collects referenced shaders, includes, textures, meshes, splats, and glTF sidecar files;
4. minifies project/shader text where appropriate;
5. appends a packed payload to the player executable.

Project-level standalone settings are stored in the `.lt` file:

- camera/light controls in exported player;
- autoplay timeline;
- exit after timeline;
- Escape closes player;
- VSync in export;
- show FPS in window title.

### Player mode without packing

The editor binary also supports launching a project in player mode:

```bat
bin\lazyTool.exe --play projects\your_project.lt
```

This is useful for checking player behavior before creating a packed EXE.

---

## 64k procedural exporter

`build64k/` is a stricter exporter for tiny procedural demos. It converts a `.lt` project into a compact single-file C D3D11 player.

Call it with the project path explicitly:

```bat
cd build64k
build.bat ..\projects\your_procedural_project.lt
```

The script builds the exporter, generates `out64k.c`, compiles `lt64k.exe`, and compresses it with the bundled UPX executable.

Supported in the procedural path:

- embedded VS/PS shader source;
- HLSL include inlining and optional source minification;
- primitive meshes and procedural draw sources;
- internal render textures and built-in scene/depth/shadow resources;
- clears, draw calls, render-target/depth binding;
- command parameters and UserCB values;
- supported timeline tracks;
- light and shadow data;
- selected export settings such as VSync, Escape-to-close, timeline exit behavior, and FPS title.

Not supported by design:

- external texture/HDR files;
- glTF/GLB mesh assets;
- Gaussian splat PLY assets;
- arbitrary external SRV resources;
- general compute/UAV/indirect GPU pipelines in the tiny player.

Use the normal packed EXE export for asset-heavy scenes. Use `build64k/` only for projects that can recreate their result procedurally from shaders, primitives, render textures, command data, and timeline data.

---

## Binding conventions

### Constant buffers

| Register | Name | Bound by | Notes |
|---|---|---|---|
| `b0` | `SceneCB` | Engine | Camera, time, light, shadow, previous-frame matrices, frame data. |
| `b1` | `ObjectCB` | Engine | Per-command `LocalToWorld` matrix when declared by the shader. |
| `b2` recommended | `UserCB` | Engine from reflection | Editable scalar/vector shader data. Actual reflected slot is used. |

### Draw shader resources

| Binding | Shader stage | Slots |
|---|---|---|
| Manual/material SRVs | Pixel Shader | `t0..t7` |
| Vertex SRVs | Vertex Shader | `t0..t7` |
| Pixel/OM UAVs | Pixel Shader / output merger | `u0..u7`, following DX11 OM UAV rules |

Common pixel texture convention:

| Slot | Common use |
|---:|---|
| `t0` | Base color / albedo. |
| `t1` | Metallic / roughness. |
| `t2` | Normal map. |
| `t3` | Emissive. |
| `t4` | Occlusion. |
| `t5` | Environment / HDRI. |
| `t6` | Free user slot. |
| `t7` | Shadow map when shadow receiving is enabled. |

### Compute shader resources

| Binding | Shader stage | Slots |
|---|---|---|
| SRVs | Compute Shader | `t0..t7` |
| UAVs | Compute Shader | `u0..u7` |
| UserCB | Compute Shader | Reflected `UserCB` register, recommended `b2` |

---

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| `Space` | Pause/resume scene execution. |
| `F6` | Restart scene from frame 0. |
| `F11` | Toggle viewport fullscreen. |
| `F5` | Compile all shaders. |
| `Ctrl+D` | Compile edited/selected shader. |
| `Ctrl+S` | Save shader source or project. |
| `F1` | Toggle shortcuts panel. |
| Arrow keys | Navigate resources/commands or timeline selection, depending on focus. |
| `Enter` | Select focused resource/command item. |
| `F2` | Rename selected resource or command. |
| `Delete` | Delete selected item or timeline key. |
| `X` | Toggle selected command enabled. |
| `Ctrl+C` / `Ctrl+V` | Copy/paste command subtree, or timeline key when timeline is focused. |
| `Ctrl+X` | Cut selected timeline key. |
| `I` | Insert/update timeline key on selected slot. |
| `Shift + Arrow` | Move timeline selection by 10 frames. |
| `1` / `2` / `3` | Move / rotate / scale viewport gizmo. |
| `Esc` | Disable active viewport gizmo; in player export, may close the player depending on settings. |

---

## Internal limits

Current fixed-size limits from the codebase:

| Limit | Value |
|---|---:|
| Maximum resources | 256 |
| Maximum commands | 256 |
| Name length | 64 |
| Path length | 256 |
| Notes per resource/command | 1024 bytes |
| Texture slots | 8 |
| SRV slots | 8 |
| UAV slots | 8 |
| Draw render targets | 4 |
| Shader resource reflection bindings | 32 |
| Mesh material textures | 5 |
| Mesh parts | 128 |
| Mesh materials | 64 |
| Shadow cascades | 4 |
| UserCB variables | 64 |
| Reflected shader CB variables | 32 |
| Command params | 32 |
| Timelines | 16 |
| Timeline tracks | 128 |
| Timeline keys | 256 |
| Timeline frames | 1024 |

---

## Known limitations

- Windows / DirectX 11 only.
- Shader Model 5.0 only.
- VS/PS shaders are expected to expose `VSMain` and `PSMain`.
- Compute shaders are expected to expose `CSMain`.
- The default mesh layout is `POSITION`, `NORMAL`, and `TEXCOORD0`.
- Editable reflected shader parameters must live in a cbuffer named `UserCB`.
- UserCB editing supports simple scalar/vector values, not matrices, arrays, or complex structs.
- `Repeat` is primarily intended for compute iteration, not as a high-level draw instancing feature.
- glTF import is focused on triangle primitives.
- Embedded glTF data URIs are not the preferred asset path.
- Some paths or names with spaces can be problematic because parts of the `.lt` parser are token-based.
- The Render Graph window is an inspector/debugger, not a node authoring graph.
- The 64k path is procedural-only and intentionally skips unsupported asset/compute features.

---

## Development notes

- Keep `SceneCB`, `ObjectCB`, and `UserCB` ownership separate.
- When adding serializable fields, update both save and load paths in `project.cpp`.
- If the feature affects exported players, update normal export packing in `embedded_pack.cpp`.
- If the feature should exist in the tiny procedural path, update `build64k/build64k.cpp` explicitly.
- When adding a new resource or command type, consider inspector UI, project serialization, execution, render graph usage tracing, and export support as separate tasks.
- Editor preferences belong in `lazytool_general.ini`; project/runtime behavior belongs in `.lt` project data.
- The value of the tool is that GPU state stays inspectable: prefer explicit fields and readable diagnostics over hidden magic.
