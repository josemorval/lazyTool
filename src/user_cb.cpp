#include "user_cb.h"
#include "resources.h"
#include "commands.h"
#include "dx11_ctx.h"
#include "log.h"
#include "ui.h"
#include "timeline.h"
#include <string.h>
#include <stdint.h>

// user_cb.cpp manages the user-defined constant buffer bound at b1. It lets
// editor variables flow into shaders every frame without manual packing.

UserCBEntry   g_user_cb_entries[MAX_USER_CB_VARS] = {};
int           g_user_cb_count = 0;
ID3D11Buffer* g_user_cb_buf   = nullptr;

#ifdef LAZYTOOL_NO_USER_CB
void user_cb_init() {}
void user_cb_shutdown() {}
void user_cb_clear() {}
void user_cb_update() {}
void user_cb_bind() {}
void user_cb_sync_command_params(Command* c, const Resource*) {
    if (c)
        c->param_count = 0;
}
void user_cb_bind_for_command(Command*, ResHandle, const Resource*, bool, bool, bool) {}
void user_cb_enforce_unique_names() {}
bool user_cb_type_supported(ResType) { return false; }
bool user_cb_add_var(const char*, ResType) { return false; }
bool user_cb_add_from_resource(ResHandle) { return false; }
int user_cb_find(const char*) { return -1; }
const UserCBEntry* user_cb_get(const char*) { return nullptr; }
bool user_cb_rename(int, const char*) { return false; }
bool user_cb_set_source(int, ResHandle) { return false; }
bool user_cb_set_scene_source(int, UserCBSourceKind, const char*) { return false; }
void user_cb_refresh_entry(int) {}
const char* user_cb_source_kind_token(UserCBSourceKind) { return "none"; }
UserCBSourceKind user_cb_source_kind_from_token(const char*) { return USER_CB_SOURCE_NONE; }
void user_cb_detach_resource(ResHandle) {}
void user_cb_rename_command_references(const char*, const char*) {}
void user_cb_rename_variable_references(const char*, const char*) {}
void user_cb_delete_variable_references(const char*) {}
void user_cb_rename_resource_references(ResHandle, const char*, const char*) {}
void user_cb_remove(int) {}
void user_cb_move(int, int) {}
int user_cb_slot_offset(int idx) { return idx * 16; }
#else
static ID3D11Buffer* s_command_cb_buf = nullptr;
static UserCBData     s_uploaded_global_user_cb = {};
static UserCBData     s_uploaded_command_user_cb = {};
static ID3D11Buffer*  s_uploaded_global_user_cb_buffer = nullptr;
static ID3D11Buffer*  s_uploaded_command_user_cb_buffer = nullptr;
static bool           s_uploaded_global_user_cb_valid = false;
static bool           s_uploaded_command_user_cb_valid = false;

enum UserCBPlanSource : uint8_t {
    USER_CB_PLAN_GLOBAL = 0,
    USER_CB_PLAN_PARAM  = 1,
};

struct UserCBPackOp {
    UserCBPlanSource source;
    int              index;
    ResType          type;
    uint32_t         offset;
    uint32_t         size;
};

static const int MAX_USER_CB_PLAN_OPS = MAX_SHADER_CB_VARS * 2;

struct UserCBCommandPackPlan {
    bool            valid;
    const Resource* shader;
    uint32_t        shader_layout_version;
    uint64_t        command_version;
    uint64_t        global_layout_revision;
    int             param_count;
    int             op_count;
    UserCBPackOp    ops[MAX_USER_CB_PLAN_OPS];
};

static UserCBCommandPackPlan s_command_pack_plans[MAX_COMMANDS] = {};
static uint64_t              s_user_cb_layout_revision = 1;

static void user_cb_invalidate_command_pack_plans() {
    for (int i = 0; i < MAX_COMMANDS; i++)
        s_command_pack_plans[i].valid = false;
}

static void user_cb_bump_layout_revision() {
    ++s_user_cb_layout_revision;
    user_cb_invalidate_command_pack_plans();
}

static int user_cb_command_index(const Command* c) {
    if (!c)
        return -1;
    if (c < g_commands || c >= g_commands + MAX_COMMANDS)
        return -1;
    return (int)(c - g_commands);
}

static bool user_cb_name_exists_except(const char* name, int except_idx) {
    if (!name || !name[0])
        return false;
    for (int i = 0; i < g_user_cb_count; i++) {
        if (i == except_idx)
            continue;
        if (strcmp(g_user_cb_entries[i].name, name) == 0)
            return true;
    }
    return false;
}

static bool user_cb_name_exists_before(const char* name, int before_idx) {
    if (!name || !name[0])
        return false;
    if (before_idx > g_user_cb_count)
        before_idx = g_user_cb_count;
    for (int i = 0; i < before_idx; i++) {
        if (strcmp(g_user_cb_entries[i].name, name) == 0)
            return true;
    }
    return false;
}

static bool user_cb_name_exists(const char* name) {
    return user_cb_name_exists_except(name, -1);
}

static int user_cb_find_global_index(const char* name, ResType type) {
    if (!name)
        return -1;
    for (int i = 0; i < g_user_cb_count; i++) {
        if (g_user_cb_entries[i].type != type)
            continue;
        if (strcmp(g_user_cb_entries[i].name, name) == 0)
            return i;
    }
    return -1;
}

static UserCBEntry* user_cb_find_global_entry(const char* name, ResType type) {
    int idx = user_cb_find_global_index(name, type);
    return idx >= 0 ? &g_user_cb_entries[idx] : nullptr;
}

