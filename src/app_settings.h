#pragma once

// Editor-wide settings live outside project files so UX toggles such as VSync,
// profiling and camera interaction survive project loads and do not pollute the
// scene description format.
void app_settings_load_or_create();
void app_settings_save();
