#include "project.h"
#include "resources.h"
#include "commands.h"
#include "dx11_ctx.h"
#include "log.h"
#include "ui.h"
#include "user_cb.h"
#include "embedded_pack.h"
#include "timeline.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <direct.h>

// project.cpp owns the text project format used to save and restore the full
// editor state in a way that remains readable and diff-friendly.

static const char* bool_str(bool v) { return v ? "1" : "0"; }
static char s_project_current_path[MAX_PATH_LEN] = {};

static const ExportSettings k_project_export_defaults = {
    /* runtime_input_enabled  */ true,
    /* escape_closes_player   */ true,
    /* force_wireframe        */ false,
    /* show_grid_overlay      */ false,
    /* vsync                  */ false,
    /* profiler               */ false,
    /* shader_binding_warnings*/ false
};

ExportSettings g_export_settings = k_project_export_defaults;

void project_reset_export_settings() {
    g_export_settings = k_project_export_defaults;
}

const ExportSettings& project_default_export_settings() {
    return k_project_export_defaults;
}

// Project text uses forward slashes for file paths. The loader still accepts
// either separator, while every saved path is written in the editor convention.
static void project_canonicalize_path_text(const char* in, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!in)
        return;

    int oi = 0;
    for (int i = 0; in[i] && oi < out_sz - 1; i++)
        out[oi++] = in[i] == '\\' ? '/' : in[i];
    out[oi] = '\0';
}

static const char* project_path_token(const char* path, char* out, int out_sz) {
    project_canonicalize_path_text(path, out, out_sz);
    return out && out[0] ? out : "-";
}

struct ProjectViewDefaults {
    float camera_pos[3];
    float view_dir[3];
    float camera_fov_y;
    float camera_near_z;
    float camera_far_z;

    float light_pos[3];
    float light_target_distance;
    float light_color[3];
    float light_intensity;

    float shadow_extent[2];
    float shadow_near;
    float shadow_far;
    int   shadow_width;
    int   shadow_height;
    int   shadow_cascade_count;
    float shadow_distance;
    float shadow_split_lambda;
};

static const ProjectViewDefaults k_project_view_defaults = {
    /* camera_pos          */ { 5.0f, 5.0f, 5.0f },
    /* view_dir            */ { -0.57735026919f, -0.57735026919f, -0.57735026919f },
    /* camera_fov_y        */ 1.047f,
    /* camera_near_z       */ 0.001f,
    /* camera_far_z        */ 100.0f,

    /* light_pos           */ { 5.0f, 5.0f, 5.0f },
    /* light_target_dist   */ 8.66025403784f,
    /* light_color         */ { 1.0f, 0.95f, 0.9f },
    /* light_intensity     */ 1.0f,

    /* shadow_extent       */ { 8.0f, 8.0f },
    /* shadow_near         */ 0.01f,
    /* shadow_far          */ 10.0f,
    /* shadow_width        */ 1024,
    /* shadow_height       */ 1204,
    /* shadow_cascade_count*/ 1,
    /* shadow_distance     */ 5.0f,
    /* shadow_split_lambda */ 0.65f
};

static void project_set_current_path(const char* path) {
    // `path` is sometimes project_current_path(), which points at
    // s_project_current_path itself. Canonicalize through a temporary buffer so
    // setting the current path is safe even when input and output alias.
    char clean[MAX_PATH_LEN] = {};
    project_canonicalize_path_text(path ? path : "", clean, MAX_PATH_LEN);
    strncpy(s_project_current_path, clean, MAX_PATH_LEN - 1);
    s_project_current_path[MAX_PATH_LEN - 1] = '\0';
}

const char* project_current_path() {
    return s_project_current_path;
}

const char* project_current_name() {
    if (!s_project_current_path[0])
        return nullptr;
    const char* slash1 = strrchr(s_project_current_path, '/');
    const char* slash2 = strrchr(s_project_current_path, '\\');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    return slash ? slash + 1 : s_project_current_path;
}

static const char* res_type_token(ResType t) {
    switch (t) {
    case RES_INT:                 return "int";
    case RES_INT2:                return "int2";
    case RES_INT3:                return "int3";
    case RES_FLOAT:               return "float";
    case RES_FLOAT2:              return "float2";
    case RES_FLOAT3:              return "float3";
    case RES_FLOAT4:              return "float4";
    case RES_TEXTURE2D:           return "texture2d";
    case RES_RENDER_TEXTURE2D:    return "render_texture2d";
    case RES_RENDER_TEXTURE3D:    return "render_texture3d";
    case RES_STRUCTURED_BUFFER:   return "structured_buffer";
    case RES_GAUSSIAN_SPLAT:      return "gaussian_splat";
    case RES_MESH:                return "mesh";
    case RES_SHADER:              return "shader";
    case RES_BUILTIN_TIME:        return "builtin_time";
    case RES_BUILTIN_SCENE_COLOR: return "builtin_scene_color";
    case RES_BUILTIN_SCENE_DEPTH: return "builtin_scene_depth";
    case RES_BUILTIN_SHADOW_MAP:  return "builtin_shadow_map";
    case RES_BUILTIN_DIRLIGHT:    return "builtin_dirlight";
    default:                      return "none";
    }
}

static ResType res_type_from_token(const char* name) {
    if (!name) return RES_NONE;
    if (strcmp(name, "int") == 0) return RES_INT;
    if (strcmp(name, "int2") == 0) return RES_INT2;
    if (strcmp(name, "int3") == 0) return RES_INT3;
    if (strcmp(name, "float") == 0) return RES_FLOAT;
    if (strcmp(name, "float2") == 0) return RES_FLOAT2;
    if (strcmp(name, "float3") == 0) return RES_FLOAT3;
    if (strcmp(name, "float4") == 0) return RES_FLOAT4;
    if (strcmp(name, "texture2d") == 0) return RES_TEXTURE2D;
    if (strcmp(name, "render_texture2d") == 0) return RES_RENDER_TEXTURE2D;
    if (strcmp(name, "render_texture3d") == 0) return RES_RENDER_TEXTURE3D;
    if (strcmp(name, "structured_buffer") == 0) return RES_STRUCTURED_BUFFER;
    if (strcmp(name, "gaussian_splat") == 0) return RES_GAUSSIAN_SPLAT;
    if (strcmp(name, "mesh") == 0) return RES_MESH;
    if (strcmp(name, "shader") == 0) return RES_SHADER;
    if (strcmp(name, "builtin_time") == 0) return RES_BUILTIN_TIME;
    if (strcmp(name, "builtin_scene_color") == 0) return RES_BUILTIN_SCENE_COLOR;
    if (strcmp(name, "builtin_scene_depth") == 0) return RES_BUILTIN_SCENE_DEPTH;
    if (strcmp(name, "builtin_shadow_map") == 0) return RES_BUILTIN_SHADOW_MAP;
    if (strcmp(name, "builtin_dirlight") == 0) return RES_BUILTIN_DIRLIGHT;
    return RES_NONE;
}

static void project_compute_default_cascade_splits(float near_z, float far_z, int cascade_count,
                                                  float lambda, float out_splits[MAX_SHADOW_CASCADES]) {
    if (!out_splits)
        return;

    if (near_z < 0.0001f)
        near_z = 0.1f;
    if (far_z <= near_z + 0.001f)
        far_z = near_z + 0.001f;
    if (cascade_count < 1)
        cascade_count = 1;
    if (cascade_count > MAX_SHADOW_CASCADES)
        cascade_count = MAX_SHADOW_CASCADES;
    lambda = clampf(lambda, 0.0f, 1.0f);

    for (int i = 0; i < MAX_SHADOW_CASCADES; i++)
        out_splits[i] = far_z;

    for (int i = 0; i < cascade_count; i++) {
        float t = (float)(i + 1) / (float)cascade_count;
        float log_split = near_z * powf(far_z / near_z, t);
        float uni_split = near_z + (far_z - near_z) * t;
        out_splits[i] = uni_split + (log_split - uni_split) * lambda;
    }
}

static void project_seed_manual_shadow_cascades(Resource* dl, float camera_near_z) {
    if (!dl)
        return;

    float seeded_splits[MAX_SHADOW_CASCADES] = {};
    project_compute_default_cascade_splits(camera_near_z, dl->shadow_distance,
                                          MAX_SHADOW_CASCADES, dl->shadow_split_lambda,
                                          seeded_splits);

    float base_extent_x = dl->shadow_extent[0] > 0.01f ? dl->shadow_extent[0] : 0.01f;
    float base_extent_y = dl->shadow_extent[1] > 0.01f ? dl->shadow_extent[1] : 0.01f;
    float base_near = dl->shadow_near > 0.0001f ? dl->shadow_near : 0.0001f;
    float base_far = dl->shadow_far > base_near + 0.001f ? dl->shadow_far : base_near + 0.001f;

    for (int i = 0; i < MAX_SHADOW_CASCADES; i++) {
        dl->shadow_cascade_split[i] = seeded_splits[i];
        dl->shadow_cascade_extent[i][0] = base_extent_x;
        dl->shadow_cascade_extent[i][1] = base_extent_y;
        dl->shadow_cascade_near[i] = base_near;
        dl->shadow_cascade_far[i] = base_far;
    }
}

