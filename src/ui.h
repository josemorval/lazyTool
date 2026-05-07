#pragma once
#include "types.h"

// UI entry points plus shared editor state queried by runtime systems such as
// camera controls and scene pause/restart handling.

extern ResHandle g_sel_res;
extern CmdHandle g_sel_cmd;
extern Camera g_camera;
extern CameraControls g_camera_controls;
extern bool g_scene_view_hovered;

enum UiWindowControlHit {
    UI_WINDOW_CONTROL_NONE = 0,
    UI_WINDOW_CONTROL_MINIMIZE,
    UI_WINDOW_CONTROL_MAXIMIZE,
    UI_WINDOW_CONTROL_CLOSE
};

void app_request_scene_restart();
void app_request_scene_surface_resize(int w, int h);
void app_request_scene_render();
void app_set_scene_paused(bool paused);
void app_set_scene_time(float seconds);
bool app_scene_paused();
float app_scene_time();
uint64_t app_scene_frame();

void ui_init();
void ui_draw();
void ui_shutdown();
void ui_set_global_scale(float scale);
float ui_global_scale();
void ui_set_code_font_size(float size);
float ui_code_font_size();
void ui_set_shader_auto_save_compile(bool enabled);
bool ui_shader_auto_save_compile();
void ui_set_shader_format_on_save(bool enabled);
bool ui_shader_format_on_save();
int ui_top_toolbar_height_px();
bool ui_hit_test_client_area_screen(int x, int y);
UiWindowControlHit ui_hit_test_window_control_screen(int x, int y);
UiWindowControlHit ui_hit_test_window_control_client(int x, int y, int client_w);
