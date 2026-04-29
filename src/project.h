#pragma once

// Project save/load helpers for the text-based scene description format.

bool project_save_text(const char* path);
bool project_load_text(const char* path);
void project_new_default();
const char* project_current_path();
const char* project_current_name();