static void user_cb_make_unique_name_except(const char* base, char* out, int out_sz, int except_idx) {
    const char* src = (base && base[0]) ? base : "var";
    if (!user_cb_name_exists_except(src, except_idx)) {
        strncpy(out, src, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    for (int i = 1; i < 1000; i++) {
        snprintf(out, out_sz, "%s_%d", src, i);
        if (!user_cb_name_exists_except(out, except_idx))
            return;
    }

    strncpy(out, src, out_sz - 1);
    out[out_sz - 1] = '\0';
}

static void user_cb_make_unique_name(const char* base, char* out, int out_sz) {
    user_cb_make_unique_name_except(base, out, out_sz, -1);
}

static void user_cb_copy_from_resource(UserCBEntry* e, const Resource* r) {
    if (!e || !r || e->type != r->type) return;

    switch (e->type) {
    case RES_FLOAT:
    case RES_FLOAT2:
    case RES_FLOAT3:
    case RES_FLOAT4:
        memcpy(e->fval, r->fval, sizeof(e->fval));
        break;
    case RES_INT:
    case RES_INT2:
    case RES_INT3:
        memcpy(e->ival, r->ival, sizeof(e->ival));
        break;
    default:
        break;
    }
}

static bool user_cb_scene_source_type_supported(ResType type) {
    return type == RES_FLOAT3 || type == RES_FLOAT4;
}

static bool user_cb_source_kind_is_command(UserCBSourceKind kind) {
    return kind == USER_CB_SOURCE_COMMAND_POSITION ||
           kind == USER_CB_SOURCE_COMMAND_ROTATION ||
           kind == USER_CB_SOURCE_COMMAND_SCALE;
}

const char* user_cb_source_kind_token(UserCBSourceKind kind) {
    switch (kind) {
    case USER_CB_SOURCE_RESOURCE:         return "resource";
    case USER_CB_SOURCE_COMMAND_POSITION: return "cmd_pos";
    case USER_CB_SOURCE_COMMAND_ROTATION: return "cmd_rot";
    case USER_CB_SOURCE_COMMAND_SCALE:    return "cmd_scale";
    case USER_CB_SOURCE_CAMERA_POSITION:  return "camera_pos";
    case USER_CB_SOURCE_CAMERA_ROTATION:  return "camera_rot";
    case USER_CB_SOURCE_DIRLIGHT_POSITION:return "dirlight_pos";
    case USER_CB_SOURCE_DIRLIGHT_TARGET:  return "dirlight_target";
    default:                             return "none";
    }
}

UserCBSourceKind user_cb_source_kind_from_token(const char* token) {
    if (!token) return USER_CB_SOURCE_NONE;
    if (strcmp(token, "resource") == 0)        return USER_CB_SOURCE_RESOURCE;
    if (strcmp(token, "cmd_pos") == 0)         return USER_CB_SOURCE_COMMAND_POSITION;
    if (strcmp(token, "cmd_rot") == 0)         return USER_CB_SOURCE_COMMAND_ROTATION;
    if (strcmp(token, "cmd_scale") == 0)       return USER_CB_SOURCE_COMMAND_SCALE;
    if (strcmp(token, "camera_pos") == 0)      return USER_CB_SOURCE_CAMERA_POSITION;
    if (strcmp(token, "camera_rot") == 0)      return USER_CB_SOURCE_CAMERA_ROTATION;
    if (strcmp(token, "dirlight_pos") == 0)    return USER_CB_SOURCE_DIRLIGHT_POSITION;
    if (strcmp(token, "dirlight_target") == 0) return USER_CB_SOURCE_DIRLIGHT_TARGET;
    return USER_CB_SOURCE_NONE;
}

static void user_cb_clear_source(UserCBEntry* e) {
    if (!e)
        return;
    e->source = INVALID_HANDLE;
    e->source_kind = USER_CB_SOURCE_NONE;
    e->source_target[0] = '\0';
}

static void user_cb_assign_float_source(UserCBEntry* e, const float* v, float w) {
    if (!e || !v)
        return;
    e->fval[0] = v[0];
    e->fval[1] = v[1];
    e->fval[2] = v[2];
    e->fval[3] = w;
}

static void user_cb_assign_camera_rotation(UserCBEntry* e) {
    if (!e)
        return;
    e->fval[0] = g_camera.yaw;
    e->fval[1] = g_camera.pitch;
    e->fval[2] = g_camera.roll;
    e->fval[3] = 0.0f;
}

static bool user_cb_read_scene_source(UserCBSourceKind kind, const char* target, ResType type, float* out) {
    if (!out || !user_cb_scene_source_type_supported(type))
        return false;

    switch (kind) {
    case USER_CB_SOURCE_COMMAND_POSITION:
    case USER_CB_SOURCE_COMMAND_ROTATION:
    case USER_CB_SOURCE_COMMAND_SCALE: {
        Command* c = cmd_get(cmd_find_by_name(target ? target : ""));
        if (!c)
            return false;
        if (kind == USER_CB_SOURCE_COMMAND_ROTATION) {
            if (type == RES_FLOAT4) {
                quat_to_array(quat_from_array(c->rotq), out);
            } else {
                quat_to_euler_xyz(quat_from_array(c->rotq), nullptr, out);
                out[3] = 0.0f;
            }
            return true;
        }
        const float* src = kind == USER_CB_SOURCE_COMMAND_POSITION ? c->pos :
                           c->scale;
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
        out[3] = 1.0f;
        return true;
    }
    case USER_CB_SOURCE_CAMERA_POSITION:
        out[0] = g_camera.position[0];
        out[1] = g_camera.position[1];
        out[2] = g_camera.position[2];
        out[3] = 1.0f;
        return true;
    case USER_CB_SOURCE_CAMERA_ROTATION:
        out[0] = g_camera.yaw;
        out[1] = g_camera.pitch;
        out[2] = g_camera.roll;
        out[3] = 0.0f;
        return true;
    case USER_CB_SOURCE_DIRLIGHT_POSITION: {
        Resource* dl = res_get(g_builtin_dirlight);
        if (!dl)
            return false;
        out[0] = dl->light_pos[0];
        out[1] = dl->light_pos[1];
        out[2] = dl->light_pos[2];
        out[3] = 1.0f;
        return true;
    }
    case USER_CB_SOURCE_DIRLIGHT_TARGET: {
        Resource* dl = res_get(g_builtin_dirlight);
        if (!dl)
            return false;
        out[0] = dl->light_target[0];
        out[1] = dl->light_target[1];
        out[2] = dl->light_target[2];
        out[3] = 1.0f;
        return true;
    }
    default:
        return false;
    }
}

static bool user_cb_refresh_scene_source(UserCBEntry* e) {
    if (!e)
        return false;
    if (!user_cb_read_scene_source(e->source_kind, e->source_target, e->type, e->fval))
        return false;
    return true;
}

static void command_param_copy_from_resource(CommandParam* p, const Resource* r) {
    if (!p || !r || p->type != r->type) return;

    switch (p->type) {
    case RES_FLOAT:
    case RES_FLOAT2:
    case RES_FLOAT3:
    case RES_FLOAT4:
        memcpy(p->fval, r->fval, sizeof(p->fval));
        break;
    case RES_INT:
    case RES_INT2:
    case RES_INT3:
        memcpy(p->ival, r->ival, sizeof(p->ival));
        break;
    default:
        break;
    }
}

static void user_cb_pack_entry(const UserCBEntry* e, float* slot) {
    switch (e->type) {
    case RES_FLOAT:  slot[0] = e->fval[0]; break;
    case RES_FLOAT2: slot[0] = e->fval[0]; slot[1] = e->fval[1]; break;
    case RES_FLOAT3: slot[0] = e->fval[0]; slot[1] = e->fval[1]; slot[2] = e->fval[2]; break;
    case RES_FLOAT4: slot[0] = e->fval[0]; slot[1] = e->fval[1]; slot[2] = e->fval[2]; slot[3] = e->fval[3]; break;
    case RES_INT:    memcpy(slot, &e->ival[0], sizeof(int));     break;
    case RES_INT2:   memcpy(slot, e->ival,     sizeof(int) * 2); break;
    case RES_INT3:   memcpy(slot, e->ival,     sizeof(int) * 3); break;
    default: break;
    }
}

static void user_cb_refresh_source(UserCBEntry* e) {
    if (!e) return;

    if (e->source_kind == USER_CB_SOURCE_NONE && e->source != INVALID_HANDLE)
        e->source_kind = USER_CB_SOURCE_RESOURCE;

    if (e->source_kind == USER_CB_SOURCE_NONE)
        return;

    if (e->source_kind != USER_CB_SOURCE_RESOURCE) {
        if (!user_cb_refresh_scene_source(e)) {
            log_warn("UserCB: detached missing scene source from '%s'", e->name);
            user_cb_clear_source(e);
        }
        return;
    }

    if (e->source == INVALID_HANDLE) {
        e->source_kind = USER_CB_SOURCE_NONE;
        return;
    }

    Resource* r = res_get(e->source);
    if (!r || !user_cb_type_supported(r->type) || r->type != e->type) {
        log_warn("UserCB: detached missing source from '%s'", e->name);
        user_cb_clear_source(e);
        return;
    }

    user_cb_copy_from_resource(e, r);
}

void user_cb_refresh_entry(int idx) {
    if (idx < 0 || idx >= g_user_cb_count)
        return;
    user_cb_refresh_source(&g_user_cb_entries[idx]);
}

void user_cb_init() {
    memset(g_user_cb_entries, 0, sizeof(g_user_cb_entries));
    g_user_cb_count = 0;
    s_user_cb_layout_revision = 1;
    user_cb_invalidate_command_pack_plans();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth      = sizeof(UserCBData);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx.dev->CreateBuffer(&bd, nullptr, &g_user_cb_buf);
    g_dx.dev->CreateBuffer(&bd, nullptr, &s_command_cb_buf);
    s_uploaded_global_user_cb_valid = false;
    s_uploaded_command_user_cb_valid = false;
    s_uploaded_global_user_cb_buffer = nullptr;
    s_uploaded_command_user_cb_buffer = nullptr;
    log_info("UserCB init (preferred UserCB = b2, %d slots x 16 bytes)", MAX_USER_CB_VARS);
}

void user_cb_shutdown() {
    if (g_user_cb_buf) { g_user_cb_buf->Release(); g_user_cb_buf = nullptr; }
    if (s_command_cb_buf) { s_command_cb_buf->Release(); s_command_cb_buf = nullptr; }
    s_uploaded_global_user_cb_valid = false;
    s_uploaded_command_user_cb_valid = false;
    s_uploaded_global_user_cb_buffer = nullptr;
    s_uploaded_command_user_cb_buffer = nullptr;
    user_cb_invalidate_command_pack_plans();
}

void user_cb_clear() {
    memset(g_user_cb_entries, 0, sizeof(g_user_cb_entries));
    g_user_cb_count = 0;
    s_uploaded_global_user_cb_valid = false;
    s_uploaded_command_user_cb_valid = false;
    user_cb_bump_layout_revision();
}

void user_cb_enforce_unique_names() {
    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry& e = g_user_cb_entries[i];
        if (e.name[0] && !user_cb_name_exists_before(e.name, i))
            continue;

        char old_name[MAX_NAME] = {};
        strncpy(old_name, e.name, MAX_NAME - 1);
        old_name[MAX_NAME - 1] = '\0';

        char next_name[MAX_NAME] = {};
        user_cb_make_unique_name_except(old_name[0] ? old_name : "var", next_name, MAX_NAME, i);
        if (!next_name[0] || strcmp(old_name, next_name) == 0)
            continue;

        strncpy(e.name, next_name, MAX_NAME - 1);
        e.name[MAX_NAME - 1] = '\0';

        if (old_name[0] && !user_cb_name_exists_before(old_name, i))
            user_cb_rename_variable_references(old_name, e.name);

        user_cb_bump_layout_revision();
        log_warn("UserCB: renamed duplicate variable '%s' -> '%s'",
                 old_name[0] ? old_name : "(empty)", e.name);
        app_request_scene_render();
    }
}

// Pack the latest user variable values into the shared GPU buffer.
void user_cb_update() {
    user_cb_enforce_unique_names();
    if (!g_user_cb_buf) return;

    UserCBData data = {};
    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry* e = &g_user_cb_entries[i];
        user_cb_refresh_source(e);
        user_cb_pack_entry(e, data.slots[i]);
    }

    bool same_buffer = s_uploaded_global_user_cb_buffer == g_user_cb_buf;
    bool same_bytes = s_uploaded_global_user_cb_valid && same_buffer &&
                      memcmp(&s_uploaded_global_user_cb, &data, sizeof(data)) == 0;
    if (!same_bytes) {
        D3D11_MAPPED_SUBRESOURCE ms = {};
        g_dx.ctx->Map(g_user_cb_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        memcpy(ms.pData, &data, sizeof(data));
        g_dx.ctx->Unmap(g_user_cb_buf, 0);
        s_uploaded_global_user_cb = data;
        s_uploaded_global_user_cb_buffer = g_user_cb_buf;
        s_uploaded_global_user_cb_valid = true;
    }
}

void user_cb_bind() {
    // Intentionally empty. User CB binding is now resolved per shader from the
    // reflected UserCB slot, which allows old b1 shaders and new b2 shaders to
    // coexist during the migration.
}

static int command_param_find(const Command* c, const char* name) {
    if (!c || !name) return -1;
    for (int i = 0; i < c->param_count; i++)
        if (strcmp(c->params[i].name, name) == 0)
            return i;
    return -1;
}

// Keep per-command parameter editors aligned with the reflected user CB
// layout while preserving matching values across shader recompiles.
void user_cb_sync_command_params(Command* c, const Resource* shader) {
    if (!c) return;
    if (!shader || !shader->shader_cb.active) {
        c->param_count = 0;
        c->synced_shader_cb_version = 0;
        c->synced_shader_handle = INVALID_HANDLE;
        return;
    }

    CommandParam old[MAX_COMMAND_PARAMS] = {};
    int old_count = c->param_count;
    if (old_count > MAX_COMMAND_PARAMS) old_count = MAX_COMMAND_PARAMS;
    memcpy(old, c->params, sizeof(CommandParam) * old_count);

    memset(c->params, 0, sizeof(c->params));
    c->param_count = 0;

    for (int i = 0; i < shader->shader_cb.var_count && c->param_count < MAX_COMMAND_PARAMS; i++) {
        const ShaderCBVar& v = shader->shader_cb.vars[i];
        CommandParam* p = &c->params[c->param_count++];
        memset(p, 0, sizeof(*p));
        p->enabled = true;
        p->type = v.type;
        p->source = INVALID_HANDLE;
        strncpy(p->name, v.name, MAX_NAME - 1);
        p->name[MAX_NAME - 1] = '\0';

        if (UserCBEntry* global = user_cb_find_global_entry(v.name, v.type)) {
            user_cb_refresh_source(global);
            p->source_kind = global->source_kind;
            p->source = global->source_kind == USER_CB_SOURCE_RESOURCE ? global->source : INVALID_HANDLE;
            strncpy(p->source_target, global->source_target, MAX_NAME - 1);
            p->source_target[MAX_NAME - 1] = '\0';
            memcpy(p->ival, global->ival, sizeof(p->ival));
            memcpy(p->fval, global->fval, sizeof(p->fval));
        }

        for (int old_i = 0; old_i < old_count; old_i++) {
            if (strcmp(old[old_i].name, v.name) != 0 || old[old_i].type != v.type)
                continue;
            *p = old[old_i];
            strncpy(p->name, v.name, MAX_NAME - 1);
            p->name[MAX_NAME - 1] = '\0';
            p->type = v.type;
            if (p->source_kind == USER_CB_SOURCE_NONE && p->source != INVALID_HANDLE)
                p->source_kind = USER_CB_SOURCE_RESOURCE;
            Resource* src = p->source_kind == USER_CB_SOURCE_RESOURCE ? res_get(p->source) : nullptr;
            if (p->source_kind == USER_CB_SOURCE_RESOURCE && (!src || src->type != p->type)) {
                p->source = INVALID_HANDLE;
                p->source_kind = USER_CB_SOURCE_NONE;
            }
            break;
        }
    }

    // Remember what we synced against so we can skip the work next frame.
    c->synced_shader_cb_version = shader->shader_cb.layout_version;
    c->synced_shader_handle = c->shader;
}

static void pack_value_bytes_direct(ResType type, const int* ival, const float* fval,
                                    uint32_t offset, uint32_t reflected_size,
                                    unsigned char* bytes, int byte_count) {
    if (!bytes) return;
    if ((int)offset >= byte_count) return;

    int max_copy = byte_count - (int)offset;
    if (max_copy <= 0) return;

    int bytes_to_copy = 0;
    const void* src_data = nullptr;
    switch (type) {
    case RES_FLOAT:  bytes_to_copy = 4;  src_data = fval; break;
    case RES_FLOAT2: bytes_to_copy = 8;  src_data = fval; break;
    case RES_FLOAT3: bytes_to_copy = 12; src_data = fval; break;
    case RES_FLOAT4: bytes_to_copy = 16; src_data = fval; break;
    case RES_INT:    bytes_to_copy = 4;  src_data = ival; break;
    case RES_INT2:   bytes_to_copy = 8;  src_data = ival; break;
    case RES_INT3:   bytes_to_copy = 12; src_data = ival; break;
    default: break;
    }

    if (!src_data || bytes_to_copy <= 0) return;
    if (bytes_to_copy > (int)reflected_size) bytes_to_copy = (int)reflected_size;
    if (bytes_to_copy > max_copy) bytes_to_copy = max_copy;
    memcpy(bytes + offset, src_data, bytes_to_copy);
}

static void pack_value_bytes(ResType type, const int* ival, const float* fval,
                             const ShaderCBVar* v, unsigned char* bytes, int byte_count) {
    if (!v) return;
    pack_value_bytes_direct(type, ival, fval, v->offset, v->size, bytes, byte_count);
}

static void pack_command_param_bytes_direct(const CommandParam* p, uint32_t offset, uint32_t reflected_size,
                                            unsigned char* bytes, int byte_count) {
    if (!p || !bytes || !p->enabled) return;

    const int* ival = p->ival;
    const float* fval = p->fval;
    UserCBSourceKind source_kind = p->source_kind;
    if (source_kind == USER_CB_SOURCE_NONE && p->source != INVALID_HANDLE)
        source_kind = USER_CB_SOURCE_RESOURCE;
    Resource* src = source_kind == USER_CB_SOURCE_RESOURCE ? res_get(p->source) : nullptr;
    float scene_fval[4] = {};
    if (src && src->type == p->type) {
        ival = src->ival;
        fval = src->fval;
    } else if (source_kind != USER_CB_SOURCE_NONE && source_kind != USER_CB_SOURCE_RESOURCE &&
               user_cb_read_scene_source(source_kind, p->source_target, p->type, scene_fval)) {
        fval = scene_fval;
    }

    pack_value_bytes_direct(p->type, ival, fval, offset, reflected_size, bytes, byte_count);
}

static void pack_command_param_bytes(const CommandParam* p, const ShaderCBVar* v, unsigned char* bytes, int byte_count) {
    if (!v) return;
    pack_command_param_bytes_direct(p, v->offset, v->size, bytes, byte_count);
}

static void user_cb_pack_command_fallback(Command* c, const Resource* shader, unsigned char* bytes, int cb_size) {
    if (!shader || !shader->shader_cb.active || !bytes)
        return;

    for (int i = 0; i < shader->shader_cb.var_count; i++) {
        const ShaderCBVar& v = shader->shader_cb.vars[i];
        UserCBEntry* global = user_cb_find_global_entry(v.name, v.type);
        if (global) {
            user_cb_refresh_source(global);
            pack_value_bytes(global->type, global->ival, global->fval, &v, bytes, cb_size);
        }
        int p_i = command_param_find(c, v.name);
        if (p_i < 0) continue;
        pack_command_param_bytes(&c->params[p_i], &v, bytes, cb_size);
    }
}

static void user_cb_build_command_pack_plan(UserCBCommandPackPlan* plan, Command* c, const Resource* shader) {
    if (!plan)
        return;

    memset(plan, 0, sizeof(*plan));
    if (!c || !shader || !shader->shader_cb.active)
        return;

    plan->valid = true;
    plan->shader = shader;
    plan->shader_layout_version = shader->shader_cb.layout_version;
    plan->command_version = c->version;
    plan->global_layout_revision = s_user_cb_layout_revision;
    plan->param_count = c->param_count;

    for (int i = 0; i < shader->shader_cb.var_count; i++) {
        const ShaderCBVar& v = shader->shader_cb.vars[i];
        int global_i = user_cb_find_global_index(v.name, v.type);
        if (global_i >= 0 && plan->op_count < MAX_USER_CB_PLAN_OPS) {
            UserCBPackOp& op = plan->ops[plan->op_count++];
            op.source = USER_CB_PLAN_GLOBAL;
            op.index  = global_i;
            op.type   = v.type;
            op.offset = v.offset;
            op.size   = v.size;
        }

        int param_i = command_param_find(c, v.name);
        if (param_i >= 0 && param_i < c->param_count &&
            c->params[param_i].type == v.type && plan->op_count < MAX_USER_CB_PLAN_OPS) {
            UserCBPackOp& op = plan->ops[plan->op_count++];
            op.source = USER_CB_PLAN_PARAM;
            op.index  = param_i;
            op.type   = v.type;
            op.offset = v.offset;
            op.size   = v.size;
        }
    }
}

static bool user_cb_command_pack_plan_matches(const UserCBCommandPackPlan* plan, const Command* c, const Resource* shader) {
    if (!plan || !plan->valid || !c || !shader)
        return false;
    return plan->shader == shader &&
           plan->shader_layout_version == shader->shader_cb.layout_version &&
           plan->command_version == c->version &&
           plan->global_layout_revision == s_user_cb_layout_revision &&
           plan->param_count == c->param_count;
}

static void user_cb_pack_from_plan(const UserCBCommandPackPlan* plan, Command* c, unsigned char* bytes, int cb_size) {
    if (!plan || !c || !bytes)
        return;

    for (int i = 0; i < plan->op_count; i++) {
        const UserCBPackOp& op = plan->ops[i];
        if (op.source == USER_CB_PLAN_GLOBAL) {
            if (op.index < 0 || op.index >= g_user_cb_count)
                continue;
            UserCBEntry* e = &g_user_cb_entries[op.index];
            if (e->type != op.type)
                continue;
            user_cb_refresh_source(e);
            pack_value_bytes_direct(e->type, e->ival, e->fval, op.offset, op.size, bytes, cb_size);
        } else {
            if (op.index < 0 || op.index >= c->param_count)
                continue;
            CommandParam* p = &c->params[op.index];
            if (p->type != op.type)
                continue;
            pack_command_param_bytes_direct(p, op.offset, op.size, bytes, cb_size);
        }
    }
}

void user_cb_bind_for_command(Command* c, ResHandle shader_handle, const Resource* shader, bool bind_vs, bool bind_ps, bool bind_cs) {
    if (!shader || !shader->shader_cb.active) {
        return;
    }

    if (!s_command_cb_buf)
        return;

    // Sync only when the reflected layout changes. Per-frame packing below uses
    // a cached plan of byte copies, so the hot path no longer searches UserCB
    // globals/command params by string for every reflected variable.
    bool need_sync = !c ||
                     c->synced_shader_cb_version != shader->shader_cb.layout_version ||
                     c->synced_shader_handle != shader_handle;
    if (need_sync) {
        user_cb_sync_command_params(c, shader);
        if (c)
            c->synced_shader_handle = shader_handle;
    }

    UserCBData data = {};
    int cb_size = (int)shader->shader_cb.size;
    if (cb_size <= 0) cb_size = sizeof(data);
    if (cb_size > (int)sizeof(data)) cb_size = sizeof(data);

    int plan_idx = user_cb_command_index(c);
    if (plan_idx >= 0) {
        UserCBCommandPackPlan* plan = &s_command_pack_plans[plan_idx];
        if (!user_cb_command_pack_plan_matches(plan, c, shader))
            user_cb_build_command_pack_plan(plan, c, shader);
        user_cb_pack_from_plan(plan, c, (unsigned char*)&data, cb_size);
    } else {
        user_cb_pack_command_fallback(c, shader, (unsigned char*)&data, cb_size);
    }

    bool same_buffer = s_uploaded_command_user_cb_buffer == s_command_cb_buf;
    bool same_bytes = s_uploaded_command_user_cb_valid && same_buffer &&
                      memcmp(&s_uploaded_command_user_cb, &data, sizeof(data)) == 0;
    if (!same_bytes) {
        D3D11_MAPPED_SUBRESOURCE ms = {};
        g_dx.ctx->Map(s_command_cb_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        memcpy(ms.pData, &data, sizeof(data));
        g_dx.ctx->Unmap(s_command_cb_buf, 0);
        s_uploaded_command_user_cb = data;
        s_uploaded_command_user_cb_buffer = s_command_cb_buf;
        s_uploaded_command_user_cb_valid = true;
    }

    UINT slot = shader->shader_cb.bind_slot;
    if (bind_vs) g_dx.ctx->VSSetConstantBuffers(slot, 1, &s_command_cb_buf);
    if (bind_ps) g_dx.ctx->PSSetConstantBuffers(slot, 1, &s_command_cb_buf);
    if (bind_cs) g_dx.ctx->CSSetConstantBuffers(slot, 1, &s_command_cb_buf);
}

int user_cb_find(const char* name) {
    if (!name || !name[0])
        return -1;
    for (int i = 0; i < g_user_cb_count; i++) {
        if (strcmp(g_user_cb_entries[i].name, name) == 0)
            return i;
    }
    return -1;
}

const UserCBEntry* user_cb_get(const char* name) {
    int idx = user_cb_find(name);
    return idx >= 0 ? &g_user_cb_entries[idx] : nullptr;
}

static bool user_cb_clear_source_is_resource(const char* source_name, ResType type) {
    Resource* r = res_get(res_find_by_name(source_name));
    return r && r->type == type;
}

void user_cb_rename_variable_references(const char* old_name, const char* new_name) {
    if (!old_name || !old_name[0] || !new_name || !new_name[0] ||
        strcmp(old_name, new_name) == 0)
        return;

    timeline_rename_tracks_for_user_var(old_name, new_name);
    for (int c_i = 0; c_i < MAX_COMMANDS; c_i++) {
        Command& c = g_commands[c_i];
        if (!c.active)
            continue;
        if (strcmp(c.clear_color_source, old_name) == 0 &&
            !user_cb_clear_source_is_resource(old_name, RES_FLOAT4)) {
            strncpy(c.clear_color_source, new_name, MAX_NAME - 1);
            c.clear_color_source[MAX_NAME - 1] = '\0';
        }
        if (strcmp(c.clear_depth_source, old_name) == 0 &&
            !user_cb_clear_source_is_resource(old_name, RES_FLOAT)) {
            strncpy(c.clear_depth_source, new_name, MAX_NAME - 1);
            c.clear_depth_source[MAX_NAME - 1] = '\0';
        }
    }
}

void user_cb_delete_variable_references(const char* name) {
    if (!name || !name[0])
        return;

    timeline_delete_tracks_for_user_var(name);
    for (int c_i = 0; c_i < MAX_COMMANDS; c_i++) {
        Command& c = g_commands[c_i];
        if (!c.active)
            continue;
        if (strcmp(c.clear_color_source, name) == 0 &&
            !user_cb_clear_source_is_resource(name, RES_FLOAT4))
            c.clear_color_source[0] = '\0';
        if (strcmp(c.clear_depth_source, name) == 0 &&
            !user_cb_clear_source_is_resource(name, RES_FLOAT))
            c.clear_depth_source[0] = '\0';
    }
}

void user_cb_rename_resource_references(ResHandle h, const char* old_name, const char* new_name) {
    if (h == INVALID_HANDLE || !old_name || !old_name[0] || !new_name || !new_name[0] ||
        strcmp(old_name, new_name) == 0)
        return;

    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry& e = g_user_cb_entries[i];
        UserCBSourceKind source_kind = e.source_kind;
        if (source_kind == USER_CB_SOURCE_NONE && e.source != INVALID_HANDLE)
            source_kind = USER_CB_SOURCE_RESOURCE;
        if (source_kind != USER_CB_SOURCE_RESOURCE || e.source != h)
            continue;
        if (strcmp(e.name, old_name) != 0)
            continue;
        user_cb_rename(i, new_name);
    }

    Resource* r = res_get(h);
    for (int c_i = 0; c_i < MAX_COMMANDS; c_i++) {
        Command& c = g_commands[c_i];
        if (!c.active)
            continue;
        if (r && r->type == RES_FLOAT4 && strcmp(c.clear_color_source, old_name) == 0) {
            strncpy(c.clear_color_source, new_name, MAX_NAME - 1);
            c.clear_color_source[MAX_NAME - 1] = '\0';
        }
        if (r && r->type == RES_FLOAT && strcmp(c.clear_depth_source, old_name) == 0) {
            strncpy(c.clear_depth_source, new_name, MAX_NAME - 1);
            c.clear_depth_source[MAX_NAME - 1] = '\0';
        }
    }
}

bool user_cb_rename(int idx, const char* name) {
    if (idx < 0 || idx >= g_user_cb_count)
        return false;

    char next_name[MAX_NAME] = {};
    user_cb_make_unique_name_except(name, next_name, MAX_NAME, idx);
    if (!next_name[0])
        return false;

    UserCBEntry& e = g_user_cb_entries[idx];
    if (strcmp(e.name, next_name) == 0)
        return true;

    char old_name[MAX_NAME] = {};
    strncpy(old_name, e.name, MAX_NAME - 1);
    old_name[MAX_NAME - 1] = '\0';

    strncpy(e.name, next_name, MAX_NAME - 1);
    e.name[MAX_NAME - 1] = '\0';
    user_cb_rename_variable_references(old_name, e.name);
    user_cb_bump_layout_revision();
    log_info("UserCB: renamed '%s' -> '%s'", old_name, e.name);
    app_request_scene_render();
    return true;
}

bool user_cb_type_supported(ResType type) {
    switch (type) {
    case RES_FLOAT:
    case RES_FLOAT2:
    case RES_FLOAT3:
    case RES_FLOAT4:
    case RES_INT:
    case RES_INT2:
    case RES_INT3:
        return true;
    default:
        return false;
    }
}

bool user_cb_add_var(const char* name, ResType type) {
    if (!user_cb_type_supported(type)) {
        log_warn("UserCB: unsupported variable type %s", res_type_str(type));
        return false;
    }
    if (g_user_cb_count >= MAX_USER_CB_VARS) {
        log_warn("UserCB: full (%d slots)", MAX_USER_CB_VARS);
        return false;
    }

    int slot = g_user_cb_count;
    UserCBEntry* e = &g_user_cb_entries[g_user_cb_count++];
    memset(e, 0, sizeof(*e));
    user_cb_make_unique_name(name, e->name, MAX_NAME);
    e->type   = type;
    e->source = INVALID_HANDLE;
    e->source_kind = USER_CB_SOURCE_NONE;
    user_cb_bump_layout_revision();

    log_info("UserCB: created '%s' at slot %d (preferred b2, offset %d bytes)",
             e->name, slot, slot * 16);
    app_request_scene_render();
    return true;
}

bool user_cb_add_from_resource(ResHandle h) {
    Resource* r = res_get(h);
    if (!r) {
        log_warn("UserCB: invalid resource handle");
        return false;
    }
    if (!user_cb_type_supported(r->type)) {
        log_warn("UserCB: resource '%s' has unsupported type %s", r->name, res_type_str(r->type));
        return false;
    }
    if (g_user_cb_count >= MAX_USER_CB_VARS) {
        log_warn("UserCB: full (%d slots)", MAX_USER_CB_VARS);
        return false;
    }

    int slot = g_user_cb_count;
    UserCBEntry* e = &g_user_cb_entries[g_user_cb_count++];
    memset(e, 0, sizeof(*e));
    user_cb_make_unique_name(r->name, e->name, MAX_NAME);
    e->type   = r->type;
    e->source = h;
    e->source_kind = USER_CB_SOURCE_RESOURCE;
    user_cb_copy_from_resource(e, r);
    user_cb_bump_layout_revision();

    log_info("UserCB: added '%s' linked to resource '%s' at slot %d (preferred b2, offset %d bytes)",
             e->name, r->name, slot, slot * 16);
    app_request_scene_render();
    return true;
}

bool user_cb_set_source(int idx, ResHandle h) {
    if (idx < 0 || idx >= g_user_cb_count) return false;

    UserCBEntry* e = &g_user_cb_entries[idx];
    if (h == INVALID_HANDLE) {
        user_cb_clear_source(e);
        app_request_scene_render();
        return true;
    }

    Resource* r = res_get(h);
    if (!r || !user_cb_type_supported(r->type) || r->type != e->type) {
        log_warn("UserCB: source must be an active %s resource for '%s'",
                 res_type_str(e->type), e->name);
        return false;
    }

    e->source = h;
    e->source_kind = USER_CB_SOURCE_RESOURCE;
    e->source_target[0] = '\0';
    user_cb_copy_from_resource(e, r);
    app_request_scene_render();
    return true;
}

bool user_cb_set_scene_source(int idx, UserCBSourceKind kind, const char* target) {
    if (idx < 0 || idx >= g_user_cb_count)
        return false;
    UserCBEntry* e = &g_user_cb_entries[idx];
    if (kind == USER_CB_SOURCE_NONE)
        return user_cb_set_source(idx, INVALID_HANDLE);
    if (kind == USER_CB_SOURCE_RESOURCE)
        return false;
    if (!user_cb_scene_source_type_supported(e->type)) {
        log_warn("UserCB: scene transform source requires float3 or float4 for '%s'", e->name);
        return false;
    }

    e->source = INVALID_HANDLE;
    e->source_kind = kind;
    strncpy(e->source_target, target ? target : "", MAX_NAME - 1);
    e->source_target[MAX_NAME - 1] = '\0';

    if (!user_cb_refresh_scene_source(e) && !user_cb_source_kind_is_command(kind)) {
        log_warn("UserCB: invalid scene transform source for '%s'", e->name);
        user_cb_clear_source(e);
        return false;
    }
    app_request_scene_render();
    return true;
}

void user_cb_detach_resource(ResHandle h) {
    Resource* r = res_get(h);
    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry* e = &g_user_cb_entries[i];
        if (e->source_kind != USER_CB_SOURCE_RESOURCE || e->source != h) continue;

        if (r && r->type == e->type)
            user_cb_copy_from_resource(e, r);
        user_cb_clear_source(e);
        log_info("UserCB: detached '%s' from deleted resource", e->name);
    }

    for (int c_i = 0; c_i < MAX_COMMANDS; c_i++) {
        Command& c = g_commands[c_i];
        if (!c.active) continue;
        if (r && strcmp(c.clear_color_source, r->name) == 0) {
            if (r->type == RES_FLOAT4)
                memcpy(c.clear_color, r->fval, sizeof(c.clear_color));
            c.clear_color_source[0] = '\0';
        }
        if (r && strcmp(c.clear_depth_source, r->name) == 0) {
            if (r->type == RES_FLOAT)
                c.depth_clear_val = clampf(r->fval[0], 0.0f, 1.0f);
            c.clear_depth_source[0] = '\0';
        }
        for (int p_i = 0; p_i < c.param_count; p_i++) {
            CommandParam& p = c.params[p_i];
            if (p.source != h) continue;
            if (r && r->type == p.type)
                command_param_copy_from_resource(&p, r);
            p.source = INVALID_HANDLE;
        }
    }
}

