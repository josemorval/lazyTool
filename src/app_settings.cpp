#include "app_settings.h"
#include "build_config.h"
#include "dx11_ctx.h"
#include "commands.h"
#include "ui.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// app_settings.cpp stores editor-wide preferences that should survive across
// projects, such as VSync, profiling, camera interaction defaults, and UI scale.

static const char* s_settings_path = "lazytool_general.ini";


static void app_settings_apply_build_config_constraints() {
#if !LAZYTOOL_ENABLE_D3D11_VALIDATION
    g_dx.d3d11_validation = false;
    g_dx.d3d11_validation_active = false;
    g_dx.d3d11_validation_supported = false;
#endif
#if !LAZYTOOL_ENABLE_SHADER_BINDING_WARNINGS
    g_dx.shader_validation_warnings = false;
#endif
#if !LAZYTOOL_ENABLE_PROFILER
    g_profiler_enabled = false;
#endif
#if !LAZYTOOL_ENABLE_DEBUG_OVERLAYS
    g_dx.scene_bounds_debug_enabled = false;
#endif
}

// Editor-wide defaults live outside project files so project-level export
// settings cannot unexpectedly re-enable expensive runtime features like profiling.
static void app_settings_apply_defaults() {
    g_dx.vsync = false;
    g_dx.d3d11_validation = false;
    g_dx.d3d11_validation_active = false;
    g_dx.d3d11_validation_supported = true;
    g_dx.shader_validation_warnings = false;
    g_dx.scene_grid_enabled = true;
    g_dx.scene_orientation_gizmo_enabled = true;
    g_dx.scene_orientation_gizmo_size_px = 80.0f;
    g_dx.scene_manual_control_overlay_enabled = true;
    g_dx.scene_bounds_debug_enabled = false;
    g_dx.scene_grid_color[0] = 1.00f;
    g_dx.scene_grid_color[1] = 0.50f;
    g_dx.scene_grid_color[2] = 0.01f;
    g_dx.scene_grid_color[3] = 0.5f;
    g_dx.scene_grid_level_alpha[0] = 0.6f;
    g_dx.scene_grid_level_alpha[1] = 0.8f;
    g_dx.scene_grid_level_alpha[2] = 1.00f;
    g_dx.scene_grid_distance_fade = true;
    g_dx.scene_grid_fade_start = 0.0f;
    g_dx.scene_grid_fade_end = 60.0f;
    g_profiler_enabled = false;
    app_set_editor_frame_cap_fps(0.0f);
    app_settings_apply_build_config_constraints();

    g_camera_controls.enabled = true;
    g_camera_controls.mouse_look = true;
    g_camera_controls.invert_y = false;
    g_camera_controls.mode = CAMERA_MODE_HORIZON_LOCKED;
    g_camera_controls.move_speed = 1.0f;
    g_camera_controls.fast_mult = 4.0f;
    g_camera_controls.slow_mult = 0.25f;
    g_camera_controls.mouse_sensitivity = 0.0025f;

    ui_set_code_font_size(16.0f);
    ui_set_show_inspector_notes(false);
    ui_set_show_interface_hints(true);
    ui_set_render_target_preview_columns(4);
}

