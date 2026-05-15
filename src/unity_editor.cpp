// unity_editor.cpp
//
// Unity translation unit for the editor build. build.bat intentionally compiles
// through this file only; isolated multi-TU editor builds are no longer kept.
//
// Keep impl.cpp first so header-only library implementations are emitted once
// and their implementation macros are undefined before the rest of the code is
// included.

// Dear ImGui's own .cpp files rely on the optional ImVec2/ImVec4 math
// operators. In unity builds, editor files may include imgui_internal.h before
// imgui.cpp, so define it globally for the whole unity TU.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "impl.cpp"
#include "log.cpp"
#include "dx11_ctx.cpp"
#include "shader.cpp"
#include "resources.cpp"
#include "commands.cpp"
#include "project.cpp"
#include "app_settings.cpp"
#include "embedded_pack.cpp"
#include "timeline.cpp"
#include "user_cb.cpp"
#include "ui.cpp"
#include "main.cpp"

// Dear ImGui editor-only implementation units.
#include "../external/imgui/imgui.cpp"
#include "../external/imgui/imgui_draw.cpp"
#include "../external/imgui/imgui_tables.cpp"
#include "../external/imgui/imgui_widgets.cpp"
#include "../external/imgui/backends/imgui_impl_win32.cpp"
#include "../external/imgui/backends/imgui_impl_dx11.cpp"
