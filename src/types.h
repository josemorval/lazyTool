#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// types.h is the shared foundation of the project: limits, handles, enums,
// POD data structures, and lightweight math helpers used by every module.

// ── limits ───────────────────────────────────────────────────────────────
#define MAX_RESOURCES    256
#define MAX_COMMANDS     256
#define MAX_NAME         64
#define MAX_PATH_LEN     256
#define MAX_TEX_SLOTS    8
#define MAX_UAV_SLOTS    8
#define MAX_SRV_SLOTS    8
#define MAX_DRAW_RENDER_TARGETS 4
#define MAX_MESH_MATERIAL_TEXTURES 5
#define MAX_MESH_PARTS 128
#define MAX_MESH_MATERIALS 64
#define MAX_USER_CB_VARS 64
#define MAX_SHADER_CB_VARS 32
#define MAX_SHADER_RESOURCE_BINDINGS 32
#define MAX_COMMAND_PARAMS 32
#define MAX_SHADOW_CASCADES 4
#define INVALID_HANDLE   0u

typedef uint32_t ResHandle;
typedef uint32_t CmdHandle;

// ── enums ─────────────────────────────────────────────────────────────────
typedef enum {
    RES_NONE = 0,
    RES_INT, RES_INT2, RES_INT3,
    RES_FLOAT, RES_FLOAT2, RES_FLOAT3, RES_FLOAT4,
    RES_TEXTURE2D,
    RES_RENDER_TEXTURE2D,
    RES_RENDER_TEXTURE3D,
    RES_STRUCTURED_BUFFER,
    RES_GAUSSIAN_SPLAT,
    RES_MESH,
    RES_SHADER,
    RES_BUILTIN_TIME,
    RES_BUILTIN_SCENE_COLOR,
    RES_BUILTIN_SCENE_DEPTH,
    RES_BUILTIN_SHADOW_MAP,
    RES_BUILTIN_DIRLIGHT,
    RES_COUNT
} ResType;

typedef enum {
    CMD_NONE = 0,
    CMD_CLEAR,
    CMD_GROUP,
    CMD_DRAW_MESH,
    CMD_DRAW_INSTANCED,
    CMD_DISPATCH,
    CMD_INDIRECT_DRAW,
    CMD_INDIRECT_DISPATCH,
    CMD_REPEAT,
    CMD_COUNT
} CmdType;

typedef enum {
    DRAW_SOURCE_MESH = 0,
    DRAW_SOURCE_PROCEDURAL
} DrawSourceType;

typedef enum {
    DRAW_TOPOLOGY_POINT_LIST = 0,
    DRAW_TOPOLOGY_TRIANGLE_LIST
} DrawTopologyType;

typedef enum {
    CAMERA_MODE_HORIZON_LOCKED = 0,
    CAMERA_MODE_FREE
} CameraMode;

typedef enum {
    SHADER_BIND_NONE = 0,
    SHADER_BIND_CBUFFER,
    SHADER_BIND_SRV,
    SHADER_BIND_UAV,
    SHADER_BIND_SAMPLER
} ShaderBindingKind;

typedef enum {
    SHADER_PROGRAM_UNKNOWN = 0,
    SHADER_PROGRAM_VSPS,
    SHADER_PROGRAM_CS
} ShaderProgramKind;

enum {
    SHADER_STAGE_VERTEX  = 1 << 0,
    SHADER_STAGE_PIXEL   = 1 << 1,
    SHADER_STAGE_COMPUTE = 1 << 2
};

typedef enum {
    USER_CB_SOURCE_NONE = 0,
    USER_CB_SOURCE_RESOURCE,
    USER_CB_SOURCE_COMMAND_POSITION,
    USER_CB_SOURCE_COMMAND_ROTATION,
    USER_CB_SOURCE_COMMAND_SCALE,
    USER_CB_SOURCE_CAMERA_POSITION,
    USER_CB_SOURCE_CAMERA_ROTATION,
    USER_CB_SOURCE_DIRLIGHT_POSITION,
    USER_CB_SOURCE_DIRLIGHT_TARGET
} UserCBSourceKind;

struct ShaderCBVar {
    char     name[MAX_NAME];
    ResType  type;
    uint32_t offset;
    uint32_t size;
};

struct ShaderBinding {
    char     name[MAX_NAME];
    uint32_t kind;
    uint32_t bind_slot;
    uint32_t bind_count;
    uint32_t stage_mask;
};

struct ShaderCBLayout {
    bool     active;
    char     name[MAX_NAME];
    uint32_t bind_slot;
    uint32_t size;
    int      var_count;
    // Bumped monotonically every time the layout is rebuilt (shader compile
    // or manual sync). Commands that cache a sync use this to skip the
    // expensive memcpy/memset/strcmp pass when nothing has changed.
    uint32_t layout_version;
    ShaderCBVar vars[MAX_SHADER_CB_VARS];
};

struct CommandParam {
    bool      enabled;
    char      name[MAX_NAME];
    ResType   type;
    ResHandle source;
    UserCBSourceKind source_kind;
    char      source_target[MAX_NAME];
    int       ival[4];
    float     fval[4];
};

