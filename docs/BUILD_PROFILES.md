# Build profiles

lazyTool currently supports three build profiles through `build.bat`:

```bat
build.bat fast
build.bat profile
build.bat release
```

Every successful invocation increments `build/build_number.txt` and generates a
compact build code from local date/time plus that counter. The editor displays
that code after the workspace name in the top bar.

## fast

Use this while editing C++ frequently. It compiles with low optimization (`/Od /Ob0`) and keeps development diagnostics available. It is the fastest profile to build, but it is not representative of final runtime performance.

Enabled by default:

- D3D11 runtime validation toggle.
- Shader binding warning toggle.
- CPU/GPU profiler UI.
- Debug overlays such as bounds visualization.
- Basic monitoring readouts.

## profile

Use this when measuring performance during development. It compiles the editor with high optimization, while keeping the profiler and diagnostic toggles available. It is slower to build than `fast`, but the runtime numbers are much closer to `release`.

Enabled by default:

- CPU/GPU profiler UI.
- Shader binding warning toggle.
- D3D11 validation toggle, off unless explicitly enabled.
- Debug overlays.
- Basic monitoring readouts.

## release

Use this for a clean editor build or final checks. It keeps high optimization and compiles out expensive debug/profiling systems instead of merely disabling them at runtime.

After compilation, `release` removes intermediate files from `bin/` and creates
`dist/lazyTool_build_<build-code>.zip`.

Compiled out / forced off:

- D3D11 debug-layer validation and InfoQueue message flushing.
- Shader binding warning diagnostics.
- GPU timestamp query profiler for commands, shadow pass and ImGui.
- Detailed CPU profiler UI.
- Debug bounds overlay and its viewport toggle.

Still available:

- Basic monitoring: FPS, frame time, scene size, command count, resource count, process memory and estimated GPU memory.
- Normal editor viewport aids such as grid and camera orientation gizmo.
- Critical error logging and shader compilation errors.

## Central switches

The profile-specific feature gates live in `src/build_config.h`:

```cpp
LAZYTOOL_ENABLE_PROFILER
LAZYTOOL_ENABLE_D3D11_VALIDATION
LAZYTOOL_ENABLE_SHADER_BINDING_WARNINGS
LAZYTOOL_ENABLE_DEBUG_OVERLAYS
LAZYTOOL_ENABLE_BASIC_MONITORING
```

`build.bat` defines one of:

```cpp
LAZYTOOL_CONFIG_FAST
LAZYTOOL_CONFIG_PROFILE
LAZYTOOL_CONFIG_RELEASE
```

Use the feature gates instead of string-comparing `LAZYTOOL_BUILD_CONFIG`.
