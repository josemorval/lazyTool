#include "project.h"
#include "resources.h"
#include "commands.h"
#include "dx11_ctx.h"
#include "log.h"
#include "ui.h"
#include "user_cb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <direct.h>

// project.cpp owns the text project format used to save and restore the full
// editor state in a way that remains readable and diff-friendly.

static const char* bool_str(bool v) { return v ? "1" : "0"; }
static char s_project_current_path[MAX_PATH_LEN] = {};

static void project_set_current_path(const char* path) {
    if (!path) path = "";
    strncpy(s_project_current_path, path, MAX_PATH_LEN - 1);
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
    if (strcmp(name, "texture2d") == 0 || strcmp(name, "Texture2D") == 0) return RES_TEXTURE2D;
    if (strcmp(name, "render_texture2d") == 0 || strcmp(name, "RenderTexture2D") == 0) return RES_RENDER_TEXTURE2D;
    if (strcmp(name, "render_texture3d") == 0 || strcmp(name, "RenderTexture3D") == 0) return RES_RENDER_TEXTURE3D;
    if (strcmp(name, "structured_buffer") == 0 || strcmp(name, "StructuredBuffer") == 0) return RES_STRUCTURED_BUFFER;
    if (strcmp(name, "mesh") == 0 || strcmp(name, "Mesh") == 0) return RES_MESH;
    if (strcmp(name, "shader") == 0 || strcmp(name, "Shader") == 0) return RES_SHADER;
    if (strcmp(name, "builtin_time") == 0) return RES_BUILTIN_TIME;
    if (strcmp(name, "builtin_scene_color") == 0) return RES_BUILTIN_SCENE_COLOR;
    if (strcmp(name, "builtin_scene_depth") == 0) return RES_BUILTIN_SCENE_DEPTH;
    if (strcmp(name, "builtin_shadow_map") == 0) return RES_BUILTIN_SHADOW_MAP;
    if (strcmp(name, "builtin_dirlight") == 0) return RES_BUILTIN_DIRLIGHT;
    return RES_NONE;
}

