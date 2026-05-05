#pragma once
#include <stddef.h>

// Tiny single-exe pack support. Export copies an existing executable and
// appends the current project plus referenced files; runtime reads them through
// the same path strings used by .lt projects.

bool lt_pack_init_from_exe(const char* exe_path);
bool lt_pack_is_loaded();
const char* lt_pack_project_path();
int lt_pack_file_count();

bool lt_read_file(const char* path, void** out_data, size_t* out_size);
void lt_free_file(void* data);

bool lt_export_single_exe(const char* base_exe_path,
                          const char* project_path,
                          const char* output_exe_path,
                          char* err,
                          int err_sz);