static void project_validate_manual_shadow_cascades(Resource* dl, float camera_near_z, float camera_far_z) {
    if (!dl)
        return;

    float prev_split = camera_near_z > 0.0001f ? camera_near_z : 0.1f;
    float max_split = camera_far_z > prev_split + 0.001f ? camera_far_z : prev_split + 0.001f;
    for (int i = 0; i < MAX_SHADOW_CASCADES; i++) {
        if (dl->shadow_cascade_extent[i][0] < 0.01f) dl->shadow_cascade_extent[i][0] = 0.01f;
        if (dl->shadow_cascade_extent[i][1] < 0.01f) dl->shadow_cascade_extent[i][1] = 0.01f;
        if (dl->shadow_cascade_near[i] < 0.0001f) dl->shadow_cascade_near[i] = 0.0001f;
        if (dl->shadow_cascade_far[i] <= dl->shadow_cascade_near[i] + 0.001f)
            dl->shadow_cascade_far[i] = dl->shadow_cascade_near[i] + 0.001f;

        float min_split = prev_split + 0.001f;
        if (min_split > max_split)
            min_split = max_split;
        if (dl->shadow_cascade_split[i] < min_split)
            dl->shadow_cascade_split[i] = min_split;
        if (dl->shadow_cascade_split[i] > max_split)
            dl->shadow_cascade_split[i] = max_split;
        prev_split = dl->shadow_cascade_split[i];
    }
}

static void project_angles_from_direction(const float dir[3], float* yaw, float* pitch) {
    Vec3 forward = v3_norm(v3(dir[0], dir[1], dir[2]));
    float dir_y = clampf(forward.y, -1.0f, 1.0f);
    if (yaw)
        *yaw = atan2f(forward.x, forward.z);
    if (pitch)
        *pitch = asinf(dir_y);
}

void project_apply_default_camera(Camera* camera) {
    if (!camera)
        return;

    camera->position[0] = k_project_view_defaults.camera_pos[0];
    camera->position[1] = k_project_view_defaults.camera_pos[1];
    camera->position[2] = k_project_view_defaults.camera_pos[2];
    project_angles_from_direction(k_project_view_defaults.view_dir, &camera->yaw, &camera->pitch);
    camera->roll = 0.0f;
    camera_set_euler(camera, camera->yaw, camera->pitch, camera->roll);
    camera->fov_y = k_project_view_defaults.camera_fov_y;
    camera->near_z = k_project_view_defaults.camera_near_z;
    camera->far_z = k_project_view_defaults.camera_far_z;
}

void project_apply_default_dirlight(Resource* dl) {
    if (!dl)
        return;

    dl->light_pos[0] = k_project_view_defaults.light_pos[0];
    dl->light_pos[1] = k_project_view_defaults.light_pos[1];
    dl->light_pos[2] = k_project_view_defaults.light_pos[2];
    dl->light_target[0] = dl->light_pos[0] + k_project_view_defaults.view_dir[0] * k_project_view_defaults.light_target_distance;
    dl->light_target[1] = dl->light_pos[1] + k_project_view_defaults.view_dir[1] * k_project_view_defaults.light_target_distance;
    dl->light_target[2] = dl->light_pos[2] + k_project_view_defaults.view_dir[2] * k_project_view_defaults.light_target_distance;
    Vec3 light_dir = v3_norm(v3_sub(
        v3(dl->light_target[0], dl->light_target[1], dl->light_target[2]),
        v3(dl->light_pos[0], dl->light_pos[1], dl->light_pos[2])));
    dl->light_dir[0] = light_dir.x;
    dl->light_dir[1] = light_dir.y;
    dl->light_dir[2] = light_dir.z;
    dl->light_color[0] = k_project_view_defaults.light_color[0];
    dl->light_color[1] = k_project_view_defaults.light_color[1];
    dl->light_color[2] = k_project_view_defaults.light_color[2];
    dl->light_intensity = k_project_view_defaults.light_intensity;
    dl->shadow_extent[0] = k_project_view_defaults.shadow_extent[0];
    dl->shadow_extent[1] = k_project_view_defaults.shadow_extent[1];
    dl->shadow_near = k_project_view_defaults.shadow_near;
    dl->shadow_far = k_project_view_defaults.shadow_far;
    dl->shadow_width = k_project_view_defaults.shadow_width;
    dl->shadow_height = k_project_view_defaults.shadow_height;
    dl->shadow_cascade_count = k_project_view_defaults.shadow_cascade_count;
    dl->shadow_distance = k_project_view_defaults.shadow_distance;
    dl->shadow_split_lambda = k_project_view_defaults.shadow_split_lambda;
    project_seed_manual_shadow_cascades(dl, k_project_view_defaults.camera_near_z);
    project_validate_manual_shadow_cascades(dl, k_project_view_defaults.camera_near_z,
                                            k_project_view_defaults.camera_far_z);
}

void project_reset_camera_defaults() {
    project_apply_default_camera(&g_camera);
}

void project_reset_dirlight_defaults() {
    Resource* dl = res_get(g_builtin_dirlight);
    if (!dl)
        return;

    project_apply_default_dirlight(dl);

    if (g_dx.dev && (g_dx.shadow_width != dl->shadow_width || g_dx.shadow_height != dl->shadow_height))
        dx_create_shadow_map(dl->shadow_width, dl->shadow_height);
}

void project_reset_view_defaults() {
    project_reset_camera_defaults();
    project_reset_dirlight_defaults();
}