static void project_compute_legacy_cascade_splits(float near_z, float far_z, int cascade_count,
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
    project_compute_legacy_cascade_splits(camera_near_z, dl->shadow_distance,
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

static void project_reset_dirlight_defaults() {
    Resource* dl = res_get(g_builtin_dirlight);
    if (!dl) return;

    dl->light_pos[0] = -0.8f;
    dl->light_pos[1] = 1.2f;
    dl->light_pos[2] = -0.8f;
    dl->light_target[0] = 0.0f;
    dl->light_target[1] = 0.0f;
    dl->light_target[2] = 0.0f;
    dl->light_dir[0] = -0.577f;
    dl->light_dir[1] = -0.577f;
    dl->light_dir[2] = -0.577f;
    dl->light_color[0] = 1.0f;
    dl->light_color[1] = 0.95f;
    dl->light_color[2] = 0.9f;
    dl->light_intensity = 1.0f;
    dl->shadow_extent[0] = 2.2f;
    dl->shadow_extent[1] = 2.2f;
    dl->shadow_near = 0.01f;
    dl->shadow_far = 4.0f;
    dl->shadow_width = 1024;
    dl->shadow_height = 1024;
    dl->shadow_cascade_count = 1;
    dl->shadow_distance = 12.0f;
    dl->shadow_split_lambda = 0.65f;
    project_seed_manual_shadow_cascades(dl, 0.1f);
    project_validate_manual_shadow_cascades(dl, 0.1f, 1000.0f);

    if (g_dx.dev && (g_dx.shadow_width != dl->shadow_width || g_dx.shadow_height != dl->shadow_height))
        dx_create_shadow_map(dl->shadow_width, dl->shadow_height);
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
    case MESH_PRIM_TETRAHEDRON: return "tetrahedron";
    case MESH_PRIM_SPHERE:      return "sphere";
    case MESH_PRIM_FULLSCREEN_TRIANGLE: return "fullscreen_triangle";
    default:                    return "unknown";
    }
}

static MeshPrimitiveType mesh_prim_from_name(const char* name) {
    if (strcmp(name, "tetrahedron") == 0) return MESH_PRIM_TETRAHEDRON;
    if (strcmp(name, "sphere") == 0) return MESH_PRIM_SPHERE;
    if (strcmp(name, "fullscreen_triangle") == 0) return MESH_PRIM_FULLSCREEN_TRIANGLE;
    return MESH_PRIM_CUBE;
}

static void project_apply_legacy_camera(float azimuth, float elevation, float distance, const float target[3]) {
    float ce = cosf(elevation);
    float se = sinf(elevation);
    float ca = cosf(azimuth);
    float sa = sinf(azimuth);
    g_camera.position[0] = target[0] + distance * ce * sa;
    g_camera.position[1] = target[1] + distance * se;
    g_camera.position[2] = target[2] + distance * ce * ca;
    g_camera.yaw = azimuth + 3.14159265358979323846f;
    g_camera.pitch = -elevation;
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
    project_reset_dirlight_defaults();
    dx_invalidate_scene_history();
    project_set_current_path("");
    float target[3] = { 0.0f, 0.0f, 0.0f };
    project_apply_legacy_camera(0.5f, 0.3f, 4.0f, target);
    g_camera.fov_y = 1.047f;
    g_camera.near_z = 0.001f;
    g_camera.far_z = 100.0f;

    ResHandle cube = res_create_mesh_primitive("normal_cube", MESH_PRIM_CUBE);
    ResHandle shader = res_create_shader("normal_color", "shaders/default.hlsl", "VSMain", "PSMain");

    CmdHandle clear_h = cmd_alloc("clear_scene", CMD_CLEAR);
    if (Command* c = cmd_get(clear_h)) {
        c->rt = g_builtin_scene_color;
        c->depth = g_builtin_scene_depth;
        c->clear_color[0] = 0.025f;
        c->clear_color[1] = 0.030f;
        c->clear_color[2] = 0.040f;
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

// Serialize the current scene/editor state into the custom text format.
bool project_save_text(const char* path) {
    ensure_parent_dir(path);
    FILE* f = fopen(path, "wb");
    if (!f) {
        log_error("Project save failed: %s", path);
        return false;
    }

    fprintf(f, "lazyTool_project 1\n\n");
    fprintf(f, "camera_fps %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n\n",
        g_camera.position[0], g_camera.position[1], g_camera.position[2],
        g_camera.yaw, g_camera.pitch,
        g_camera.fov_y, g_camera.near_z, g_camera.far_z);

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
            fprintf(f, "resource structured_buffer %s %d %d %s %s\n",
                r.name, r.elem_size, r.elem_count, bool_str(r.has_srv), bool_str(r.has_uav));
        } else if (r.type == RES_MESH) {
            if (r.path[0]) {
                fprintf(f, "resource mesh_gltf %s %s\n", r.name, r.path);
            } else {
                fprintf(f, "resource mesh_primitive %s %s\n", r.name, mesh_prim_name(r.mesh_primitive_type));
            }
        } else if (r.type == RES_SHADER) {
            fprintf(f, "resource %s %s %s\n", r.cs ? "shader_cs" : "shader_vsps", r.name, r.path[0] ? r.path : "-");
        } else if (r.type == RES_TEXTURE2D) {
            fprintf(f, "resource texture2d %s %s\n", r.name, r.path[0] ? r.path : "-");
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

    fprintf(f, "\ncommands\n");
    for (int i = 0; i < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active) continue;

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
        fprintf(f, "  shadow_shader %s\n", shadow_shader_ref);
        fprintf(f, "  render_state %s %s %s %s %s %s %s\n",
            bool_str(c.color_write), bool_str(c.depth_test), bool_str(c.depth_write),
            bool_str(c.alpha_blend), bool_str(c.cull_back), bool_str(c.shadow_cast), bool_str(c.shadow_receive));
        fprintf(f, "  transform %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
            c.pos[0], c.pos[1], c.pos[2], c.rot[0], c.rot[1], c.rot[2], c.scale[0], c.scale[1], c.scale[2]);
        fprintf(f, "  clear %s %.9g %.9g %.9g %.9g %s %.9g\n",
            bool_str(c.clear_color_enabled), c.clear_color[0], c.clear_color[1], c.clear_color[2], c.clear_color[3],
            bool_str(c.clear_depth), c.depth_clear_val);
        fprintf(f, "  instance %d\n", c.instance_count);
        fprintf(f, "  threads %d %d %d\n", c.thread_x, c.thread_y, c.thread_z);
        char dispatch_ref[MAX_PATH_LEN] = {};
        res_ref(c.dispatch_size_source, dispatch_ref, MAX_PATH_LEN);
        fprintf(f, "  dispatch_from %s\n", dispatch_ref);
        fprintf(f, "  repeat %d %s\n", c.repeat_count, bool_str(c.repeat_expanded));
        Command* parent_cmd = cmd_get(c.parent);
        fprintf(f, "  parent %s\n", parent_cmd ? parent_cmd->name : "-");

        int tex_count = 0;
        for (int t = 0; t < c.tex_count; t++) if (res_get(c.tex_handles[t])) tex_count++;
        fprintf(f, "  textures %d", tex_count);
        for (int t = 0; t < c.tex_count; t++) {
            if (!res_get(c.tex_handles[t])) continue;
            char ref[MAX_PATH_LEN] = {};
            res_ref(c.tex_handles[t], ref, MAX_PATH_LEN);
            fprintf(f, " %s %u", ref, c.tex_slots[t]);
        }
        fprintf(f, "\n");

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
            res_ref(p.source, source_ref, MAX_PATH_LEN);
            fprintf(f, "  param %s %s %s %s %d %d %d %d %.9g %.9g %.9g %.9g\n",
                p.name, res_type_str(p.type), bool_str(p.enabled), source_ref,
                p.ival[0], p.ival[1], p.ival[2], p.ival[3],
                p.fval[0], p.fval[1], p.fval[2], p.fval[3]);
        }
        fprintf(f, "end_command\n");
    }

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

// Parse a saved project file and rebuild the in-memory editor state.
bool project_load_text(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        log_error("Project load failed: %s", path);
        return false;
    }

    project_clear_user_data();
    project_reset_dirlight_defaults();

    char line[1024] = {};
    Command* cur = nullptr;
    CmdHandle cur_h = INVALID_HANDLE;
    CmdHandle pending_parent_cmds[MAX_COMMANDS] = {};
    char pending_parent_names[MAX_COMMANDS][MAX_NAME] = {};
    int pending_parent_count = 0;
    while (fgets(line, sizeof(line), f)) {
        char tmp[1024];
        strncpy(tmp, line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* tag = strtok(tmp, " \t\r\n");
        if (!tag || tag[0] == '#') continue;

        if (strcmp(tag, "camera") == 0) {
            float azimuth = (float)atof(strtok(nullptr, " \t\r\n"));
            float elevation = (float)atof(strtok(nullptr, " \t\r\n"));
            float distance = (float)atof(strtok(nullptr, " \t\r\n"));
            float target[3] = {};
            for (int i = 0; i < 3; i++) target[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            project_apply_legacy_camera(azimuth, elevation, distance, target);
            g_camera.fov_y = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.near_z = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.far_z = (float)atof(strtok(nullptr, " \t\r\n"));
        } else if (strcmp(tag, "camera_fps") == 0) {
            g_camera.position[0] = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.position[1] = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.position[2] = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.yaw = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.pitch = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.fov_y = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.near_z = (float)atof(strtok(nullptr, " \t\r\n"));
            g_camera.far_z = (float)atof(strtok(nullptr, " \t\r\n"));
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
            dl->shadow_distance = shadow_distance_tok ? (float)atof(shadow_distance_tok) : 12.0f;
            dl->shadow_split_lambda = split_lambda_tok ? (float)atof(split_lambda_tok) : 0.65f;
            if (dl->shadow_cascade_count < 1) dl->shadow_cascade_count = 1;
            if (dl->shadow_cascade_count > MAX_SHADOW_CASCADES) dl->shadow_cascade_count = MAX_SHADOW_CASCADES;
            if (dl->shadow_distance < 0.1f) dl->shadow_distance = 0.1f;
            dl->shadow_split_lambda = clampf(dl->shadow_split_lambda, 0.0f, 1.0f);
            project_seed_manual_shadow_cascades(dl, g_camera.near_z);
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
                res_create_structured_buffer(name, stride, count, srv, uav);
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
            } else if (strcmp(tag, "transform") == 0) {
                for (int i = 0; i < 3; i++) cur->pos[i] = (float)atof(strtok(nullptr, " \t\r\n"));
                for (int i = 0; i < 3; i++) cur->rot[i] = (float)atof(strtok(nullptr, " \t\r\n"));
                for (int i = 0; i < 3; i++) cur->scale[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "clear") == 0) {
                cur->clear_color_enabled = atoi(strtok(nullptr, " \t\r\n")) != 0;
                for (int i = 0; i < 4; i++) cur->clear_color[i] = (float)atof(strtok(nullptr, " \t\r\n"));
                cur->clear_depth = atoi(strtok(nullptr, " \t\r\n")) != 0;
                cur->depth_clear_val = (float)atof(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "instance") == 0) {
                cur->instance_count = atoi(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "threads") == 0) {
                cur->thread_x = atoi(strtok(nullptr, " \t\r\n"));
                cur->thread_y = atoi(strtok(nullptr, " \t\r\n"));
                cur->thread_z = atoi(strtok(nullptr, " \t\r\n"));
            } else if (strcmp(tag, "dispatch_from") == 0) {
                cur->dispatch_size_source = res_by_ref(strtok(nullptr, " \t\r\n"), res_lookup_types());
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
                for (int i = 0; i < 4; i++) p->ival[i] = atoi(strtok(nullptr, " \t\r\n"));
                for (int i = 0; i < 4; i++) p->fval[i] = (float)atof(strtok(nullptr, " \t\r\n"));
            }
        }
    }

    for (int i = 0; i < pending_parent_count; i++) {
        Command* child = cmd_get(pending_parent_cmds[i]);
        CmdHandle parent = cmd_find_by_name(pending_parent_names[i]);
        if (child && parent != INVALID_HANDLE && parent != pending_parent_cmds[i])
            child->parent = parent;
    }

    fclose(f);
    project_set_current_path(path);
    dx_invalidate_scene_history();
    log_info("Project loaded: %s", path);
    return true;
}