struct MeshMaterial {
    char      name[MAX_NAME];
    ResHandle textures[MAX_MESH_MATERIAL_TEXTURES];
    bool      double_sided;
    bool      alpha_blend;
};

struct MeshPart {
    char  name[MAX_NAME];
    int   start_index;
    int   index_count;
    int   material_index;
    bool  enabled;
    bool  bounds_valid;
    float bounds_min[3];
    float bounds_max[3];
    float local_transform[16];
};

// ── resource ─────────────────────────────────────────────────────────────
struct Resource {
    char     name[MAX_NAME];
    ResType  type;
    bool     active;
    bool     is_builtin;
    bool     is_generated;

    int      ival[4];
    float    fval[4];

    ID3D11Texture2D*            tex;
    ID3D11Texture3D*            tex3d;
    ID3D11ShaderResourceView*   srv;
    ID3D11RenderTargetView*     rtv;
    ID3D11UnorderedAccessView*  uav;
    ID3D11DepthStencilView*     dsv;
    ID3D11Buffer*               buf;
    ID3D11Buffer*               vb;
    ID3D11Buffer*               ib;
    ID3D11VertexShader*         vs;
    ID3D11PixelShader*          ps;
    ID3D11ComputeShader*        cs;
    ID3D11InputLayout*          il;

    int         width, height, depth;
    DXGI_FORMAT tex_fmt;
    bool        has_rtv, has_srv, has_uav, has_dsv;
    bool        indirect_args;
    int         scene_scale_divisor;
    ResHandle   generated_from;
    ResHandle   size_handle;

    int  elem_size, elem_count;
    int  splat_sh_degree;
    int  splat_rest_count;
    float splat_bounds_min[3];
    float splat_bounds_max[3];
    float splat_avg_scale;
    int  vert_count, idx_count, vert_stride;
    MeshPart     mesh_parts[MAX_MESH_PARTS];
    int          mesh_part_count;
    MeshMaterial mesh_materials[MAX_MESH_MATERIALS];
    int          mesh_material_count;
    bool         mesh_bounds_valid;
    float        mesh_bounds_min[3];
    float        mesh_bounds_max[3];
    int  mesh_primitive_type;

    char path[MAX_PATH_LEN];
    // Explicit shader program kind used by UI, project serialization, and
    // recompilation. It stays valid even when a path is missing and the
    // runtime shader pointer contains only a fallback object.
    ShaderProgramKind shader_kind;
    bool compiled_ok;
    bool using_fallback;
    char compile_err[512];
    ShaderCBLayout shader_cb;
    bool object_cb_active;
    uint32_t object_cb_bind_slot;
    int shader_binding_count;
    ShaderBinding shader_bindings[MAX_SHADER_RESOURCE_BINDINGS];

    float light_dir[3];
    float light_pos[3];
    float light_target[3];
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
    float shadow_cascade_split[MAX_SHADOW_CASCADES];
    float shadow_cascade_extent[MAX_SHADOW_CASCADES][2];
    float shadow_cascade_near[MAX_SHADOW_CASCADES];
    float shadow_cascade_far[MAX_SHADOW_CASCADES];
};

// ── command ──────────────────────────────────────────────────────────────
struct Command {
    char    name[MAX_NAME];
    CmdType type;
    bool    active;
    bool    enabled;
    CmdHandle parent;
    int     repeat_count;
    bool    repeat_expanded;

    ResHandle rt;
    ResHandle depth;
    ResHandle mrt_handles[MAX_DRAW_RENDER_TARGETS - 1];
    int       mrt_count;
    ResHandle mesh;
    ResHandle shader;
    ResHandle shadow_shader;
    int       draw_source;
    int       draw_topology;
    bool      color_write;
    bool      depth_test;
    bool      depth_write;
    bool      alpha_blend;
    bool      cull_back;
    bool      shadow_cast;
    bool      shadow_receive;
    float     pos[3];
    float     rotq[4];
    float     scale[3];
    bool      bbox_valid;
    bool      bbox_identity;
    float     bbox_min[3];
    float     bbox_max[3];

    ResHandle tex_handles[MAX_TEX_SLOTS];
    uint32_t  tex_slots[MAX_TEX_SLOTS];
    int       tex_count;

    ResHandle uav_handles[MAX_UAV_SLOTS];
    uint32_t  uav_slots[MAX_UAV_SLOTS];
    int       uav_count;

    ResHandle srv_handles[MAX_SRV_SLOTS];
    uint32_t  srv_slots[MAX_SRV_SLOTS];
    int       srv_count;

    float    clear_color[4];
    bool     clear_color_enabled;
    char     clear_color_source[MAX_NAME];
    bool     clear_depth;
    float    depth_clear_val;
    char     clear_depth_source[MAX_NAME];
    int      vertex_count;

    int      instance_count;
    int      thread_x, thread_y, thread_z;
    bool     compute_on_reset;
    // Optional source for dispatch dimensions. When this is set, thread_x/y/z
    // stop meaning "explicit dispatch counts" and become divisors applied to
    // the source size. That lets a command express "dispatch from texture
    // size" without hardcoding scene-dependent counts in the project file.
    ResHandle dispatch_size_source;