static void res_ref(ResHandle h, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    Resource* r = res_get(h);
    if (!r) {
        strncpy(out, "-", out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    snprintf(out, out_sz, "%s|%s", r->name, res_type_token(r->type));
    out[out_sz - 1] = '\0';
}

static void ref_base_name(const char* ref, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = '\0';
    if (!ref) return;
    strncpy(out, ref, out_sz - 1);
    out[out_sz - 1] = '\0';
    char* type_sep = strrchr(out, '|');
    if (type_sep)
        *type_sep = '\0';
}

static void clear_source_ref(const char* source_name, ResType type, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    strncpy(out, "-", out_sz - 1);
    out[out_sz - 1] = '\0';
    if (!source_name || !source_name[0])
        return;

    ResHandle h = res_find_by_name(source_name);
    Resource* r = res_get(h);
    if (r && r->type == type) {
        res_ref(h, out, out_sz);
        return;
    }

    strncpy(out, source_name, out_sz - 1);
    out[out_sz - 1] = '\0';
}

struct ResourceLookup {
    ResType types[4];
    int type_count;
    bool need_srv;
    bool need_uav;
};

static bool res_type_in_lookup(ResType type, const ResourceLookup& lookup) {
    if (lookup.type_count <= 0) return true;
    for (int i = 0; i < lookup.type_count; i++)
        if (lookup.types[i] == type)
            return true;
    return false;
}

static bool res_can_bind_srv(const Resource& r) {
    switch (r.type) {
    case RES_TEXTURE2D:
    case RES_RENDER_TEXTURE2D:
    case RES_RENDER_TEXTURE3D:
    case RES_STRUCTURED_BUFFER:
    case RES_BUILTIN_SCENE_COLOR:
    case RES_BUILTIN_SCENE_DEPTH:
    case RES_BUILTIN_SHADOW_MAP:
        return true;
    default:
        return r.has_srv;
    }
}

static bool res_can_bind_uav(const Resource& r) {
    switch (r.type) {
    case RES_RENDER_TEXTURE2D:
    case RES_RENDER_TEXTURE3D:
    case RES_STRUCTURED_BUFFER:
    case RES_BUILTIN_SCENE_COLOR:
        return true;
    default:
        return r.has_uav;
    }
}

static bool res_matches_lookup(const Resource& r, const ResourceLookup& lookup) {
    if (!res_type_in_lookup(r.type, lookup)) return false;
    if (lookup.need_srv && !res_can_bind_srv(r)) return false;
    if (lookup.need_uav && !res_can_bind_uav(r)) return false;
    return true;
}

static ResourceLookup res_lookup_types(ResType a = RES_NONE, ResType b = RES_NONE,
                                       ResType c = RES_NONE, ResType d = RES_NONE) {
    ResourceLookup lookup = {};
    ResType values[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        if (values[i] == RES_NONE) continue;
        lookup.types[lookup.type_count++] = values[i];
    }
    return lookup;
}

static ResourceLookup res_lookup_srv() {
    ResourceLookup lookup = {};
    lookup.need_srv = true;
    return lookup;
}

static ResourceLookup res_lookup_uav() {
    ResourceLookup lookup = {};
    lookup.need_uav = true;
    return lookup;
}

static ResHandle res_by_ref(const char* token, const ResourceLookup& lookup) {
    if (!token || strcmp(token, "-") == 0) return INVALID_HANDLE;

    char name[MAX_PATH_LEN] = {};
    strncpy(name, token, MAX_PATH_LEN - 1);
    name[MAX_PATH_LEN - 1] = '\0';

    ResType explicit_type = RES_NONE;
    char* type_sep = strrchr(name, '|');
    if (type_sep) {
        *type_sep = '\0';
        explicit_type = res_type_from_token(type_sep + 1);
    }

    ResHandle first_same_name = INVALID_HANDLE;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (!r.active || strcmp(r.name, name) != 0)
            continue;

        ResHandle h = (ResHandle)(i + 1);
        if (first_same_name == INVALID_HANDLE)
            first_same_name = h;

        if (explicit_type != RES_NONE && r.type != explicit_type)
            continue;
        if (res_matches_lookup(r, lookup))
            return h;
    }

    return lookup.type_count == 0 && !lookup.need_srv && !lookup.need_uav ? first_same_name : INVALID_HANDLE;
}

static void ensure_parent_dir(const char* path) {
    char dir[MAX_PATH_LEN] = {};
    const char* slash1 = strrchr(path, '/');
    const char* slash2 = strrchr(path, '\\');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    if (!slash) return;

    int len = (int)(slash - path);
    if (len <= 0 || len >= MAX_PATH_LEN) return;
    memcpy(dir, path, len);
    dir[len] = '\0';
    _mkdir(dir);
}

static const char* mesh_prim_name(int prim) {
    switch ((MeshPrimitiveType)prim) {
    case MESH_PRIM_CUBE:        return "cube";
    case MESH_PRIM_QUAD:        return "quad";
    case MESH_PRIM_TETRAHEDRON: return "tetrahedron";
    case MESH_PRIM_SPHERE:      return "sphere";
    case MESH_PRIM_FULLSCREEN_TRIANGLE: return "fullscreen_triangle";
    default:                    return "unknown";
    }
}

static MeshPrimitiveType mesh_prim_from_name(const char* name) {
    if (strcmp(name, "quad") == 0) return MESH_PRIM_QUAD;
    if (strcmp(name, "tetrahedron") == 0) return MESH_PRIM_TETRAHEDRON;
    if (strcmp(name, "sphere") == 0) return MESH_PRIM_SPHERE;
    if (strcmp(name, "fullscreen_triangle") == 0) return MESH_PRIM_FULLSCREEN_TRIANGLE;
    return MESH_PRIM_CUBE;
}

static const char* draw_source_name(int source) {
    switch ((DrawSourceType)source) {
    case DRAW_SOURCE_PROCEDURAL: return "procedural";
    case DRAW_SOURCE_MESH:
    default:                     return "mesh";
    }
}

static DrawSourceType draw_source_from_name(const char* name) {
    if (name && strcmp(name, "procedural") == 0) return DRAW_SOURCE_PROCEDURAL;
    return DRAW_SOURCE_MESH;
}

static const char* draw_topology_name(int topology) {
    switch ((DrawTopologyType)topology) {
    case DRAW_TOPOLOGY_POINT_LIST:    return "point_list";
    case DRAW_TOPOLOGY_TRIANGLE_LIST:
    default:                          return "triangle_list";
    }
}

static DrawTopologyType draw_topology_from_name(const char* name) {
    if (name && strcmp(name, "point_list") == 0) return DRAW_TOPOLOGY_POINT_LIST;
    return DRAW_TOPOLOGY_TRIANGLE_LIST;
}

static CmdType cmd_type_from_name(const char* name) {
    if (strcmp(name, "clear") == 0) return CMD_CLEAR;
    if (strcmp(name, "group") == 0) return CMD_GROUP;
    if (strcmp(name, "draw_instanced") == 0) return CMD_DRAW_INSTANCED;
    if (strcmp(name, "dispatch") == 0) return CMD_DISPATCH;
    if (strcmp(name, "indirect_draw") == 0) return CMD_INDIRECT_DRAW;
    if (strcmp(name, "indirect_dispatch") == 0) return CMD_INDIRECT_DISPATCH;
    if (strcmp(name, "repeat") == 0) return CMD_REPEAT;
    return CMD_DRAW_MESH;
}

static const char* cmd_type_name(CmdType type) {
    switch (type) {
    case CMD_CLEAR:             return "clear";
    case CMD_GROUP:             return "group";
    case CMD_DRAW_MESH:         return "draw_mesh";
    case CMD_DRAW_INSTANCED:    return "draw_instanced";
    case CMD_DISPATCH:          return "dispatch";
    case CMD_INDIRECT_DRAW:     return "indirect_draw";
    case CMD_INDIRECT_DISPATCH: return "indirect_dispatch";
    case CMD_REPEAT:            return "repeat";
    default:                    return "none";
    }
}

static void project_clear_user_data() {
    timeline_reset();
    user_cb_clear();
    project_reset_export_settings();

    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (g_commands[i].active)
            cmd_free((CmdHandle)(i + 1));
    }

    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (g_resources[i].active && !g_resources[i].is_builtin)
            res_free((ResHandle)(i + 1));
    }
}

// Create a minimal scene so the editor always opens with valid content.
void project_new_default() {
    project_clear_user_data();
    project_reset_camera_defaults();
    if (Resource* dl = res_get(g_builtin_dirlight))
        project_apply_default_dirlight(dl);
    dx_invalidate_scene_history();
    project_set_current_path("");

    ResHandle cube = res_create_mesh_primitive("normal_cube", MESH_PRIM_CUBE);
    ResHandle shader = res_create_shader("normal_color", "shaders/default.hlsl", "VSMain", "PSMain");

    CmdHandle clear_h = cmd_alloc("clear_scene", CMD_CLEAR);
    if (Command* c = cmd_get(clear_h)) {
        c->rt = g_builtin_scene_color;
        c->depth = g_builtin_scene_depth;
        c->clear_color[0] = 0.04f;
        c->clear_color[1] = 0.02f;
        c->clear_color[2] = 0.0f;
        c->clear_color[3] = 1.0f;
        c->clear_depth = true;
        c->depth_clear_val = 1.0f;
    }

    CmdHandle draw_h = cmd_alloc("draw_normal_cube", CMD_DRAW_MESH);
    if (Command* c = cmd_get(draw_h)) {
        c->mesh = cube;
        c->shader = shader;
        c->rt = g_builtin_scene_color;
        c->depth = g_builtin_scene_depth;
        c->shadow_cast = true;
        c->shadow_receive = true;
    }

    g_sel_cmd = draw_h;
    g_sel_res = INVALID_HANDLE;
    log_info("New project: cube colored by normal.");
}


// Command shader inputs are serialized as SRV/UAV bindings. Texture2D
// resources bind through SRV slots, so t# assignment is represented once.
static bool project_command_has_srv_slot(const Command& c, uint32_t slot) {
    for (int i = 0; i < c.srv_count; i++) {
        if (res_get(c.srv_handles[i]) && c.srv_slots[i] == slot)
            return true;
    }
    return false;
}

static void project_command_append_srv(Command* c, ResHandle h, uint32_t slot) {
    if (!c || h == INVALID_HANDLE || !res_get(h))
        return;
    if (project_command_has_srv_slot(*c, slot))
        return;
    if (c->srv_count >= MAX_SRV_SLOTS) {
        log_warn("Command '%s': dropping texture binding at t%u; SRV slots are full", c->name, slot);
        return;
    }
    c->srv_handles[c->srv_count] = h;
    c->srv_slots[c->srv_count] = slot;
    c->srv_count++;
}

static void project_command_fold_texture_slots_into_srvs(Command* c) {
    if (!c || c->tex_count <= 0)
        return;
    for (int i = 0; i < c->tex_count; i++)
        project_command_append_srv(c, c->tex_handles[i], c->tex_slots[i]);
    for (int i = 0; i < MAX_TEX_SLOTS; i++) {
        c->tex_handles[i] = INVALID_HANDLE;
        c->tex_slots[i] = 0;
    }
    c->tex_count = 0;
}