void user_cb_rename_command_references(const char* old_name, const char* new_name) {
    if (!old_name || !old_name[0] || !new_name || !new_name[0] ||
        strcmp(old_name, new_name) == 0)
        return;

    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry& e = g_user_cb_entries[i];
        if (!user_cb_source_kind_is_command(e.source_kind))
            continue;
        if (strcmp(e.source_target, old_name) != 0)
            continue;
        strncpy(e.source_target, new_name, MAX_NAME - 1);
        e.source_target[MAX_NAME - 1] = '\0';
        user_cb_refresh_entry(i);
    }

    for (int c_i = 0; c_i < MAX_COMMANDS; c_i++) {
        Command& c = g_commands[c_i];
        if (!c.active)
            continue;
        for (int p_i = 0; p_i < c.param_count; p_i++) {
            CommandParam& p = c.params[p_i];
            if (!user_cb_source_kind_is_command(p.source_kind))
                continue;
            if (strcmp(p.source_target, old_name) != 0)
                continue;
            strncpy(p.source_target, new_name, MAX_NAME - 1);
            p.source_target[MAX_NAME - 1] = '\0';
        }
    }
}

void user_cb_remove(int idx) {
    if (idx < 0 || idx >= g_user_cb_count) return;
    char removed_name[MAX_NAME] = {};
    strncpy(removed_name, g_user_cb_entries[idx].name, MAX_NAME - 1);
    removed_name[MAX_NAME - 1] = '\0';
    user_cb_delete_variable_references(removed_name);
    for (int i = idx; i < g_user_cb_count - 1; i++)
        g_user_cb_entries[i] = g_user_cb_entries[i + 1];
    memset(&g_user_cb_entries[--g_user_cb_count], 0, sizeof(UserCBEntry));
    user_cb_bump_layout_revision();
    app_request_scene_render();
}

void user_cb_move(int from, int to) {
    if (from < 0 || from >= g_user_cb_count) return;
    if (to   < 0 || to   >= g_user_cb_count) return;
    if (from == to) return;
    UserCBEntry tmp = g_user_cb_entries[from];
    int dir = (to > from) ? 1 : -1;
    for (int i = from; i != to; i += dir)
        g_user_cb_entries[i] = g_user_cb_entries[i + dir];
    g_user_cb_entries[to] = tmp;
    user_cb_bump_layout_revision();
    app_request_scene_render();
}

int user_cb_slot_offset(int idx) { return idx * 16; }
#endif