    ResHandle indirect_buf;
    uint32_t  indirect_offset;

    CommandParam params[MAX_COMMAND_PARAMS];
    int          param_count;

    // Monotonic edit revision. This is bumped whenever an inspector/project
    // edit changes the command. Render-side caches use it to know when a
    // command-derived packet/list entry needs to be rebuilt.
    uint64_t     version;

    // Version of the shader's cbuffer layout that params[] is synced against.
    // When the shader's layout_version differs, sync needs to run once.
    // Zero means "not yet synced" (default from cmd_alloc memset).
    uint32_t     synced_shader_cb_version;
    ResHandle    synced_shader_handle;
    uint32_t     validation_warning_hash;
};

// ── user cbuffer entry ────────────────────────────────────────────────────
struct UserCBEntry {
    char      name[MAX_NAME];
    ResType   type;
    ResHandle source;
    UserCBSourceKind source_kind;
    char      source_target[MAX_NAME];
    int       ival[4];
    float     fval[4];
};

// ── scene cbuffer (b0, must match shaders/scene.hlsl) ────────────────────
#pragma pack(push, 16)
struct SceneCBData {
    float view_proj[16];
    float time_vec[4];
    float light_dir[4];
    float light_color[4];
    float cam_pos[4];
    float shadow_view_proj[16];
    float inv_view_proj[16];
    float prev_view_proj[16];
    float prev_inv_view_proj[16];
    float prev_shadow_view_proj[16];
    float cam_dir[4];
    float shadow_cascade_splits[4];
    float shadow_params[4];
    float shadow_cascade_rects[MAX_SHADOW_CASCADES][4];
    float shadow_cascade_view_proj[MAX_SHADOW_CASCADES][16];
};

struct UserCBData {
    float slots[MAX_USER_CB_VARS][4];
};

struct ObjectCBData {
    float world[16];
};
#pragma pack(pop)

// ── camera ────────────────────────────────────────────────────────────────
struct Camera {
    float position[3];
    // yaw/pitch/roll are kept for UI, save files, and timeline tracks.
    // rotq is the canonical viewport orientation (local +X right, +Y up, +Z forward).
    float yaw;
    float pitch;
    float roll;
    float rotq[4];
    float fov_y;
    float near_z, far_z;
};

struct CameraControls {
    bool  enabled;
    bool  mouse_look;
    bool  invert_y;
    int   mode;
    float move_speed;
    float fast_mult;
    float slow_mult;
    float mouse_sensitivity;
};

// ── math ──────────────────────────────────────────────────────────────────
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Vec3 { float x, y, z; };
struct Mat4 { float m[16]; };
struct Quat { float x, y, z, w; };

inline Vec3 v3(float x, float y, float z)         { return {x, y, z}; }
inline Vec3 v3_add(Vec3 a, Vec3 b)                { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 v3_sub(Vec3 a, Vec3 b)                { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 v3_scale(Vec3 a, float s)             { return {a.x*s, a.y*s, a.z*s}; }
inline float v3_dot(Vec3 a, Vec3 b)               { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 v3_cross(Vec3 a, Vec3 b)              { return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }
inline Vec3 v3_norm(Vec3 a) {
    float l = sqrtf(v3_dot(a, a));
    return l > 1e-6f ? v3_scale(a, 1.f / l) : v3(0, 0, 1);
}

Mat4 mat4_identity();
Mat4 mat4_transpose(const Mat4& m);
Mat4 mat4_mul(const Mat4& a, const Mat4& b);
Mat4 mat4_inverse(const Mat4& m);
Mat4 mat4_lookat(Vec3 eye, Vec3 at, Vec3 up);
Mat4 mat4_perspective(float fov_y, float aspect, float near_z, float far_z);
Mat4 mat4_orthographic(float width, float height, float near_z, float far_z);
Mat4 mat4_translation(Vec3 t);
Mat4 mat4_scale(Vec3 s);
Mat4 mat4_rotation_xyz(Vec3 r);
Mat4 mat4_rotation_quat(Quat q);
Quat quat_identity();
Quat quat_normalize(Quat q);
Quat quat_from_array(const float q[4]);
void quat_to_array(Quat q, float out[4]);
Quat quat_from_mat4(const Mat4& m);
Quat quat_from_euler_xyz(Vec3 r);
Quat quat_mul(Quat a, Quat b);
Quat quat_from_axis_angle(Vec3 axis, float angle);
Vec3 quat_rotate_vec3(Quat q, Vec3 v);
Quat quat_slerp(Quat a, Quat b, float t);
void quat_to_euler_xyz(Quat q, const float* reference, float out[3]);
Vec3 camera_eye(const Camera& c);
Vec3 camera_forward(const Camera& c);
Vec3 camera_right(const Camera& c);
Vec3 camera_up(const Camera& c);
void camera_set_euler(Camera* c, float yaw, float pitch, float roll);
void camera_sync_euler_from_quat(Camera* c);
void camera_ensure_orientation(Camera* c);