void app_settings_save() {
    app_settings_apply_build_config_constraints();

    FILE* f = fopen(s_settings_path, "wb");
    if (!f) {
        log_warn("Settings save failed: %s", s_settings_path);
        return;
    }

    fprintf(f, "vsync %d\n", g_dx.vsync ? 1 : 0);
    fprintf(f, "d3d11_validation %d\n", g_dx.d3d11_validation ? 1 : 0);
    fprintf(f, "shader_validation_warnings %d\n", g_dx.shader_validation_warnings ? 1 : 0);
    fprintf(f, "profiler %d\n", g_profiler_enabled ? 1 : 0);
    fprintf(f, "editor_frame_cap_fps %.9g\n", app_editor_frame_cap_fps());
    fprintf(f, "ui_scale %.9g\n", ui_global_scale());
    fprintf(f, "code_font_size %.9g\n", ui_code_font_size());
    fprintf(f, "show_inspector_notes %d\n", ui_show_inspector_notes() ? 1 : 0);
    fprintf(f, "show_interface_hints %d\n", ui_show_interface_hints() ? 1 : 0);
    fprintf(f, "render_target_preview_columns %d\n", ui_render_target_preview_columns());
    fprintf(f, "scene_grid_enabled %d\n", g_dx.scene_grid_enabled ? 1 : 0);
    fprintf(f, "scene_orientation_gizmo_enabled %d\n", g_dx.scene_orientation_gizmo_enabled ? 1 : 0);
    fprintf(f, "scene_orientation_gizmo_size_px %.9g\n", g_dx.scene_orientation_gizmo_size_px);
    fprintf(f, "scene_manual_control_overlay_enabled %d\n", g_dx.scene_manual_control_overlay_enabled ? 1 : 0);
    fprintf(f, "scene_bounds_debug_enabled %d\n", g_dx.scene_bounds_debug_enabled ? 1 : 0);
    fprintf(f, "scene_grid_color_r %.9g\n", g_dx.scene_grid_color[0]);
    fprintf(f, "scene_grid_color_g %.9g\n", g_dx.scene_grid_color[1]);
    fprintf(f, "scene_grid_color_b %.9g\n", g_dx.scene_grid_color[2]);
    fprintf(f, "scene_grid_color_a %.9g\n", g_dx.scene_grid_color[3]);
    fprintf(f, "scene_grid_level_alpha_fine %.9g\n", g_dx.scene_grid_level_alpha[0]);
    fprintf(f, "scene_grid_level_alpha_mid %.9g\n", g_dx.scene_grid_level_alpha[1]);
    fprintf(f, "scene_grid_level_alpha_coarse %.9g\n", g_dx.scene_grid_level_alpha[2]);
    fprintf(f, "scene_grid_distance_fade %d\n", g_dx.scene_grid_distance_fade ? 1 : 0);
    fprintf(f, "scene_grid_fade_start %.9g\n", g_dx.scene_grid_fade_start);
    fprintf(f, "scene_grid_fade_end %.9g\n", g_dx.scene_grid_fade_end);
    fprintf(f, "camera_enabled %d\n", g_camera_controls.enabled ? 1 : 0);
    fprintf(f, "camera_mouse_look %d\n", g_camera_controls.mouse_look ? 1 : 0);
    fprintf(f, "camera_invert_y %d\n", g_camera_controls.invert_y ? 1 : 0);
    fprintf(f, "camera_mode %d\n", g_camera_controls.mode);
    fprintf(f, "camera_move_speed %.9g\n", g_camera_controls.move_speed);
    fprintf(f, "camera_fast_mult %.9g\n", g_camera_controls.fast_mult);
    fprintf(f, "camera_slow_mult %.9g\n", g_camera_controls.slow_mult);
    fprintf(f, "camera_mouse_sensitivity %.9g\n", g_camera_controls.mouse_sensitivity);
    fclose(f);
}

