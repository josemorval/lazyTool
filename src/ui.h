#pragma once
#include "types.h"

extern ResHandle g_sel_res;
extern CmdHandle g_sel_cmd;
extern Camera g_camera;
extern CameraControls g_camera_controls;
extern bool g_scene_view_hovered;

void app_request_scene_restart();
void app_request_scene_surface_resize(int w, int h);
void app_set_scene_paused(bool paused);
bool app_scene_paused();
float app_scene_time();
uint64_t app_scene_frame();

void ui_init();
void ui_draw();
void ui_shutdown();
