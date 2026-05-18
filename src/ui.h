#pragma once
#include "types.h"

// UI entry points plus shared editor state queried by runtime systems such as
// camera controls and scene pause/restart handling.

extern ResHandle g_sel_res;
extern CmdHandle g_sel_cmd;
extern Camera g_camera;
extern CameraControls g_camera_controls;
extern bool g_scene_view_hovered;
extern bool g_editor_mouse_capture;

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
float app_cpu_frame_ms();
float app_cpu_scene_ms();
float app_cpu_ui_build_ms();
float app_cpu_ui_render_ms();
float app_cpu_present_ms();
float app_cpu_other_ms();
float app_editor_frame_cap_fps();
void app_set_editor_frame_cap_fps(float fps);

void ui_init();
void ui_draw();
void ui_shutdown();
void ui_set_global_scale(float scale);
float ui_global_scale();
void ui_set_code_font_size(float size);
float ui_code_font_size();
void ui_set_show_inspector_notes(bool show);
bool ui_show_inspector_notes();
int ui_top_toolbar_height_px();
bool ui_scene_view_contains_screen_point(int x, int y);
bool ui_scene_view_screen_rect(RECT* out_rect);
bool ui_hit_test_client_area_screen(int x, int y);
UiWindowControlHit ui_hit_test_window_control_screen(int x, int y);
UiWindowControlHit ui_hit_test_window_control_client(int x, int y, int client_w);