void app_settings_load_or_create() {
    app_settings_apply_defaults();

    FILE* f = fopen(s_settings_path, "rb");
    if (!f) {
        app_settings_save();
        log_info("Settings created: %s", s_settings_path);
        return;
    }

    char line[256] = {};
    while (fgets(line, sizeof(line), f)) {
        char key[128] = {};
        char value[128] = {};
        if (sscanf(line, "%127s %127s", key, value) != 2)
            continue;

        if (strcmp(key, "vsync") == 0) g_dx.vsync = atoi(value) != 0;
        else if (strcmp(key, "d3d11_validation") == 0) g_dx.d3d11_validation = atoi(value) != 0;
        else if (strcmp(key, "shader_validation_warnings") == 0) g_dx.shader_validation_warnings = atoi(value) != 0;
        else if (strcmp(key, "profiler") == 0) g_profiler_enabled = atoi(value) != 0;
        else if (strcmp(key, "editor_frame_cap_fps") == 0) app_set_editor_frame_cap_fps((float)atof(value));
        else if (strcmp(key, "ui_scale") == 0) ui_set_global_scale((float)atof(value));
        else if (strcmp(key, "code_font_size") == 0) ui_set_code_font_size((float)atof(value));
        else if (strcmp(key, "show_inspector_notes") == 0) ui_set_show_inspector_notes(atoi(value) != 0);
        else if (strcmp(key, "show_interface_hints") == 0) ui_set_show_interface_hints(atoi(value) != 0);
        else if (strcmp(key, "render_target_preview_columns") == 0) ui_set_render_target_preview_columns(atoi(value));
        else if (strcmp(key, "scene_grid_enabled") == 0) g_dx.scene_grid_enabled = atoi(value) != 0;
        else if (strcmp(key, "scene_orientation_gizmo_enabled") == 0) g_dx.scene_orientation_gizmo_enabled = atoi(value) != 0;
        else if (strcmp(key, "scene_orientation_gizmo_size_px") == 0) g_dx.scene_orientation_gizmo_size_px = (float)atof(value);
        else if (strcmp(key, "scene_manual_control_overlay_enabled") == 0) g_dx.scene_manual_control_overlay_enabled = atoi(value) != 0;
        else if (strcmp(key, "scene_bounds_debug_enabled") == 0) g_dx.scene_bounds_debug_enabled = atoi(value) != 0;
        else if (strcmp(key, "scene_grid_color_r") == 0) g_dx.scene_grid_color[0] = (float)atof(value);
        else if (strcmp(key, "scene_grid_color_g") == 0) g_dx.scene_grid_color[1] = (float)atof(value);
        else if (strcmp(key, "scene_grid_color_b") == 0) g_dx.scene_grid_color[2] = (float)atof(value);
        else if (strcmp(key, "scene_grid_color_a") == 0) g_dx.scene_grid_color[3] = (float)atof(value);
        else if (strcmp(key, "scene_grid_level_alpha_fine") == 0) g_dx.scene_grid_level_alpha[0] = (float)atof(value);
        else if (strcmp(key, "scene_grid_level_alpha_mid") == 0) g_dx.scene_grid_level_alpha[1] = (float)atof(value);
        else if (strcmp(key, "scene_grid_level_alpha_coarse") == 0) g_dx.scene_grid_level_alpha[2] = (float)atof(value);
        else if (strcmp(key, "scene_grid_distance_fade") == 0) g_dx.scene_grid_distance_fade = atoi(value) != 0;
        else if (strcmp(key, "scene_grid_fade_start") == 0) g_dx.scene_grid_fade_start = (float)atof(value);
        else if (strcmp(key, "scene_grid_fade_end") == 0) g_dx.scene_grid_fade_end = (float)atof(value);
        else if (strcmp(key, "camera_enabled") == 0) g_camera_controls.enabled = atoi(value) != 0;
        else if (strcmp(key, "camera_mouse_look") == 0) g_camera_controls.mouse_look = atoi(value) != 0;
        else if (strcmp(key, "camera_invert_y") == 0) g_camera_controls.invert_y = atoi(value) != 0;
        else if (strcmp(key, "camera_mode") == 0) g_camera_controls.mode = atoi(value);
        else if (strcmp(key, "camera_move_speed") == 0) g_camera_controls.move_speed = (float)atof(value);
        else if (strcmp(key, "camera_fast_mult") == 0) g_camera_controls.fast_mult = (float)atof(value);
        else if (strcmp(key, "camera_slow_mult") == 0) g_camera_controls.slow_mult = (float)atof(value);
        else if (strcmp(key, "camera_mouse_sensitivity") == 0) g_camera_controls.mouse_sensitivity = (float)atof(value);
    }
    fclose(f);

    // Clamp loaded values so a broken ini stays recoverable instead of poisoning
    // input/navigation on the next launch.
    if (g_camera_controls.move_speed < 0.001f) g_camera_controls.move_speed = 0.001f;
    if (g_camera_controls.fast_mult < 1.0f) g_camera_controls.fast_mult = 1.0f;
    if (g_camera_controls.slow_mult < 0.01f) g_camera_controls.slow_mult = 0.01f;
    if (g_camera_controls.mouse_sensitivity < 0.0001f) g_camera_controls.mouse_sensitivity = 0.0001f;
    if (g_camera_controls.mode != CAMERA_MODE_FREE) g_camera_controls.mode = CAMERA_MODE_HORIZON_LOCKED;
    g_dx.scene_grid_color[0] = clampf(g_dx.scene_grid_color[0], 0.0f, 1.0f);
    g_dx.scene_grid_color[1] = clampf(g_dx.scene_grid_color[1], 0.0f, 1.0f);
    g_dx.scene_grid_color[2] = clampf(g_dx.scene_grid_color[2], 0.0f, 1.0f);
    g_dx.scene_grid_color[3] = clampf(g_dx.scene_grid_color[3], 0.0f, 1.0f);
    g_dx.scene_grid_level_alpha[0] = clampf(g_dx.scene_grid_level_alpha[0], 0.0f, 1.0f);
    g_dx.scene_grid_level_alpha[1] = clampf(g_dx.scene_grid_level_alpha[1], 0.0f, 1.0f);
    g_dx.scene_grid_level_alpha[2] = clampf(g_dx.scene_grid_level_alpha[2], 0.0f, 1.0f);
    g_dx.scene_orientation_gizmo_size_px = clampf(g_dx.scene_orientation_gizmo_size_px, 72.0f, 180.0f);
    if (g_dx.scene_grid_fade_start < 0.0f) g_dx.scene_grid_fade_start = 0.0f;
    if (g_dx.scene_grid_fade_end < g_dx.scene_grid_fade_start + 1.0f)
        g_dx.scene_grid_fade_end = g_dx.scene_grid_fade_start + 1.0f;
    app_settings_apply_build_config_constraints();
}
