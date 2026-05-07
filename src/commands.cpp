#include "commands.h"
#include "resources.h"
#include "dx11_ctx.h"
#include "user_cb.h"
#include "log.h"
#include "timeline.h"
#include <stdarg.h>
#include <string.h>

// The command system is the runtime execution graph. Each command describes
// one draw, dispatch, clear, or grouping step in the frame pipeline.

Command g_commands[MAX_COMMANDS] = {};
int     g_command_count = 0;
bool    g_profiler_enabled = false;

static Command s_cmd_move_old[MAX_COMMANDS] = {};
static Command s_cmd_move_new[MAX_COMMANDS] = {};
static float   s_cmd_profile_ms[MAX_COMMANDS] = {};
static float   s_frame_profile_ms = 0.0f;
static float   s_total_frame_profile_ms = 0.0f;
static bool    s_gpu_profile_ready = false;
static bool    s_gpu_total_ready = false;
static bool    s_gpu_profiler_ok = false;
static bool    s_prev_profiler_enabled = false;
static bool    s_gpu_overflow_warned = false;
static uint64_t s_gpu_submit_frame = 0;
static uint64_t s_gpu_last_ready_frame = 0;
static bool    s_gpu_frame_capture_open = false;
static bool    s_cmd_reset_execution = true;

#define GPU_PROFILE_LATENCY 6
#define GPU_PROFILE_MAX_EVENTS 512
static const UINT k_shadow_map_ps_slot = 7;

