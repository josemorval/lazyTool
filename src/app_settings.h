#pragma once

// Editor-wide settings live outside project files so UX toggles such as VSync,
// profiling, camera interaction, and UI scale survive project loads and do not
// pollute the scene description format.
void app_settings_load_or_create();
void app_settings_save();