// Serialize the current scene/editor state into the custom text format.
bool project_save_text(const char* path) {
    user_cb_enforce_unique_names();
    ensure_parent_dir(path);
    FILE* f = fopen(path, "wb");
    if (!f) {
        log_error("Project save failed: %s", path);
        return false;
    }

    fprintf(f, "lazyTool_project 1\n\n");
    camera_sync_euler_from_quat(&g_camera);
    fprintf(f, "camera_fps %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n\n",
        g_camera.position[0], g_camera.position[1], g_camera.position[2],
        g_camera.yaw, g_camera.pitch,
        g_camera.fov_y, g_camera.near_z, g_camera.far_z, g_camera.roll);

    if (Resource* dl = res_get(g_builtin_dirlight)) {
        fprintf(f, "dirlight %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d %.9g %.9g %.9g %.9g %d %.9g %.9g",
            dl->light_pos[0], dl->light_pos[1], dl->light_pos[2],
            dl->light_target[0], dl->light_target[1], dl->light_target[2],
            dl->light_color[0], dl->light_color[1], dl->light_color[2],
            dl->light_intensity,
            dl->shadow_width, dl->shadow_height,
            dl->shadow_near, dl->shadow_far, dl->shadow_extent[0], dl->shadow_extent[1],
            dl->shadow_cascade_count, dl->shadow_distance, dl->shadow_split_lambda);
        for (int i = 0; i < MAX_SHADOW_CASCADES; i++) {
            fprintf(f, " %.9g %.9g %.9g %.9g %.9g",
                dl->shadow_cascade_split[i],
                dl->shadow_cascade_extent[i][0], dl->shadow_cascade_extent[i][1],
                dl->shadow_cascade_near[i], dl->shadow_cascade_far[i]);
        }
        fprintf(f, "\n\n");
    }

    fprintf(f, "export_settings %s %s %s %s %s %s %s\n\n",
        bool_str(g_export_settings.runtime_input_enabled),
        bool_str(g_export_settings.escape_closes_player),
        bool_str(g_export_settings.force_wireframe),
        bool_str(g_export_settings.show_grid_overlay),
        bool_str(g_export_settings.vsync),
        bool_str(g_export_settings.profiler),
        bool_str(g_export_settings.shader_binding_warnings));

    fprintf(f, "resources\n");
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (!r.active || r.is_builtin || r.is_generated) continue;

        if (r.type == RES_RENDER_TEXTURE2D) {
            fprintf(f, "resource render_texture %s %d %d %d %s %s %s %s %d\n",
                r.name, r.width, r.height, (int)r.tex_fmt,
                bool_str(r.has_rtv), bool_str(r.has_srv), bool_str(r.has_uav), bool_str(r.has_dsv),
                r.scene_scale_divisor);
        } else if (r.type == RES_INT || r.type == RES_INT2 || r.type == RES_INT3 ||
                   r.type == RES_FLOAT || r.type == RES_FLOAT2 || r.type == RES_FLOAT3 || r.type == RES_FLOAT4) {
            fprintf(f, "resource value %s %s %d %d %d %d %.9g %.9g %.9g %.9g\n",
                res_type_token(r.type), r.name,
                r.ival[0], r.ival[1], r.ival[2], r.ival[3],
                r.fval[0], r.fval[1], r.fval[2], r.fval[3]);
        } else if (r.type == RES_RENDER_TEXTURE3D) {
            fprintf(f, "resource render_texture3d %s %d %d %d %d %s %s %s\n",
                r.name, r.width, r.height, r.depth, (int)r.tex_fmt,
                bool_str(r.has_rtv), bool_str(r.has_srv), bool_str(r.has_uav));
        } else if (r.type == RES_STRUCTURED_BUFFER) {
            fprintf(f, "resource structured_buffer %s %d %d %s %s %s\n",
                r.name, r.elem_size, r.elem_count,
                bool_str(r.has_srv), bool_str(r.has_uav), bool_str(r.indirect_args));
        } else if (r.type == RES_GAUSSIAN_SPLAT) {
            char path_ref[MAX_PATH_LEN] = {};
            fprintf(f, "resource gaussian_splat %s %s\n", r.name, project_path_token(r.path, path_ref, MAX_PATH_LEN));
        } else if (r.type == RES_MESH) {
            if (r.path[0]) {
                char path_ref[MAX_PATH_LEN] = {};
                fprintf(f, "resource mesh_gltf %s %s\n", r.name, project_path_token(r.path, path_ref, MAX_PATH_LEN));
            } else {
                fprintf(f, "resource mesh_primitive %s %s\n", r.name, mesh_prim_name(r.mesh_primitive_type));
            }
        } else if (r.type == RES_SHADER) {
            char path_ref[MAX_PATH_LEN] = {};
            // The serialized shader kind is the editor intent, not the current
            // GPU pointer. Missing files and fallback shaders still round-trip
            // as the program kind the user created.
            bool is_compute = r.shader_kind == SHADER_PROGRAM_CS ||
                              (r.shader_kind == SHADER_PROGRAM_UNKNOWN && r.cs != nullptr);
            fprintf(f, "resource %s %s %s\n", is_compute ? "shader_cs" : "shader_vsps",
                r.name, project_path_token(r.path, path_ref, MAX_PATH_LEN));
        } else if (r.type == RES_TEXTURE2D) {
            char path_ref[MAX_PATH_LEN] = {};
            fprintf(f, "resource texture2d %s %s\n", r.name, project_path_token(r.path, path_ref, MAX_PATH_LEN));
        }
    }

    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (!r.active || r.type != RES_MESH || r.is_builtin || r.is_generated)
            continue;
        for (int pi = 0; pi < r.mesh_part_count; pi++) {
            if (!r.mesh_parts[pi].enabled)
                fprintf(f, "mesh_part_disabled %s %d\n", r.name, pi);
        }
    }

    fprintf(f, "\nuser_cb\n");
    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry& e = g_user_cb_entries[i];
        char source_ref[MAX_PATH_LEN] = {};
        if (e.source_kind == USER_CB_SOURCE_RESOURCE)
            res_ref(e.source, source_ref, MAX_PATH_LEN);
        else
            snprintf(source_ref, sizeof(source_ref), "-");
        fprintf(f, "user_var %s %s %s %d %d %d %d %.9g %.9g %.9g %.9g\n",
            e.name, res_type_token(e.type), source_ref,
            e.ival[0], e.ival[1], e.ival[2], e.ival[3],
            e.fval[0], e.fval[1], e.fval[2], e.fval[3]);
        if (e.source_kind != USER_CB_SOURCE_NONE && e.source_kind != USER_CB_SOURCE_RESOURCE) {
            fprintf(f, "user_var_source %s %s %s\n",
                e.name, user_cb_source_kind_token(e.source_kind),
                e.source_target[0] ? e.source_target : "-");
        }
    }
    fprintf(f, "end_user_cb\n");

    fprintf(f, "\ncommands\n");
    for (int i = 0; i < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active) continue;
        project_command_fold_texture_slots_into_srvs(&c);

        char rt_ref[MAX_PATH_LEN] = {};
        char depth_ref[MAX_PATH_LEN] = {};
        char mesh_ref[MAX_PATH_LEN] = {};
        char shader_ref[MAX_PATH_LEN] = {};
        char shadow_shader_ref[MAX_PATH_LEN] = {};
        res_ref(c.rt, rt_ref, MAX_PATH_LEN);
        res_ref(c.depth, depth_ref, MAX_PATH_LEN);
        res_ref(c.mesh, mesh_ref, MAX_PATH_LEN);
        res_ref(c.shader, shader_ref, MAX_PATH_LEN);
        res_ref(c.shadow_shader, shadow_shader_ref, MAX_PATH_LEN);

        fprintf(f, "command %s %s %s\n", cmd_type_name(c.type), c.name, bool_str(c.enabled));
        fprintf(f, "  targets %s %s\n", rt_ref, depth_ref);
        fprintf(f, "  mrts %d", c.mrt_count);
        for (int rt_i = 0; rt_i < c.mrt_count; rt_i++) {
            char ref[MAX_PATH_LEN] = {};
            res_ref(c.mrt_handles[rt_i], ref, MAX_PATH_LEN);
            fprintf(f, " %s", ref);
        }
        fprintf(f, "\n");
        fprintf(f, "  mesh_shader %s %s\n", mesh_ref, shader_ref);
        fprintf(f, "  draw_source %s\n", draw_source_name(c.draw_source));
        fprintf(f, "  topology %s\n", draw_topology_name(c.draw_topology));
        fprintf(f, "  shadow_shader %s\n", shadow_shader_ref);
        fprintf(f, "  render_state %s %s %s %s %s %s %s\n",
            bool_str(c.color_write), bool_str(c.depth_test), bool_str(c.depth_write),
            bool_str(c.alpha_blend), bool_str(c.cull_back), bool_str(c.shadow_cast), bool_str(c.shadow_receive));
        Quat rotq = quat_from_array(c.rotq);
        fprintf(f, "  transformq %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
            c.pos[0], c.pos[1], c.pos[2],
            rotq.x, rotq.y, rotq.z, rotq.w,
            c.scale[0], c.scale[1], c.scale[2]);
        fprintf(f, "  clear %s %.9g %.9g %.9g %.9g %s %.9g\n",
            bool_str(c.clear_color_enabled), c.clear_color[0], c.clear_color[1], c.clear_color[2], c.clear_color[3],
            bool_str(c.clear_depth), c.depth_clear_val);
        char clear_color_ref[MAX_PATH_LEN] = {};
        char clear_depth_ref[MAX_PATH_LEN] = {};
        clear_source_ref(c.clear_color_source, RES_FLOAT4, clear_color_ref, MAX_PATH_LEN);
        clear_source_ref(c.clear_depth_source, RES_FLOAT, clear_depth_ref, MAX_PATH_LEN);
        fprintf(f, "  clear_sources %s %s\n", clear_color_ref, clear_depth_ref);
        fprintf(f, "  vertex_count %d\n", c.vertex_count);
        fprintf(f, "  instance %d\n", c.instance_count);
        fprintf(f, "  threads %d %d %d\n", c.thread_x, c.thread_y, c.thread_z);
        fprintf(f, "  compute_on_reset %s\n", bool_str(c.compute_on_reset));
        char dispatch_ref[MAX_PATH_LEN] = {};
        res_ref(c.dispatch_size_source, dispatch_ref, MAX_PATH_LEN);
        fprintf(f, "  dispatch_from %s\n", dispatch_ref);
        char indirect_ref[MAX_PATH_LEN] = {};
        res_ref(c.indirect_buf, indirect_ref, MAX_PATH_LEN);
        fprintf(f, "  indirect_args %s %u\n", indirect_ref, c.indirect_offset);
        fprintf(f, "  repeat %d %s\n", c.repeat_count, bool_str(c.repeat_expanded));
        Command* parent_cmd = cmd_get(c.parent);
        fprintf(f, "  parent %s\n", parent_cmd ? parent_cmd->name : "-");

        int srv_count = 0;
        for (int s = 0; s < c.srv_count; s++) if (res_get(c.srv_handles[s])) srv_count++;
        fprintf(f, "  srvs %d", srv_count);
        for (int s = 0; s < c.srv_count; s++) {
            if (!res_get(c.srv_handles[s])) continue;
            char ref[MAX_PATH_LEN] = {};
            res_ref(c.srv_handles[s], ref, MAX_PATH_LEN);
            fprintf(f, " %s %u", ref, c.srv_slots[s]);
        }
        fprintf(f, "\n");

        int uav_count = 0;
        for (int u = 0; u < c.uav_count; u++) if (res_get(c.uav_handles[u])) uav_count++;
        fprintf(f, "  uavs %d", uav_count);
        for (int u = 0; u < c.uav_count; u++) {
            if (!res_get(c.uav_handles[u])) continue;
            char ref[MAX_PATH_LEN] = {};
            res_ref(c.uav_handles[u], ref, MAX_PATH_LEN);
            fprintf(f, " %s %u", ref, c.uav_slots[u]);
        }
        fprintf(f, "\n");

        if (Resource* param_shader = res_get(c.shader))
            user_cb_sync_command_params(&c, param_shader);
        fprintf(f, "  params %d\n", c.param_count);
        for (int p_i = 0; p_i < c.param_count; p_i++) {
            CommandParam& p = c.params[p_i];
            char source_ref[MAX_PATH_LEN] = {};
            UserCBSourceKind source_kind = p.source_kind;
            if (source_kind == USER_CB_SOURCE_NONE && p.source != INVALID_HANDLE)
                source_kind = USER_CB_SOURCE_RESOURCE;
            if (source_kind == USER_CB_SOURCE_RESOURCE)
                res_ref(p.source, source_ref, MAX_PATH_LEN);
            else
                snprintf(source_ref, sizeof(source_ref), "-");
            fprintf(f, "  param %s %s %s %s %d %d %d %d %.9g %.9g %.9g %.9g\n",
                p.name, res_type_str(p.type), bool_str(p.enabled), source_ref,
                p.ival[0], p.ival[1], p.ival[2], p.ival[3],
                p.fval[0], p.fval[1], p.fval[2], p.fval[3]);
            if (source_kind != USER_CB_SOURCE_NONE && source_kind != USER_CB_SOURCE_RESOURCE) {
                fprintf(f, "  param_source %s %s %s\n",
                    p.name, user_cb_source_kind_token(source_kind),
                    p.source_target[0] ? p.source_target : "-");
            }
        }
        fprintf(f, "end_command\n");
    }

    timeline_write_project(f);

    fclose(f);
    project_set_current_path(path);
    log_info("Project saved: %s", path);
    return true;
}