static uint32_t validation_hash_text(const char* text) {
    uint32_t hash = 2166136261u;
    if (!text)
        return 0;
    while (*text) {
        hash ^= (unsigned char)*text++;
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

static void validation_issue_append(char* out, int out_sz, const char* fmt, ...) {
    if (!out || out_sz <= 0 || !fmt)
        return;

    int len = (int)strlen(out);
    if (len >= out_sz - 1)
        return;

    if (len > 0) {
        int sep = snprintf(out + len, out_sz - len, "; ");
        if (sep < 0)
            return;
        len += sep;
        if (len >= out_sz - 1)
            return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + len, out_sz - len, fmt, ap);
    va_end(ap);
}

static void validation_finish(Command& c, const char* issues) {
    if (!g_dx.shader_validation_warnings) {
        c.validation_warning_hash = 0;
        return;
    }

    if (!issues || !issues[0]) {
        c.validation_warning_hash = 0;
        return;
    }

    uint32_t hash = validation_hash_text(issues);
    if (c.validation_warning_hash == hash)
        return;

    c.validation_warning_hash = hash;
    log_warn("Command '%s': %s", c.name, issues);
}

struct GPUProfileEvent {
    CmdHandle    handle;
    ID3D11Query* begin;
    ID3D11Query* end;
};

struct GPUProfileFrameSlot {
    ID3D11Query* disjoint;
    ID3D11Query* total_begin;
    ID3D11Query* total_end;
    ID3D11Query* frame_begin;
    ID3D11Query* frame_end;
    GPUProfileEvent events[GPU_PROFILE_MAX_EVENTS];
    int        event_count;
    bool       issued;
    bool       total_range_issued;
    bool       command_range_issued;
    uint64_t   frame_id;
};

static GPUProfileFrameSlot s_gpu_slots[GPU_PROFILE_LATENCY] = {};
static GPUProfileFrameSlot* s_gpu_active_slot = nullptr;

const char* cmd_type_str(CmdType t) {
    switch (t) {
    case CMD_CLEAR:             return "Clear";
    case CMD_GROUP:             return "Group";
    case CMD_DRAW_MESH:         return "DrawMesh";
    case CMD_DRAW_INSTANCED:    return "DrawInstanced";
    case CMD_DISPATCH:          return "Dispatch";
    case CMD_INDIRECT_DRAW:     return "IndirectDraw";
    case CMD_INDIRECT_DISPATCH: return "IndirectDispatch";
    case CMD_REPEAT:            return "Repeat";
    default:                    return "?";
    }
}

void cmd_init() {
    memset(g_commands, 0, sizeof(g_commands));
    memset(s_cmd_profile_ms, 0, sizeof(s_cmd_profile_ms));
    memset(s_gpu_slots, 0, sizeof(s_gpu_slots));
    g_command_count = 0;

    D3D11_QUERY_DESC timestamp_desc = {};
    timestamp_desc.Query = D3D11_QUERY_TIMESTAMP;
    D3D11_QUERY_DESC disjoint_desc = {};
    disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;

    s_gpu_profiler_ok = g_dx.dev != nullptr;
    for (int f = 0; f < GPU_PROFILE_LATENCY && s_gpu_profiler_ok; f++) {
        GPUProfileFrameSlot& slot = s_gpu_slots[f];
        if (FAILED(g_dx.dev->CreateQuery(&disjoint_desc, &slot.disjoint))) s_gpu_profiler_ok = false;
        if (FAILED(g_dx.dev->CreateQuery(&timestamp_desc, &slot.total_begin))) s_gpu_profiler_ok = false;
        if (FAILED(g_dx.dev->CreateQuery(&timestamp_desc, &slot.total_end))) s_gpu_profiler_ok = false;
        if (FAILED(g_dx.dev->CreateQuery(&timestamp_desc, &slot.frame_begin))) s_gpu_profiler_ok = false;
        if (FAILED(g_dx.dev->CreateQuery(&timestamp_desc, &slot.frame_end))) s_gpu_profiler_ok = false;
        for (int i = 0; i < GPU_PROFILE_MAX_EVENTS && s_gpu_profiler_ok; i++) {
            if (FAILED(g_dx.dev->CreateQuery(&timestamp_desc, &slot.events[i].begin))) s_gpu_profiler_ok = false;
            if (FAILED(g_dx.dev->CreateQuery(&timestamp_desc, &slot.events[i].end))) s_gpu_profiler_ok = false;
        }
    }

    if (!s_gpu_profiler_ok)
        log_warn("GPU profiler disabled: failed to create D3D11 timestamp queries.");
}

void cmd_shutdown() {
    s_gpu_active_slot = nullptr;
    for (int f = 0; f < GPU_PROFILE_LATENCY; f++) {
        GPUProfileFrameSlot& slot = s_gpu_slots[f];
        if (slot.disjoint) { slot.disjoint->Release(); slot.disjoint = nullptr; }
        if (slot.total_begin) { slot.total_begin->Release(); slot.total_begin = nullptr; }
        if (slot.total_end) { slot.total_end->Release(); slot.total_end = nullptr; }
        if (slot.frame_begin) { slot.frame_begin->Release(); slot.frame_begin = nullptr; }
        if (slot.frame_end) { slot.frame_end->Release(); slot.frame_end = nullptr; }
        for (int i = 0; i < GPU_PROFILE_MAX_EVENTS; i++) {
            if (slot.events[i].begin) { slot.events[i].begin->Release(); slot.events[i].begin = nullptr; }
            if (slot.events[i].end) { slot.events[i].end->Release(); slot.events[i].end = nullptr; }
            slot.events[i].handle = INVALID_HANDLE;
        }
        slot.event_count = 0;
        slot.issued = false;
        slot.frame_id = 0;
    }
    s_gpu_profiler_ok = false;
    s_gpu_profile_ready = false;
    s_prev_profiler_enabled = false;
    s_gpu_overflow_warned = false;
    s_gpu_submit_frame = 0;
    s_gpu_last_ready_frame = 0;
    s_gpu_frame_capture_open = false;
    memset(s_cmd_profile_ms, 0, sizeof(s_cmd_profile_ms));
    s_frame_profile_ms = 0.0f;
}

void cmd_set_reset_execution(bool active) {
    s_cmd_reset_execution = active;
}

float cmd_profile_ms(CmdHandle h) {
    if (h == INVALID_HANDLE || h > MAX_COMMANDS)
        return 0.0f;
    return s_cmd_profile_ms[h - 1];
}

float cmd_profile_frame_ms() {
    return s_frame_profile_ms;
}

bool cmd_profile_ready() {
    return s_gpu_profile_ready;
}

float cmd_profile_total_frame_ms() {
    return s_total_frame_profile_ms;
}

bool cmd_profile_total_ready() {
    return s_gpu_total_ready;
}

static void cmd_profile_reset_results() {
    memset(s_cmd_profile_ms, 0, sizeof(s_cmd_profile_ms));
    s_frame_profile_ms = 0.0f;
    s_total_frame_profile_ms = 0.0f;
    s_gpu_profile_ready = false;
    s_gpu_total_ready = false;
    s_gpu_last_ready_frame = 0;
}

static void cmd_profile_reset_slots() {
    s_gpu_active_slot = nullptr;
    s_gpu_submit_frame = 0;
    s_gpu_frame_capture_open = false;
    for (int i = 0; i < GPU_PROFILE_LATENCY; i++) {
        s_gpu_slots[i].event_count = 0;
        s_gpu_slots[i].issued = false;
        s_gpu_slots[i].total_range_issued = false;
        s_gpu_slots[i].command_range_issued = false;
        s_gpu_slots[i].frame_id = 0;
    }
}

static bool cmd_gpu_get_data(ID3D11Query* query, void* out, UINT out_sz) {
    if (!query || !g_dx.ctx)
        return false;
    return g_dx.ctx->GetData(query, out, out_sz, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
}

static void cmd_gpu_collect_slot(GPUProfileFrameSlot& slot) {
    if (!slot.issued)
        return;

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
    UINT64 total_begin = 0;
    UINT64 total_end = 0;
    UINT64 frame_begin = 0;
    UINT64 frame_end = 0;
    if (!cmd_gpu_get_data(slot.disjoint, &disjoint, sizeof(disjoint))) return;
    if (!cmd_gpu_get_data(slot.total_begin, &total_begin, sizeof(total_begin))) return;
    if (!cmd_gpu_get_data(slot.total_end, &total_end, sizeof(total_end))) return;
    if (!cmd_gpu_get_data(slot.frame_begin, &frame_begin, sizeof(frame_begin))) return;
    if (!cmd_gpu_get_data(slot.frame_end, &frame_end, sizeof(frame_end))) return;

    float cmd_ms[MAX_COMMANDS] = {};
    for (int i = 0; i < slot.event_count; i++) {
        UINT64 ev_begin = 0;
        UINT64 ev_end = 0;
        if (!cmd_gpu_get_data(slot.events[i].begin, &ev_begin, sizeof(ev_begin))) return;
        if (!cmd_gpu_get_data(slot.events[i].end, &ev_end, sizeof(ev_end))) return;
        CmdHandle h = slot.events[i].handle;
        if (h != INVALID_HANDLE && h <= MAX_COMMANDS && ev_end >= ev_begin && !disjoint.Disjoint && disjoint.Frequency > 0) {
            double ms = (double)(ev_end - ev_begin) * 1000.0 / (double)disjoint.Frequency;
            cmd_ms[h - 1] += (float)ms;
        }
    }

    slot.issued = false;
    if (disjoint.Disjoint || disjoint.Frequency == 0 || frame_end < frame_begin || total_end < total_begin)
        return;
    if (slot.frame_id < s_gpu_last_ready_frame)
        return;

    memcpy(s_cmd_profile_ms, cmd_ms, sizeof(s_cmd_profile_ms));
    s_frame_profile_ms = (float)((double)(frame_end - frame_begin) * 1000.0 / (double)disjoint.Frequency);
    s_total_frame_profile_ms = (float)((double)(total_end - total_begin) * 1000.0 / (double)disjoint.Frequency);
    s_gpu_profile_ready = true;
    s_gpu_total_ready = true;
    s_gpu_last_ready_frame = slot.frame_id;
}

static void cmd_profile_sync_enable_state() {
    if (g_profiler_enabled != s_prev_profiler_enabled) {
        cmd_profile_reset_results();
        cmd_profile_reset_slots();
        s_prev_profiler_enabled = g_profiler_enabled;
    }
}

static void cmd_gpu_begin_total_frame() {
    s_gpu_active_slot = nullptr;
    if (!s_gpu_profiler_ok || !g_dx.ctx)
        return;

    GPUProfileFrameSlot& slot = s_gpu_slots[s_gpu_submit_frame % GPU_PROFILE_LATENCY];
    if (slot.issued) {
        cmd_gpu_collect_slot(slot);
        if (slot.issued)
            return;
    }

    slot.event_count = 0;
    slot.frame_id = ++s_gpu_submit_frame;
    slot.issued = true;
    slot.total_range_issued = true;
    slot.command_range_issued = false;
    s_gpu_overflow_warned = false;

    g_dx.ctx->Begin(slot.disjoint);
    g_dx.ctx->End(slot.total_begin);
    s_gpu_active_slot = &slot;
}

static void cmd_gpu_end_command_frame() {
    if (!s_gpu_active_slot || !g_dx.ctx)
        return;
    if (!s_gpu_active_slot->command_range_issued) {
        g_dx.ctx->End(s_gpu_active_slot->frame_begin);
        s_gpu_active_slot->command_range_issued = true;
    }
    g_dx.ctx->End(s_gpu_active_slot->frame_end);
}

static void cmd_gpu_end_total_frame() {
    if (!s_gpu_active_slot || !g_dx.ctx)
        return;
    g_dx.ctx->End(s_gpu_active_slot->total_end);
    g_dx.ctx->End(s_gpu_active_slot->disjoint);
    s_gpu_active_slot = nullptr;
}

static int cmd_gpu_begin_command(CmdHandle h) {
    if (!g_profiler_enabled || !s_gpu_active_slot || !g_dx.ctx || h == INVALID_HANDLE || h > MAX_COMMANDS)
        return -1;
    if (s_gpu_active_slot->event_count >= GPU_PROFILE_MAX_EVENTS) {
        if (!s_gpu_overflow_warned) {
            log_warn("GPU profiler event pool exhausted; some commands will be skipped.");
            s_gpu_overflow_warned = true;
        }
        return -1;
    }

    int event_index = s_gpu_active_slot->event_count++;
    s_gpu_active_slot->events[event_index].handle = h;
    g_dx.ctx->End(s_gpu_active_slot->events[event_index].begin);
    return event_index;
}

static void cmd_gpu_end_command(int event_index) {
    if (event_index < 0 || !s_gpu_active_slot || !g_dx.ctx || event_index >= s_gpu_active_slot->event_count)
        return;
    g_dx.ctx->End(s_gpu_active_slot->events[event_index].end);
}

static bool cmd_name_exists_except(const char* name, CmdHandle except) {
    if (!name || !name[0])
        return false;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        CmdHandle h = (CmdHandle)(i + 1);
        if (h == except)
            continue;
        if (g_commands[i].active && strcmp(g_commands[i].name, name) == 0)
            return true;
    }
    return false;
}

static void cmd_make_unique_name_except(const char* base, char* out, int out_sz, CmdHandle except) {
    if (!out || out_sz <= 0)
        return;
    const char* src = (base && base[0]) ? base : "cmd";
    if (!cmd_name_exists_except(src, except)) {
        strncpy(out, src, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    for (int i = 1; i < 1000; i++) {
        snprintf(out, out_sz, "%s_%d", src, i);
        if (!cmd_name_exists_except(out, except))
            return;
    }

    strncpy(out, src, out_sz - 1);
    out[out_sz - 1] = '\0';
}

CmdHandle cmd_alloc(const char* name, CmdType type) {
    char unique_name[MAX_NAME] = {};
    cmd_make_unique_name_except(name, unique_name, MAX_NAME, INVALID_HANDLE);
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (!g_commands[i].active) {
            memset(&g_commands[i], 0, sizeof(Command));
            strncpy(g_commands[i].name, unique_name, MAX_NAME - 1);
            g_commands[i].name[MAX_NAME - 1] = '\0';
            g_commands[i].type             = type;
            g_commands[i].active           = true;
            g_commands[i].enabled          = true;
            g_commands[i].parent           = INVALID_HANDLE;
            g_commands[i].repeat_count     = 1;
            g_commands[i].repeat_expanded  = true;
            g_commands[i].rt               = INVALID_HANDLE;
            g_commands[i].depth            = INVALID_HANDLE;
            g_commands[i].mrt_count        = 0;
            g_commands[i].mesh             = INVALID_HANDLE;
            g_commands[i].shader           = INVALID_HANDLE;
            g_commands[i].shadow_shader    = INVALID_HANDLE;
            g_commands[i].draw_source      = DRAW_SOURCE_MESH;
            g_commands[i].draw_topology    = DRAW_TOPOLOGY_TRIANGLE_LIST;
            g_commands[i].color_write      = true;
            g_commands[i].depth_test       = true;
            g_commands[i].depth_write      = true;
            g_commands[i].alpha_blend      = false;
            g_commands[i].cull_back        = true;
            g_commands[i].shadow_cast      = false;
            g_commands[i].shadow_receive   = false;
            g_commands[i].scale[0]         = 1.0f;
            g_commands[i].scale[1]         = 1.0f;
            g_commands[i].scale[2]         = 1.0f;
            g_commands[i].indirect_buf     = INVALID_HANDLE;
            g_commands[i].clear_color[0]   = 0.05f;
            g_commands[i].clear_color[1]   = 0.05f;
            g_commands[i].clear_color[2]   = 0.08f;
            g_commands[i].clear_color[3]   = 1.0f;
            g_commands[i].clear_color_enabled = true;
            g_commands[i].clear_depth      = true;
            g_commands[i].depth_clear_val  = 1.0f;
            g_commands[i].vertex_count     = 3;
            g_commands[i].instance_count   = 1;
            g_commands[i].thread_x         = 1;
            g_commands[i].thread_y         = 1;
            g_commands[i].thread_z         = 1;
            g_commands[i].compute_on_reset = false;
            g_commands[i].dispatch_size_source = INVALID_HANDLE;
            for (int r = 0; r < MAX_DRAW_RENDER_TARGETS - 1; r++) g_commands[i].mrt_handles[r] = INVALID_HANDLE;
            for (int t = 0; t < MAX_TEX_SLOTS; t++) g_commands[i].tex_handles[t] = INVALID_HANDLE;
            for (int s = 0; s < MAX_SRV_SLOTS; s++) g_commands[i].srv_handles[s] = INVALID_HANDLE;
            for (int u = 0; u < MAX_UAV_SLOTS; u++) g_commands[i].uav_handles[u] = INVALID_HANDLE;
            g_command_count++;
            return (CmdHandle)(i + 1);
        }
    }
    log_error("cmd_alloc: out of command slots");
    return INVALID_HANDLE;
}

void cmd_free(CmdHandle h) {
    Command* c = cmd_get(h);
    if (!c) return;
    char deleted_name[MAX_NAME] = {};
    strncpy(deleted_name, c->name, MAX_NAME - 1);
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (g_commands[i].active && g_commands[i].parent == h)
            cmd_free((CmdHandle)(i + 1));
    }
    timeline_delete_tracks_for_command(deleted_name);
    c->active = false;
    c->parent = INVALID_HANDLE;
    g_command_count--;
}

Command* cmd_get(CmdHandle h) {
    if (h == INVALID_HANDLE || h > MAX_COMMANDS) return nullptr;
    Command* c = &g_commands[h - 1];
    return c->active ? c : nullptr;
}

CmdHandle cmd_find_by_name(const char* name) {
    if (!name || !name[0] || strcmp(name, "-") == 0)
        return INVALID_HANDLE;

    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (g_commands[i].active && strcmp(g_commands[i].name, name) == 0)
            return (CmdHandle)(i + 1);
    }
    return INVALID_HANDLE;
}

bool cmd_rename(CmdHandle h, const char* name) {
    Command* c = cmd_get(h);
    if (!c)
        return false;

    char next_name[MAX_NAME] = {};
    cmd_make_unique_name_except(name, next_name, MAX_NAME, h);
    if (!next_name[0])
        return false;
    if (strcmp(c->name, next_name) == 0)
        return true;

    char old_name[MAX_NAME] = {};
    strncpy(old_name, c->name, MAX_NAME - 1);
    old_name[MAX_NAME - 1] = '\0';

    strncpy(c->name, next_name, MAX_NAME - 1);
    c->name[MAX_NAME - 1] = '\0';

    timeline_rename_tracks_for_command(old_name, c->name);
    user_cb_rename_command_references(old_name, c->name);
    log_info("Command renamed: %s -> %s", old_name, c->name);
    return true;
}

CmdHandle cmd_move(CmdHandle moving, CmdHandle target, bool after_target) {
    if (moving == INVALID_HANDLE || target == INVALID_HANDLE || moving == target)
        return moving;
    if (!cmd_get(moving) || !cmd_get(target))
        return moving;

    int active[MAX_COMMANDS] = {};
    int count = 0;
    int move_pos = -1;
    int target_pos = -1;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (!g_commands[i].active) continue;
        if ((CmdHandle)(i + 1) == moving) move_pos = count;
        if ((CmdHandle)(i + 1) == target) target_pos = count;
        active[count++] = i;
    }

    if (move_pos < 0 || target_pos < 0)
        return moving;

    int without[MAX_COMMANDS] = {};
    int without_count = 0;
    for (int i = 0; i < count; i++)
        if (i != move_pos)
            without[without_count++] = active[i];

    int target_without_pos = target_pos;
    if (move_pos < target_pos)
        target_without_pos--;
    int insert_pos = target_without_pos + (after_target ? 1 : 0);
    if (insert_pos < 0) insert_pos = 0;
    if (insert_pos > without_count) insert_pos = without_count;

    int order[MAX_COMMANDS] = {};
    int out_count = 0;
    for (int i = 0; i <= without_count; i++) {
        if (i == insert_pos)
            order[out_count++] = active[move_pos];
        if (i < without_count)
            order[out_count++] = without[i];
    }

    memcpy(s_cmd_move_old, g_commands, sizeof(g_commands));
    memset(s_cmd_move_new, 0, sizeof(s_cmd_move_new));

    CmdHandle remap[MAX_COMMANDS + 1] = {};
    CmdHandle moved_handle = moving;
    for (int i = 0; i < count; i++) {
        int old_index = order[i];
        s_cmd_move_new[i] = s_cmd_move_old[old_index];
        remap[old_index + 1] = (CmdHandle)(i + 1);
        if ((CmdHandle)(old_index + 1) == moving)
            moved_handle = (CmdHandle)(i + 1);
    }

    for (int i = 0; i < count; i++) {
        CmdHandle old_parent = s_cmd_move_new[i].parent;
        if (old_parent != INVALID_HANDLE && old_parent <= MAX_COMMANDS)
            s_cmd_move_new[i].parent = remap[old_parent];
        if (s_cmd_move_new[i].parent == INVALID_HANDLE)
            s_cmd_move_new[i].parent = INVALID_HANDLE;
    }

    memcpy(g_commands, s_cmd_move_new, sizeof(g_commands));
    return moved_handle;
}

