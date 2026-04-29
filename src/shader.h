#pragma once
#include "types.h"
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

// Shader compilation and reflection entry points used by the resource system.

bool shader_compile_vs_ps(Resource* r, const char* path,
                           const char* vs_entry, const char* ps_entry);
bool shader_compile_cs(Resource* r, const char* path, const char* cs_entry);
void shader_release(Resource* r);
uint32_t shader_cb_next_layout_version();