static void parse_slots(char* line, ResHandle* handles, uint32_t* slots, int* count, int max_count,
                        const ResourceLookup& lookup) {
    char* tok = strtok(line, " \t\r\n");
    tok = strtok(nullptr, " \t\r\n");
    int n = tok ? atoi(tok) : 0;
    if (n < 0) n = 0;
    if (n > max_count) n = max_count;
    *count = n;

    for (int i = 0; i < n; i++) {
        char* name = strtok(nullptr, " \t\r\n");
        char* slot = strtok(nullptr, " \t\r\n");
        handles[i] = res_by_ref(name, lookup);
        slots[i] = slot ? (uint32_t)atoi(slot) : 0;
    }
}

static void parse_handles(char* line, ResHandle* handles, int* count, int max_count, const ResourceLookup& lookup) {
    for (int i = 0; i < max_count; i++)
        handles[i] = INVALID_HANDLE;

    char* tok = strtok(line, " \t\r\n");
    tok = strtok(nullptr, " \t\r\n");
    int n = tok ? atoi(tok) : 0;
    if (n < 0) n = 0;
    if (n > max_count) n = max_count;
    *count = n;

    for (int i = 0; i < n; i++) {
        char* name = strtok(nullptr, " \t\r\n");
        handles[i] = res_by_ref(name, lookup);
    }
}

static bool project_read_line(const char*& cursor, const char* end, char* out, int out_sz) {
    if (!cursor || !end || cursor >= end || !out || out_sz <= 0)
        return false;

    int n = 0;
    while (cursor < end && *cursor != '\n' && *cursor != '\r') {
        if (n < out_sz - 1)
            out[n++] = *cursor;
        cursor++;
    }
    while (cursor < end && (*cursor == '\n' || *cursor == '\r'))
        cursor++;
    out[n] = '\0';
    return true;
}