static ID3D11RenderTargetView* get_rtv(ResHandle h) {
    if (h == INVALID_HANDLE) return g_dx.scene_rtv;
    Resource* r = res_get(h);
    if (!r) return g_dx.scene_rtv;
    if (r->type == RES_BUILTIN_SCENE_COLOR) return g_dx.scene_rtv;
    return r->rtv ? r->rtv : g_dx.scene_rtv;
}

static ID3D11RenderTargetView* get_optional_rtv(ResHandle h) {
    if (h == INVALID_HANDLE)
        return nullptr;
    Resource* r = res_get(h);
    if (!r)
        return nullptr;
    if (r->type == RES_BUILTIN_SCENE_COLOR)
        return g_dx.scene_rtv;
    return r->rtv;
}

static ID3D11DepthStencilView* get_dsv(ResHandle h) {
    if (h == INVALID_HANDLE) return g_dx.depth_dsv;
    Resource* r = res_get(h);
    if (!r) return g_dx.depth_dsv;
    if (r->type == RES_BUILTIN_SCENE_DEPTH) return g_dx.depth_dsv;
    return r->dsv ? r->dsv : g_dx.depth_dsv;
}

static Resource* get_output_size_resource(ResHandle h) {
    if (h == INVALID_HANDLE)
        return nullptr;
    return res_get(h);
}

static void set_viewport_for_target(ResHandle color, ResHandle depth, bool prefer_color) {
    int w = g_dx.scene_width;
    int hgt = g_dx.scene_height;

    Resource* r = res_get(prefer_color ? color : depth);
    if (!r)
        r = res_get(prefer_color ? depth : color);
    if (r && (r->type == RES_RENDER_TEXTURE2D || r->type == RES_RENDER_TEXTURE3D) &&
        r->width > 0 && r->height > 0) {
        w = r->width;
        hgt = r->height;
    }

    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)hgt, 0, 1 };
    g_dx.ctx->RSSetViewports(1, &vp);
}

static void set_viewport_for_draw_outputs(const Command& c) {
    int w = g_dx.scene_width;
    int hgt = g_dx.scene_height;

    Resource* r = nullptr;
    if (c.color_write)
        r = get_output_size_resource(c.rt);
    if (!r) {
        for (int i = 0; i < c.mrt_count; i++) {
            r = get_output_size_resource(c.mrt_handles[i]);
            if (r)
                break;
        }
    }
    if (!r)
        r = get_output_size_resource(c.depth);
    if (!r) {
        for (int i = 0; i < c.uav_count; i++) {
            r = get_output_size_resource(c.uav_handles[i]);
            if (r && r->width > 0 && r->height > 0)
                break;
            r = nullptr;
        }
    }

    if (r && r->width > 0 && r->height > 0) {
        w = r->width;
        hgt = r->height;
    }

    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)hgt, 0, 1 };
    g_dx.ctx->RSSetViewports(1, &vp);
}

static ID3D11DepthStencilView* get_draw_dsv(const Command& c) {
    if (!c.depth_test && !c.depth_write)
        return nullptr;
    return get_dsv(c.depth);
}

static Mat4 mat4_from_raw(const float raw[16]) {
    Mat4 m = {};
    if (raw)
        memcpy(m.m, raw, sizeof(m.m));
    return m;
}

static const MeshMaterial* mesh_material_for_part(const Resource* mesh, const MeshPart* part) {
    if (!mesh || !part)
        return nullptr;
    int mi = part->material_index;
    if (mi < 0 || mi >= mesh->mesh_material_count)
        return nullptr;
    return &mesh->mesh_materials[mi];
}

