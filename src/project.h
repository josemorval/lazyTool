#pragma once
#include "types.h"

// Project save/load helpers for the text-based scene description format.

bool project_save_text(const char* path);
bool project_load_text(const char* path);
void project_new_default();
void project_apply_default_camera(Camera* camera);
void project_apply_default_dirlight(Resource* dirlight);
void project_reset_camera_defaults();
void project_reset_dirlight_defaults();
void project_reset_view_defaults();
void project_reset_export_settings();
const ExportSettings& project_default_export_settings();
const char* project_current_path();
const char* project_current_name();

extern ExportSettings g_export_settings;
