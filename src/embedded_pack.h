#pragma once
#include <stddef.h>

// Normal single-exe pack support.
//
// At runtime, the player calls lt_pack_init_from_exe() once. If the executable
// has an appended lazyTool pack footer, lt_read_file() serves project and asset
// bytes from memory before falling back to disk. This keeps editor code and
// packed-player code using the same resource paths.

bool lt_pack_init_from_exe(const char* exe_path);
bool lt_pack_is_loaded();
const char* lt_pack_project_path();
int lt_pack_file_count();

// Read from the appended pack when possible, otherwise read from disk in normal
// builds. The caller owns the returned buffer and must release it with
// lt_free_file().
bool lt_read_file(const char* path, void** out_data, size_t* out_size);
void lt_free_file(void* data);

// Export the standalone player with the current project and referenced assets
// appended to the executable.
bool lt_export_normal_exe(const char* base_exe_path,
                          const char* project_path,
                          const char* output_exe_path,
                          char* err,
                          int err_sz);