static ID3D11RasterizerState* rasterizer_state_for(const Command& c, const MeshMaterial* material,
                                                   bool wireframe) {
    bool cull_back = c.cull_back;
    if (material && material->double_sided)
        cull_back = false;
    if (wireframe)
        return cull_back ? g_dx.rs_wire_solid : g_dx.rs_wire_cull_none;
    return cull_back ? g_dx.rs_solid : g_dx.rs_cull_none;
}

static void apply_draw_state(const Command& c, const MeshMaterial* material) {
    ID3D11RasterizerState* rs = rasterizer_state_for(c, material, g_dx.scene_wireframe);
    g_dx.ctx->RSSetState(rs ? rs : g_dx.rs_solid);

    ID3D11DepthStencilState* dss = g_dx.dss_default;
    if (!c.depth_test) dss = g_dx.dss_depth_off;
    else if (!c.depth_write) dss = g_dx.dss_depth_read;
    if (!dss) dss = g_dx.dss_default;
    g_dx.ctx->OMSetDepthStencilState(dss, 0);

    float bf[4] = {};
    bool alpha_blend = c.alpha_blend || (material && material->alpha_blend);
    ID3D11BlendState* bs = alpha_blend ? g_dx.bs_alpha : g_dx.bs_opaque;
    g_dx.ctx->OMSetBlendState(bs ? bs : g_dx.bs_opaque, bf, 0xFFFFFFFF);
}

static bool is_draw_command(CmdType type) {
    return type == CMD_DRAW_MESH || type == CMD_DRAW_INSTANCED || type == CMD_INDIRECT_DRAW;
}

static bool command_uses_procedural_draw(const Command& c) {
    return c.draw_source == DRAW_SOURCE_PROCEDURAL;
}

static int mesh_enabled_part_count(const Resource* mesh) {
    if (!mesh)
        return 0;
    if (mesh->mesh_part_count <= 0)
        return 1;

    int count = 0;
    for (int i = 0; i < mesh->mesh_part_count; i++) {
        if (mesh->mesh_parts[i].enabled)
            count++;
    }
    return count;
}

static const MeshPart* indirect_draw_part_context(const Resource* mesh) {
    if (!mesh || mesh->mesh_part_count <= 0)
        return nullptr;

    const MeshPart* selected = nullptr;
    for (int i = 0; i < mesh->mesh_part_count; i++) {
        const MeshPart* part = &mesh->mesh_parts[i];
        if (!part->enabled)
            continue;
        if (selected)
            return nullptr;
        selected = part;
    }
    return selected;
}

static bool command_uses_indexed_mesh_draw(const Command& c, const Resource* mesh) {
    return !command_uses_procedural_draw(c) && mesh && mesh->ib;
}

static UINT indirect_draw_args_required_bytes(const Command& c, const Resource* mesh) {
    return command_uses_indexed_mesh_draw(c, mesh)
        ? (UINT)sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)
        : (UINT)sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS);
}

static UINT indirect_dispatch_args_required_bytes() {
    return (UINT)(sizeof(UINT) * 3);
}

static bool indirect_args_buffer_has_range(const Resource* buf, uint32_t offset, UINT required_bytes) {
    if (!buf || buf->elem_size < 1 || buf->elem_count < 1)
        return false;
    if ((offset & 3u) != 0u)
        return false;

    uint64_t total_bytes = (uint64_t)buf->elem_size * (uint64_t)buf->elem_count;
    uint64_t end = (uint64_t)offset + (uint64_t)required_bytes;
    return end <= total_bytes;
}