// Parse a saved project file and rebuild the in-memory editor state. Only the
// current project text format is accepted by the runtime loader.
bool project_load_text(const char* path) {
    void* project_bytes = nullptr;
    size_t project_size = 0;
    if (!lt_read_file(path, &project_bytes, &project_size)) {
        log_error("Project load failed: %s", path);
        return false;
    }

    {
        bool has_export_settings = false;
        bool has_timeline_global = false;
        bool has_timeline_clip = false;
        char scan_line[1024] = {};
        const char* scan = (const char*)project_bytes;
        const char* scan_end = scan + project_size;
        while (project_read_line(scan, scan_end, scan_line, sizeof(scan_line))) {
            char tmp[1024] = {};
            strncpy(tmp, scan_line, sizeof(tmp) - 1);
            char* tag = strtok(tmp, " \t\r\n");
            if (!tag || tag[0] == '#')
                continue;
            if (strcmp(tag, "export_settings") == 0) has_export_settings = true;
            else if (strcmp(tag, "timeline_global") == 0) has_timeline_global = true;
            else if (strcmp(tag, "timeline_clip") == 0) has_timeline_clip = true;
        }
        if (!has_export_settings || !has_timeline_global || !has_timeline_clip) {
            lt_free_file(project_bytes);
            log_error("Project load failed: project is not saved in the current .lt format.");
            return false;
        }
    }

    project_clear_user_data();
    project_reset_view_defaults();

    char line[1024] = {};
    Command* cur = nullptr;
    CmdHandle cur_h = INVALID_HANDLE;
    CmdHandle pending_parent_cmds[MAX_COMMANDS] = {};
    char pending_parent_names[MAX_COMMANDS][MAX_NAME] = {};
    int pending_parent_count = 0;
    struct UserVarNameRemap {
        char old_name[MAX_NAME];
        char new_name[MAX_NAME];
        ResType type;
    };
    UserVarNameRemap user_var_remaps[MAX_USER_CB_VARS] = {};
    int user_var_remap_count = 0;
    auto remember_user_var_name = [&](const char* old_name, ResType type, const char* new_name) {
        if (!old_name || !old_name[0] || !new_name || !new_name[0] ||
            strcmp(old_name, new_name) == 0 || user_var_remap_count >= MAX_USER_CB_VARS)
            return;
        UserVarNameRemap& remap = user_var_remaps[user_var_remap_count++];
        strncpy(remap.old_name, old_name, MAX_NAME - 1);
        remap.old_name[MAX_NAME - 1] = '\0';
        strncpy(remap.new_name, new_name, MAX_NAME - 1);
        remap.new_name[MAX_NAME - 1] = '\0';
        remap.type = type;
    };
    auto remap_user_var_name = [&](const char* name, ResType type) -> const char* {
        if (!name)
            return "";
        for (int i = user_var_remap_count - 1; i >= 0; i--) {
            const UserVarNameRemap& remap = user_var_remaps[i];
            if (strcmp(remap.old_name, name) != 0)
                continue;
            if (type != RES_NONE && remap.type != type)
                continue;
            return remap.new_name;
        }
        return name;
    };
    int timeline_load_track = -1;
    int timeline_clip_count = 0;
    int timeline_current_clip = -1;
    int timeline_global_current = 0;
    bool timeline_global_loop = false;
    bool timeline_global_enabled = false;
    bool timeline_clip_enabled_load[MAX_TIMELINES] = {};
    bool saw_export_settings = false;
    bool saw_timeline_global = false;
    const char* cursor = (const char*)project_bytes;
    const char* end = cursor + project_size;
    while (project_read_line(cursor, end, line, sizeof(line))) {
        char tmp[1024];
        strncpy(tmp, line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* tag = strtok(tmp, " \t\r\n");
        if (!tag || tag[0] == '#') continue;

        if (strcmp(tag, "export_settings") == 0) {
            saw_export_settings = true;
            char* runtime_input = strtok(nullptr, " \t\r\n");
            char* esc_close = strtok(nullptr, " \t\r\n");
            char* wireframe = strtok(nullptr, " \t\r\n");
            char* grid = strtok(nullptr, " \t\r\n");
            char* vsync = strtok(nullptr, " \t\r\n");
            char* profiler = strtok(nullptr, " \t\r\n");
            char* shader_warnings = strtok(nullptr, " \t\r\n");
            if (runtime_input) g_export_settings.runtime_input_enabled = atoi(runtime_input) != 0;
            if (esc_close) g_export_settings.escape_closes_player = atoi(esc_close) != 0;
            if (wireframe) g_export_settings.force_wireframe = atoi(wireframe) != 0;
            if (grid) g_export_settings.show_grid_overlay = atoi(grid) != 0;
            if (vsync) g_export_settings.vsync = atoi(vsync) != 0;
            if (profiler) g_export_settings.profiler = atoi(profiler) != 0;
            if (shader_warnings) g_export_settings.shader_binding_warnings = atoi(shader_warnings) != 0;
        } else if (strcmp(tag, "timeline") == 0) {
            timeline_reset();
            timeline_load_track = -1;
            timeline_current_clip = -1;
            timeline_clip_count = 0;
            timeline_global_current = 0;
            timeline_global_loop = false;
            timeline_global_enabled = false;
            saw_timeline_global = false;
            memset(timeline_clip_enabled_load, 0, sizeof(timeline_clip_enabled_load));
        } else if (strcmp(tag, "end_timeline") == 0) {
            timeline_load_track = -1;
            timeline_current_clip = -1;
            for (int i = 0; i < timeline_clip_count && i < timeline_count(); i++)
                timeline_set_timeline_enabled(i, timeline_clip_enabled_load[i]);
            timeline_set_loop(timeline_global_loop);
            timeline_set_enabled(timeline_global_enabled);
            timeline_set_current_index(timeline_global_current);
        } else if (strcmp(tag, "end_timeline_clip") == 0) {
            timeline_load_track = -1;
            timeline_current_clip = -1;
        } else if (strcmp(tag, "timeline_global") == 0) {
            char* current = strtok(nullptr, " \t\r\n");
            char* loop = strtok(nullptr, " \t\r\n");
            char* enabled = strtok(nullptr, " \t\r\n");
            saw_timeline_global = true;
            if (current) timeline_global_current = atoi(current);
            if (loop) timeline_global_loop = atoi(loop) != 0;
            if (enabled) timeline_global_enabled = atoi(enabled) != 0;
        } else if (strcmp(tag, "timeline_clip") == 0) {
            char* name = strtok(nullptr, " \t\r\n");
            char* fps = strtok(nullptr, " \t\r\n");
            char* length = strtok(nullptr, " \t\r\n");
            char* frame = strtok(nullptr, " \t\r\n");
            char* enabled = strtok(nullptr, " \t\r\n");
            char* interpolate = strtok(nullptr, " \t\r\n");
            int clip_index = timeline_clip_count;
            if (clip_index == 0)
                timeline_set_current_index(0);
            else
                clip_index = timeline_add(name ? name : nullptr);
            if (clip_index >= 0) {
                timeline_set_current_index(clip_index);
                if (name) timeline_set_name(clip_index, name);
                if (fps) timeline_set_fps(atoi(fps));
                if (length) timeline_set_length_frames(atoi(length));
                if (frame) timeline_set_current_frame(atoi(frame));
                if (interpolate) timeline_set_interpolate_frames(atoi(interpolate) != 0);
                if (clip_index < MAX_TIMELINES)
                    timeline_clip_enabled_load[clip_index] = enabled ? (atoi(enabled) != 0) : true;
                timeline_current_clip = clip_index;
                timeline_clip_count++;
            }
            timeline_load_track = -1;
        } else if (strcmp(tag, "timeline_track") == 0) {
            char* kind = strtok(nullptr, " \t\r\n");
            char* target = strtok(nullptr, " \t\r\n");
            char* type = strtok(nullptr, " \t\r\n");
            strtok(nullptr, " \t\r\n"); // key_count, kept for readability in the text format.
            char* enabled = strtok(nullptr, " \t\r\n");
            if (timeline_current_clip < 0) {
                timeline_load_track = -1;
                continue;
            }
            TimelineTrackKind track_kind = timeline_track_kind_from_token(kind);
            ResType track_type = res_type_from_token(type);
            const char* remapped_target = track_kind == TIMELINE_TRACK_USER_VAR ?
                remap_user_var_name(target ? target : "", track_type) : (target ? target : "");
            timeline_load_track = timeline_add_track(
                track_kind,
                remapped_target,
                track_type);
            if (timeline_load_track >= 0 && enabled)
                g_timeline_tracks[timeline_load_track].enabled = atoi(enabled) != 0;
        } else if (strcmp(tag, "timeline_key") == 0) {
            char* frame_tok = strtok(nullptr, " \t\r\n");
            TimelineKey* key = frame_tok ? timeline_set_key(timeline_load_track, atoi(frame_tok)) : nullptr;
            if (key && timeline_load_track >= 0 && timeline_load_track < g_timeline_track_count) {
                TimelineTrack& track = g_timeline_tracks[timeline_load_track];
                int n = timeline_track_value_count(track);
                if (timeline_track_uses_integral_values(track)) {
                    for (int i = 0; i < n; i++) {
                        char* v = strtok(nullptr, " \t\r\n");
                        key->ival[i] = v ? atoi(v) : 0;
                    }
                } else {
                    float values[16] = {};
                    int value_count = 0;
                    for (char* v = strtok(nullptr, " \t\r\n");
                         v && value_count < 16;
                         v = strtok(nullptr, " \t\r\n")) {
                        values[value_count++] = (float)atof(v);
                    }

                    for (int i = 0; i < n && i < value_count; i++)
                        key->fval[i] = values[i];
                    if (track.kind == TIMELINE_TRACK_COMMAND_TRANSFORM)
                        quat_to_array(quat_from_array(&key->fval[3]), &key->fval[3]);
                }
            }
        } else if (strcmp(tag, "camera_fps") == 0) {
            g_camera.position[0] = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.position[1] = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.position[2] = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.yaw = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.pitch = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.fov_y = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.near_z = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.far_z = (float)atof(strtok(nullptr, " \t\r\n"));
            char* roll_tok = strtok(nullptr, " \t\r\n");
            g_camera.roll = roll_tok ? (float)atof(roll_tok) : 0.0f;
            camera_set_euler(&g_camera, g_camera.yaw, g_camera.pitch, g_camera.roll);
        } else if (strcmp(tag, "dirlight") == 0) {
            Resource* dl = res_get(g_builtin_dirlight);
            if (!dl) continue;
            for (int i = 0; i < 3; i++) dl->light_pos[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            for (int i = 0; i < 3; i++) dl->light_target[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            for (int i = 0; i < 3; i++) dl->light_color[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            dl->light_intensity = (float)atof(strtok(nullptr, " \t\r\n"));
            dl->shadow_width = atoi(strtok(nullptr, " \t\r\n"));
            dl->shadow_height = atoi(strtok(nullptr, " \t\r\n"));
            dl->shadow_near = (float)atof(strtok(nullptr, " \t\r\n"));
            dl->shadow_far = (float)atof(strtok(nullptr, " \t\r\n"));
            dl->shadow_extent[0] = (float)atof(strtok(nullptr, " \t\r\n"));
            dl->shadow_extent[1] = (float)atof(strtok(nullptr, " \t\r\n"));
            char* cascade_count_tok = strtok(nullptr, " \t\r\n");
            char* shadow_distance_tok = strtok(nullptr, " \t\r\n");
            char* split_lambda_tok = strtok(nullptr, " \t\r\n");
            dl->shadow_cascade_count = cascade_count_tok ? atoi(cascade_count_tok) : 1;
            dl->shadow_distance = shadow_distance_tok ? (float)atof(shadow_distance_tok) : dl->shadow_distance;
            dl->shadow_split_lambda = split_lambda_tok ? (float)atof(split_lambda_tok) : dl->shadow_split_lambda;
            if (dl->shadow_cascade_count < 1) dl->shadow_cascade_count = 1;
            if (dl->shadow_cascade_count > MAX_SHADOW_CASCADES) dl->shadow_cascade_count = MAX_SHADOW_CASCADES;
            if (dl->shadow_distance < 0.1f) dl->shadow_distance = 0.1f;
            dl->shadow_split_lambda = clampf(dl->shadow_split_lambda, 0.0f, 1.0f);
            for (int i = 0; i < MAX_SHADOW_CASCADES; i++) {
                char* split_tok = strtok(nullptr, " \t\r\n");
                char* extent_x_tok = strtok(nullptr, " \t\r\n");
                char* extent_y_tok = strtok(nullptr, " \t\r\n");
                char* near_tok = strtok(nullptr, " \t\r\n");
                char* far_tok = strtok(nullptr, " \t\r\n");
                if (!split_tok || !extent_x_tok || !extent_y_tok || !near_tok || !far_tok)
                    break;
                dl->shadow_cascade_split[i] = (float)atof(split_tok);
                dl->shadow_cascade_extent[i][0] = (float)atof(extent_x_tok);
                dl->shadow_cascade_extent[i][1] = (float)atof(extent_y_tok);
                dl->shadow_cascade_near[i] = (float)atof(near_tok);
                dl->shadow_cascade_far[i] = (float)atof(far_tok);
            }
            project_validate_manual_shadow_cascades(dl, g_camera.near_z, g_camera.far_z);
            if (dl->shadow_width > 0 && dl->shadow_height > 0 && g_dx.dev)
                dx_create_shadow_map(dl->shadow_width, dl->shadow_height);
        } else if (strcmp(tag, "resource") == 0) {
            char* kind = strtok(nullptr, " \t\r\n");
            char* name = strtok(nullptr, " \t\r\n");
            if (!kind || !name) continue;

            if (strcmp(kind, "render_texture") == 0) {
                int w = atoi(strtok(nullptr, " \t\r\n"));
                int h = atoi(strtok(nullptr, " \t\r\n"));
                DXGI_FORMAT fmt = (DXGI_FORMAT)atoi(strtok(nullptr, " \t\r\n"));
                bool rtv = atoi(strtok(nullptr, " \t\r\n")) != 0;
                bool srv = atoi(strtok(nullptr, " \t\r\n")) != 0;
                bool uav = atoi(strtok(nullptr, " \t\r\n")) != 0;
                bool dsv = atoi(strtok(nullptr, " \t\r\n")) != 0;
                char* scene_div_tok = strtok(nullptr, " \t\r\n");
                int scene_div = scene_div_tok ? atoi(scene_div_tok) : 0;
                res_create_render_texture(name, w, h, fmt, rtv, srv, uav, dsv, scene_div);
            } else if (strcmp(kind, "value") == 0) {
                char* type_name = name;
                char* value_name = strtok(nullptr, " \t\r\n");
                ResType type = res_type_from_token(type_name);
                if (type == RES_NONE || !value_name)
                    continue;
                ResHandle h = res_alloc(value_name, type);
                Resource* r = res_get(h);
                if (!r)
                    continue;
                for (int i = 0; i < 4; i++) r->ival[i] = atoi(strtok(nullptr, " \t\r\n"));
                for (int i = 0; i < 4; i++) r->fval[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(kind, "render_texture3d") == 0) {
                int w = atoi(strtok(nullptr, " \t\r\n"));
                int h = atoi(strtok(nullptr, " \t\r\n"));
                int d = atoi(strtok(nullptr, " \t\r\n"));
                DXGI_FORMAT fmt = (DXGI_FORMAT)atoi(strtok(nullptr, " \t\r\n"));
                bool rtv = atoi(strtok(nullptr, " \t\r\n")) != 0;
                bool srv = atoi(strtok(nullptr, " \t\r\n")) != 0;
                bool uav = atoi(strtok(nullptr, " \t\r\n")) != 0;
                res_create_render_texture3d(name, w, h, d, fmt, rtv, srv, uav);
            } else if (strcmp(kind, "structured_buffer") == 0) {
                int stride = atoi(strtok(nullptr, " \t\r\n"));
                int count = atoi(strtok(nullptr, " \t\r\n"));
                bool srv = atoi(strtok(nullptr, " \t\r\n")) != 0;
                bool uav = atoi(strtok(nullptr, " \t\r\n")) != 0;
                char* indirect_tok = strtok(nullptr, " \t\r\n");
                bool indirect_args = indirect_tok ? atoi(indirect_tok) != 0 : false;
                res_create_structured_buffer(name, stride, count, srv, uav, indirect_args);
            } else if (strcmp(kind, "gaussian_splat") == 0) {
                char* p = strtok(nullptr, " \t\r\n");
                res_load_gaussian_splat(name, p && strcmp(p, "-") != 0 ? p : "");
            } else if (strcmp(kind, "mesh_primitive") == 0) {
                char* prim = strtok(nullptr, " \t\r\n");
                res_create_mesh_primitive(name, mesh_prim_from_name(prim ? prim : "cube"));
            } else if (strcmp(kind, "mesh_gltf") == 0) {
                char* p = strtok(nullptr, " \t\r\n");
                res_load_mesh(name, p ? p : "");
            } else if (strcmp(kind, "shader_vsps") == 0) {
                char* p = strtok(nullptr, " \t\r\n");
                res_create_shader(name, p && strcmp(p, "-") != 0 ? p : "", "VSMain", "PSMain");
            } else if (strcmp(kind, "shader_cs") == 0) {
                char* p = strtok(nullptr, " \t\r\n");
                res_create_compute_shader(name, p && strcmp(p, "-") != 0 ? p : "", "CSMain");
            } else if (strcmp(kind, "texture2d") == 0) {
                char* p = strtok(nullptr, " \t\r\n");
                if (p && strcmp(p, "-") != 0) res_load_texture(name, p);
            }
        } else if (strcmp(tag, "mesh_part_disabled") == 0) {
            char* mesh_ref = strtok(nullptr, " \t\r\n");
            char* part_tok = strtok(nullptr, " \t\r\n");
            ResHandle mesh_h = res_by_ref(mesh_ref, res_lookup_types(RES_MESH));
            Resource* mesh = res_get(mesh_h);
            int part_index = part_tok ? atoi(part_tok) : -1;
            if (mesh && part_index >= 0 && part_index < mesh->mesh_part_count)
                mesh->mesh_parts[part_index].enabled = false;
        } else if (strcmp(tag, "user_var") == 0) {
            char* name = strtok(nullptr, " \t\r\n");
            char* type_name = strtok(nullptr, " \t\r\n");
            char* source = strtok(nullptr, " \t\r\n");
            ResType type = res_type_from_token(type_name);
            if (name && type != RES_NONE && user_cb_add_var(name, type)) {
                int idx = g_user_cb_count - 1;
                UserCBEntry& e = g_user_cb_entries[idx];
                remember_user_var_name(name, type, e.name);
                for (int i = 0; i < 4; i++) {
                    char* v = strtok(nullptr, " \t\r\n");
                    e.ival[i] = v ? atoi(v) : 0;
                }
                for (int i = 0; i < 4; i++) {
                    char* v = strtok(nullptr, " \t\r\n");
                    e.fval[i] = v ? (float)atof(v) : 0.0f;
                }
                ResHandle source_h = res_by_ref(source, res_lookup_types(type));
                if (source_h != INVALID_HANDLE)
                    user_cb_set_source(idx, source_h);
            }
        } else if (strcmp(tag, "user_var_source") == 0) {
            char* name = strtok(nullptr, " \t\r\n");
            char* kind_tok = strtok(nullptr, " \t\r\n");
            char* target = strtok(nullptr, " \t\r\n");
            if (name && kind_tok) {
                const char* actual_name = remap_user_var_name(name, RES_NONE);
                for (int i = 0; i < g_user_cb_count; i++) {
                    if (strcmp(g_user_cb_entries[i].name, actual_name) != 0)
                        continue;
                    user_cb_set_scene_source(i, user_cb_source_kind_from_token(kind_tok),
                                             target && strcmp(target, "-") != 0 ? target : "");
                    break;
                }
            }
        } else if (strcmp(tag, "command") == 0) {
            char* kind = strtok(nullptr, " \t\r\n");
            char* name = strtok(nullptr, " \t\r\n");
            char* enabled = strtok(nullptr, " \t\r\n");
            CmdHandle h = cmd_alloc(name ? name : "cmd", cmd_type_from_name(kind ? kind : "draw_mesh"));
            cur = cmd_get(h);
            cur_h = h;
            if (cur && enabled) cur->enabled = atoi(enabled) != 0;
        } else if (strcmp(tag, "end_command") == 0) {
            cur = nullptr;
            cur_h = INVALID_HANDLE;
        } else if (cur) {
            if (strcmp(tag, "targets") == 0) {
                cur->rt = res_by_ref(strtok(nullptr, " \t\r\n"),
                    res_lookup_types(RES_RENDER_TEXTURE2D, RES_RENDER_TEXTURE3D, RES_BUILTIN_SCENE_COLOR));
                cur->depth = res_by_ref(strtok(nullptr, " \t\r\n"),
                    res_lookup_types(RES_RENDER_TEXTURE2D, RES_BUILTIN_SCENE_DEPTH));
            } else if (strcmp(tag, "mrts") == 0) {
                parse_handles(line, cur->mrt_handles, &cur->mrt_count, MAX_DRAW_RENDER_TARGETS - 1,
                    res_lookup_types(RES_RENDER_TEXTURE2D, RES_RENDER_TEXTURE3D, RES_BUILTIN_SCENE_COLOR));
            } else if (strcmp(tag, "mesh_shader") == 0) {
                cur->mesh = res_by_ref(strtok(nullptr, " \t\r\n"), res_lookup_types(RES_MESH));
                cur->shader = res_by_ref(strtok(nullptr, " \t\r\n"), res_lookup_types(RES_SHADER));
            } else if (strcmp(tag, "draw_source") == 0) {
                cur->draw_source = (int)draw_source_from_name(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "topology") == 0) {
                cur->draw_topology = (int)draw_topology_from_name(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "shadow_shader") == 0) {
                cur->shadow_shader = res_by_ref(strtok(nullptr, " \t\r\n"), res_lookup_types(RES_SHADER));
            } else if (strcmp(tag, "render_state") == 0) {
                cur->color_write = atoi(strtok(nullptr, " \t\r\n")) != 0;
                cur->depth_test = atoi(strtok(nullptr, " \t\r\n")) != 0;
                cur->depth_write = atoi(strtok(nullptr, " \t\r\n")) != 0;
                cur->alpha_blend = atoi(strtok(nullptr, " \t\r\n")) != 0;
                cur->cull_back = atoi(strtok(nullptr, " \t\r\n")) != 0;
                cur->shadow_cast = atoi(strtok(nullptr, " \t\r\n")) != 0;
                cur->shadow_receive = atoi(strtok(nullptr, " \t\r\n")) != 0;
            } else if (strcmp(tag, "transformq") == 0) {
                for (int i = 0; i < 3; i++) cur->pos[i] = (float)atof(strtok(nullptr, " \t\r\n"));
                float q[4] = {};
                for (int i = 0; i < 4; i++) q[i] = (float)atof(strtok(nullptr, " \t\r\n"));
                quat_to_array(quat_from_array(q), cur->rotq);
                for (int i = 0; i < 3; i++) cur->scale[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "clear") == 0) {
                cur->clear_color_enabled = atoi(strtok(nullptr, " \t\r\n")) != 0;
                for (int i = 0; i < 4; i++) cur->clear_color[i] = (float)atof(strtok(nullptr, " \t\r\n"));
                cur->clear_depth = atoi(strtok(nullptr, " \t\r\n")) != 0;
                cur->depth_clear_val = (float)atof(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "clear_sources") == 0) {
                char* color_src = strtok(nullptr, " \t\r\n");
                char* depth_src = strtok(nullptr, " \t\r\n");
                if (color_src && strcmp(color_src, "-") != 0) {
                    ResHandle h = res_by_ref(color_src, res_lookup_types(RES_FLOAT4));
                    Resource* r = res_get(h);
                    if (r) {
                        strncpy(cur->clear_color_source, r->name, MAX_NAME - 1);
                        cur->clear_color_source[MAX_NAME - 1] = '\0';
                    }
                }
                if (depth_src && strcmp(depth_src, "-") != 0) {
                    ResHandle h = res_by_ref(depth_src, res_lookup_types(RES_FLOAT));
                    Resource* r = res_get(h);
                    if (r) {
                        strncpy(cur->clear_depth_source, r->name, MAX_NAME - 1);
                        cur->clear_depth_source[MAX_NAME - 1] = '\0';
                    }
                }
            } else if (strcmp(tag, "vertex_count") == 0) {
                cur->vertex_count = atoi(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "instance") == 0) {
                cur->instance_count = atoi(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "threads") == 0) {
                cur->thread_x = atoi(strtok(nullptr, " \t\r\n"));
                cur->thread_y = atoi(strtok(nullptr, " \t\r\n"));
                cur->thread_z = atoi(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "compute_on_reset") == 0) {
                char* enabled = strtok(nullptr, " \t\r\n");
                cur->compute_on_reset = enabled ? atoi(enabled) != 0 : false;
            } else if (strcmp(tag, "dispatch_from") == 0) {
                cur->dispatch_size_source = res_by_ref(strtok(nullptr, " \t\r\n"), res_lookup_types());
            } else if (strcmp(tag, "indirect_args") == 0) {
                cur->indirect_buf = res_by_ref(strtok(nullptr, " \t\r\n"), res_lookup_types(RES_STRUCTURED_BUFFER));
                char* offset_tok = strtok(nullptr, " \t\r\n");
                cur->indirect_offset = offset_tok ? (uint32_t)strtoul(offset_tok, nullptr, 10) : 0u;
            } else if (strcmp(tag, "repeat") == 0) {
                char* count = strtok(nullptr, " \t\r\n");
                char* expanded = strtok(nullptr, " \t\r\n");
                cur->repeat_count = count ? atoi(count) : 1;
                if (cur->repeat_count < 1) cur->repeat_count = 1;
                cur->repeat_expanded = expanded ? atoi(expanded) != 0 : true;
            } else if (strcmp(tag, "parent") == 0) {
                char* parent_name = strtok(nullptr, " \t\r\n");
                if (parent_name && strcmp(parent_name, "-") != 0 && cur_h != INVALID_HANDLE &&
                    pending_parent_count < MAX_COMMANDS) {
                    pending_parent_cmds[pending_parent_count] = cur_h;
                    strncpy(pending_parent_names[pending_parent_count], parent_name, MAX_NAME - 1);
                    pending_parent_names[pending_parent_count][MAX_NAME - 1] = '\0';
                    pending_parent_count++;
                }
            } else if (strcmp(tag, "textures") == 0) {
                // The text format accepts texture slot lists as SRV-compatible t# bindings.
                parse_slots(line, cur->tex_handles, cur->tex_slots, &cur->tex_count, MAX_TEX_SLOTS, res_lookup_srv());
            } else if (strcmp(tag, "srvs") == 0) {
                parse_slots(line, cur->srv_handles, cur->srv_slots, &cur->srv_count, MAX_SRV_SLOTS, res_lookup_srv());
            } else if (strcmp(tag, "uavs") == 0) {
                parse_slots(line, cur->uav_handles, cur->uav_slots, &cur->uav_count, MAX_UAV_SLOTS, res_lookup_uav());
            } else if (strcmp(tag, "params") == 0) {
                cur->param_count = 0;
            } else if (strcmp(tag, "param") == 0 && cur->param_count < MAX_COMMAND_PARAMS) {
                CommandParam* p = &cur->params[cur->param_count++];
                memset(p, 0, sizeof(*p));
                char* name = strtok(nullptr, " \t\r\n");
                char* type = strtok(nullptr, " \t\r\n");
                char* enabled = strtok(nullptr, " \t\r\n");
                char* source = strtok(nullptr, " \t\r\n");
                if (name) {
                    strncpy(p->name, name, MAX_NAME - 1);
                    p->name[MAX_NAME - 1] = '\0';
                }
                p->type = res_type_from_token(type);
                p->enabled = enabled ? atoi(enabled) != 0 : true;
                p->source = res_by_ref(source, res_lookup_types(p->type));
                p->source_kind = p->source != INVALID_HANDLE ? USER_CB_SOURCE_RESOURCE : USER_CB_SOURCE_NONE;
                for (int i = 0; i < 4; i++) p->ival[i] = atoi(strtok(nullptr, " \t\r\n"));
                for (int i = 0; i < 4; i++) p->fval[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "param_source") == 0 && cur) {
                char* name = strtok(nullptr, " \t\r\n");
                char* kind_tok = strtok(nullptr, " \t\r\n");
                char* target = strtok(nullptr, " \t\r\n");
                if (name && kind_tok) {
                    for (int p_i = 0; p_i < cur->param_count; p_i++) {
                        CommandParam& p = cur->params[p_i];
                        if (strcmp(p.name, name) != 0)
                            continue;
                        p.source_kind = user_cb_source_kind_from_token(kind_tok);
                        p.source = INVALID_HANDLE;
                        strncpy(p.source_target, target && strcmp(target, "-") != 0 ? target : "", MAX_NAME - 1);
                        p.source_target[MAX_NAME - 1] = '\0';
                        break;
                    }
                }
            }
        }
    }

    for (int i = 0; i < pending_parent_count; i++) {
        Command* child = cmd_get(pending_parent_cmds[i]);
        CmdHandle parent = cmd_find_by_name(pending_parent_names[i]);
        if (child && parent != INVALID_HANDLE && parent != pending_parent_cmds[i])
            child->parent = parent;
    }

    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (g_commands[i].active)
            project_command_fold_texture_slots_into_srvs(&g_commands[i]);
    }
    cmd_mark_all_dirty();

    if (!saw_export_settings) {
        lt_free_file(project_bytes);
        log_error("Project load failed: missing export_settings in current .lt format.");
        return false;
    }
    if (!saw_timeline_global || timeline_clip_count <= 0) {
        lt_free_file(project_bytes);
        log_error("Project load failed: missing current timeline_global/timeline_clip block.");
        return false;
    }

    lt_free_file(project_bytes);
    project_set_current_path(path);
    dx_invalidate_scene_history();
    log_info("Project loaded: %s", path);
    return true;
}
