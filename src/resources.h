#pragma once
#include "types.h"

// The resource API creates and tracks every asset-like object visible to the
// editor: shaders, textures, meshes, buffers, and built-in engine resources.

extern Resource  g_resources[MAX_RESOURCES];
extern int       g_resource_count;

extern ResHandle g_builtin_time;
extern ResHandle g_builtin_scene_color;
extern ResHandle g_builtin_scene_depth;
extern ResHandle g_builtin_shadow_map;
extern ResHandle g_builtin_dirlight;

typedef enum {
    MESH_PRIM_CUBE = 0,
    MESH_PRIM_QUAD,
    MESH_PRIM_TETRAHEDRON,
    MESH_PRIM_SPHERE,
    MESH_PRIM_FULLSCREEN_TRIANGLE
} MeshPrimitiveType;

void      res_init();
void      res_shutdown();

ResHandle res_alloc(const char* name, ResType type);
void      res_free(ResHandle h);
Resource* res_get(ResHandle h);
ResHandle res_find_by_name(const char* name);

ResHandle res_create_render_texture(const char* name, int w, int h, DXGI_FORMAT fmt,
                                     bool want_rtv, bool want_srv, bool want_uav, bool want_dsv,
                                     int scene_scale_divisor = 0);
ResHandle res_create_render_texture3d(const char* name, int w, int h, int d, DXGI_FORMAT fmt,
                                       bool want_rtv, bool want_srv, bool want_uav);
ResHandle res_create_structured_buffer(const char* name, int elem_size, int elem_count,
                                        bool want_srv, bool want_uav,
                                        bool want_indirect_args = false);
bool      res_recreate_render_texture(ResHandle h, int w, int hgt, DXGI_FORMAT fmt,
                                       bool want_rtv, bool want_srv, bool want_uav, bool want_dsv,
                                       int scene_scale_divisor = -1);
bool      res_recreate_render_texture3d(ResHandle h, int w, int hgt, int d, DXGI_FORMAT fmt,
                                         bool want_rtv, bool want_srv, bool want_uav);
bool      res_recreate_structured_buffer(ResHandle h, int elem_size, int elem_count,
                                          bool want_srv, bool want_uav,
                                          bool want_indirect_args = false);
ResHandle res_create_shader(const char* name, const char* path,
                             const char* vs_entry, const char* ps_entry);
ResHandle res_create_compute_shader(const char* name, const char* path, const char* cs_entry);
ResHandle res_load_texture(const char* name, const char* path);
bool      res_reload_texture(Resource* r, const char* path);
ResHandle res_load_mesh(const char* name, const char* path);
ResHandle res_create_mesh_primitive(const char* name, MeshPrimitiveType type);
bool      res_set_mesh_primitive(Resource* r, MeshPrimitiveType type);
void      res_sync_size_resource(ResHandle h);

void      res_release_gpu(Resource* r);
void      res_reset_transient_gpu_resources();
void      res_sync_scene_dependent_render_textures();
void      res_rename(ResHandle h, const char* new_name);
void      res_make_unique_name(const char* base, char* out, int out_sz);
const char* res_type_str(ResType t);
uint64_t  res_estimate_gpu_bytes(const Resource& r);
uint64_t  res_estimate_gpu_total(bool include_builtin = true);