static D3D11_PRIMITIVE_TOPOLOGY command_draw_topology(const Command& c) {
    switch ((DrawTopologyType)c.draw_topology) {
    case DRAW_TOPOLOGY_POINT_LIST:    return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case DRAW_TOPOLOGY_TRIANGLE_LIST:
    default:                          return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static Mat4 command_world_matrix(const Command& c) {
    Mat4 s = mat4_scale(v3(c.scale[0], c.scale[1], c.scale[2]));
    Mat4 r = mat4_rotation_xyz(v3(c.rot[0], c.rot[1], c.rot[2]));
    Mat4 t = mat4_translation(v3(c.pos[0], c.pos[1], c.pos[2]));
    return mat4_mul(mat4_mul(s, r), t);
}

static void bind_object_cb_for_shader(const Resource* shader, bool bind_vs, bool bind_ps) {
    if (!g_dx.object_cb)
        return;

    UINT slot = 1;
    if (shader && shader->object_cb_active)
        slot = shader->object_cb_bind_slot;

    if (bind_vs) g_dx.ctx->VSSetConstantBuffers(slot, 1, &g_dx.object_cb);
    if (bind_ps) g_dx.ctx->PSSetConstantBuffers(slot, 1, &g_dx.object_cb);
}

static void update_object_cb_for_command(const Command& c, const MeshPart* part) {
    ObjectCBData cb = {};
    Mat4 world = command_world_matrix(c);
    if (part) {
        Mat4 local = mat4_from_raw(part->local_transform);
        world = mat4_mul(local, world);
    }
    // See main.cpp: with default column-major HLSL and mul(M, v), upload the
    // CPU row-major matrix as-is so the shader sees the required transpose.
    memcpy(cb.world, world.m, sizeof(world.m));
    dx_update_object_cb(cb);
}

static void bind_mesh_geometry(Resource* mesh) {
    if (!mesh || !mesh->vb)
        return;
    UINT stride = (UINT)mesh->vert_stride, offset = 0;
    g_dx.ctx->IASetVertexBuffers(0, 1, &mesh->vb, &stride, &offset);
    g_dx.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx.ctx->IASetIndexBuffer(mesh->ib, DXGI_FORMAT_R32_UINT, 0);
}

static void bind_command_geometry(const Command& c, Resource* mesh) {
    if (command_uses_procedural_draw(c)) {
        g_dx.ctx->IASetInputLayout(nullptr);
        g_dx.ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        g_dx.ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        g_dx.ctx->IASetPrimitiveTopology(command_draw_topology(c));
        return;
    }

    bind_mesh_geometry(mesh);
}

static void draw_command_geometry(const Command& c, Resource* mesh, const MeshPart* part) {
    int inst = (c.type == CMD_DRAW_INSTANCED) ? c.instance_count : 1;
    if (command_uses_procedural_draw(c)) {
        if (c.vertex_count > 0)
            g_dx.ctx->DrawInstanced((UINT)c.vertex_count, (UINT)inst, 0, 0);
        return;
    }
    if (mesh->ib) {
        UINT index_count = (UINT)(part ? part->index_count : mesh->idx_count);
        UINT start_index = (UINT)(part ? part->start_index : 0);
        g_dx.ctx->DrawIndexedInstanced(index_count, (UINT)inst, start_index, 0, 0);
    } else {
        UINT vertex_count = (UINT)(part ? part->index_count : mesh->vert_count);
        UINT start_vertex = (UINT)(part ? part->start_index : 0);
        g_dx.ctx->DrawInstanced(vertex_count, (UINT)inst, start_vertex, 0);
    }
}

static void bind_mesh_material_textures(const Resource* mesh, const MeshPart* part) {
    const MeshMaterial* material = mesh_material_for_part(mesh, part);
    for (int slot = 0; slot < MAX_MESH_MATERIAL_TEXTURES; slot++) {
        ResHandle tex_h = material ? material->textures[slot] : INVALID_HANDLE;
        Resource* tex = res_get(tex_h);
        ID3D11ShaderResourceView* srv = tex ? tex->srv : nullptr;
        g_dx.ctx->PSSetShaderResources((UINT)slot, 1, &srv);
    }
}

static bool command_has_bound_srv(const ResHandle* handles, const uint32_t* slots, int count, uint32_t slot) {
    if (!handles || !slots || count <= 0)
        return false;

    for (int i = 0; i < count; i++) {
        if (slots[i] != slot)
            continue;
        Resource* r = res_get(handles[i]);
        if (r && r->srv)
            return true;
    }
    return false;
}

static bool command_has_bound_uav(const ResHandle* handles, const uint32_t* slots, int count, uint32_t slot) {
    if (!handles || !slots || count <= 0)
        return false;

    for (int i = 0; i < count; i++) {
        if (slots[i] != slot)
            continue;
        Resource* r = res_get(handles[i]);
        if (r && r->uav)
            return true;
    }
    return false;
}

static bool mesh_has_material_srv_binding(const Resource* mesh, uint32_t slot) {
    if (!mesh || slot >= MAX_MESH_MATERIAL_TEXTURES)
        return false;

    for (int i = 0; i < mesh->mesh_material_count; i++) {
        Resource* tex = res_get(mesh->mesh_materials[i].textures[slot]);
        if (tex && tex->srv)
            return true;
    }
    return false;
}

static bool is_valid_indirect_args_buffer(const Resource* r) {
    return r && r->buf && r->indirect_args;
}

static bool is_valid_indirect_draw_call(const Command& c, const Resource* mesh, const Resource* indirect_buf) {
    return is_valid_indirect_args_buffer(indirect_buf) &&
           indirect_args_buffer_has_range(indirect_buf, c.indirect_offset,
                                          indirect_draw_args_required_bytes(c, mesh));
}

static bool is_valid_indirect_dispatch_call(const Command& c, const Resource* indirect_buf) {
    return is_valid_indirect_args_buffer(indirect_buf) &&
           indirect_args_buffer_has_range(indirect_buf, c.indirect_offset,
                                          indirect_dispatch_args_required_bytes());
}

static bool draw_command_has_ps_srv_binding(const Command& c, const Resource* mesh, uint32_t slot) {
    if (slot == k_shadow_map_ps_slot && c.shadow_receive && g_dx.shadow_srv)
        return true;
    if (command_has_bound_srv(c.tex_handles, c.tex_slots, c.tex_count, slot))
        return true;
    if (mesh_has_material_srv_binding(mesh, slot))
        return true;
    return false;
}

static void validate_draw_command(Command& c, const Resource* mesh, const Resource* shader,
                                  bool procedural, bool indirect, const Resource* indirect_buf) {
    char issues[768] = {};

    if (!shader || !shader->vs || !shader->ps)
        validation_issue_append(issues, sizeof(issues), "shader missing or not compiled");
    if (!procedural && (!mesh || !mesh->vb))
        validation_issue_append(issues, sizeof(issues), "mesh missing or not uploaded");
    if (indirect && !is_valid_indirect_args_buffer(indirect_buf))
        validation_issue_append(issues, sizeof(issues),
            "indirect args buffer missing, invalid, or not flagged");
    if (indirect && indirect_buf) {
        if ((c.indirect_offset & 3u) != 0u) {
            validation_issue_append(issues, sizeof(issues),
                "indirect offset %u is not DWORD-aligned", c.indirect_offset);
        } else if (!indirect_args_buffer_has_range(indirect_buf, c.indirect_offset,
                                                   indirect_draw_args_required_bytes(c, mesh))) {
            validation_issue_append(issues, sizeof(issues),
                "indirect args buffer too small for offset %u", c.indirect_offset);
        }
    }
    if (procedural && !indirect && c.vertex_count <= 0)
        validation_issue_append(issues, sizeof(issues), "procedural draw has vertex_count <= 0");
    if (procedural && c.shadow_cast) {
        Resource* shadow_shader = res_get(c.shadow_shader);
        if (!shadow_shader || !shadow_shader->vs)
            validation_issue_append(issues, sizeof(issues),
                "procedural shadow caster needs shadow_shader with a valid VS");
    }
    if (indirect && !procedural && mesh && mesh->mesh_material_count > 0 &&
        mesh_enabled_part_count(mesh) != 1) {
        validation_issue_append(issues, sizeof(issues),
            "multi-part indirect mesh draw cannot infer a single mesh material/part");
    }

    if (shader) {
        if (shader->shader_cb.active && shader->object_cb_active &&
            shader->shader_cb.bind_slot == shader->object_cb_bind_slot) {
            validation_issue_append(issues, sizeof(issues),
                "UserCB and ObjectCB share b%u", shader->shader_cb.bind_slot);
        }
        for (int i = 0; i < shader->shader_binding_count; i++) {
            const ShaderBinding& bind = shader->shader_bindings[i];
            for (uint32_t offset = 0; offset < bind.bind_count; offset++) {
                uint32_t slot = bind.bind_slot + offset;

                if (bind.kind == SHADER_BIND_SRV) {
                    if ((bind.stage_mask & SHADER_STAGE_VERTEX) &&
                        !command_has_bound_srv(c.srv_handles, c.srv_slots, c.srv_count, slot)) {
                        validation_issue_append(issues, sizeof(issues), "missing VS t%u '%s'", slot, bind.name);
                    }
                    if ((bind.stage_mask & SHADER_STAGE_PIXEL) &&
                        !draw_command_has_ps_srv_binding(c, mesh, slot)) {
                        validation_issue_append(issues, sizeof(issues), "missing PS t%u '%s'", slot, bind.name);
                    }
                } else if (bind.kind == SHADER_BIND_UAV) {
                    if ((bind.stage_mask & SHADER_STAGE_PIXEL) &&
                        !command_has_bound_uav(c.uav_handles, c.uav_slots, c.uav_count, slot)) {
                        validation_issue_append(issues, sizeof(issues), "missing PS u%u '%s'", slot, bind.name);
                    }
                }
            }
        }
    }

    validation_finish(c, issues);
}

static void validate_dispatch_command(Command& c, const Resource* shader, bool indirect, const Resource* indirect_buf) {
    char issues[768] = {};

    if (!shader || !shader->cs)
        validation_issue_append(issues, sizeof(issues), "compute shader missing or not compiled");
    if (indirect && !is_valid_indirect_args_buffer(indirect_buf))
        validation_issue_append(issues, sizeof(issues),
            "indirect args buffer missing, invalid, or not flagged");
    if (indirect && indirect_buf) {
        if ((c.indirect_offset & 3u) != 0u) {
            validation_issue_append(issues, sizeof(issues),
                "indirect offset %u is not DWORD-aligned", c.indirect_offset);
        } else if (!indirect_args_buffer_has_range(indirect_buf, c.indirect_offset,
                                                   indirect_dispatch_args_required_bytes())) {
            validation_issue_append(issues, sizeof(issues),
                "indirect args buffer too small for offset %u", c.indirect_offset);
        }
    }

    if (shader) {
        for (int i = 0; i < shader->shader_binding_count; i++) {
            const ShaderBinding& bind = shader->shader_bindings[i];
            if ((bind.stage_mask & SHADER_STAGE_COMPUTE) == 0)
                continue;

            for (uint32_t offset = 0; offset < bind.bind_count; offset++) {
                uint32_t slot = bind.bind_slot + offset;
                if (bind.kind == SHADER_BIND_SRV &&
                    !command_has_bound_srv(c.srv_handles, c.srv_slots, c.srv_count, slot)) {
                    validation_issue_append(issues, sizeof(issues), "missing CS t%u '%s'", slot, bind.name);
                } else if (bind.kind == SHADER_BIND_UAV &&
                           !command_has_bound_uav(c.uav_handles, c.uav_slots, c.uav_count, slot)) {
                    validation_issue_append(issues, sizeof(issues), "missing CS u%u '%s'", slot, bind.name);
                }
            }
        }
    }

    validation_finish(c, issues);
}

static UINT collect_draw_rtvs(const Command& c, ID3D11RenderTargetView** out_rtvs, UINT max_rtvs) {
    if (!out_rtvs || max_rtvs == 0)
        return 0;

    for (UINT i = 0; i < max_rtvs; i++)
        out_rtvs[i] = nullptr;
    if (!c.color_write)
        return 0;

    UINT rtv_count = 0;
    out_rtvs[0] = get_optional_rtv(c.rt);
    if (out_rtvs[0])
        rtv_count = 1;

    int extra_count = c.mrt_count;
    if (extra_count < 0) extra_count = 0;
    if (extra_count > (int)(max_rtvs - 1)) extra_count = (int)(max_rtvs - 1);
    for (int i = 0; i < extra_count; i++) {
        out_rtvs[i + 1] = get_optional_rtv(c.mrt_handles[i]);
        if (out_rtvs[i + 1])
            rtv_count = (UINT)(i + 2);
    }

    return rtv_count;
}

static UINT collect_draw_uavs(const Command& c, UINT rtv_count, UINT* out_start_slot,
                              ID3D11UnorderedAccessView** out_uavs, UINT max_uavs) {
    if (out_start_slot)
        *out_start_slot = rtv_count;
    if (!out_uavs || max_uavs == 0 || rtv_count >= max_uavs)
        return 0;

    ID3D11UnorderedAccessView* by_slot[MAX_UAV_SLOTS] = {};
    UINT highest_slot = rtv_count;
    bool has_uav = false;

    for (int i = 0; i < c.uav_count; i++) {
        UINT slot = c.uav_slots[i];
        if (slot < rtv_count || slot >= max_uavs)
            continue;
        Resource* ur = res_get(c.uav_handles[i]);
        ID3D11UnorderedAccessView* uav = ur ? ur->uav : nullptr;
        if (!uav)
            continue;

        by_slot[slot] = uav;
        if (!has_uav || slot > highest_slot)
            highest_slot = slot;
        has_uav = true;
    }

    if (!has_uav)
        return 0;

    UINT start_slot = rtv_count;
    UINT uav_count = highest_slot - start_slot + 1;
    if (uav_count > max_uavs)
        uav_count = max_uavs;

    for (UINT i = 0; i < uav_count; i++)
        out_uavs[i] = by_slot[start_slot + i];
    for (UINT i = uav_count; i < max_uavs; i++)
        out_uavs[i] = nullptr;

    if (out_start_slot)
        *out_start_slot = start_slot;
    return uav_count;
}

static void clear_draw_uavs(UINT start_slot, UINT uav_count) {
    if (uav_count == 0 || start_slot >= MAX_UAV_SLOTS)
        return;

    ID3D11UnorderedAccessView* null_uavs[MAX_UAV_SLOTS] = {};
    g_dx.ctx->OMSetRenderTargetsAndUnorderedAccessViews(
        D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr,
        start_slot, uav_count, null_uavs, nullptr);
}

static bool command_is_effectively_enabled(CmdHandle h) {
    int depth = 0;
    while (h != INVALID_HANDLE && depth < MAX_COMMANDS) {
        Command* c = cmd_get(h);
        if (!c || !c->enabled)
            return false;
        h = c->parent;
        depth++;
    }
    return depth < MAX_COMMANDS;
}

static void execute_shadow_prepass_command(CmdHandle h) {
    Command* cp = cmd_get(h);
    if (!cp || !command_is_effectively_enabled(h))
        return;

    Command& c = *cp;
    if (c.type == CMD_GROUP) {
        for (int i = 0; i < MAX_COMMANDS; i++) {
            if (!g_commands[i].active || g_commands[i].parent != h)
                continue;
            execute_shadow_prepass_command((CmdHandle)(i + 1));
        }
        return;
    }

    if (c.type == CMD_REPEAT) {
        int repeat_count = c.repeat_count > 0 ? c.repeat_count : 1;
        for (int repeat_i = 0; repeat_i < repeat_count; repeat_i++) {
            for (int i = 0; i < MAX_COMMANDS; i++) {
                if (!g_commands[i].active || g_commands[i].parent != h)
                    continue;
                execute_shadow_prepass_command((CmdHandle)(i + 1));
            }
        }
        return;
    }

    if (!c.shadow_cast || !is_draw_command(c.type))
        return;

    bool procedural = command_uses_procedural_draw(c);
    Resource* mesh = res_get(c.mesh);
    if (!procedural && (!mesh || !mesh->vb))
        return;
    Resource* indirect_buf = res_get(c.indirect_buf);
    if (c.type == CMD_INDIRECT_DRAW && !is_valid_indirect_draw_call(c, mesh, indirect_buf))
        return;

    ID3D11VertexShader* shadow_vs = g_dx.shadow_vs;
    ID3D11InputLayout* shadow_il = g_dx.shadow_il;
    Resource* shadow_shader = res_get(c.shadow_shader);
    if (shadow_shader && shadow_shader->vs && (procedural || shadow_shader->il)) {
        shadow_vs = shadow_shader->vs;
        shadow_il = shadow_shader->il;
        user_cb_bind_for_command(&c, shadow_shader, true, false, false);
    } else {
        if (procedural)
            return;
        user_cb_bind();
    }

    g_dx.ctx->VSSetShader(shadow_vs, nullptr, 0);
    g_dx.ctx->IASetInputLayout(procedural ? nullptr : shadow_il);
    bind_command_geometry(c, mesh);
    for (int s = 0; s < c.srv_count; s++) {
        Resource* sr = res_get(c.srv_handles[s]);
        ID3D11ShaderResourceView* srv = sr ? sr->srv : nullptr;
        g_dx.ctx->VSSetShaderResources(c.srv_slots[s], 1, &srv);
    }

    if (c.type == CMD_INDIRECT_DRAW) {
        const MeshPart* draw_part = procedural ? nullptr : indirect_draw_part_context(mesh);
        const MeshMaterial* material = mesh_material_for_part(mesh, draw_part);
        ID3D11RasterizerState* rs = rasterizer_state_for(c, material, false);
        g_dx.ctx->RSSetState(rs ? rs : g_dx.rs_solid);
        update_object_cb_for_command(c, draw_part);
        bind_object_cb_for_shader(shadow_shader, true, false);
        if (procedural || !mesh || !mesh->ib) {
            g_dx.ctx->IASetPrimitiveTopology(
                procedural ? command_draw_topology(c) : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_dx.ctx->DrawInstancedIndirect(indirect_buf->buf, c.indirect_offset);
        } else {
            g_dx.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_dx.ctx->DrawIndexedInstancedIndirect(indirect_buf->buf, c.indirect_offset);
        }

        for (int s = 0; s < c.srv_count; s++) {
            ID3D11ShaderResourceView* cleared_srv = nullptr;
            g_dx.ctx->VSSetShaderResources(c.srv_slots[s], 1, &cleared_srv);
        }
        return;
    }

    int part_count = procedural ? 1 : (mesh->mesh_part_count > 0 ? mesh->mesh_part_count : 1);
    for (int pi = 0; pi < part_count; pi++) {
        const MeshPart* part = (!procedural && mesh->mesh_part_count > 0) ? &mesh->mesh_parts[pi] : nullptr;
        if (part && !part->enabled)
            continue;

        const MeshMaterial* material = mesh_material_for_part(mesh, part);
        ID3D11RasterizerState* rs = rasterizer_state_for(c, material, false);
        g_dx.ctx->RSSetState(rs ? rs : g_dx.rs_solid);
        update_object_cb_for_command(c, part);
        bind_object_cb_for_shader(shadow_shader, true, false);
        draw_command_geometry(c, mesh, part);
    }

    for (int s = 0; s < c.srv_count; s++) {
        ID3D11ShaderResourceView* cleared_srv = nullptr;
        g_dx.ctx->VSSetShaderResources(c.srv_slots[s], 1, &cleared_srv);
    }
}

static void execute_shadow_prepass() {
    if (!g_dx.shadow_dsv || !g_dx.shadow_vs || !g_dx.shadow_il)
        return;

    ID3D11ShaderResourceView* null_srv = nullptr;
    g_dx.ctx->PSSetShaderResources(1, 1, &null_srv);
    g_dx.ctx->OMSetRenderTargets(0, nullptr, g_dx.shadow_dsv);
    g_dx.ctx->ClearDepthStencilView(g_dx.shadow_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    g_dx.ctx->OMSetDepthStencilState(g_dx.dss_default, 0);
    float bf[4] = {};
    g_dx.ctx->OMSetBlendState(g_dx.bs_opaque, bf, 0xFFFFFFFF);
    g_dx.ctx->PSSetShader(nullptr, nullptr, 0);

    SceneCBData scene_cb_backup = g_dx.scene_cb_data;
    int cascade_count = (int)scene_cb_backup.shadow_params[0];
    if (cascade_count < 1) cascade_count = 1;
    if (cascade_count > MAX_SHADOW_CASCADES) cascade_count = MAX_SHADOW_CASCADES;

    for (int cascade = 0; cascade < cascade_count; cascade++) {
        const float* rect = scene_cb_backup.shadow_cascade_rects[cascade];
        float scale_x = rect[0] > 0.0f ? rect[0] : 1.0f;
        float scale_y = rect[1] > 0.0f ? rect[1] : 1.0f;
        float offset_x = rect[2];
        float offset_y = rect[3];

        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = offset_x * (float)g_dx.shadow_width;
        vp.TopLeftY = offset_y * (float)g_dx.shadow_height;
        vp.Width = scale_x * (float)g_dx.shadow_width;
        vp.Height = scale_y * (float)g_dx.shadow_height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        g_dx.ctx->RSSetViewports(1, &vp);

        SceneCBData shadow_cb = scene_cb_backup;
        memcpy(shadow_cb.shadow_view_proj,
               scene_cb_backup.shadow_cascade_view_proj[cascade],
               sizeof(shadow_cb.shadow_view_proj));
        dx_update_scene_cb(shadow_cb);

        for (int i = 0; i < MAX_COMMANDS; i++) {
            Command& c = g_commands[i];
            if (!c.active || !c.enabled || c.parent != INVALID_HANDLE)
                continue;
            execute_shadow_prepass_command((CmdHandle)(i + 1));
        }
    }

    dx_update_scene_cb(scene_cb_backup);
}

static void clear_compute_bindings() {
    ID3D11ShaderResourceView* null_srvs[MAX_SRV_SLOTS] = {};
    ID3D11UnorderedAccessView* null_uavs[MAX_UAV_SLOTS] = {};
    g_dx.ctx->CSSetShaderResources(0, MAX_SRV_SLOTS, null_srvs);
    g_dx.ctx->CSSetUnorderedAccessViews(0, MAX_UAV_SLOTS, null_uavs, nullptr);
}

static void bind_compute_resources(Command& c) {
    for (int s = 0; s < c.srv_count; s++) {
        Resource* sr = res_get(c.srv_handles[s]);
        ID3D11ShaderResourceView* srv = sr ? sr->srv : nullptr;
        g_dx.ctx->CSSetShaderResources(c.srv_slots[s], 1, &srv);
    }
    for (int u = 0; u < c.uav_count; u++) {
        Resource* ur = res_get(c.uav_handles[u]);
        ID3D11UnorderedAccessView* uav = ur ? ur->uav : nullptr;
        g_dx.ctx->CSSetUnorderedAccessViews(c.uav_slots[u], 1, &uav, nullptr);
    }
}

static void resolve_dispatch_counts(const Command& c, UINT* out_x, UINT* out_y, UINT* out_z) {
    UINT x = c.thread_x > 0 ? (UINT)c.thread_x : 0;
    UINT y = c.thread_y > 0 ? (UINT)c.thread_y : 0;
    UINT z = c.thread_z > 0 ? (UINT)c.thread_z : 0;
    Resource* explicit_src = res_get(c.dispatch_size_source);
    if (explicit_src) {
        // When a command is driven from a size source we interpret thread_x/y/z
        // as divisors, so a source like 512x512 with 8x8x1 yields 64x64x1
        // dispatch groups. This keeps projects resolution-agnostic.
        int src_x = 1;
        int src_y = 1;
        int src_z = 1;

        switch (explicit_src->type) {
        case RES_INT:
            src_x = explicit_src->ival[0];
            break;
        case RES_INT2:
            src_x = explicit_src->ival[0];
            src_y = explicit_src->ival[1];
            break;
        case RES_INT3:
            src_x = explicit_src->ival[0];
            src_y = explicit_src->ival[1];
            src_z = explicit_src->ival[2];
            break;
        case RES_TEXTURE2D:
        case RES_RENDER_TEXTURE2D:
        case RES_BUILTIN_SCENE_COLOR:
        case RES_BUILTIN_SCENE_DEPTH:
        case RES_BUILTIN_SHADOW_MAP:
            src_x = explicit_src->width;
            src_y = explicit_src->height;
            break;
        case RES_RENDER_TEXTURE3D:
            src_x = explicit_src->width;
            src_y = explicit_src->height;
            src_z = explicit_src->depth;
            break;
        case RES_STRUCTURED_BUFFER:
            src_x = explicit_src->elem_count;
            break;
        default:
            break;
        }

        if (src_x < 1) src_x = 1;
        if (src_y < 1) src_y = 1;
        if (src_z < 1) src_z = 1;
        if (x < 1) x = 1;
        if (y < 1) y = 1;
        if (z < 1) z = 1;
        *out_x = (UINT)((src_x + (int)x - 1) / (int)x);
        *out_y = (UINT)((src_y + (int)y - 1) / (int)y);
        *out_z = (UINT)((src_z + (int)z - 1) / (int)z);
        return;
    }

    if (x > 0 && y > 0 && z > 0) {
        *out_x = x; *out_y = y; *out_z = z;
        return;
    }

    int w = 1;
    int h = 1;
    int d = 1;
    Resource* dim_src = nullptr;
    if (c.uav_count > 0)
        dim_src = res_get(c.uav_handles[0]);
    if (!dim_src && c.srv_count > 0)
        dim_src = res_get(c.srv_handles[0]);

    if (dim_src) {
        if (dim_src->width > 0) w = dim_src->width;
        if (dim_src->height > 0) h = dim_src->height;
        if (dim_src->depth > 0) d = dim_src->depth;
    }

    if (x == 0) x = (UINT)((w + 7) / 8);
    if (y == 0) y = (UINT)((h + 7) / 8);
    if (z == 0) z = (UINT)((d > 1) ? d : 1);
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (z < 1) z = 1;
    *out_x = x;
    *out_y = y;
    *out_z = z;
}

static void execute_dispatch_command(Command& c) {
    Resource* shader = res_get(c.shader);
    validate_dispatch_command(c, shader, false, nullptr);
    if (!shader || !shader->cs) return;
    g_dx.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    clear_compute_bindings();
    g_dx.ctx->CSSetShader(shader->cs, nullptr, 0);
    user_cb_bind_for_command(&c, shader, false, false, true);
    bind_compute_resources(c);

    UINT dispatch_x = 1, dispatch_y = 1, dispatch_z = 1;
    resolve_dispatch_counts(c, &dispatch_x, &dispatch_y, &dispatch_z);
    g_dx.ctx->Dispatch(dispatch_x, dispatch_y, dispatch_z);

    clear_compute_bindings();
    g_dx.ctx->CSSetShader(nullptr, nullptr, 0);
}

static void execute_indirect_dispatch_command(Command& c) {
    Resource* ibuf   = res_get(c.indirect_buf);
    Resource* shader = res_get(c.shader);
    validate_dispatch_command(c, shader, true, ibuf);
    if (!is_valid_indirect_dispatch_call(c, ibuf) || !shader || !shader->cs) return;
    g_dx.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    clear_compute_bindings();
    g_dx.ctx->CSSetShader(shader->cs, nullptr, 0);
    user_cb_bind_for_command(&c, shader, false, false, true);
    bind_compute_resources(c);
    g_dx.ctx->DispatchIndirect(ibuf->buf, c.indirect_offset);
    clear_compute_bindings();
    g_dx.ctx->CSSetShader(nullptr, nullptr, 0);
}

static void execute_command_handle(CmdHandle h, bool& shadow_prepass_done);
static void execute_repeat_command(CmdHandle repeat_h, Command& repeat, bool& shadow_prepass_done) {
    int count = repeat.repeat_count;
    if (count < 1) count = 1;

    for (int pass = 0; pass < count; pass++) {
        for (int i = 0; i < MAX_COMMANDS; i++) {
            Command& child = g_commands[i];
            if (!child.active || child.parent != repeat_h)
                continue;

            CmdHandle child_h = (CmdHandle)(i + 1);
            execute_command_handle(child_h, shadow_prepass_done);
        }
    }
}

static void execute_command_children(CmdHandle parent_h, bool& shadow_prepass_done);
static void execute_command_handle(CmdHandle h, bool& shadow_prepass_done) {
    Command* cp = cmd_get(h);
    if (!cp || !command_is_effectively_enabled(h))
        return;

    Command& c = *cp;
    if ((c.type == CMD_DISPATCH || c.type == CMD_INDIRECT_DISPATCH) &&
        c.compute_on_reset && !s_cmd_reset_execution) {
        return;
    }

    int gpu_event = cmd_gpu_begin_command(h);

    switch (c.type) {

    case CMD_CLEAR: {
        ID3D11RenderTargetView* rtv = c.clear_color_enabled ? get_rtv(c.rt) : nullptr;
        ID3D11DepthStencilView* dsv = get_draw_dsv(c);
        if (rtv && c.clear_color_enabled) g_dx.ctx->ClearRenderTargetView(rtv, c.clear_color);
        if (dsv && c.clear_depth)
            g_dx.ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, c.depth_clear_val, 0);
        break;
    }

    case CMD_GROUP: {
        execute_command_children(h, shadow_prepass_done);
        break;
    }

    case CMD_DRAW_MESH:
    case CMD_DRAW_INSTANCED: {
        bool procedural = command_uses_procedural_draw(c);
        Resource* mesh   = res_get(c.mesh);
        Resource* shader = res_get(c.shader);
        validate_draw_command(c, mesh, shader, procedural, false, nullptr);
        if (!shader || !shader->vs || !shader->ps)       break;
        if (!procedural && (!mesh || !mesh->vb))         break;

        if (c.shadow_receive && !shadow_prepass_done) {
            execute_shadow_prepass();
            shadow_prepass_done = true;
        }

        g_dx.ctx->VSSetShader(shader->vs, nullptr, 0);
        g_dx.ctx->PSSetShader(shader->ps, nullptr, 0);
        g_dx.ctx->IASetInputLayout(procedural ? nullptr : shader->il);
        user_cb_bind_for_command(&c, shader, true, true, false);

        ID3D11RenderTargetView* rtvs[MAX_DRAW_RENDER_TARGETS] = {};
        UINT rtv_count = collect_draw_rtvs(c, rtvs, MAX_DRAW_RENDER_TARGETS);
        ID3D11DepthStencilView* dsv = get_draw_dsv(c);
        ID3D11UnorderedAccessView* om_uavs[MAX_UAV_SLOTS] = {};
        UINT uav_start_slot = 0;
        UINT om_uav_count = collect_draw_uavs(c, rtv_count, &uav_start_slot, om_uavs, MAX_UAV_SLOTS);
        if (om_uav_count > 0) {
            g_dx.ctx->OMSetRenderTargetsAndUnorderedAccessViews(
                rtv_count, rtv_count > 0 ? rtvs : nullptr, dsv,
                uav_start_slot, om_uav_count, om_uavs, nullptr);
        } else if (rtv_count > 0) {
            g_dx.ctx->OMSetRenderTargets(rtv_count, rtvs, dsv);
        } else {
            g_dx.ctx->OMSetRenderTargets(0, nullptr, dsv);
        }
        set_viewport_for_draw_outputs(c);
        bind_command_geometry(c, mesh);

        for (int s = 0; s < c.srv_count; s++) {
            Resource* sr = res_get(c.srv_handles[s]);
            ID3D11ShaderResourceView* srv = sr ? sr->srv : nullptr;
            g_dx.ctx->VSSetShaderResources(c.srv_slots[s], 1, &srv);
        }

        int part_count = procedural ? 1 : (mesh->mesh_part_count > 0 ? mesh->mesh_part_count : 1);
        for (int pi = 0; pi < part_count; pi++) {
            const MeshPart* part = (!procedural && mesh->mesh_part_count > 0) ? &mesh->mesh_parts[pi] : nullptr;
            if (part && !part->enabled)
                continue;

            const MeshMaterial* material = mesh_material_for_part(mesh, part);
            apply_draw_state(c, material);
            update_object_cb_for_command(c, part);
            bind_object_cb_for_shader(shader, true, true);
            bind_mesh_material_textures(mesh, part);

            for (int t = 0; t < c.tex_count; t++) {
                Resource* tr = res_get(c.tex_handles[t]);
                ID3D11ShaderResourceView* srv = tr ? tr->srv : nullptr;
                g_dx.ctx->PSSetShaderResources(c.tex_slots[t], 1, &srv);
            }
            if (c.shadow_receive) {
                ID3D11ShaderResourceView* srv = g_dx.shadow_srv;
                g_dx.ctx->PSSetShaderResources(k_shadow_map_ps_slot, 1, &srv);
            }

            draw_command_geometry(c, mesh, part);
        }

        ID3D11ShaderResourceView* null_ps_srvs[MAX_TEX_SLOTS] = {};
        g_dx.ctx->PSSetShaderResources(0, MAX_TEX_SLOTS, null_ps_srvs);
        for (int s = 0; s < c.srv_count; s++) {
            ID3D11ShaderResourceView* null_srv = nullptr;
            g_dx.ctx->VSSetShaderResources(c.srv_slots[s], 1, &null_srv);
        }
        clear_draw_uavs(uav_start_slot, om_uav_count);
        break;
    }

    case CMD_DISPATCH: {
        execute_dispatch_command(c);
        break;
    }

    case CMD_INDIRECT_DRAW: {
        Resource* ibuf   = res_get(c.indirect_buf);
        Resource* mesh   = res_get(c.mesh);
        Resource* shader = res_get(c.shader);
        bool procedural = command_uses_procedural_draw(c);
        const MeshPart* draw_part = procedural ? nullptr : indirect_draw_part_context(mesh);
        const MeshMaterial* material = mesh_material_for_part(mesh, draw_part);
        validate_draw_command(c, mesh, shader, procedural, true, ibuf);
        if (!is_valid_indirect_draw_call(c, mesh, ibuf) || !shader || !shader->vs || !shader->ps) break;
        if (!procedural && (!mesh || !mesh->vb)) break;
        if (c.shadow_receive && !shadow_prepass_done) {
            execute_shadow_prepass();
            shadow_prepass_done = true;
        }

        g_dx.ctx->VSSetShader(shader->vs, nullptr, 0);
        g_dx.ctx->PSSetShader(shader->ps, nullptr, 0);
        g_dx.ctx->IASetInputLayout(procedural ? nullptr : shader->il);
        user_cb_bind_for_command(&c, shader, true, true, false);

        ID3D11RenderTargetView* rtvs[MAX_DRAW_RENDER_TARGETS] = {};
        UINT rtv_count = collect_draw_rtvs(c, rtvs, MAX_DRAW_RENDER_TARGETS);
        ID3D11DepthStencilView* dsv = get_draw_dsv(c);
        ID3D11UnorderedAccessView* om_uavs[MAX_UAV_SLOTS] = {};
        UINT uav_start_slot = 0;
        UINT om_uav_count = collect_draw_uavs(c, rtv_count, &uav_start_slot, om_uavs, MAX_UAV_SLOTS);
        if (om_uav_count > 0) {
            g_dx.ctx->OMSetRenderTargetsAndUnorderedAccessViews(
                rtv_count, rtv_count > 0 ? rtvs : nullptr, dsv,
                uav_start_slot, om_uav_count, om_uavs, nullptr);
        } else if (rtv_count > 0) {
            g_dx.ctx->OMSetRenderTargets(rtv_count, rtvs, dsv);
        } else {
            g_dx.ctx->OMSetRenderTargets(0, nullptr, dsv);
        }
        set_viewport_for_draw_outputs(c);
        bind_command_geometry(c, mesh);

        for (int s = 0; s < c.srv_count; s++) {
            Resource* sr = res_get(c.srv_handles[s]);
            ID3D11ShaderResourceView* srv = sr ? sr->srv : nullptr;
            g_dx.ctx->VSSetShaderResources(c.srv_slots[s], 1, &srv);
        }

        apply_draw_state(c, material);
        update_object_cb_for_command(c, draw_part);
        bind_object_cb_for_shader(shader, true, true);
        bind_mesh_material_textures(mesh, draw_part);

        for (int t = 0; t < c.tex_count; t++) {
            Resource* tr = res_get(c.tex_handles[t]);
            ID3D11ShaderResourceView* srv = tr ? tr->srv : nullptr;
            g_dx.ctx->PSSetShaderResources(c.tex_slots[t], 1, &srv);
        }
        if (c.shadow_receive) {
            ID3D11ShaderResourceView* srv = g_dx.shadow_srv;
            g_dx.ctx->PSSetShaderResources(k_shadow_map_ps_slot, 1, &srv);
        }

        if (procedural || !mesh->ib) {
            if (procedural) {
                g_dx.ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
                g_dx.ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
                g_dx.ctx->IASetPrimitiveTopology(command_draw_topology(c));
            }
            g_dx.ctx->DrawInstancedIndirect(ibuf->buf, c.indirect_offset);
        } else {
            g_dx.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_dx.ctx->DrawIndexedInstancedIndirect(ibuf->buf, c.indirect_offset);
        }

        ID3D11ShaderResourceView* null_ps_srvs[MAX_TEX_SLOTS] = {};
        g_dx.ctx->PSSetShaderResources(0, MAX_TEX_SLOTS, null_ps_srvs);
        for (int s = 0; s < c.srv_count; s++) {
            ID3D11ShaderResourceView* null_srv = nullptr;
            g_dx.ctx->VSSetShaderResources(c.srv_slots[s], 1, &null_srv);
        }
        clear_draw_uavs(uav_start_slot, om_uav_count);
        break;
    }

    case CMD_INDIRECT_DISPATCH: {
        execute_indirect_dispatch_command(c);
        break;
    }

    case CMD_REPEAT: {
        execute_repeat_command(h, c, shadow_prepass_done);
        break;
    }

    default: break;
    }

    cmd_gpu_end_command(gpu_event);
}

static void execute_command_children(CmdHandle parent_h, bool& shadow_prepass_done) {
    for (int i = 0; i < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active || c.parent != parent_h)
            continue;
        execute_command_handle((CmdHandle)(i + 1), shadow_prepass_done);
    }
}

// Execute the visible command list in editor order. Child/group commands are
// expanded by the traversal helpers that feed this top-level entry point.
void cmd_execute_all() {
    bool shadow_prepass_done = false;
    cmd_profile_sync_enable_state();

    if (g_profiler_enabled && !s_gpu_active_slot)
        cmd_profile_begin_frame_capture();
    else if (!g_profiler_enabled)
        s_gpu_active_slot = nullptr;

    if (g_profiler_enabled && s_gpu_active_slot && !s_gpu_active_slot->command_range_issued) {
        g_dx.ctx->End(s_gpu_active_slot->frame_begin);
        s_gpu_active_slot->command_range_issued = true;
    }

    execute_command_children(INVALID_HANDLE, shadow_prepass_done);

    if (g_profiler_enabled)
        cmd_gpu_end_command_frame();
}

void cmd_make_unique_name(const char* base, char* out, int out_sz) {
    cmd_make_unique_name_except(base, out, out_sz, INVALID_HANDLE);
}

void cmd_profile_begin_frame_capture() {
    cmd_profile_sync_enable_state();
    if (!g_profiler_enabled) {
        s_gpu_active_slot = nullptr;
        s_gpu_frame_capture_open = false;
        return;
    }

    cmd_gpu_begin_total_frame();
    s_gpu_frame_capture_open = s_gpu_active_slot != nullptr;
}

void cmd_profile_end_frame_capture() {
    if (!s_gpu_frame_capture_open)
        return;

    if (!s_gpu_active_slot) {
        s_gpu_frame_capture_open = false;
        return;
    }

    if (!s_gpu_active_slot->command_range_issued) {
        g_dx.ctx->End(s_gpu_active_slot->frame_begin);
        g_dx.ctx->End(s_gpu_active_slot->frame_end);
        s_gpu_active_slot->command_range_issued = true;
    }
    cmd_gpu_end_total_frame();
    s_gpu_frame_capture_open = false;
}
