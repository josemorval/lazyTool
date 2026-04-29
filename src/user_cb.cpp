#include "user_cb.h"
#include "resources.h"
#include "commands.h"
#include "dx11_ctx.h"
#include "log.h"
#include <string.h>

// user_cb.cpp manages the user-defined constant buffer bound at b1. It lets
// editor variables flow into shaders every frame without manual packing.

UserCBEntry   g_user_cb_entries[MAX_USER_CB_VARS] = {};
int           g_user_cb_count = 0;
ID3D11Buffer* g_user_cb_buf   = nullptr;
static ID3D11Buffer* s_command_cb_buf = nullptr;

static bool user_cb_name_exists(const char* name) {
    for (int i = 0; i < g_user_cb_count; i++) {
        if (strcmp(g_user_cb_entries[i].name, name) == 0)
            return true;
    }
    return false;
}

static void user_cb_make_unique_name(const char* base, char* out, int out_sz) {
    const char* src = (base && base[0]) ? base : "var";
    if (!user_cb_name_exists(src)) {
        strncpy(out, src, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    for (int i = 1; i < 1000; i++) {
        snprintf(out, out_sz, "%s_%d", src, i);
        if (!user_cb_name_exists(out))
            return;
    }

    strncpy(out, src, out_sz - 1);
    out[out_sz - 1] = '\0';
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
    if (!e || e->source == INVALID_HANDLE) return;

    Resource* r = res_get(e->source);
    if (!r || !user_cb_type_supported(r->type) || r->type != e->type) {
        log_warn("UserCB: detached missing source from '%s'", e->name);
        e->source = INVALID_HANDLE;
        return;
    }

    user_cb_copy_from_resource(e, r);
}

void user_cb_init() {
    memset(g_user_cb_entries, 0, sizeof(g_user_cb_entries));
    g_user_cb_count = 0;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth      = sizeof(UserCBData);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dx.dev->CreateBuffer(&bd, nullptr, &g_user_cb_buf);
    g_dx.dev->CreateBuffer(&bd, nullptr, &s_command_cb_buf);
    log_info("UserCB init (global b1 + command cb, %d slots x 16 bytes)", MAX_USER_CB_VARS);
}

void user_cb_shutdown() {
    if (g_user_cb_buf) { g_user_cb_buf->Release(); g_user_cb_buf = nullptr; }
    if (s_command_cb_buf) { s_command_cb_buf->Release(); s_command_cb_buf = nullptr; }
}

// Pack the latest user variable values into the shared GPU buffer.
void user_cb_update() {
    if (!g_user_cb_buf) return;

    UserCBData data = {};
    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry* e = &g_user_cb_entries[i];
        user_cb_refresh_source(e);
        user_cb_pack_entry(e, data.slots[i]);
    }

    D3D11_MAPPED_SUBRESOURCE ms = {};
    g_dx.ctx->Map(g_user_cb_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &data, sizeof(data));
    g_dx.ctx->Unmap(g_user_cb_buf, 0);
}

void user_cb_bind() {
    g_dx.ctx->VSSetConstantBuffers(1, 1, &g_user_cb_buf);
    g_dx.ctx->PSSetConstantBuffers(1, 1, &g_user_cb_buf);
    g_dx.ctx->CSSetConstantBuffers(1, 1, &g_user_cb_buf);
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

        for (int old_i = 0; old_i < old_count; old_i++) {
            if (strcmp(old[old_i].name, v.name) != 0 || old[old_i].type != v.type)
                continue;
            *p = old[old_i];
            strncpy(p->name, v.name, MAX_NAME - 1);
            p->name[MAX_NAME - 1] = '\0';
            p->type = v.type;
            Resource* src = res_get(p->source);
            if (!src || src->type != p->type)
                p->source = INVALID_HANDLE;
            break;
        }
    }

    // Remember what we synced against so we can skip the work next frame.
    c->synced_shader_cb_version = shader->shader_cb.layout_version;
    c->synced_shader_handle = c->shader;
}

