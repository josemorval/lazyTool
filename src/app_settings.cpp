#include "app_settings.h"
#include "dx11_ctx.h"
#include "commands.h"
#include "ui.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* s_settings_path = "lazytool_general.ini";

// Editor-wide defaults live outside project files so opening an old project
// cannot unexpectedly re-enable expensive runtime features like profiling.
static void app_settings_apply_defaults() {
    g_dx.vsync = false;
    g_profiler_enabled = false;

    g_camera_controls.enabled = true;
    g_camera_controls.mouse_look = true;
    g_camera_controls.invert_y = false;
    g_camera_controls.move_speed = 6.0f;
    g_camera_controls.fast_mult = 4.0f;
    g_camera_controls.slow_mult = 0.25f;
    g_camera_controls.mouse_sensitivity = 0.0025f;
}

void app_settings_save() {
    FILE* f = fopen(s_settings_path, "wb");
    if (!f) {
        log_warn("Settings save failed: %s", s_settings_path);
        return;
    }

    fprintf(f, "vsync %d\n", g_dx.vsync ? 1 : 0);
    fprintf(f, "profiler %d\n", g_profiler_enabled ? 1 : 0);
    fprintf(f, "camera_enabled %d\n", g_camera_controls.enabled ? 1 : 0);
    fprintf(f, "camera_mouse_look %d\n", g_camera_controls.mouse_look ? 1 : 0);
    fprintf(f, "camera_invert_y %d\n", g_camera_controls.invert_y ? 1 : 0);
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
        else if (strcmp(key, "profiler") == 0) g_profiler_enabled = atoi(value) != 0;
        else if (strcmp(key, "camera_enabled") == 0) g_camera_controls.enabled = atoi(value) != 0;
        else if (strcmp(key, "camera_mouse_look") == 0) g_camera_controls.mouse_look = atoi(value) != 0;
        else if (strcmp(key, "camera_invert_y") == 0) g_camera_controls.invert_y = atoi(value) != 0;
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
}