static void pack_command_param_bytes(const CommandParam* p, const ShaderCBVar* v, unsigned char* bytes, int byte_count) {
    if (!p || !v || !bytes || !p->enabled) return;
    if ((int)v->offset >= byte_count) return;

    int max_copy = byte_count - (int)v->offset;
    if (max_copy <= 0) return;

    const int* ival = p->ival;
    const float* fval = p->fval;
    Resource* src = res_get(p->source);
    if (src && src->type == p->type) {
        ival = src->ival;
        fval = src->fval;
    }

    int bytes_to_copy = 0;
    const void* src_data = nullptr;
    switch (p->type) {
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
    if (bytes_to_copy > (int)v->size) bytes_to_copy = (int)v->size;
    if (bytes_to_copy > max_copy) bytes_to_copy = max_copy;
    memcpy(bytes + v->offset, src_data, bytes_to_copy);
}

void user_cb_bind_for_command(Command* c, const Resource* shader, bool bind_vs, bool bind_ps, bool bind_cs) {
    if (!g_user_cb_buf)
        return;

    if (!shader || !shader->shader_cb.active) {
        if (bind_vs) g_dx.ctx->VSSetConstantBuffers(1, 1, &g_user_cb_buf);
        if (bind_ps) g_dx.ctx->PSSetConstantBuffers(1, 1, &g_user_cb_buf);
        if (bind_cs) g_dx.ctx->CSSetConstantBuffers(1, 1, &g_user_cb_buf);
        return;
    }

    if (!s_command_cb_buf)
        return;

    // Heavy fix: the previous version re-ran user_cb_sync_command_params on
    // every single draw/dispatch every frame. For a project like fluid_ninja
    // that's ~56 invocations per frame of ~6.5 KB memcpy + ~6.5 KB memset +
    // up to 1024 string comparisons each. That steady CPU drain is what made
    // the editor feel like it had a "memory leak" (the heap was fine, but the
    // CPU was thrashing more every passing minute as ImGui also kept drawing
    // the complex command tree).
    //
    // The sync is only actually needed when the shader's reflected layout
    // changes (compile / hot-reload) or when the command is first bound to a
    // new shader. Skip it otherwise.
    bool need_sync = !c ||
                     c->synced_shader_cb_version != shader->shader_cb.layout_version ||
                     c->synced_shader_handle != c->shader;
    if (need_sync)
        user_cb_sync_command_params(c, shader);

    UserCBData data = {};
    int cb_size = (int)shader->shader_cb.size;
    if (cb_size <= 0) cb_size = sizeof(data);
    if (cb_size > (int)sizeof(data)) cb_size = sizeof(data);

    for (int i = 0; i < shader->shader_cb.var_count; i++) {
        const ShaderCBVar& v = shader->shader_cb.vars[i];
        int p_i = command_param_find(c, v.name);
        if (p_i < 0) continue;
        pack_command_param_bytes(&c->params[p_i], &v, (unsigned char*)&data, cb_size);
    }

    D3D11_MAPPED_SUBRESOURCE ms = {};
    g_dx.ctx->Map(s_command_cb_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &data, sizeof(data));
    g_dx.ctx->Unmap(s_command_cb_buf, 0);

    UINT slot = shader->shader_cb.bind_slot;
    if (bind_vs) g_dx.ctx->VSSetConstantBuffers(slot, 1, &s_command_cb_buf);
    if (bind_ps) g_dx.ctx->PSSetConstantBuffers(slot, 1, &s_command_cb_buf);
    if (bind_cs) g_dx.ctx->CSSetConstantBuffers(slot, 1, &s_command_cb_buf);
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

    log_info("UserCB: created '%s' at slot %d (b1, offset %d bytes)",
             e->name, slot, slot * 16);
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
    user_cb_copy_from_resource(e, r);

    log_info("UserCB: added '%s' linked to resource '%s' at slot %d (b1, offset %d bytes)",
             e->name, r->name, slot, slot * 16);
    return true;
}

bool user_cb_set_source(int idx, ResHandle h) {
    if (idx < 0 || idx >= g_user_cb_count) return false;

    UserCBEntry* e = &g_user_cb_entries[idx];
    if (h == INVALID_HANDLE) {
        e->source = INVALID_HANDLE;
        return true;
    }

    Resource* r = res_get(h);
    if (!r || !user_cb_type_supported(r->type) || r->type != e->type) {
        log_warn("UserCB: source must be an active %s resource for '%s'",
                 res_type_str(e->type), e->name);
        return false;
    }

    e->source = h;
    user_cb_copy_from_resource(e, r);
    return true;
}

void user_cb_detach_resource(ResHandle h) {
    Resource* r = res_get(h);
    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry* e = &g_user_cb_entries[i];
        if (e->source != h) continue;

        if (r && r->type == e->type)
            user_cb_copy_from_resource(e, r);
        e->source = INVALID_HANDLE;
        log_info("UserCB: detached '%s' from deleted resource", e->name);
    }

    for (int c_i = 0; c_i < MAX_COMMANDS; c_i++) {
        Command& c = g_commands[c_i];
        if (!c.active) continue;
        for (int p_i = 0; p_i < c.param_count; p_i++) {
            CommandParam& p = c.params[p_i];
            if (p.source != h) continue;
            if (r && r->type == p.type)
                command_param_copy_from_resource(&p, r);
            p.source = INVALID_HANDLE;
        }
    }
}

void user_cb_remove(int idx) {
    if (idx < 0 || idx >= g_user_cb_count) return;
    for (int i = idx; i < g_user_cb_count - 1; i++)
        g_user_cb_entries[i] = g_user_cb_entries[i + 1];
    memset(&g_user_cb_entries[--g_user_cb_count], 0, sizeof(UserCBEntry));
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
}

int user_cb_slot_offset(int idx) { return idx * 16; }
