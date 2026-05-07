#include "ui.h"
#include "resources.h"
#include "commands.h"
#include "project.h"
#include "app_settings.h"
#include "user_cb.h"
#include "dx11_ctx.h"
#include "log.h"
#include "shader.h"
#include "embedded_pack.h"
#include "timeline.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "nanosvg/nanosvg.h"
#include <d3dcompiler.h>
#include <psapi.h>
#include <float.h>
#include <stdlib.h>
#include <vector>
#pragma comment(lib, "psapi.lib")

// The UI module is the editor shell. It presents resources, commands,
// inspectors, logs, and the live scene view on top of the runtime state.

ResHandle g_sel_res = INVALID_HANDLE;
CmdHandle g_sel_cmd = INVALID_HANDLE;
bool g_scene_view_hovered = false;

static bool s_rename_active = false;
static char s_rename_buf[MAX_NAME] = {};
static bool s_rename_is_cmd = false;
static char s_project_path[MAX_PATH_LEN] = "project.lt";
static ResHandle s_res_nav = INVALID_HANDLE;
static CmdHandle s_cmd_nav = INVALID_HANDLE;

enum ProjectFileMode {
    PROJECT_FILE_NONE = 0,
    PROJECT_FILE_SAVE,
    PROJECT_FILE_LOAD
};

static ProjectFileMode s_project_file_mode = PROJECT_FILE_NONE;
static bool s_project_path_focus = false;
static bool s_viewport_fullscreen = false;
static bool s_right_panel_general_open = false;
static bool s_shortcuts_popup_open = false;
static bool s_timeline_window_open = false;
static bool s_timeline_keyboard_focus = false;
static int s_timeline_visible_first_frame = 0;
static bool s_timeline_ensure_current_visible = false;
static bool s_scene_surface_resize_armed = true;
static int s_scene_surface_host_w = 0;
static int s_scene_surface_host_h = 0;
static bool s_scene_surface_fullscreen = false;
static RECT s_ui_top_toolbar_screen_rect = {};
static bool s_ui_top_toolbar_screen_rect_valid = false;
static RECT s_ui_window_control_screen_rects[3] = {};
static bool s_ui_window_control_screen_rects_valid[3] = {};
static const float k_ui_scale_default = 1.06f;
static const float k_ui_scale_min = 0.75f;
static const float k_ui_scale_max = 1.25f;
static float s_ui_global_scale = k_ui_scale_default;
static bool s_ui_scale_dirty = false;
static ImGuiStyle s_ui_base_style = {};
static bool s_ui_base_style_valid = false;
static ImFont* s_code_font = nullptr;
static float s_code_font_size = 16.0f;
static bool s_shader_editor_auto_save_compile = false;
static bool s_shader_source_editor_focused = false;

enum UiViewportGizmoMode {
    UI_GIZMO_NONE = 0,
    UI_GIZMO_TRANSLATE,
    UI_GIZMO_ROTATE,
    UI_GIZMO_SCALE
};

struct UiViewportGizmoDrag {
    bool                 active;
    UiViewportGizmoMode  mode;
    CmdHandle            cmd;
    int                  axis;
    float                initial_pos[3];
    float                initial_rot[3];
    float                initial_scale[3];
    float                axis_world_len;
    float                axis_screen_len;
    ImVec2               mouse_start;
    ImVec2               origin_screen;
    ImVec2               axis_end_screen;
    Mat4                 initial_rot_matrix;
    ImVec2               ring_basis_u_screen;
    ImVec2               ring_basis_v_screen;
    float                ring_start_angle;
};

static UiViewportGizmoMode s_viewport_gizmo_mode = UI_GIZMO_NONE;
static UiViewportGizmoDrag s_viewport_gizmo_drag = {};

struct UiCommandClipboardEntry {
    Command cmd;
    int     parent_index;
};

static UiCommandClipboardEntry s_cmd_clipboard[MAX_COMMANDS] = {};
static int s_cmd_clipboard_count = 0;

static bool ui_begin_shortcut_section(const char* id, const char* title, ImGuiTableFlags table_flags);
static void ui_draw_shortcut_row(const char* key, const char* desc);
static void ui_align_frame_row(float row_y);
static void ui_align_text_row(float row_y);
static float ui_px(float v);

struct PathCandidate {
    char display[MAX_PATH_LEN];
    char value[MAX_PATH_LEN];
    bool is_dir;
};

struct PathInputResult {
    bool changed;
    bool file_selected;
    bool dir_selected;
};

struct PathInputCallbackState {
    bool completion_requested;
    int nav_delta;
};

static int ui_path_completion_callback(ImGuiInputTextCallbackData* data) {
    if (!data || !data->UserData)
        return 0;

    PathInputCallbackState* state = (PathInputCallbackState*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        state->completion_requested = true;
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (data->EventKey == ImGuiKey_DownArrow) state->nav_delta = 1;
        if (data->EventKey == ImGuiKey_UpArrow) state->nav_delta = -1;
    }
    return 0;
}

static bool ui_prefix_ci(const char* text, const char* prefix) {
    if (!prefix || !prefix[0]) return true;
    if (!text) return false;
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static float ui_clamp_global_scale(float scale) {
    if (scale < k_ui_scale_min) return k_ui_scale_min;
    if (scale > k_ui_scale_max) return k_ui_scale_max;
    return scale;
}

static float ui_chrome_scale() {
    return s_ui_global_scale < 1.0f ? 1.0f : s_ui_global_scale;
}

static float ui_px(float v) {
    return floorf(v * s_ui_global_scale + 0.5f);
}

static float ui_margin_px(float v) {
    return floorf(v * ui_chrome_scale() + 0.5f);
}

static void ui_apply_global_scale_now() {
    if (!ImGui::GetCurrentContext() || !s_ui_base_style_valid)
        return;

    ImGuiStyle& style = ImGui::GetStyle();
    style = s_ui_base_style;
    style.ScaleAllSizes(ui_chrome_scale());
    ImGui::GetIO().FontGlobalScale = s_ui_global_scale;
}

static bool ui_ext_allowed(const char* name, const char* filter) {
    if (!filter || !filter[0]) return true;
    const char* ext = strrchr(name, '.');
    if (!ext) return false;

    const char* p = filter;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '|') p++;
        const char* start = p;
        while (*p && *p != ';' && *p != '|') p++;
        int len = (int)(p - start);
        if (len > 0 && (int)strlen(ext) == len && _strnicmp(ext, start, len) == 0)
            return true;
    }
    return false;
}

static void ui_split_path_for_completion(const char* path, char* dir, int dir_sz,
                                         char* base, int base_sz, char* prefix, int prefix_sz,
                                         char* sep)
{
    dir[0] = base[0] = prefix[0] = '\0';
    *sep = '\\';
    if (!path || !path[0]) {
        strncpy(dir, ".", dir_sz - 1);
        dir[dir_sz - 1] = '\0';
        return;
    }

    int len = (int)strlen(path);
    if (len == 2 && path[1] == ':') {
        snprintf(dir, dir_sz, "%s\\", path);
        snprintf(base, base_sz, "%s\\", path);
        return;
    }

    const char* slash1 = strrchr(path, '/');
    const char* slash2 = strrchr(path, '\\');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    if (!slash) {
        strncpy(dir, ".", dir_sz - 1);
        dir[dir_sz - 1] = '\0';
        strncpy(prefix, path, prefix_sz - 1);
        prefix[prefix_sz - 1] = '\0';
        return;
    }

    *sep = *slash;
    int base_len = (int)(slash - path) + 1;
    if (base_len >= base_sz) base_len = base_sz - 1;
    memcpy(base, path, base_len);
    base[base_len] = '\0';

    int dir_len = (int)(slash - path);
    if (dir_len == 0) dir_len = 1;
    if (dir_len == 2 && path[1] == ':') dir_len = 3;
    if (dir_len >= dir_sz) dir_len = dir_sz - 1;
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';

    strncpy(prefix, slash + 1, prefix_sz - 1);
    prefix[prefix_sz - 1] = '\0';
}

static int ui_collect_path_candidates(const char* path, const char* ext_filter,
                                      PathCandidate* out, int out_count)
{
    if (!out || out_count <= 0) return 0;

    char dir[MAX_PATH_LEN] = {};
    char base[MAX_PATH_LEN] = {};
    char prefix[MAX_PATH_LEN] = {};
    char sep = '\\';
    ui_split_path_for_completion(path, dir, MAX_PATH_LEN, base, MAX_PATH_LEN,
                                 prefix, MAX_PATH_LEN, &sep);

    char pattern[MAX_PATH_LEN] = {};
    int dir_len = (int)strlen(dir);
    if (strcmp(dir, ".") == 0) {
        strncpy(pattern, "*", MAX_PATH_LEN - 1);
    } else if (dir_len > 0 && (dir[dir_len - 1] == '\\' || dir[dir_len - 1] == '/')) {
        snprintf(pattern, MAX_PATH_LEN, "%s*", dir);
    } else {
        snprintf(pattern, MAX_PATH_LEN, "%s\\*", dir);
    }
    pattern[MAX_PATH_LEN - 1] = '\0';

    int count = 0;
    WIN32_FIND_DATAA fd = {};
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return 0;

    do {
        const char* name = fd.cFileName;
        if (strcmp(name, ".") == 0) continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!ui_prefix_ci(name, prefix)) continue;
        if (!is_dir && !ui_ext_allowed(name, ext_filter)) continue;
        if (count >= out_count) break;

        PathCandidate& c = out[count++];
        c.is_dir = is_dir;
        snprintf(c.display, MAX_PATH_LEN, "%s%s", name, is_dir ? "/" : "");
        snprintf(c.value, MAX_PATH_LEN, "%s%s%s", base, name, is_dir ? (sep == '/' ? "/" : "\\") : "");
        c.display[MAX_PATH_LEN - 1] = '\0';
        c.value[MAX_PATH_LEN - 1] = '\0';
    } while (FindNextFileA(find, &fd));

    FindClose(find);
    return count;
}

static void ui_apply_path_candidate(const PathCandidate& c, char* buf, int buf_sz,
                                    PathInputResult* result)
{
    strncpy(buf, c.value, buf_sz - 1);
    buf[buf_sz - 1] = '\0';
    result->changed = true;
    result->dir_selected = c.is_dir;
    result->file_selected = !c.is_dir;
}

static PathInputResult ui_path_input_ex(const char* label, char* buf, int buf_sz, const char* ext_filter,
                                        ImGuiInputTextFlags extra_flags = 0) {
    static ImGuiID s_refocus_id = 0;
    static ImGuiID s_open_id = 0;
    static int s_nav_index = 0;

    ImGuiID path_id = ImGui::GetID(label);
    if (s_refocus_id == path_id) {
        ImGui::SetKeyboardFocusHere();
        s_refocus_id = 0;
    }

    PathInputResult result = {};
    PathInputCallbackState cb_state = {};
    bool enter_requested = ImGui::InputText(label, buf, buf_sz,
        ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory |
        ImGuiInputTextFlags_EnterReturnsTrue | extra_flags,
        ui_path_completion_callback, &cb_state);
    ImGui::SetItemKeyOwner(ImGuiKey_UpArrow);
    ImGui::SetItemKeyOwner(ImGuiKey_DownArrow);
    ImGui::SetItemKeyOwner(ImGuiKey_Tab);
    ImGui::SetItemKeyOwner(ImGuiKey_Enter);
    ImGui::SetItemKeyOwner(ImGuiKey_KeypadEnter);
    result.changed = ImGui::IsItemEdited();
    bool activated = ImGui::IsItemActivated();
    bool active = ImGui::IsItemActive();
    ImVec2 input_min = ImGui::GetItemRectMin();
    ImVec2 input_max = ImGui::GetItemRectMax();

    ImGui::PushID(label);
    if (activated || (active && result.changed)) {
        if (s_open_id != path_id)
            s_nav_index = 0;
    }
    if (cb_state.completion_requested || cb_state.nav_delta != 0 || enter_requested ||
        activated || (active && result.changed)) {
        if (s_open_id != path_id)
            s_nav_index = 0;
        s_open_id = path_id;
    }

    if (s_open_id == path_id) {
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        char popup_name[64] = {};
        snprintf(popup_name, sizeof(popup_name), "##path_complete_%08X", (unsigned int)path_id);
        float popup_w = input_max.x - input_min.x;
        if (popup_w < 180.0f) popup_w = 180.0f;
        ImGui::SetNextWindowPos({input_min.x, input_max.y}, ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints({popup_w, 0.0f}, {popup_w, 240.0f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.100f, 0.096f, 0.100f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::Begin(popup_name, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_AlwaysAutoResize);
        bool popup_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        PathCandidate candidates[64] = {};
        int count = ui_collect_path_candidates(buf, ext_filter, candidates, 64);
        if (count == 0) {
            ImGui::TextDisabled("No matches");
        } else {
            if (s_nav_index < 0) s_nav_index = 0;
            if (s_nav_index >= count) s_nav_index = count - 1;

            bool nav_moved = false;
            bool accepted = false;
            if (active || enter_requested || cb_state.completion_requested ||
                cb_state.nav_delta != 0 || ImGui::IsWindowFocused(ImGuiFocusedFlags_None)) {
                if (cb_state.nav_delta != 0) {
                    s_nav_index += cb_state.nav_delta;
                    if (s_nav_index < 0) s_nav_index = 0;
                    if (s_nav_index >= count) s_nav_index = count - 1;
                    nav_moved = true;
                }

                bool accept = cb_state.completion_requested ||
                    enter_requested ||
                    ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
                if (accept) {
                    ui_apply_path_candidate(candidates[s_nav_index], buf, buf_sz, &result);
                    if (!candidates[s_nav_index].is_dir)
                        s_open_id = 0;
                    s_refocus_id = path_id;
                    s_nav_index = 0;
                    accepted = true;
                }
            }

            if (!accepted) {
                for (int i = 0; i < count; i++) {
                    ImGuiSelectableFlags flags = candidates[i].is_dir ? ImGuiSelectableFlags_NoAutoClosePopups : 0;
                    bool selected = i == s_nav_index;
                    if (ImGui::Selectable(candidates[i].display, selected, flags)) {
                        ui_apply_path_candidate(candidates[i], buf, buf_sz, &result);
                        if (!candidates[i].is_dir)
                            s_open_id = 0;
                        s_refocus_id = path_id;
                        s_nav_index = 0;
                    }
                    if (ImGui::IsItemHovered())
                        s_nav_index = i;
                    if (selected && nav_moved)
                        ImGui::SetScrollHereY(0.5f);
                }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            s_open_id = 0;
        if (!active && !popup_hovered && ImGui::IsMouseClicked(0))
            s_open_id = 0;
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    ImGui::PopID();

    return result;
}

static bool ui_path_input(const char* label, char* buf, int buf_sz, const char* ext_filter) {
    return ui_path_input_ex(label, buf, buf_sz, ext_filter).changed;
}

static void ui_sync_commands_for_shader(ResHandle shader_h) {
    for (int i = 0; i < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active || c.shader != shader_h) continue;
        user_cb_sync_command_params(&c, res_get(shader_h));
    }
}

static bool ui_recompile_shader_resource(ResHandle h, Resource* r, const char* path) {
    if (!r || r->type != RES_SHADER) return false;
    bool was_cs = r->cs != nullptr;
    char local_path[MAX_PATH_LEN] = {};
    strncpy(local_path, path ? path : r->path, MAX_PATH_LEN - 1);
    local_path[MAX_PATH_LEN - 1] = '\0';

    bool ok = was_cs
        ? shader_compile_cs(r, local_path, "CSMain")
        : shader_compile_vs_ps(r, local_path, "VSMain", "PSMain");
    ui_sync_commands_for_shader(h);
    return ok;
}

static bool ui_reload_mesh_resource(Resource* r, const char* path) {
    if (!r || r->type != RES_MESH || !path || !path[0])
        return false;

    strncpy(r->path, path, MAX_PATH_LEN - 1);
    r->path[MAX_PATH_LEN - 1] = '\0';
    res_release_gpu(r);

    ResHandle tmp = res_load_mesh(r->name, r->path);
    if (tmp == INVALID_HANDLE)
        return false;

    Resource* src = res_get(tmp);
    if (src) {
        r->vb = src->vb; r->ib = src->ib;
        r->vert_count = src->vert_count;
        r->idx_count  = src->idx_count;
        r->vert_stride= src->vert_stride;
        r->mesh_part_count = src->mesh_part_count;
        r->mesh_material_count = src->mesh_material_count;
        memcpy(r->mesh_parts, src->mesh_parts, sizeof(r->mesh_parts));
        memcpy(r->mesh_materials, src->mesh_materials, sizeof(r->mesh_materials));
        r->mesh_primitive_type = src->mesh_primitive_type;
        r->compiled_ok = src->compiled_ok;
        r->using_fallback = src->using_fallback;
        strncpy(r->compile_err, src->compile_err, sizeof(r->compile_err) - 1);
        r->compile_err[sizeof(r->compile_err) - 1] = '\0';
        src->vb = nullptr; src->ib = nullptr;
        src->mesh_part_count = 0;
        src->mesh_material_count = 0;
    }
    res_free(tmp);
    return true;
}

static const char* ui_mesh_material_slot_name(int slot) {
    switch (slot) {
    case 0: return "Base Color";
    case 1: return "Metal Rough";
    case 2: return "Normal";
    case 3: return "Emissive";
    case 4: return "Occlusion";
    default: return "Texture";
    }
}

static const char* ui_draw_texture_slot_usage(int slot) {
    switch (slot) {
    case 0: return "Mesh material Base Color";
    case 1: return "Mesh material Metal Rough";
    case 2: return "Mesh material Normal";
    case 3: return "Mesh material Emissive";
    case 4: return "Mesh material Occlusion";
    case 5: return "Environment map for PBR";
    case 7: return "Shadow map when Shadow Receiver is on";
    default: return "Free / user-defined";
    }
}

static void ui_draw_texture_slot_row(const char* slot, const char* usage) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", slot);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(usage);
}

static void ui_draw_texture_slot_reference(const char* table_id) {
    if (!ImGui::BeginTable(table_id, 2,
            ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_PadOuterX))
        return;

    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 116.0f);
    ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthStretch);

    ui_draw_texture_slot_row("t0", ui_draw_texture_slot_usage(0));
    ui_draw_texture_slot_row("t1", ui_draw_texture_slot_usage(1));
    ui_draw_texture_slot_row("t2", ui_draw_texture_slot_usage(2));
    ui_draw_texture_slot_row("t3", ui_draw_texture_slot_usage(3));
    ui_draw_texture_slot_row("t4", ui_draw_texture_slot_usage(4));
    ui_draw_texture_slot_row("t5", ui_draw_texture_slot_usage(5));
    ui_draw_texture_slot_row("t6", ui_draw_texture_slot_usage(6));
    ui_draw_texture_slot_row("t7", ui_draw_texture_slot_usage(7));
    ImGui::EndTable();
}

static bool ui_command_uses_procedural_draw(const Command& c) {
    return c.draw_source == DRAW_SOURCE_PROCEDURAL;
}

static const char* ui_draw_source_name(int source) {
    switch ((DrawSourceType)source) {
    case DRAW_SOURCE_PROCEDURAL: return "Procedural";
    case DRAW_SOURCE_MESH:
    default:                     return "Mesh";
    }
}

static bool ui_draw_source_combo(const char* label, int* source) {
    if (!source)
        return false;

    bool changed = false;
    if (ImGui::BeginCombo(label, ui_draw_source_name(*source))) {
        const DrawSourceType options[] = { DRAW_SOURCE_MESH, DRAW_SOURCE_PROCEDURAL };
        for (int i = 0; i < 2; i++) {
            bool selected = *source == (int)options[i];
            if (ImGui::Selectable(ui_draw_source_name((int)options[i]), selected)) {
                *source = (int)options[i];
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static const char* ui_draw_topology_name(int topology) {
    switch ((DrawTopologyType)topology) {
    case DRAW_TOPOLOGY_POINT_LIST:    return "Point List";
    case DRAW_TOPOLOGY_TRIANGLE_LIST:
    default:                          return "Triangle List";
    }
}

static bool ui_draw_topology_combo(const char* label, int* topology) {
    if (!topology)
        return false;

    bool changed = false;
    if (ImGui::BeginCombo(label, ui_draw_topology_name(*topology))) {
        const DrawTopologyType options[] = { DRAW_TOPOLOGY_TRIANGLE_LIST, DRAW_TOPOLOGY_POINT_LIST };
        for (int i = 0; i < 2; i++) {
            bool selected = *topology == (int)options[i];
            if (ImGui::Selectable(ui_draw_topology_name((int)options[i]), selected)) {
                *topology = (int)options[i];
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void ui_recompile_all_shaders() {
    int total = 0;
    int ok = 0;
    int fallback = 0;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (!r.active || r.type != RES_SHADER) continue;
        ResHandle h = (ResHandle)(i + 1);
        total++;
        bool compiled = ui_recompile_shader_resource(h, &r, r.path);
        if (compiled && r.compiled_ok)
            ok++;
        else
            fallback++;
    }
    log_info("Recompiled shaders: %d total, %d OK, %d fallback/error", total, ok, fallback);
}

static void ui_recompile_selected_shader() {
    ResHandle h = INVALID_HANDLE;
    if (Resource* selected = res_get(g_sel_res)) {
        if (selected->type == RES_SHADER)
            h = g_sel_res;
    }
    if (h == INVALID_HANDLE) {
        if (Command* c = cmd_get(g_sel_cmd))
            h = c->shader;
    }

    Resource* r = res_get(h);
    if (!r || r->type != RES_SHADER) {
        log_warn("No selected shader to compile.");
        return;
    }
    ui_recompile_shader_resource(h, r, r->path);
}

static void ui_make_standalone_output_path(const char* project_path, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!project_path || !project_path[0])
        return;

    strncpy(out, project_path, out_sz - 1);
    out[out_sz - 1] = '\0';
    const char* slash1 = strrchr(out, '/');
    const char* slash2 = strrchr(out, '\\');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    char* base = slash ? (char*)slash + 1 : out;
    char* dot = strrchr(base, '.');
    if (dot)
        *dot = '\0';

    int len = (int)strlen(out);
    snprintf(out + len, out_sz - len, "_standalone.exe");
}

static void ui_export_current_project_single_exe() {
    const char* project_path = project_current_path();
    if (!project_path || !project_path[0]) {
        log_warn("Export EXE needs a saved project path first.");
        return;
    }

    if (!project_save_text(project_path))
        return;

    char exe_path[MAX_PATH_LEN] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH_LEN);
    exe_path[MAX_PATH_LEN - 1] = '\0';

    char output_path[MAX_PATH_LEN] = {};
    ui_make_standalone_output_path(project_path, output_path, MAX_PATH_LEN);
    if (!output_path[0]) {
        log_error("Export EXE failed: could not build output path.");
        return;
    }

    char err[8192] = {};
    if (!lt_export_normal_exe(exe_path, project_path, output_path, err, sizeof(err))) {
        log_error("Export EXE failed: %s", err[0] ? err : "unknown error");
        return;
    }

    log_info("Standalone EXE exported: %s", output_path);
}

static void ui_project_file_bar() {
    if (s_project_file_mode == PROJECT_FILE_NONE)
        return;

    const bool save = s_project_file_mode == PROJECT_FILE_SAVE;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {5.0f, 4.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
    float bar_h = ImGui::GetFrameHeight() + 10.0f;
    ImGui::BeginChild("##project_file_bar", {0.0f, bar_h}, true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorPosY((bar_h - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(save ? "Save project" : "Load project");
    ImGui::SameLine();

    float buttons_w = 118.0f;
    float input_w = ImGui::GetContentRegionAvail().x - buttons_w;
    if (input_w < 120.0f) input_w = 120.0f;
    ImGui::SetNextItemWidth(input_w);
    bool focus_path = s_project_path_focus;
    if (focus_path) {
        ImGui::SetKeyboardFocusHere();
        s_project_path_focus = false;
    }
    PathInputResult path_result = ui_path_input_ex("##project_path", s_project_path, MAX_PATH_LEN, ".lt",
        focus_path ? ImGuiInputTextFlags_AutoSelectAll : 0);
    if (!save && path_result.file_selected) {
        project_load_text(s_project_path);
        s_project_file_mode = PROJECT_FILE_NONE;
    }
    ImGui::SameLine();

    if (ImGui::Button(save ? "Save" : "Load")) {
        if (save)
            project_save_text(s_project_path);
        else
            project_load_text(s_project_path);
        s_project_file_mode = PROJECT_FILE_NONE;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        s_project_file_mode = PROJECT_FILE_NONE;

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// ── resource combo helper ─────────────────────────────────────────────────

static bool ui_resource_has_implicit_size_source(const Resource& r) {
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
        return false;
    }
}

static ResHandle ui_resource_handle_from_ptr(const Resource* r) {
    if (!r) return INVALID_HANDLE;
    int idx = (int)(r - g_resources);
    if (idx < 0 || idx >= MAX_RESOURCES)
        return INVALID_HANDLE;
    return (ResHandle)(idx + 1);
}

static const char* ui_resource_base_display_name(const Resource& r) {
    switch (r.type) {
    case RES_BUILTIN_TIME:        return "Scene Time";
    case RES_BUILTIN_SCENE_COLOR: return "Scene Color";
    case RES_BUILTIN_SCENE_DEPTH: return "Scene Depth";
    case RES_BUILTIN_SHADOW_MAP:  return "Shadow Map";
    case RES_BUILTIN_DIRLIGHT:    return "Directional Light";
    default:                      return r.name;
    }
}

static bool ui_resource_is_implicit_size_resource(const Resource& r, Resource** owner_out = nullptr) {
    if (!r.is_generated || r.generated_from == INVALID_HANDLE)
        return false;
    Resource* owner = res_get(r.generated_from);
    if (!owner || !ui_resource_has_implicit_size_source(*owner))
        return false;
    if (owner->size_handle != ui_resource_handle_from_ptr(&r))
        return false;
    if (owner_out)
        *owner_out = owner;
    return true;
}

static void ui_resource_display_name_buf(const Resource& r, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;

    Resource* owner = nullptr;
    if (ui_resource_is_implicit_size_resource(r, &owner)) {
        const char* owner_name = ui_resource_base_display_name(*owner);
        if (owner->type == RES_STRUCTURED_BUFFER)
            snprintf(out, out_sz, "%s Count", owner_name);
        else
            snprintf(out, out_sz, "%s Size", owner_name);
        out[out_sz - 1] = '\0';
        return;
    }

    snprintf(out, out_sz, "%s", ui_resource_base_display_name(r));
    out[out_sz - 1] = '\0';
}

static const char* ui_resource_display_name(const Resource& r) {
    static char s_labels[8][MAX_NAME + 24] = {};
    static int s_label_index = 0;
    char* out = s_labels[s_label_index];
    s_label_index = (s_label_index + 1) % 8;
    ui_resource_display_name_buf(r, out, MAX_NAME + 24);
    return out;
}

static const char* ui_resource_display_type(const Resource& r) {
    switch (r.type) {
    case RES_BUILTIN_TIME:        return "float";
    case RES_BUILTIN_SCENE_COLOR: return "RenderTexture2D";
    case RES_BUILTIN_SCENE_DEPTH: return "DepthTexture2D";
    case RES_BUILTIN_SHADOW_MAP:  return "DepthTexture2D";
    case RES_BUILTIN_DIRLIGHT:    return "DirectionalLight";
    default:                      return res_type_str(r.type);
    }
}

static void res_combo(const char* label, ResHandle* h, ResType filter, bool allow_invalid = true,
                      ResType filter2 = RES_NONE, ResType filter3 = RES_NONE) {
    Resource* cur     = res_get(*h);
    const char* prev  = cur ? ui_resource_display_name(*cur) : "(none)";
    if (ImGui::BeginCombo(label, prev)) {
        if (allow_invalid && ImGui::Selectable("(none)", *h == INVALID_HANDLE))
            *h = INVALID_HANDLE;
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active) continue;
            bool match = filter == RES_NONE || r.type == filter ||
                         (filter2 != RES_NONE && r.type == filter2) ||
                         (filter3 != RES_NONE && r.type == filter3);
            if (!match) continue;
            bool sel = (*h == (ResHandle)(i + 1));
            ImGui::PushID(i);
            if (ImGui::Selectable(ui_resource_display_name(r), sel))
                *h = (ResHandle)(i + 1);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

// ── resources panel ───────────────────────────────────────────────────────

static void res_combo_render_target(const char* label, ResHandle* h) {
    res_combo(label, h, RES_RENDER_TEXTURE2D, true, RES_BUILTIN_SCENE_COLOR, RES_RENDER_TEXTURE3D);
}

static void res_combo_depth_target(const char* label, ResHandle* h) {
    res_combo(label, h, RES_RENDER_TEXTURE2D, true, RES_BUILTIN_SCENE_DEPTH);
}

static bool ui_resource_is_dispatch_source_candidate(const Resource& r) {
    if (r.type != RES_INT && r.type != RES_INT2 && r.type != RES_INT3)
        return false;
    if (!r.is_generated)
        return true;
    return ui_resource_is_implicit_size_resource(r);
}

static ResHandle ui_dispatch_source_normalize(ResHandle h) {
    Resource* r = res_get(h);
    if (!r)
        return INVALID_HANDLE;
    if (ui_resource_is_dispatch_source_candidate(*r))
        return h;
    if (r->size_handle == INVALID_HANDLE)
        return h;
    Resource* implicit = res_get(r->size_handle);
    if (!implicit || !ui_resource_is_dispatch_source_candidate(*implicit))
        return h;
    return r->size_handle;
}

static void res_combo_dispatch_source(const char* label, ResHandle* h) {
    ResHandle normalized = ui_dispatch_source_normalize(*h);
    if (normalized != *h)
        *h = normalized;
    Resource* cur = res_get(*h);
    const char* prev = cur ? ui_resource_display_name(*cur) : "(none)";
    if (ImGui::BeginCombo(label, prev)) {
        if (ImGui::Selectable("(none)", *h == INVALID_HANDLE))
            *h = INVALID_HANDLE;
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active)
                continue;
            if (!ui_resource_is_dispatch_source_candidate(r))
                continue;
            bool sel = (*h == (ResHandle)(i + 1));
            ImGui::PushID(i);
            if (ImGui::Selectable(ui_resource_display_name(r), sel))
                *h = (ResHandle)(i + 1);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

static float s_asset_preview_scale = 1.0f;
static bool s_asset_preview_point_filter = false;

struct UiTexture3DPreviewCBData {
    UINT slice;
    UINT mode;
    UINT width;
    UINT height;
};

static ID3D11Texture2D*           s_rt3d_preview_tex = nullptr;
static ID3D11RenderTargetView*    s_rt3d_preview_rtv = nullptr;
static ID3D11ShaderResourceView*  s_rt3d_preview_srv = nullptr;
static ID3D11VertexShader*        s_rt3d_preview_vs = nullptr;
static ID3D11PixelShader*         s_rt3d_preview_ps_float = nullptr;
static ID3D11PixelShader*         s_rt3d_preview_ps_uint = nullptr;
static ID3D11Buffer*              s_rt3d_preview_cb = nullptr;
static int                        s_rt3d_preview_w = 0;
static int                        s_rt3d_preview_h = 0;

static const char* s_rt3d_preview_vs_src = R"HLSL(
struct VSOut {
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID) {
    float2 pos;
    if (vid == 0) pos = float2(-1.0, -1.0);
    else if (vid == 1) pos = float2(-1.0, 3.0);
    else pos = float2(3.0, -1.0);

    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);
    return o;
}
)HLSL";

static const char* s_rt3d_preview_ps_float_src = R"HLSL(
cbuffer PreviewCB : register(b0)
{
    uint Slice;
    uint Mode;
    uint Width;
    uint Height;
};

Texture3D<float4> PreviewTex : register(t0);

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    uint x = Width  > 0 ? min((uint)pos.x, Width  - 1) : 0;
    uint y = Height > 0 ? min((uint)pos.y, Height - 1) : 0;
    float4 value = PreviewTex.Load(int4((int)x, (int)y, (int)Slice, 0));
    if (Mode != 0)
        value = float4(value.xxx, 1.0);
    return saturate(value);
}
)HLSL";

static const char* s_rt3d_preview_ps_uint_src = R"HLSL(
cbuffer PreviewCB : register(b0)
{
    uint Slice;
    uint Mode;
    uint Width;
    uint Height;
};

Texture3D<uint> PreviewTex : register(t0);

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    uint x = Width  > 0 ? min((uint)pos.x, Width  - 1) : 0;
    uint y = Height > 0 ? min((uint)pos.y, Height - 1) : 0;
    uint value = PreviewTex.Load(int4((int)x, (int)y, (int)Slice, 0));
    float gray = saturate((float)value / 255.0);
    return float4(gray, gray, gray, 1.0);
}
)HLSL";

static void ui_release_rt3d_preview_surface() {
    if (s_rt3d_preview_srv) { s_rt3d_preview_srv->Release(); s_rt3d_preview_srv = nullptr; }
    if (s_rt3d_preview_rtv) { s_rt3d_preview_rtv->Release(); s_rt3d_preview_rtv = nullptr; }
    if (s_rt3d_preview_tex) { s_rt3d_preview_tex->Release(); s_rt3d_preview_tex = nullptr; }
    s_rt3d_preview_w = 0;
    s_rt3d_preview_h = 0;
}

static void ui_release_rt3d_preview_pipeline() {
    ui_release_rt3d_preview_surface();
    if (s_rt3d_preview_cb) { s_rt3d_preview_cb->Release(); s_rt3d_preview_cb = nullptr; }
    if (s_rt3d_preview_ps_uint) { s_rt3d_preview_ps_uint->Release(); s_rt3d_preview_ps_uint = nullptr; }
    if (s_rt3d_preview_ps_float) { s_rt3d_preview_ps_float->Release(); s_rt3d_preview_ps_float = nullptr; }
    if (s_rt3d_preview_vs) { s_rt3d_preview_vs->Release(); s_rt3d_preview_vs = nullptr; }
}

static bool ui_compile_preview_shader_blob(const char* source, const char* entry,
                                           const char* target, ID3DBlob** out_blob)
{
    if (!out_blob)
        return false;
    *out_blob = nullptr;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), "ui_rt3d_preview",
        nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr) || !blob) {
        const char* msg = err ? (const char*)err->GetBufferPointer() : "unknown error";
        log_error("Texture3D preview shader compile failed (%s/%s): %s", entry, target, msg);
        if (err) err->Release();
        if (blob) blob->Release();
        return false;
    }
    if (err) err->Release();
    *out_blob = blob;
    return true;
}

static bool ui_init_rt3d_preview_pipeline() {
    if (!g_dx.dev || !g_dx.ctx)
        return false;
    if (s_rt3d_preview_vs && s_rt3d_preview_ps_float && s_rt3d_preview_ps_uint && s_rt3d_preview_cb)
        return true;

    ui_release_rt3d_preview_pipeline();

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_float_blob = nullptr;
    ID3DBlob* ps_uint_blob = nullptr;
    if (!ui_compile_preview_shader_blob(s_rt3d_preview_vs_src, "VSMain", "vs_5_0", &vs_blob))
        return false;
    if (!ui_compile_preview_shader_blob(s_rt3d_preview_ps_float_src, "PSMain", "ps_5_0", &ps_float_blob)) {
        vs_blob->Release();
        return false;
    }
    if (!ui_compile_preview_shader_blob(s_rt3d_preview_ps_uint_src, "PSMain", "ps_5_0", &ps_uint_blob)) {
        ps_float_blob->Release();
        vs_blob->Release();
        return false;
    }

    HRESULT hr = g_dx.dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                              nullptr, &s_rt3d_preview_vs);
    if (SUCCEEDED(hr))
        hr = g_dx.dev->CreatePixelShader(ps_float_blob->GetBufferPointer(), ps_float_blob->GetBufferSize(),
                                         nullptr, &s_rt3d_preview_ps_float);
    if (SUCCEEDED(hr))
        hr = g_dx.dev->CreatePixelShader(ps_uint_blob->GetBufferPointer(), ps_uint_blob->GetBufferSize(),
                                         nullptr, &s_rt3d_preview_ps_uint);

    vs_blob->Release();
    ps_float_blob->Release();
    ps_uint_blob->Release();

    if (FAILED(hr)) {
        log_error("Texture3D preview shader create failed: 0x%08X", hr);
        ui_release_rt3d_preview_pipeline();
        return false;
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = (UINT)((sizeof(UiTexture3DPreviewCBData) + 15) & ~15);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dx.dev->CreateBuffer(&cbd, nullptr, &s_rt3d_preview_cb);
    if (FAILED(hr) || !s_rt3d_preview_cb) {
        log_error("Texture3D preview cbuffer create failed: 0x%08X", hr);
        ui_release_rt3d_preview_pipeline();
        return false;
    }

    return true;
}

static bool ui_ensure_rt3d_preview_surface(int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (s_rt3d_preview_tex && s_rt3d_preview_w == width && s_rt3d_preview_h == height)
        return true;

    ui_release_rt3d_preview_surface();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_dx.dev->CreateTexture2D(&td, nullptr, &s_rt3d_preview_tex);
    if (FAILED(hr) || !s_rt3d_preview_tex) {
        log_error("Texture3D preview texture create failed: 0x%08X", hr);
        ui_release_rt3d_preview_surface();
        return false;
    }

    hr = g_dx.dev->CreateRenderTargetView(s_rt3d_preview_tex, nullptr, &s_rt3d_preview_rtv);
    if (FAILED(hr) || !s_rt3d_preview_rtv) {
        log_error("Texture3D preview RTV create failed: 0x%08X", hr);
        ui_release_rt3d_preview_surface();
        return false;
    }

    hr = g_dx.dev->CreateShaderResourceView(s_rt3d_preview_tex, nullptr, &s_rt3d_preview_srv);
    if (FAILED(hr) || !s_rt3d_preview_srv) {
        log_error("Texture3D preview SRV create failed: 0x%08X", hr);
        ui_release_rt3d_preview_surface();
        return false;
    }

    s_rt3d_preview_w = width;
    s_rt3d_preview_h = height;
    return true;
}

static bool ui_rt3d_preview_supported_format(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           fmt == DXGI_FORMAT_R32G32B32A32_FLOAT ||
           fmt == DXGI_FORMAT_R32_FLOAT ||
           fmt == DXGI_FORMAT_R32_UINT;
}

static UINT ui_rt3d_preview_mode(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R32_FLOAT ? 1u : 0u;
}

static bool ui_rt3d_preview_is_uint(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R32_UINT;
}

static ID3D11ShaderResourceView* ui_render_texture3d_preview_slice(Resource* r, int slice) {
    if (!r || !r->tex3d || !r->srv)
        return nullptr;
    if (!ui_rt3d_preview_supported_format(r->tex_fmt))
        return nullptr;
    if (!ui_init_rt3d_preview_pipeline())
        return nullptr;
    if (!ui_ensure_rt3d_preview_surface(r->width, r->height))
        return nullptr;

    if (slice < 0) slice = 0;
    if (slice >= r->depth) slice = r->depth - 1;
    if (slice < 0) slice = 0;

    D3D11_MAPPED_SUBRESOURCE ms = {};
    HRESULT hr = g_dx.ctx->Map(s_rt3d_preview_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr)) {
        log_error("Texture3D preview cbuffer map failed: 0x%08X", hr);
        return nullptr;
    }
    UiTexture3DPreviewCBData* cb = (UiTexture3DPreviewCBData*)ms.pData;
    cb->slice = (UINT)slice;
    cb->mode = ui_rt3d_preview_mode(r->tex_fmt);
    cb->width = (UINT)r->width;
    cb->height = (UINT)r->height;
    g_dx.ctx->Unmap(s_rt3d_preview_cb, 0);

    ID3D11ShaderResourceView* null_srvs[MAX_SRV_SLOTS] = {};
    ID3D11UnorderedAccessView* null_uavs[MAX_UAV_SLOTS] = {};
    UINT null_counts[MAX_UAV_SLOTS] = {};
    ID3D11RenderTargetView* null_rtv = nullptr;
    g_dx.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    g_dx.ctx->PSSetShaderResources(0, MAX_SRV_SLOTS, null_srvs);
    g_dx.ctx->CSSetShaderResources(0, MAX_SRV_SLOTS, null_srvs);
    g_dx.ctx->CSSetUnorderedAccessViews(0, MAX_UAV_SLOTS, null_uavs, null_counts);

    float clear[4] = {};
    g_dx.ctx->OMSetRenderTargets(1, &s_rt3d_preview_rtv, nullptr);
    g_dx.ctx->ClearRenderTargetView(s_rt3d_preview_rtv, clear);

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)r->width, (float)r->height, 0.0f, 1.0f };
    g_dx.ctx->RSSetViewports(1, &vp);
    g_dx.ctx->RSSetState(g_dx.rs_cull_none);
    g_dx.ctx->OMSetDepthStencilState(g_dx.dss_depth_off, 0);
    float blend_factor[4] = {};
    g_dx.ctx->OMSetBlendState(g_dx.bs_opaque, blend_factor, 0xFFFFFFFF);
    g_dx.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx.ctx->IASetInputLayout(nullptr);
    ID3D11Buffer* null_vb = nullptr;
    UINT stride = 0;
    UINT offset = 0;
    g_dx.ctx->IASetVertexBuffers(0, 1, &null_vb, &stride, &offset);
    g_dx.ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
    g_dx.ctx->VSSetShader(s_rt3d_preview_vs, nullptr, 0);
    g_dx.ctx->GSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->HSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->DSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->PSSetShader(ui_rt3d_preview_is_uint(r->tex_fmt) ? s_rt3d_preview_ps_uint : s_rt3d_preview_ps_float,
                          nullptr, 0);
    g_dx.ctx->PSSetConstantBuffers(0, 1, &s_rt3d_preview_cb);
    ID3D11ShaderResourceView* src_srv = r->srv;
    g_dx.ctx->PSSetShaderResources(0, 1, &src_srv);
    g_dx.ctx->Draw(3, 0);

    ID3D11ShaderResourceView* null_srv = nullptr;
    g_dx.ctx->PSSetShaderResources(0, 1, &null_srv);
    g_dx.ctx->OMSetRenderTargets(1, &g_dx.back_rtv, nullptr);
    return s_rt3d_preview_srv;
}

static void ui_imgui_set_preview_sampler(const ImDrawList*, const ImDrawCmd* cmd) {
    ImGui_ImplDX11_RenderState* rs = (ImGui_ImplDX11_RenderState*)ImGui::GetPlatformIO().Renderer_RenderState;
    if (!rs || !rs->DeviceContext)
        return;

    bool point_filter = cmd && cmd->UserCallbackData != nullptr;
    ID3D11SamplerState* sampler = point_filter ? rs->SamplerNearest : rs->SamplerLinear;
    rs->DeviceContext->PSSetSamplers(0, 1, &sampler);
}

static void ui_preview_toolbar() {
    ImGui::SetNextItemWidth(170.0f);
    ImGui::SliderFloat("Preview Scale", &s_asset_preview_scale, 0.10f, 1.00f, "%.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("Point", &s_asset_preview_point_filter);
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit"))
        s_asset_preview_scale = 1.0f;
}

static void ui_image_preview(ID3D11ShaderResourceView* srv, const ImVec2& size) {
    if (!srv)
        return;
    if (!s_asset_preview_point_filter) {
        ImGui::Image((ImTextureID)srv, size);
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCallback(ui_imgui_set_preview_sampler, (void*)1);
    ImGui::Image((ImTextureID)srv, size);
    dl->AddCallback(ui_imgui_set_preview_sampler, nullptr);
}

static void ui_image_fit_panel(ID3D11ShaderResourceView* srv, int width, int height) {
    if (!srv) return;

    ImGui::PushID((void*)srv);
    ui_preview_toolbar();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float max_w = avail.x > 1.0f ? avail.x : 1.0f;
    float max_h = avail.y > 1.0f ? avail.y : max_w;
    float src_w = width  > 0 ? (float)width  : 1.0f;
    float src_h = height > 0 ? (float)height : 1.0f;
    float scale = max_w / src_w;
    float h_scale = max_h / src_h;
    if (h_scale < scale) scale = h_scale;
    if (scale <= 0.0f) scale = 1.0f;
    scale *= s_asset_preview_scale;

    ImVec2 size = { src_w * scale, src_h * scale };
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;
    ui_image_preview(srv, size);
    ImGui::PopID();
}

static void ui_image_fill_panel_width(ID3D11ShaderResourceView* srv, int width, int height) {
    if (!srv) return;

    ImGui::PushID((void*)srv);
    ui_preview_toolbar();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float max_w = avail.x > 1.0f ? avail.x : 1.0f;
    float src_w = width  > 0 ? (float)width  : 1.0f;
    float src_h = height > 0 ? (float)height : 1.0f;
    float scale = max_w / src_w;
    if (scale <= 0.0f) scale = 1.0f;
    scale *= s_asset_preview_scale;

    ImVec2 size = { src_w * scale, src_h * scale };
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;
    ui_image_preview(srv, size);
    ImGui::PopID();
}

static const char* user_cb_default_base_name(ResType type) {
    switch (type) {
    case RES_INT:    return "int_0";
    case RES_INT2:   return "int2_0";
    case RES_INT3:   return "int3_0";
    case RES_FLOAT:  return "float_0";
    case RES_FLOAT2: return "float2_0";
    case RES_FLOAT3: return "float3_0";
    case RES_FLOAT4: return "float4_0";
    default:         return "var_0";
    }
}

static const char* user_cb_hlsl_type(ResType type) {
    switch (type) {
    case RES_FLOAT:  return "float";
    case RES_FLOAT2: return "float2";
    case RES_FLOAT3: return "float3";
    case RES_FLOAT4: return "float4";
    case RES_INT:    return "int";
    case RES_INT2:   return "int2";
    case RES_INT3:   return "int3";
    default:         return "float4";
    }
}

static bool ui_user_cb_value_editor(ResType type, int* ival, float* fval, float reserve = 78.0f) {
    float width = reserve > 0.0f ? -reserve : -1.0f;
    switch (type) {
    case RES_FLOAT:  ImGui::SetNextItemWidth(width); return ImGui::DragFloat("##v",  &fval[0], 0.01f);
    case RES_FLOAT2: ImGui::SetNextItemWidth(width); return ImGui::DragFloat2("##v",  fval,    0.01f);
    case RES_FLOAT3: ImGui::SetNextItemWidth(width); return ImGui::ColorEdit3("##v",  fval);
    case RES_FLOAT4: ImGui::SetNextItemWidth(width); return ImGui::ColorEdit4("##v",  fval);
    case RES_INT:    ImGui::SetNextItemWidth(width); return ImGui::InputInt("##v",   &ival[0]);
    case RES_INT2:   ImGui::SetNextItemWidth(width); return ImGui::InputInt2("##v",   ival);
    case RES_INT3:   ImGui::SetNextItemWidth(width); return ImGui::InputInt3("##v",   ival);
    default:         ImGui::TextDisabled("(unsupported)"); return false;
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

static void ui_command_param_source_combo(CommandParam* p) {
    UserCBSourceKind source_kind = p->source_kind;
    if (source_kind == USER_CB_SOURCE_NONE && p->source != INVALID_HANDLE)
        source_kind = USER_CB_SOURCE_RESOURCE;
    Resource* src = source_kind == USER_CB_SOURCE_RESOURCE ? res_get(p->source) : nullptr;
    char label[160] = {};
    if (source_kind == USER_CB_SOURCE_RESOURCE && src) {
        snprintf(label, sizeof(label), "%s", ui_resource_display_name(*src));
    } else if (source_kind == USER_CB_SOURCE_COMMAND_POSITION) {
        snprintf(label, sizeof(label), "%s Position", p->source_target);
    } else if (source_kind == USER_CB_SOURCE_COMMAND_ROTATION) {
        snprintf(label, sizeof(label), "%s Rotation", p->source_target);
    } else if (source_kind == USER_CB_SOURCE_COMMAND_SCALE) {
        snprintf(label, sizeof(label), "%s Scale", p->source_target);
    } else if (source_kind == USER_CB_SOURCE_CAMERA_POSITION) {
        snprintf(label, sizeof(label), "Camera Position");
    } else if (source_kind == USER_CB_SOURCE_CAMERA_ROTATION) {
        snprintf(label, sizeof(label), "Camera Rotation");
    } else if (source_kind == USER_CB_SOURCE_DIRLIGHT_POSITION) {
        snprintf(label, sizeof(label), "Dir Light Position");
    } else if (source_kind == USER_CB_SOURCE_DIRLIGHT_TARGET) {
        snprintf(label, sizeof(label), "Dir Light Target");
    } else {
        snprintf(label, sizeof(label), "(hardcoded)");
    }

    if (ImGui::BeginCombo("##source", label)) {
        if (ImGui::Selectable("(hardcoded)", source_kind == USER_CB_SOURCE_NONE)) {
            p->source = INVALID_HANDLE;
            p->source_kind = USER_CB_SOURCE_NONE;
            p->source_target[0] = '\0';
        }
        ImGui::Separator();
        ImGui::TextDisabled("Resources");
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active || r.is_builtin || r.type != p->type) continue;
            ResHandle h = (ResHandle)(i + 1);
            bool sel = source_kind == USER_CB_SOURCE_RESOURCE && p->source == h;
            ImGui::PushID(i);
            if (ImGui::Selectable(ui_resource_display_name(r), sel)) {
                p->source = h;
                p->source_kind = USER_CB_SOURCE_RESOURCE;
                p->source_target[0] = '\0';
                command_param_copy_from_resource(p, &r);
            }
            ImGui::PopID();
        }
        if (p->type == RES_FLOAT3 || p->type == RES_FLOAT4) {
            ImGui::Separator();
            ImGui::TextDisabled("Scene transforms");
            if (ImGui::Selectable("Camera Position", source_kind == USER_CB_SOURCE_CAMERA_POSITION)) {
                p->source = INVALID_HANDLE;
                p->source_kind = USER_CB_SOURCE_CAMERA_POSITION;
                snprintf(p->source_target, MAX_NAME, "camera");
            }
            if (ImGui::Selectable("Camera Rotation", source_kind == USER_CB_SOURCE_CAMERA_ROTATION)) {
                p->source = INVALID_HANDLE;
                p->source_kind = USER_CB_SOURCE_CAMERA_ROTATION;
                snprintf(p->source_target, MAX_NAME, "camera");
            }
            if (ImGui::Selectable("Dir Light Position", source_kind == USER_CB_SOURCE_DIRLIGHT_POSITION)) {
                p->source = INVALID_HANDLE;
                p->source_kind = USER_CB_SOURCE_DIRLIGHT_POSITION;
                snprintf(p->source_target, MAX_NAME, "dirlight");
            }
            if (ImGui::Selectable("Dir Light Target", source_kind == USER_CB_SOURCE_DIRLIGHT_TARGET)) {
                p->source = INVALID_HANDLE;
                p->source_kind = USER_CB_SOURCE_DIRLIGHT_TARGET;
                snprintf(p->source_target, MAX_NAME, "dirlight");
            }
            for (int c_i = 0; c_i < MAX_COMMANDS; c_i++) {
                Command& c = g_commands[c_i];
                bool has_transform = c.active && (c.type == CMD_DRAW_MESH ||
                                                  c.type == CMD_DRAW_INSTANCED ||
                                                  c.type == CMD_INDIRECT_DRAW);
                if (!has_transform)
                    continue;
                ImGui::PushID(10000 + c_i);
                char item[128] = {};
                snprintf(item, sizeof(item), "%s Position", c.name);
                if (ImGui::Selectable(item, source_kind == USER_CB_SOURCE_COMMAND_POSITION &&
                                            strcmp(p->source_target, c.name) == 0)) {
                    p->source = INVALID_HANDLE;
                    p->source_kind = USER_CB_SOURCE_COMMAND_POSITION;
                    strncpy(p->source_target, c.name, MAX_NAME - 1);
                    p->source_target[MAX_NAME - 1] = '\0';
                }
                snprintf(item, sizeof(item), "%s Rotation", c.name);
                if (ImGui::Selectable(item, source_kind == USER_CB_SOURCE_COMMAND_ROTATION &&
                                            strcmp(p->source_target, c.name) == 0)) {
                    p->source = INVALID_HANDLE;
                    p->source_kind = USER_CB_SOURCE_COMMAND_ROTATION;
                    strncpy(p->source_target, c.name, MAX_NAME - 1);
                    p->source_target[MAX_NAME - 1] = '\0';
                }
                snprintf(item, sizeof(item), "%s Scale", c.name);
                if (ImGui::Selectable(item, source_kind == USER_CB_SOURCE_COMMAND_SCALE &&
                                            strcmp(p->source_target, c.name) == 0)) {
                    p->source = INVALID_HANDLE;
                    p->source_kind = USER_CB_SOURCE_COMMAND_SCALE;
                    strncpy(p->source_target, c.name, MAX_NAME - 1);
                    p->source_target[MAX_NAME - 1] = '\0';
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
}

static void ui_inspector_section(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.57f, 0.55f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

static bool ui_ascii_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool ui_ascii_ident_char(char c) {
    return ui_ascii_ident_start(c) || (c >= '0' && c <= '9');
}

static bool ui_token_matches(const char* token, int len, const char* word) {
    return len == (int)strlen(word) && strncmp(token, word, len) == 0;
}

static bool ui_hlsl_keyword(const char* token, int len) {
    static const char* words[] = {
        "if", "else", "for", "while", "do", "switch", "case", "default", "break",
        "continue", "return", "discard", "struct", "cbuffer", "tbuffer", "class",
        "namespace", "static", "const", "uniform", "volatile", "groupshared",
        "in", "out", "inout", "true", "false", "register", "packoffset",
        "numthreads", "linear", "centroid", "nointerpolation", "precise"
    };
    for (int i = 0; i < (int)(sizeof(words) / sizeof(words[0])); i++)
        if (ui_token_matches(token, len, words[i]))
            return true;
    return false;
}

static bool ui_hlsl_type_keyword(const char* token, int len) {
    static const char* words[] = {
        "void", "bool", "int", "uint", "dword", "half", "float", "double",
        "bool2", "bool3", "bool4", "int2", "int3", "int4", "uint2", "uint3", "uint4",
        "half2", "half3", "half4", "float2", "float3", "float4",
        "float2x2", "float2x3", "float2x4", "float3x2", "float3x3", "float3x4",
        "float4x2", "float4x3", "float4x4", "matrix",
        "Texture1D", "Texture2D", "Texture3D", "TextureCube",
        "Texture1DArray", "Texture2DArray", "TextureCubeArray",
        "RWTexture1D", "RWTexture2D", "RWTexture3D", "RWTexture1DArray", "RWTexture2DArray",
        "Buffer", "RWBuffer", "StructuredBuffer", "RWStructuredBuffer",
        "AppendStructuredBuffer", "ConsumeStructuredBuffer", "ByteAddressBuffer",
        "RWByteAddressBuffer", "SamplerState", "SamplerComparisonState"
    };
    for (int i = 0; i < (int)(sizeof(words) / sizeof(words[0])); i++)
        if (ui_token_matches(token, len, words[i]))
            return true;
    return false;
}

static bool ui_hlsl_semantic(const char* token, int len) {
    return (len > 3 && strncmp(token, "SV_", 3) == 0) ||
           ui_token_matches(token, len, "POSITION") ||
           ui_token_matches(token, len, "NORMAL") ||
           ui_token_matches(token, len, "TEXCOORD") ||
           ui_token_matches(token, len, "COLOR") ||
           ui_token_matches(token, len, "TARGET");
}

static const char* ui_skip_spaces(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

static void ui_hlsl_emit_span(const char* begin, const char* end, ImVec4 color) {
    if (!begin || !end || end <= begin)
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(begin, end);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 0.0f);
}

static void ui_hlsl_render_line(const char* begin, const char* end, int line_no) {
    ImVec4 normal = ImVec4(0.83f, 0.81f, 0.78f, 1.0f);
    ImVec4 muted = ImVec4(0.55f, 0.52f, 0.50f, 1.0f);
    ImVec4 keyword = ImVec4(0.95f, 0.50f, 0.30f, 1.0f);
    ImVec4 type = ImVec4(0.48f, 0.72f, 1.00f, 1.0f);
    ImVec4 number = ImVec4(0.80f, 0.62f, 0.95f, 1.0f);
    ImVec4 string_col = ImVec4(0.86f, 0.72f, 0.38f, 1.0f);
    ImVec4 comment = ImVec4(0.42f, 0.68f, 0.44f, 1.0f);
    ImVec4 preproc = ImVec4(0.72f, 0.58f, 0.86f, 1.0f);
    ImVec4 function_col = ImVec4(0.88f, 0.82f, 0.55f, 1.0f);

    ImGui::TextDisabled("%4d  ", line_no);
    ImGui::SameLine(0.0f, 0.0f);

    const char* first = ui_skip_spaces(begin, end);
    if (first < end && *first == '#') {
        if (first > begin)
            ui_hlsl_emit_span(begin, first, normal);
        ui_hlsl_emit_span(first, end, preproc);
        ImGui::NewLine();
        return;
    }

    const char* p = begin;
    while (p < end) {
        if (*p == ' ' || *p == '\t') {
            const char* s = p++;
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            ui_hlsl_emit_span(s, p, normal);
            continue;
        }

        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            ui_hlsl_emit_span(p, end, comment);
            p = end;
            continue;
        }

        if (*p == '"' || *p == '\'') {
            char quote = *p;
            const char* s = p++;
            while (p < end) {
                if (*p == '\\' && p + 1 < end) {
                    p += 2;
                    continue;
                }
                if (*p++ == quote)
                    break;
            }
            ui_hlsl_emit_span(s, p, string_col);
            continue;
        }

        if (ui_ascii_ident_start(*p)) {
            const char* s = p++;
            while (p < end && ui_ascii_ident_char(*p))
                p++;
            int len = (int)(p - s);
            const char* next = ui_skip_spaces(p, end);
            ImVec4 col = normal;
            if (ui_hlsl_type_keyword(s, len))
                col = type;
            else if (ui_hlsl_keyword(s, len))
                col = keyword;
            else if (ui_hlsl_semantic(s, len))
                col = preproc;
            else if (next < end && *next == '(')
                col = function_col;
            ui_hlsl_emit_span(s, p, col);
            continue;
        }

        if ((*p >= '0' && *p <= '9') ||
            (*p == '.' && p + 1 < end && p[1] >= '0' && p[1] <= '9')) {
            const char* s = p++;
            while (p < end && (ui_ascii_ident_char(*p) || *p == '.' || *p == '+' || *p == '-'))
                p++;
            ui_hlsl_emit_span(s, p, number);
            continue;
        }

        ui_hlsl_emit_span(p, p + 1, muted);
        p++;
    }
    ImGui::NewLine();
}

static void ui_hlsl_code_view(const char* text) {
    if (!text || !text[0]) {
        ImGui::TextDisabled("(empty source)");
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    int line_no = 1;
    const char* line = text;
    while (*line) {
        const char* end = line;
        while (*end && *end != '\n' && *end != '\r')
            end++;
        ui_hlsl_render_line(line, end, line_no++);
        if (*end == '\r' && end[1] == '\n')
            line = end + 2;
        else if (*end)
            line = end + 1;
        else
            line = end;
    }
    ImGui::PopStyleVar();
}

struct UiShaderSourceEditor {
    ResHandle h;
    char      path[MAX_PATH_LEN];
    char*     text;
    size_t    cap;
    int       cursor;
    int       select_anchor;
    bool      editor_focused;
    bool      dragging_selection;
    bool      cursor_follow;
    bool      ok;
    bool      dirty;
    double    last_edit_time;
};

struct UiTextResizeData {
    char**  text;
    size_t* cap;
};

static int ui_text_resize_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackResize)
        return 0;
    UiTextResizeData* resize = (UiTextResizeData*)data->UserData;
    if (!resize || !resize->text || !resize->cap)
        return 1;
    size_t new_cap = (size_t)data->BufTextLen + 4096;
    char* next = (char*)realloc(*resize->text, new_cap);
    if (!next)
        return 1;
    *resize->text = next;
    *resize->cap = new_cap;
    data->Buf = next;
    return 0;
}

struct UiCodeLine {
    const char* begin;
    const char* end;
    int         offset;
};

static int ui_code_text_len(const UiShaderSourceEditor* ed) {
    return (ed && ed->text) ? (int)strlen(ed->text) : 0;
}

static int ui_code_clamp_offset(const UiShaderSourceEditor* ed, int offset) {
    int len = ui_code_text_len(ed);
    if (offset < 0) return 0;
    if (offset > len) return len;
    return offset;
}

static int ui_code_tab_next_col(int col) {
    return ((col / 4) + 1) * 4;
}

static int ui_code_visual_cols(const char* begin, const char* end) {
    int col = 0;
    for (const char* p = begin; p && p < end; p++)
        col = (*p == '\t') ? ui_code_tab_next_col(col) : col + 1;
    return col;
}

static int ui_code_visual_cols_to(const char* begin, const char* at) {
    return ui_code_visual_cols(begin, at);
}

static int ui_code_offset_from_col(const UiCodeLine& line, int target_col) {
    if (target_col <= 0)
        return line.offset;

    int col = 0;
    const char* p = line.begin;
    while (p < line.end) {
        int next_col = (*p == '\t') ? ui_code_tab_next_col(col) : col + 1;
        if (target_col < next_col || (target_col == next_col && *p != '\t'))
            break;
        col = next_col;
        p++;
    }
    return line.offset + (int)(p - line.begin);
}

static void ui_code_build_lines(const char* text, std::vector<UiCodeLine>& lines, int* max_cols) {
    lines.clear();
    if (max_cols) *max_cols = 0;
    if (!text)
        return;

    const char* p = text;
    while (true) {
        const char* begin = p;
        const char* end = p;
        while (*end && *end != '\n' && *end != '\r')
            end++;
        UiCodeLine line = { begin, end, (int)(begin - text) };
        lines.push_back(line);
        if (max_cols) {
            int cols = ui_code_visual_cols(begin, end);
            if (cols > *max_cols)
                *max_cols = cols;
        }
        if (!*end)
            break;
        if (*end == '\r' && end[1] == '\n')
            p = end + 2;
        else
            p = end + 1;
        if (!*p) {
            UiCodeLine empty = { p, p, (int)(p - text) };
            lines.push_back(empty);
            break;
        }
    }
}

static int ui_code_line_from_offset(const std::vector<UiCodeLine>& lines, int offset) {
    if (lines.empty())
        return 0;
    int best = 0;
    for (int i = 0; i < (int)lines.size(); i++) {
        if (lines[i].offset <= offset)
            best = i;
        else
            break;
    }
    return best;
}

struct UiShaderErrorMarker {
    int  line;
    char message[256];
};

static bool ui_shader_compile_error_marker(const Resource* r, UiShaderErrorMarker* out) {
    if (!r || !out || !r->compile_err[0])
        return false;

    const char* p = r->compile_err;
    while ((p = strchr(p, '(')) != nullptr) {
        char* end_line = nullptr;
        long line = strtol(p + 1, &end_line, 10);
        if (line > 0 && end_line && (*end_line == ',' || *end_line == ')')) {
            const char* close = strstr(end_line, "):");
            if (close) {
                const char* msg = close + 2;
                while (*msg == ' ' || *msg == '\t')
                    msg++;
                const char* msg_end = msg;
                while (*msg_end && *msg_end != '\r' && *msg_end != '\n')
                    msg_end++;
                int len = (int)(msg_end - msg);
                if (len >= (int)sizeof(out->message))
                    len = (int)sizeof(out->message) - 1;
                out->line = (int)line;
                memcpy(out->message, msg, len);
                out->message[len] = '\0';
                return true;
            }
        }
        p++;
    }
    return false;
}

static bool ui_shader_editor_has_selection(const UiShaderSourceEditor* ed) {
    return ed && ed->select_anchor != ed->cursor;
}

static void ui_shader_editor_selection(const UiShaderSourceEditor* ed, int* out_a, int* out_b) {
    int a = ed ? ed->select_anchor : 0;
    int b = ed ? ed->cursor : 0;
    if (a > b) {
        int tmp = a;
        a = b;
        b = tmp;
    }
    if (out_a) *out_a = a;
    if (out_b) *out_b = b;
}

static void ui_shader_editor_mark_dirty(UiShaderSourceEditor* ed) {
    if (!ed)
        return;
    ed->dirty = true;
    ed->last_edit_time = ImGui::GetTime();
}

static bool ui_shader_editor_reserve(UiShaderSourceEditor* ed, size_t need) {
    if (!ed)
        return false;
    if (need <= ed->cap)
        return true;

    size_t next_cap = ed->cap ? ed->cap : 8192;
    while (next_cap < need)
        next_cap += 4096;
    char* next = (char*)realloc(ed->text, next_cap);
    if (!next)
        return false;
    ed->text = next;
    ed->cap = next_cap;
    return true;
}

static bool ui_shader_editor_delete_range(UiShaderSourceEditor* ed, int a, int b) {
    if (!ed || !ed->text)
        return false;
    int len = ui_code_text_len(ed);
    if (a < 0) a = 0;
    if (b > len) b = len;
    if (a > b) {
        int tmp = a;
        a = b;
        b = tmp;
    }
    if (a == b)
        return false;
    memmove(ed->text + a, ed->text + b, (size_t)(len - b + 1));
    ed->cursor = a;
    ed->select_anchor = a;
    ed->cursor_follow = true;
    ui_shader_editor_mark_dirty(ed);
    return true;
}

static bool ui_shader_editor_delete_selection(UiShaderSourceEditor* ed) {
    if (!ui_shader_editor_has_selection(ed))
        return false;
    int a = 0, b = 0;
    ui_shader_editor_selection(ed, &a, &b);
    return ui_shader_editor_delete_range(ed, a, b);
}

static bool ui_shader_editor_insert_bytes(UiShaderSourceEditor* ed, const char* text, int insert_len) {
    if (!ed || !text || insert_len <= 0)
        return false;
    if (!ed->text) {
        if (!ui_shader_editor_reserve(ed, 8192))
            return false;
        ed->text[0] = '\0';
    }
    if (ui_shader_editor_has_selection(ed))
        ui_shader_editor_delete_selection(ed);

    int len = ui_code_text_len(ed);
    ed->cursor = ui_code_clamp_offset(ed, ed->cursor);
    if (!ui_shader_editor_reserve(ed, (size_t)len + (size_t)insert_len + 1))
        return false;

    memmove(ed->text + ed->cursor + insert_len, ed->text + ed->cursor,
            (size_t)(len - ed->cursor + 1));
    memcpy(ed->text + ed->cursor, text, (size_t)insert_len);
    ed->cursor += insert_len;
    ed->select_anchor = ed->cursor;
    ed->cursor_follow = true;
    ui_shader_editor_mark_dirty(ed);
    return true;
}

static void ui_shader_editor_set_cursor(UiShaderSourceEditor* ed, int cursor, bool extend_selection) {
    if (!ed)
        return;
    ed->cursor = ui_code_clamp_offset(ed, cursor);
    if (!extend_selection)
        ed->select_anchor = ed->cursor;
    ed->cursor_follow = true;
}

static void ui_shader_editor_copy_selection(UiShaderSourceEditor* ed) {
    if (!ed || !ed->text || !ui_shader_editor_has_selection(ed))
        return;
    int a = 0, b = 0;
    ui_shader_editor_selection(ed, &a, &b);
    int len = b - a;
    char* tmp = (char*)malloc((size_t)len + 1);
    if (!tmp)
        return;
    memcpy(tmp, ed->text + a, (size_t)len);
    tmp[len] = '\0';
    ImGui::SetClipboardText(tmp);
    free(tmp);
}

static int ui_shader_editor_word_left(const UiShaderSourceEditor* ed) {
    if (!ed || !ed->text)
        return 0;
    int p = ui_code_clamp_offset(ed, ed->cursor);
    while (p > 0 && (ed->text[p - 1] == ' ' || ed->text[p - 1] == '\t' ||
                     ed->text[p - 1] == '\n' || ed->text[p - 1] == '\r'))
        p--;
    if (p > 0 && ui_ascii_ident_char(ed->text[p - 1])) {
        while (p > 0 && ui_ascii_ident_char(ed->text[p - 1]))
            p--;
    } else {
        while (p > 0 && ed->text[p - 1] != ' ' && ed->text[p - 1] != '\t' &&
               ed->text[p - 1] != '\n' && ed->text[p - 1] != '\r' &&
               !ui_ascii_ident_char(ed->text[p - 1]))
            p--;
    }
    return p;
}

static int ui_shader_editor_word_right(const UiShaderSourceEditor* ed) {
    if (!ed || !ed->text)
        return 0;
    int len = ui_code_text_len(ed);
    int p = ui_code_clamp_offset(ed, ed->cursor);
    while (p < len && (ed->text[p] == ' ' || ed->text[p] == '\t' ||
                       ed->text[p] == '\n' || ed->text[p] == '\r'))
        p++;
    if (p < len && ui_ascii_ident_char(ed->text[p])) {
        while (p < len && ui_ascii_ident_char(ed->text[p]))
            p++;
    } else {
        while (p < len && ed->text[p] != ' ' && ed->text[p] != '\t' &&
               ed->text[p] != '\n' && ed->text[p] != '\r' &&
               !ui_ascii_ident_char(ed->text[p]))
            p++;
    }
    return p;
}

static void ui_shader_editor_move_vertical(UiShaderSourceEditor* ed,
                                           const std::vector<UiCodeLine>& lines,
                                           int dir,
                                           bool extend_selection) {
    if (!ed || lines.empty())
        return;
    int line_i = ui_code_line_from_offset(lines, ui_code_clamp_offset(ed, ed->cursor));
    int next_i = line_i + dir;
    if (next_i < 0) next_i = 0;
    if (next_i >= (int)lines.size()) next_i = (int)lines.size() - 1;
    int col = ui_code_visual_cols_to(lines[line_i].begin,
        lines[line_i].begin + (ed->cursor - lines[line_i].offset));
    ui_shader_editor_set_cursor(ed, ui_code_offset_from_col(lines[next_i], col), extend_selection);
}

static int ui_utf8_encode(unsigned int c, char out[5]) {
    if (c < 0x80) {
        out[0] = (char)c;
        out[1] = '\0';
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        out[2] = '\0';
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        out[3] = '\0';
        return 3;
    }
    if (c <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (c >> 18));
        out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[3] = (char)(0x80 | (c & 0x3F));
        out[4] = '\0';
        return 4;
    }
    return 0;
}

struct UiHlslDrawPalette {
    ImU32 normal;
    ImU32 muted;
    ImU32 keyword;
    ImU32 type;
    ImU32 number;
    ImU32 string_col;
    ImU32 comment;
    ImU32 preproc;
    ImU32 function_col;
};

static UiHlslDrawPalette ui_hlsl_draw_palette() {
    UiHlslDrawPalette p = {};
    p.normal       = ImGui::GetColorU32(ImVec4(0.83f, 0.81f, 0.78f, 1.0f));
    p.muted        = ImGui::GetColorU32(ImVec4(0.55f, 0.52f, 0.50f, 1.0f));
    p.keyword      = ImGui::GetColorU32(ImVec4(0.95f, 0.50f, 0.30f, 1.0f));
    p.type         = ImGui::GetColorU32(ImVec4(0.48f, 0.72f, 1.00f, 1.0f));
    p.number       = ImGui::GetColorU32(ImVec4(0.80f, 0.62f, 0.95f, 1.0f));
    p.string_col   = ImGui::GetColorU32(ImVec4(0.86f, 0.72f, 0.38f, 1.0f));
    p.comment      = ImGui::GetColorU32(ImVec4(0.42f, 0.68f, 0.44f, 1.0f));
    p.preproc      = ImGui::GetColorU32(ImVec4(0.72f, 0.58f, 0.86f, 1.0f));
    p.function_col = ImGui::GetColorU32(ImVec4(0.88f, 0.82f, 0.55f, 1.0f));
    return p;
}

static float ui_code_draw_span(ImDrawList* dl, ImFont* font, float font_size,
                               float x, float y, float line_x,
                               const char* begin, const char* end,
                               ImU32 color, float char_w) {
    const char* p = begin;
    while (p < end) {
        if (*p == '\t') {
            int col = (int)((x - line_x) / char_w + 0.01f);
            x = line_x + (float)ui_code_tab_next_col(col) * char_w;
            p++;
            continue;
        }
        if (*p == ' ') {
            x += char_w;
            p++;
            continue;
        }
        const char* s = p;
        while (p < end && *p != '\t' && *p != ' ')
            p++;
        dl->AddText(font, font_size, ImVec2(x, y), color, s, p);
        x += ImGui::CalcTextSize(s, p).x;
    }
    return x;
}

static void ui_hlsl_draw_line_at(ImDrawList* dl, ImVec2 pos,
                                 const char* begin, const char* end,
                                 float char_w, const UiHlslDrawPalette& pal) {
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();
    float x = pos.x;
    float line_x = pos.x;

    const char* first = ui_skip_spaces(begin, end);
    if (first < end && *first == '#') {
        x = ui_code_draw_span(dl, font, font_size, x, pos.y, line_x, begin, first, pal.normal, char_w);
        ui_code_draw_span(dl, font, font_size, x, pos.y, line_x, first, end, pal.preproc, char_w);
        return;
    }

    const char* p = begin;
    while (p < end) {
        if (*p == ' ' || *p == '\t') {
            const char* s = p++;
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            x = ui_code_draw_span(dl, font, font_size, x, pos.y, line_x, s, p, pal.normal, char_w);
            continue;
        }

        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            ui_code_draw_span(dl, font, font_size, x, pos.y, line_x, p, end, pal.comment, char_w);
            return;
        }

        if (*p == '"' || *p == '\'') {
            char quote = *p;
            const char* s = p++;
            while (p < end) {
                if (*p == '\\' && p + 1 < end) {
                    p += 2;
                    continue;
                }
                if (*p++ == quote)
                    break;
            }
            x = ui_code_draw_span(dl, font, font_size, x, pos.y, line_x, s, p, pal.string_col, char_w);
            continue;
        }

        if (ui_ascii_ident_start(*p)) {
            const char* s = p++;
            while (p < end && ui_ascii_ident_char(*p))
                p++;
            int len = (int)(p - s);
            const char* next = ui_skip_spaces(p, end);
            ImU32 col = pal.normal;
            if (ui_hlsl_type_keyword(s, len))
                col = pal.type;
            else if (ui_hlsl_keyword(s, len))
                col = pal.keyword;
            else if (ui_hlsl_semantic(s, len))
                col = pal.preproc;
            else if (next < end && *next == '(')
                col = pal.function_col;
            x = ui_code_draw_span(dl, font, font_size, x, pos.y, line_x, s, p, col, char_w);
            continue;
        }

        if ((*p >= '0' && *p <= '9') ||
            (*p == '.' && p + 1 < end && p[1] >= '0' && p[1] <= '9')) {
            const char* s = p++;
            while (p < end && (ui_ascii_ident_char(*p) || *p == '.' || *p == '+' || *p == '-'))
                p++;
            x = ui_code_draw_span(dl, font, font_size, x, pos.y, line_x, s, p, pal.number, char_w);
            continue;
        }

        x = ui_code_draw_span(dl, font, font_size, x, pos.y, line_x, p, p + 1, pal.muted, char_w);
        p++;
    }
}

static int ui_shader_editor_cursor_from_mouse(const std::vector<UiCodeLine>& lines,
                                              ImVec2 origin,
                                              float gutter_w,
                                              float line_h,
                                              float char_w,
                                              ImVec2 mouse) {
    if (lines.empty())
        return 0;
    int line_i = (int)floorf((mouse.y - origin.y) / line_h);
    if (line_i < 0) line_i = 0;
    if (line_i >= (int)lines.size()) line_i = (int)lines.size() - 1;
    float local_x = mouse.x - origin.x - gutter_w;
    int col = (int)floorf(local_x / char_w + 0.5f);
    return ui_code_offset_from_col(lines[line_i], col);
}

static bool ui_shader_code_editor_handle_input(UiShaderSourceEditor* ed,
                                               const std::vector<UiCodeLine>& lines) {
    if (!ed || !ed->editor_focused)
        return false;

    ImGuiIO& io = ImGui::GetIO();
    bool changed = false;
    bool shift = io.KeyShift;
    bool ctrl = io.KeyCtrl;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        ed->select_anchor = 0;
        ed->cursor = ui_code_text_len(ed);
        ed->cursor_follow = true;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
        ui_shader_editor_copy_selection(ed);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) {
        ui_shader_editor_copy_selection(ed);
        changed |= ui_shader_editor_delete_selection(ed);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
        if (const char* clip = ImGui::GetClipboardText())
            changed |= ui_shader_editor_insert_bytes(ed, clip, (int)strlen(clip));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
        int next = ctrl ? ui_shader_editor_word_left(ed) : ed->cursor - 1;
        ui_shader_editor_set_cursor(ed, next, shift);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
        int next = ctrl ? ui_shader_editor_word_right(ed) : ed->cursor + 1;
        ui_shader_editor_set_cursor(ed, next, shift);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        ui_shader_editor_move_vertical(ed, lines, -1, shift);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        ui_shader_editor_move_vertical(ed, lines, 1, shift);

    if (ImGui::IsKeyPressed(ImGuiKey_Home, true)) {
        int line_i = ui_code_line_from_offset(lines, ed->cursor);
        ui_shader_editor_set_cursor(ed, lines.empty() ? 0 : lines[line_i].offset, shift);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End, true)) {
        int line_i = ui_code_line_from_offset(lines, ed->cursor);
        int end_offset = lines.empty() ? ui_code_text_len(ed) :
            lines[line_i].offset + (int)(lines[line_i].end - lines[line_i].begin);
        ui_shader_editor_set_cursor(ed, end_offset, shift);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
        if (ui_shader_editor_has_selection(ed))
            changed |= ui_shader_editor_delete_selection(ed);
        else
            changed |= ui_shader_editor_delete_range(ed, ed->cursor - 1, ed->cursor);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, true)) {
        if (ui_shader_editor_has_selection(ed))
            changed |= ui_shader_editor_delete_selection(ed);
        else
            changed |= ui_shader_editor_delete_range(ed, ed->cursor, ed->cursor + 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true))
        changed |= ui_shader_editor_insert_bytes(ed, "\n", 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, true))
        changed |= ui_shader_editor_insert_bytes(ed, "\t", 1);

    if (!ctrl || io.KeyAlt) {
        for (int n = 0; n < io.InputQueueCharacters.Size; n++) {
            unsigned int c = (unsigned int)io.InputQueueCharacters[n];
            if (c < 32 || c == 127 || c == '\t' || c == '\r' || c == '\n')
                continue;
            char utf8[5] = {};
            int len = ui_utf8_encode(c, utf8);
            if (len > 0)
                changed |= ui_shader_editor_insert_bytes(ed, utf8, len);
        }
    }
    io.InputQueueCharacters.resize(0);
    return changed;
}

static bool ui_shader_code_editor(UiShaderSourceEditor* ed, const Resource* shader_resource) {
    if (!ed || !ed->text)
        return false;

    bool changed = false;
    float editor_h = ImGui::GetContentRegionAvail().y;
    if (editor_h < ui_px(300.0f))
        editor_h = ui_px(360.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.052f, 0.055f, 1.0f));
    if (s_code_font)
        ImGui::PushFont(s_code_font, s_code_font_size);
    ImGui::BeginChild("##shader_source_code_editor", ImVec2(0.0f, editor_h), true,
        ImGuiWindowFlags_HorizontalScrollbar);

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiID editor_id = window->GetID("##shader_source_code_editor_active");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float line_h = ImGui::GetTextLineHeight();
    float char_w = ImGui::CalcTextSize("M").x;
    if (char_w <= 0.0f)
        char_w = ImGui::GetFontSize() * 0.6f;
    float gutter_w = ImGui::CalcTextSize("0000  ").x + ui_px(10.0f);

    std::vector<UiCodeLine> lines;
    int max_cols = 0;
    ui_code_build_lines(ed->text, lines, &max_cols);
    if (lines.empty()) {
        UiCodeLine line = { ed->text, ed->text, 0 };
        lines.push_back(line);
    }

    ImGuiIO& io = ImGui::GetIO();
    bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool content_hovered = hovered && window->InnerRect.Contains(io.MousePos);
    if (content_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ed->editor_focused = true;
        ed->dragging_selection = true;
        ImGui::SetActiveID(editor_id, window);
        ImGui::SetFocusID(editor_id, window);
        ImGui::FocusWindow(window);
        int cursor = ui_shader_editor_cursor_from_mouse(lines, origin, gutter_w, line_h, char_w, io.MousePos);
        ui_shader_editor_set_cursor(ed, cursor, io.KeyShift);
    } else if (ed->editor_focused && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !content_hovered) {
        ed->editor_focused = false;
        ed->dragging_selection = false;
        if (GImGui && GImGui->ActiveId == editor_id)
            ImGui::ClearActiveID();
    }

    if (ed->dragging_selection) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && content_hovered) {
            int cursor = ui_shader_editor_cursor_from_mouse(lines, origin, gutter_w, line_h, char_w, io.MousePos);
            ui_shader_editor_set_cursor(ed, cursor, true);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            ed->dragging_selection = false;
    }

    if (ed->editor_focused) {
        ImGui::SetActiveID(editor_id, window);
        ImGui::SetFocusID(editor_id, window);
        ImGui::FocusWindow(window);
        ImGui::SetActiveIdUsingAllKeyboardKeys();
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        ImGui::SetKeyOwner(ImGuiKey_Tab, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_LeftArrow, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_RightArrow, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_UpArrow, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_DownArrow, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_Backspace, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_Delete, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_Enter, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_KeypadEnter, editor_id);
        changed |= ui_shader_code_editor_handle_input(ed, lines);
        if (changed) {
            ui_code_build_lines(ed->text, lines, &max_cols);
            if (lines.empty()) {
                UiCodeLine line = { ed->text, ed->text, 0 };
                lines.push_back(line);
            }
        }
    }

    int cursor_line = ui_code_line_from_offset(lines, ui_code_clamp_offset(ed, ed->cursor));
    int cursor_col = 0;
    if (!lines.empty()) {
        const UiCodeLine& line = lines[cursor_line];
        const char* cursor_ptr = line.begin + (ed->cursor - line.offset);
        if (cursor_ptr < line.begin) cursor_ptr = line.begin;
        if (cursor_ptr > line.end) cursor_ptr = line.end;
        cursor_col = ui_code_visual_cols_to(line.begin, cursor_ptr);
    }

    float content_w = gutter_w + (float)(max_cols + 8) * char_w;
    float min_w = ImGui::GetWindowWidth() - ui_px(6.0f);
    if (content_w < min_w)
        content_w = min_w;
    float content_h = (float)lines.size() * line_h + ui_px(12.0f);

    if (ed->cursor_follow) {
        float scroll_y = ImGui::GetScrollY();
        float scroll_x = ImGui::GetScrollX();
        float view_h = ImGui::GetWindowHeight() - ui_px(14.0f);
        float view_w = ImGui::GetWindowWidth() - ui_px(18.0f);
        float cursor_y = (float)cursor_line * line_h;
        float cursor_x = gutter_w + (float)cursor_col * char_w;
        if (cursor_y < scroll_y)
            ImGui::SetScrollY(cursor_y);
        else if (cursor_y + line_h > scroll_y + view_h)
            ImGui::SetScrollY(cursor_y + line_h - view_h);
        if (cursor_x < scroll_x)
            ImGui::SetScrollX(cursor_x);
        else if (cursor_x + char_w > scroll_x + view_w)
            ImGui::SetScrollX(cursor_x + char_w - view_w);
        ed->cursor_follow = false;
    }

    float scroll_y = ImGui::GetScrollY();
    int first_line = (int)floorf(scroll_y / line_h) - 2;
    int last_line = (int)ceilf((scroll_y + ImGui::GetWindowHeight()) / line_h) + 2;
    if (first_line < 0) first_line = 0;
    if (last_line > (int)lines.size()) last_line = (int)lines.size();

    ImRect clip = window->ClipRect;
    dl->PushClipRect(clip.Min, clip.Max, true);
    UiHlslDrawPalette pal = ui_hlsl_draw_palette();
    int sel_a = 0, sel_b = 0;
    bool has_selection = ui_shader_editor_has_selection(ed);
    if (has_selection)
        ui_shader_editor_selection(ed, &sel_a, &sel_b);
    UiShaderErrorMarker error_marker = {};
    bool has_error_marker = ui_shader_compile_error_marker(shader_resource, &error_marker);

    for (int i = first_line; i < last_line; i++) {
        const UiCodeLine& line = lines[i];
        float y = origin.y + (float)i * line_h;
        if (i == cursor_line && ed->editor_focused) {
            dl->AddRectFilled(ImVec2(clip.Min.x, y),
                              ImVec2(clip.Max.x, y + line_h),
                              ImGui::GetColorU32(ImVec4(0.13f, 0.10f, 0.09f, 0.75f)));
        }
        if (has_error_marker && i == error_marker.line - 1) {
            float text_x = origin.x + gutter_w;
            int cols = ui_code_visual_cols(line.begin, line.end);
            if (cols < 1)
                cols = 1;
            float underline_x1 = text_x + (float)cols * char_w;
            if (underline_x1 > clip.Max.x - ui_px(4.0f))
                underline_x1 = clip.Max.x - ui_px(4.0f);
            ImU32 error_col = ImGui::GetColorU32(ImVec4(1.0f, 0.22f, 0.18f, 1.0f));
            ImU32 error_bg = ImGui::GetColorU32(ImVec4(0.36f, 0.04f, 0.04f, 0.92f));
            float underline_y = y + line_h - ui_px(2.0f);
            dl->AddLine(ImVec2(text_x, underline_y), ImVec2(underline_x1, underline_y),
                        error_col, ui_px(1.6f));
            if (error_marker.message[0]) {
                ImVec2 msg_sz = ImGui::CalcTextSize(error_marker.message);
                float msg_x = text_x;
                float msg_y = y - line_h;
                if (msg_y < clip.Min.y)
                    msg_y = y + ui_px(1.0f);
                float msg_w = msg_sz.x + ui_px(10.0f);
                if (msg_x + msg_w > clip.Max.x - ui_px(4.0f))
                    msg_w = clip.Max.x - ui_px(4.0f) - msg_x;
                if (msg_w > ui_px(30.0f)) {
                    ImVec2 msg_min = ImVec2(msg_x, msg_y);
                    ImVec2 msg_max = ImVec2(msg_x + msg_w, msg_y + line_h);
                    dl->AddRectFilled(msg_min, msg_max, error_bg, ui_px(3.0f));
                    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                                ImVec2(msg_min.x + ui_px(5.0f), msg_min.y),
                                ImGui::GetColorU32(ImVec4(1.0f, 0.82f, 0.80f, 1.0f)),
                                error_marker.message);
                }
            }
        }

        if (has_selection) {
            int line_start = line.offset;
            int line_end = line.offset + (int)(line.end - line.begin);
            if (sel_b >= line_start && sel_a <= line_end + 1) {
                int a = sel_a > line_start ? sel_a : line_start;
                int b = sel_b < line_end ? sel_b : line_end;
                if (a <= b) {
                    const char* pa = line.begin + (a - line_start);
                    const char* pb = line.begin + (b - line_start);
                    float x0 = origin.x + gutter_w + (float)ui_code_visual_cols_to(line.begin, pa) * char_w;
                    float x1 = origin.x + gutter_w + (float)ui_code_visual_cols_to(line.begin, pb) * char_w;
                    if (sel_b > line_end && b == line_end)
                        x1 += char_w * 0.55f;
                    if (x1 <= x0)
                        x1 = x0 + char_w * 0.55f;
                    dl->AddRectFilled(ImVec2(x0, y),
                                      ImVec2(x1, y + line_h),
                                      ImGui::GetColorU32(ImVec4(0.55f, 0.28f, 0.16f, 0.55f)));
                }
            }
        }

        char line_no[16] = {};
        snprintf(line_no, sizeof(line_no), "%4d", i + 1);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(origin.x, y), pal.muted, line_no);
        ui_hlsl_draw_line_at(dl, ImVec2(origin.x + gutter_w, y),
                             line.begin, line.end, char_w, pal);
    }

    if (ed->editor_focused) {
        double t = ImGui::GetTime();
        if (fmod(t, 1.2) < 0.8) {
            float x = origin.x + gutter_w + (float)cursor_col * char_w;
            float y = origin.y + (float)cursor_line * line_h;
            dl->AddLine(ImVec2(x, y + 1.0f),
                        ImVec2(x, y + line_h - 1.0f),
                        ImGui::GetColorU32(ImGuiCol_InputTextCursor), 1.0f);
        }
    }
    dl->PopClipRect();

    ImGui::Dummy(ImVec2(content_w, content_h));
    ImGui::EndChild();
    if (s_code_font)
        ImGui::PopFont();
    ImGui::PopStyleColor();
    return changed;
}

static bool ui_shader_editor_load(UiShaderSourceEditor* ed, ResHandle h, const char* path) {
    if (!ed)
        return false;
    free(ed->text);
    ed->text = nullptr;
    ed->cap = 0;
    ed->ok = false;
    ed->dirty = false;
    ed->cursor = 0;
    ed->select_anchor = 0;
    ed->editor_focused = false;
    ed->dragging_selection = false;
    ed->cursor_follow = false;
    ed->h = h;
    strncpy(ed->path, path ? path : "", MAX_PATH_LEN - 1);
    ed->path[MAX_PATH_LEN - 1] = '\0';

    if (!ed->path[0])
        return false;

    void* data = nullptr;
    size_t size = 0;
    if (!lt_read_file(ed->path, &data, &size))
        return false;

    ed->cap = size + 4096;
    if (ed->cap < 8192)
        ed->cap = 8192;
    ed->text = (char*)malloc(ed->cap);
    if (!ed->text) {
        lt_free_file(data);
        ed->cap = 0;
        return false;
    }
    memcpy(ed->text, data, size);
    ed->text[size] = '\0';
    lt_free_file(data);
    ed->ok = true;
    return true;
}

static bool ui_write_text_file_atomic(const char* path, const char* text) {
    if (!path || !path[0] || !text)
        return false;
    char tmp_path[MAX_PATH_LEN + 16] = {};
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE* f = fopen(tmp_path, "wb");
    if (!f)
        return false;
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        DeleteFileA(tmp_path);
        return false;
    }
    if (!MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp_path);
        return false;
    }
    return true;
}

static bool ui_shader_editor_save(UiShaderSourceEditor* ed, ResHandle h, Resource* r, bool compile_after) {
    if (!ed || !ed->ok || !ed->text || !r)
        return false;
    if (!ui_write_text_file_atomic(ed->path, ed->text)) {
        log_error("Shader source save failed: %s", ed->path);
        return false;
    }
    ed->dirty = false;
    log_info("Shader source saved: %s", ed->path);
    if (compile_after)
        ui_recompile_shader_resource(h, r, r->path);
    return true;
}

static void ui_shader_source_viewer(ResHandle h, Resource* r) {
    static UiShaderSourceEditor ed = {};
    const char* path = r ? r->path : "";
    bool reload = ed.h != h || strcmp(ed.path, path ? path : "") != 0;

    if (reload) {
        ui_shader_editor_load(&ed, h, path);
    }

    if (ImGui::Button("Reload Source", ImVec2(-1.0f, 0.0f)))
        ui_shader_editor_load(&ed, h, path);

    if (!ed.path[0]) {
        ImGui::TextDisabled("No shader path.");
        return;
    }
    if (!ed.ok || !ed.text) {
        ImGui::TextDisabled("Could not read source: %s", ed.path);
        return;
    }

    ImGui::TextDisabled("%zu bytes%s", strlen(ed.text), ed.dirty ? "  modified" : "");
    if (ed.editor_focused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        ui_shader_editor_save(&ed, h, r, false);
    ui_shader_code_editor(&ed, r);
    s_shader_source_editor_focused = ed.editor_focused;

    if (s_shader_editor_auto_save_compile && ed.dirty && ImGui::GetTime() - ed.last_edit_time > 0.65)
        ui_shader_editor_save(&ed, h, r, true);
}

static bool ui_tinted_transform_row(const char* label, float* v, float speed,
                                    ImVec4 row_bg, ImVec4 frame_bg) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    float w = ImGui::GetContentRegionAvail().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(
        ImVec2(pos.x - 4.0f, pos.y - 2.0f),
        ImVec2(pos.x + w,     pos.y + h + 2.0f),
        ImGui::GetColorU32(row_bg),
        4.0f
    );

    ImGui::PushStyleColor(ImGuiCol_FrameBg, frame_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
        ImVec4(frame_bg.x * 1.25f, frame_bg.y * 1.25f, frame_bg.z * 1.25f, frame_bg.w));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
        ImVec4(frame_bg.x * 1.55f, frame_bg.y * 1.35f, frame_bg.z * 1.25f, frame_bg.w));

    bool changed = ImGui::DragFloat3(label, v, speed);

    ImGui::PopStyleColor(3);
    return changed;
}

static void ui_key_value(const char* key, const char* value) {
    ImGui::TextDisabled("%s", key);
    ImGui::SameLine(120.0f);
    ImGui::TextUnformatted(value ? value : "-");
}

static void ui_key_value_handle(const char* key, ResHandle h) {
    Resource* r = res_get(h);
    ui_key_value(key, r ? r->name : "(none)");
}

static void ui_command_shader_params(Command* c, Resource* shader) {
    if (!c || !shader)
        return;

    if (!shader->shader_cb.active) {
        ImGui::TextDisabled("No UserCB cbuffer reflected. Recommended: register(b2).");
        return;
    }

    user_cb_sync_command_params(c, shader);
    ImGui::TextDisabled("%s: register(b%u), %u bytes",
        shader->shader_cb.name, shader->shader_cb.bind_slot, shader->shader_cb.size);

    if (shader->shader_cb.var_count == 0) {
        ImGui::TextDisabled("No supported scalar/vector variables found.");
        return;
    }

    for (int i = 0; i < c->param_count; i++) {
        CommandParam& p = c->params[i];
        Resource* src = res_get(p.source);
        ImGui::PushID(i);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.080f, 0.077f, 0.081f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
        ImGui::BeginChild("##param_card", ImVec2(0.0f, 108.0f), true);

        ImGui::Checkbox("##enabled", &p.enabled);
        ImGui::SameLine();
        ImGui::TextUnformatted(p.name);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", res_type_str(p.type));

        if (!p.enabled)
            ImGui::BeginDisabled();

        ImGui::TextDisabled("Source");
        ImGui::SameLine(96.0f);
        ImGui::SetNextItemWidth(-1.0f);
        ui_command_param_source_combo(&p);

        ImGui::TextDisabled("Value");
        ImGui::SameLine(96.0f);
        UserCBSourceKind source_kind = p.source_kind;
        if (source_kind == USER_CB_SOURCE_NONE && p.source != INVALID_HANDLE)
            source_kind = USER_CB_SOURCE_RESOURCE;
        if (source_kind != USER_CB_SOURCE_NONE) {
            ImGui::TextDisabled("(source driven)");
        } else if (src && src->type == p.type) {
            ui_user_cb_value_editor(p.type, src->ival, src->fval, 0.0f);
        } else {
            ui_user_cb_value_editor(p.type, p.ival, p.fval, 0.0f);
        }

        if (!p.enabled)
            ImGui::EndDisabled();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

struct RTFormatOption {
    const char* name;
    DXGI_FORMAT fmt;
    bool depth;
    bool uav;
};

static const RTFormatOption k_rt_formats[] = {
    {"RGBA8 UNORM",       DXGI_FORMAT_R8G8B8A8_UNORM,       false, true},
    {"RGBA8 sRGB",        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  false, false},
    {"RGBA16 FLOAT",      DXGI_FORMAT_R16G16B16A16_FLOAT,   false, true},
    {"RGBA32 FLOAT",      DXGI_FORMAT_R32G32B32A32_FLOAT,   false, true},
    {"R32 FLOAT",         DXGI_FORMAT_R32_FLOAT,            false, true},
    {"R32 UINT",          DXGI_FORMAT_R32_UINT,             false, true},
    {"Depth24 Stencil8",  DXGI_FORMAT_D24_UNORM_S8_UINT,    true,  false},
    {"Depth32 FLOAT",     DXGI_FORMAT_D32_FLOAT,            true,  false},
};

static const RTFormatOption* ui_rt_format_info(DXGI_FORMAT fmt) {
    for (int i = 0; i < (int)(sizeof(k_rt_formats) / sizeof(k_rt_formats[0])); i++)
        if (k_rt_formats[i].fmt == fmt) return &k_rt_formats[i];
    return nullptr;
}

static const char* ui_rt_format_name(DXGI_FORMAT fmt) {
    const RTFormatOption* info = ui_rt_format_info(fmt);
    return info ? info->name : "Custom";
}

static bool ui_rt_format_combo(const char* label, DXGI_FORMAT* fmt) {
    bool changed = false;
    if (ImGui::BeginCombo(label, ui_rt_format_name(*fmt))) {
        for (int i = 0; i < (int)(sizeof(k_rt_formats) / sizeof(k_rt_formats[0])); i++) {
            bool sel = (*fmt == k_rt_formats[i].fmt);
            if (ImGui::Selectable(k_rt_formats[i].name, sel)) {
                *fmt = k_rt_formats[i].fmt;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void ui_clamp_rt_flags(DXGI_FORMAT fmt, bool* rtv, bool* srv, bool* uav, bool* dsv) {
    const RTFormatOption* info = ui_rt_format_info(fmt);
    if (info && info->depth) {
        *rtv = false;
        *uav = false;
        *dsv = true;
    } else {
        *dsv = false;
        if (info && !info->uav)
            *uav = false;
    }
    (void)srv;
}

static void ui_clamp_rt3d_flags(DXGI_FORMAT fmt, bool* rtv, bool* srv, bool* uav) {
    const RTFormatOption* info = ui_rt_format_info(fmt);
    if (info && info->depth) {
        *rtv = false;
        *uav = false;
    } else if (info && !info->uav) {
        *uav = false;
    }
    (void)srv;
}

static ImVec4 ui_type_color(ResType type) {
    switch (type) {
    case RES_SHADER:              return ImVec4(0.64f, 0.50f, 0.72f, 1.0f);
    case RES_MESH:                return ImVec4(0.55f, 0.66f, 0.72f, 1.0f);
    case RES_TEXTURE2D:
    case RES_RENDER_TEXTURE2D:
    case RES_RENDER_TEXTURE3D:
    case RES_BUILTIN_SCENE_COLOR:
    case RES_BUILTIN_SHADOW_MAP:  return ImVec4(0.72f, 0.58f, 0.42f, 1.0f);
    case RES_STRUCTURED_BUFFER:
    case RES_BUILTIN_SCENE_DEPTH: return ImVec4(0.46f, 0.66f, 0.61f, 1.0f);
    case RES_INT:
    case RES_INT2:
    case RES_INT3:
    case RES_FLOAT:
    case RES_FLOAT2:
    case RES_FLOAT3:
    case RES_FLOAT4:
    case RES_BUILTIN_TIME:        return ImVec4(0.68f, 0.68f, 0.68f, 1.0f);
    default:                      return ImVec4(0.58f, 0.60f, 0.63f, 1.0f);
    }
}

struct RTSceneScaleOption {
    const char* label;
    int divisor;
};

static const RTSceneScaleOption k_rt_scene_scale_options[] = {
    {"Fixed", 0},
    {"Scene", 1},
    {"Scene /2", 2},
    {"Scene /3", 3},
    {"Scene /4", 4},
    {"Scene /8", 8},
};

static const char* ui_rt_scene_scale_name(int divisor) {
    for (int i = 0; i < (int)(sizeof(k_rt_scene_scale_options) / sizeof(k_rt_scene_scale_options[0])); i++)
        if (k_rt_scene_scale_options[i].divisor == divisor)
            return k_rt_scene_scale_options[i].label;
    return divisor > 0 ? "Scene-scaled" : "Fixed";
}

static bool ui_rt_scene_scale_combo(const char* label, int* divisor) {
    bool changed = false;
    if (ImGui::BeginCombo(label, ui_rt_scene_scale_name(*divisor))) {
        for (int i = 0; i < (int)(sizeof(k_rt_scene_scale_options) / sizeof(k_rt_scene_scale_options[0])); i++) {
            bool sel = *divisor == k_rt_scene_scale_options[i].divisor;
            if (ImGui::Selectable(k_rt_scene_scale_options[i].label, sel)) {
                *divisor = k_rt_scene_scale_options[i].divisor;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static bool ui_resource_has_warning(const Resource& r) {
    return (r.type == RES_SHADER && !r.compiled_ok) ||
           (r.type == RES_MESH && r.using_fallback);
}

static bool ui_resource_filter_match(const Resource& r, int filter) {
    switch (filter) {
    case 1: return r.type == RES_MESH;
    case 2: return r.type == RES_SHADER;
    case 3: return r.type == RES_TEXTURE2D || r.type == RES_RENDER_TEXTURE2D ||
                   r.type == RES_RENDER_TEXTURE3D ||
                   r.type == RES_BUILTIN_SCENE_COLOR || r.type == RES_BUILTIN_SHADOW_MAP;
    case 4: return r.type == RES_STRUCTURED_BUFFER || r.type == RES_BUILTIN_SCENE_DEPTH;
    case 5: return r.type == RES_INT || r.type == RES_INT2 || r.type == RES_INT3 || r.type == RES_FLOAT ||
                   r.type == RES_FLOAT2 || r.type == RES_FLOAT3 || r.type == RES_FLOAT4 ||
                   r.type == RES_BUILTIN_TIME;
    default: return true;
    }
}

static void ui_filter_button(const char* label, int value, int* current) {
    bool selected = (*current == value);
    if (selected) {
        ImVec4 accent = ImVec4(0.78f, 0.42f, 0.32f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x * 0.32f, accent.y * 0.32f, accent.z * 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.95f, 0.96f, 1.0f));
    }
    if (ImGui::SmallButton(label))
        *current = value;
    if (selected)
        ImGui::PopStyleColor(2);
}

static void ui_draw_badge(ImDrawList* dl, ImVec2 min, const char* text, ImVec4 tint) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 max = ImVec2(min.x + ts.x + 12.0f, min.y + ts.y + 6.0f);
    ImVec4 bg = ImVec4(tint.x * 0.22f, tint.y * 0.22f, tint.z * 0.22f, 1.0f);
    dl->AddRectFilled(min, max, ImGui::GetColorU32(bg), 3.0f);
    dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(tint.x * 0.42f, tint.y * 0.42f, tint.z * 0.42f, 1.0f)), 3.0f);
    dl->AddText(ImVec2(min.x + 6.0f, min.y + 3.0f), ImGui::GetColorU32(tint), text);
}

static void ui_inline_badge(const char* id, const char* text, ImVec4 tint, float min_h = 0.0f) {
    if (!text || !text[0])
        return;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(text);
    float badge_h = ts.y + 6.0f;
    ImVec2 size(ts.x + 12.0f, badge_h > min_h ? badge_h : min_h);
    ImGui::InvisibleButton(id, size);
    float badge_y = pos.y + floorf((size.y - badge_h) * 0.5f);
    ui_draw_badge(ImGui::GetWindowDrawList(), ImVec2(pos.x, badge_y), text, tint);
}

static bool ui_has_draw_rtv(ResHandle h) {
    Resource* r = res_get(h);
    if (!r)
        return false;
    if (r->type == RES_BUILTIN_SCENE_COLOR)
        return true;
    return r->rtv != nullptr;
}

static UINT ui_draw_command_rtv_count(const Command& c) {
    if (!c.color_write)
        return 0;

    UINT count = ui_has_draw_rtv(c.rt) ? 1u : 0u;
    int extra_count = c.mrt_count;
    if (extra_count < 0) extra_count = 0;
    if (extra_count > MAX_DRAW_RENDER_TARGETS - 1) extra_count = MAX_DRAW_RENDER_TARGETS - 1;
    for (int i = 0; i < extra_count; i++) {
        if (ui_has_draw_rtv(c.mrt_handles[i]))
            count = (UINT)(i + 2);
    }
    return count;
}

static bool ui_command_supports_gizmo_type(CmdType type) {
    return type == CMD_DRAW_MESH || type == CMD_DRAW_INSTANCED || type == CMD_INDIRECT_DRAW;
}

static int ui_mesh_enabled_part_count(const Resource* mesh) {
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

static bool ui_command_uses_indexed_mesh_draw(const Command& c, const Resource* mesh) {
    return !ui_command_uses_procedural_draw(c) && mesh && mesh->ib;
}

static UINT ui_indirect_draw_args_required_bytes(const Command& c, const Resource* mesh) {
    return ui_command_uses_indexed_mesh_draw(c, mesh)
        ? (UINT)sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)
        : (UINT)sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS);
}

static UINT ui_indirect_dispatch_args_required_bytes() {
    return (UINT)(sizeof(UINT) * 3);
}

static bool ui_indirect_args_buffer_has_range(const Resource* buf, uint32_t offset, UINT required_bytes) {
    if (!buf || !buf->buf || !buf->indirect_args || buf->elem_size < 1 || buf->elem_count < 1)
        return false;
    if ((offset & 3u) != 0u)
        return false;

    uint64_t total_bytes = (uint64_t)buf->elem_size * (uint64_t)buf->elem_count;
    uint64_t end = (uint64_t)offset + (uint64_t)required_bytes;
    return end <= total_bytes;
}

static bool ui_draw_command_has_common_warning(const Command& c, bool indirect) {
    bool procedural = ui_command_uses_procedural_draw(c);
    Resource* mesh = res_get(c.mesh);
    Resource* shader = res_get(c.shader);
    if (!shader || !shader->vs || !shader->ps) return true;
    if (!shader->compiled_ok) return true;

    if (procedural) {
        if (!indirect && c.vertex_count < 1) return true;
        if (c.draw_topology != DRAW_TOPOLOGY_TRIANGLE_LIST &&
            c.draw_topology != DRAW_TOPOLOGY_POINT_LIST) return true;
    } else {
        if (!mesh || !mesh->vb || !shader->il) return true;
        if (mesh->using_fallback) return true;
    }

    if (indirect) {
        Resource* ibuf = res_get(c.indirect_buf);
        if (!ui_indirect_args_buffer_has_range(ibuf, c.indirect_offset,
                                               ui_indirect_draw_args_required_bytes(c, mesh))) {
            return true;
        }
        if (!procedural && mesh && mesh->mesh_material_count > 0 &&
            ui_mesh_enabled_part_count(mesh) != 1) {
            return true;
        }
    }

    int extra_count = c.mrt_count;
    if (extra_count < 0 || extra_count > MAX_DRAW_RENDER_TARGETS - 1) return true;
    for (int i = 0; i < extra_count; i++) {
        if (!res_get(c.mrt_handles[i]) || !ui_has_draw_rtv(c.mrt_handles[i]))
            return true;
    }
    UINT rtv_count = ui_draw_command_rtv_count(c);
    for (int i = 0; i < c.uav_count; i++) {
        if (c.uav_slots[i] >= MAX_UAV_SLOTS)
            return true;
        Resource* ur = res_get(c.uav_handles[i]);
        if (!ur || !ur->uav)
            return true;
        if (c.uav_slots[i] < rtv_count)
            return true;
    }
    return false;
}

static bool ui_command_has_warning(const Command& c) {
    if (!c.enabled) return false;

    switch (c.type) {
    case CMD_CLEAR:
        if (c.clear_color_enabled && !res_get(c.rt)) return true;
        if (c.clear_depth && !res_get(c.depth)) return true;
        return false;

    case CMD_DRAW_MESH:
    case CMD_DRAW_INSTANCED: {
        return ui_draw_command_has_common_warning(c, false);
    }

    case CMD_DISPATCH: {
        Resource* shader = res_get(c.shader);
        if (!shader || !shader->cs || !shader->compiled_ok) return true;
        return false;
    }

    case CMD_INDIRECT_DRAW: {
        return ui_draw_command_has_common_warning(c, true);
    }

    case CMD_INDIRECT_DISPATCH: {
        Resource* ibuf = res_get(c.indirect_buf);
        Resource* shader = res_get(c.shader);
        if (!shader || !shader->cs || !shader->compiled_ok) return true;
        if (!ui_indirect_args_buffer_has_range(ibuf, c.indirect_offset,
                                               ui_indirect_dispatch_args_required_bytes())) return true;
        return false;
    }

    case CMD_REPEAT: {
        if (c.repeat_count < 1) return true;
        CmdHandle h = (CmdHandle)(&c - g_commands + 1);
        bool has_child = false;
        for (int i = 0; i < MAX_COMMANDS; i++) {
            Command& child = g_commands[i];
            if (!child.active || child.parent != h) continue;
            has_child = true;
            if (ui_command_has_warning(child)) return true;
        }
        return !has_child;
    }

    case CMD_GROUP: {
        CmdHandle h = (CmdHandle)(&c - g_commands + 1);
        for (int i = 0; i < MAX_COMMANDS; i++) {
            Command& child = g_commands[i];
            if (!child.active || child.parent != h)
                continue;
            if (ui_command_has_warning(child))
                return true;
        }
        return false;
    }

    default:
        return false;
    }
}

static void ui_draw_list_row_bg(ImVec2 min, ImVec2 max, bool selected, bool hovered, bool nav_target, int index) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.190f, 0.172f, 0.168f, 1.0f)), 3.0f);
        dl->AddRectFilled(min, ImVec2(min.x + 3.0f, max.y), ImGui::GetColorU32(ImVec4(0.78f, 0.42f, 0.32f, 0.78f)), 2.0f);
    } else if (nav_target) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.135f, 0.123f, 0.120f, 1.0f)), 3.0f);
        dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.60f, 0.36f, 0.30f, 0.75f)), 3.0f, 0, 1.0f);
    } else if (hovered) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.140f, 0.128f, 0.126f, 1.0f)), 3.0f);
    } else if ((index & 1) != 0) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.100f, 0.096f, 0.098f, 0.55f)), 3.0f);
    }
}

static bool ui_command_is_inside_group(CmdHandle h) {
    CmdHandle cur = h;
    for (int depth = 0; depth < MAX_COMMANDS && cur != INVALID_HANDLE; depth++) {
        Command* c = cmd_get(cur);
        if (!c)
            return false;
        cur = c->parent;
        if (cur == INVALID_HANDLE)
            return false;
        Command* parent = cmd_get(cur);
        if (!parent)
            return false;
        if (parent->type == CMD_GROUP)
            return true;
    }
    return false;
}

static void ui_draw_command_row_bg(ImVec2 min, ImVec2 max, bool selected, bool hovered,
                                   bool nav_target, int index, bool inside_group)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.190f, 0.172f, 0.168f, 0.98f)), 3.0f);
        dl->AddRectFilled(min, ImVec2(min.x + 3.0f, max.y), ImGui::GetColorU32(ImVec4(0.78f, 0.42f, 0.32f, 0.78f)), 2.0f);
        return;
    }

    if (inside_group) {
        if (nav_target) {
            dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.21f, 0.19f, 0.17f, 0.34f)), 3.0f);
            dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.60f, 0.36f, 0.30f, 0.52f)), 3.0f, 0, 1.0f);
        } else if (hovered) {
            dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.22f, 0.20f, 0.18f, 0.20f)), 3.0f);
        }
        return;
    }

    ui_draw_list_row_bg(min, max, selected, hovered, nav_target, index);
}

static bool ui_command_is_descendant(CmdHandle child_h, CmdHandle possible_ancestor) {
    CmdHandle cur = child_h;
    for (int depth = 0; depth < MAX_COMMANDS && cur != INVALID_HANDLE; depth++) {
        Command* c = cmd_get(cur);
        if (!c)
            return false;
        if (c->parent == possible_ancestor)
            return true;
        cur = c->parent;
    }
    return false;
}

static bool ui_resource_row(int index, Resource& r) {
    ResHandle h = (ResHandle)(index + 1);
    bool selected = (g_sel_res == h);
    bool nav_target = (s_res_nav == h);
    bool bad = ui_resource_has_warning(r);
    bool deleted = false;

    ImGui::PushID(index);
    if (s_rename_active && selected && !s_rename_is_cmd) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##rename_res", s_rename_buf, MAX_NAME,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            res_rename(h, s_rename_buf);
            s_rename_active = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s_rename_active = false;
        ImGui::PopID();
        return false;
    }

    const float row_h = ImGui::GetTextLineHeight() + 10.0f;
    float width = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##res_row", ImVec2(width, row_h));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        g_sel_res = h;
        g_sel_cmd = INVALID_HANDLE;
        s_res_nav = h;
    }

    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui_draw_list_row_bg(min, max, selected, hovered, nav_target, index);

    ImVec4 tint = ui_type_color(r.type);
    ImU32 dot_col = ImGui::GetColorU32(bad ? ImVec4(0.96f, 0.70f, 0.28f, 1.0f) : tint);
    float cy = (min.y + max.y) * 0.5f;
    dl->AddCircleFilled(ImVec2(min.x + 11.0f, cy), 3.5f, dot_col, 12);

    const char* type = ui_resource_display_type(r);
    ImVec2 badge_ts = ImGui::CalcTextSize(type);
    float badge_w = badge_ts.x + 12.0f;
    ImVec2 badge_min = ImVec2(max.x - badge_w - 6.0f, min.y + 4.0f);
    if (badge_min.x > min.x + 96.0f)
        ui_draw_badge(dl, badge_min, type, tint);

    ImU32 name_col = ImGui::GetColorU32(r.is_builtin ? ImVec4(0.62f, 0.64f, 0.67f, 1.0f) :
        (bad ? ImVec4(0.96f, 0.72f, 0.36f, 1.0f) : ImVec4(0.90f, 0.91f, 0.92f, 1.0f)));
    ImVec2 name_pos = ImVec2(min.x + 22.0f, min.y + 5.0f);
    dl->PushClipRect(name_pos, ImVec2(badge_min.x - 6.0f, max.y), true);
    dl->AddText(name_pos, name_col, ui_resource_display_name(r));
    dl->PopClipRect();

    if (bad)
        dl->AddText(ImVec2(max.x - 18.0f, min.y + 5.0f), ImGui::GetColorU32(ImVec4(1.0f, 0.78f, 0.28f, 1.0f)), "!");

    if (!r.is_builtin && ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Add to User CB", nullptr, false, user_cb_type_supported(r.type))) {
            user_cb_add_from_resource(h);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename")) {
            g_sel_res = h;
            strncpy(s_rename_buf, r.name, MAX_NAME - 1);
            s_rename_active = true; s_rename_is_cmd = false;
        }
        if (ImGui::MenuItem("Delete")) {
            res_free(h);
            if (g_sel_res == h) g_sel_res = INVALID_HANDLE;
            app_request_scene_render();
            deleted = true;
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();
    return deleted;
}

static bool ui_command_is_container(const Command* c) {
    return c && (c->type == CMD_GROUP || c->type == CMD_REPEAT);
}

static bool ui_can_paste_command_clipboard() {
    return s_cmd_clipboard_count > 0;
}

static void ui_copy_command_subtree_recursive(CmdHandle h, int parent_index) {
    Command* c = cmd_get(h);
    if (!c || s_cmd_clipboard_count >= MAX_COMMANDS)
        return;

    int index = s_cmd_clipboard_count++;
    s_cmd_clipboard[index].cmd = *c;
    s_cmd_clipboard[index].parent_index = parent_index;

    for (int i = 0; i < MAX_COMMANDS && s_cmd_clipboard_count < MAX_COMMANDS; i++) {
        Command& child = g_commands[i];
        if (!child.active || child.parent != h)
            continue;
        ui_copy_command_subtree_recursive((CmdHandle)(i + 1), index);
    }
}

static bool ui_copy_command_to_clipboard(CmdHandle h) {
    s_cmd_clipboard_count = 0;
    if (!cmd_get(h))
        return false;
    ui_copy_command_subtree_recursive(h, -1);
    return s_cmd_clipboard_count > 0;
}

static CmdHandle ui_find_last_command_child(CmdHandle parent) {
    CmdHandle last = INVALID_HANDLE;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (!g_commands[i].active || g_commands[i].parent != parent)
            continue;
        last = (CmdHandle)(i + 1);
    }
    return last;
}

static CmdHandle ui_paste_command_clipboard(CmdHandle target_h) {
    CmdHandle mapped[MAX_COMMANDS] = {};
    CmdHandle allocated[MAX_COMMANDS] = {};
    int allocated_count = 0;
    CmdHandle paste_parent = INVALID_HANDLE;
    CmdHandle anchor = INVALID_HANDLE;
    CmdHandle new_root = INVALID_HANDLE;
    Command* target = cmd_get(target_h);

    if (!ui_can_paste_command_clipboard())
        return INVALID_HANDLE;

    if (target) {
        if (ui_command_is_container(target)) {
            paste_parent = target_h;
            anchor = ui_find_last_command_child(paste_parent);
        } else {
            paste_parent = target->parent;
            anchor = target_h;
        }
    } else {
        anchor = ui_find_last_command_child(INVALID_HANDLE);
    }

    for (int i = 0; i < s_cmd_clipboard_count; i++) {
        Command src = s_cmd_clipboard[i].cmd;
        int parent_index = s_cmd_clipboard[i].parent_index;
        char unique_name[MAX_NAME] = {};

        cmd_make_unique_name(src.name, unique_name, MAX_NAME);
        CmdHandle new_h = cmd_alloc(unique_name, src.type);
        Command* dst = cmd_get(new_h);
        if (new_h == INVALID_HANDLE || !dst)
            goto fail;

        allocated[allocated_count++] = new_h;
        *dst = src;
        strncpy(dst->name, unique_name, MAX_NAME - 1);
        dst->name[MAX_NAME - 1] = '\0';
        dst->active = true;
        dst->parent = parent_index >= 0 ? mapped[parent_index] : paste_parent;
        mapped[i] = new_h;
        if (i == 0)
            new_root = new_h;
    }

    if (target && ui_command_is_container(target))
        target->repeat_expanded = true;
    if (anchor != INVALID_HANDLE)
        new_root = cmd_move(new_root, anchor, true);
    return new_root;

fail:
    for (int i = allocated_count - 1; i >= 0; i--) {
        if (cmd_get(allocated[i]))
            cmd_free(allocated[i]);
    }
    return INVALID_HANDLE;
}

static bool ui_command_row(int index, Command& c, int depth = 0) {
    CmdHandle h = (CmdHandle)(index + 1);
    bool selected = (g_sel_cmd == h);
    bool nav_target = (s_cmd_nav == h);
    bool warning = ui_command_has_warning(c);
    bool deleted = false;
    float indent = (float)depth * 18.0f;
    bool inside_group = ui_command_is_inside_group(h);

    ImGui::PushID(index);
    if (s_rename_active && selected && s_rename_is_cmd) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##rename_cmd", s_rename_buf, MAX_NAME,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            cmd_rename(h, s_rename_buf);
            s_rename_active = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) s_rename_active = false;
        ImGui::PopID();
        return false;
    }

    const float row_h = c.type == CMD_GROUP ? (ImGui::GetTextLineHeight() + 8.0f) : (ImGui::GetTextLineHeight() + 12.0f);
    float width = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##cmd_row", ImVec2(width, row_h));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        ImVec2 item_min = ImGui::GetItemRectMin();
        ImVec2 mp = ImGui::GetIO().MousePos;
        if ((c.type == CMD_REPEAT || c.type == CMD_GROUP) &&
            mp.x >= item_min.x + indent && mp.x <= item_min.x + indent + 24.0f)
            c.repeat_expanded = !c.repeat_expanded;
        if (selected && s_viewport_gizmo_mode != UI_GIZMO_NONE &&
            ui_command_supports_gizmo_type(c.type)) {
            s_viewport_gizmo_mode = UI_GIZMO_NONE;
            memset(&s_viewport_gizmo_drag, 0, sizeof(s_viewport_gizmo_drag));
        }
        g_sel_cmd = h;
        g_sel_res = INVALID_HANDLE;
        s_cmd_nav = h;
    }

    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui_draw_command_row_bg(min, max, selected, hovered, nav_target, index, inside_group);

    float cy = (min.y + max.y) * 0.5f;
    float row_x = min.x + indent;
    if (c.type == CMD_REPEAT || c.type == CMD_GROUP) {
        ImU32 arrow_col = ImGui::GetColorU32(c.enabled ?
            ImVec4(0.72f, 0.74f, 0.78f, 1.0f) : ImVec4(0.42f, 0.43f, 0.45f, 1.0f));
        ImVec2 center = ImVec2(row_x + 13.0f, cy);
        if (c.repeat_expanded) {
            dl->AddTriangleFilled(ImVec2(center.x - 4.5f, center.y - 2.5f),
                                  ImVec2(center.x + 4.5f, center.y - 2.5f),
                                  ImVec2(center.x,        center.y + 3.5f),
                                  arrow_col);
        } else {
            dl->AddTriangleFilled(ImVec2(center.x - 2.5f, center.y - 4.5f),
                                  ImVec2(center.x - 2.5f, center.y + 4.5f),
                                  ImVec2(center.x + 3.5f, center.y),
                                  arrow_col);
        }
    }
    ImVec4 dot = !c.enabled ? ImVec4(0.35f, 0.36f, 0.38f, 1.0f) :
        (warning ? ImVec4(0.96f, 0.70f, 0.28f, 1.0f) : ImVec4(0.38f, 0.76f, 0.52f, 1.0f));
    if (c.type != CMD_REPEAT && c.type != CMD_GROUP)
        dl->AddCircleFilled(ImVec2(row_x + 13.0f, cy), 4.0f, ImGui::GetColorU32(dot), 12);

    const char* type = cmd_type_str(c.type);
    char type_buf[32] = {};
    if (c.type == CMD_REPEAT) {
        snprintf(type_buf, sizeof(type_buf), "Repeat x%d", c.repeat_count);
        type = type_buf;
    } else if (c.type == CMD_GROUP) {
        type = "Group";
    }
    ImVec2 badge_ts = ImGui::CalcTextSize(type);
    float badge_w = badge_ts.x + 12.0f;
    float badge_h = badge_ts.y + 6.0f;
    float text_y = floorf(cy - ImGui::GetTextLineHeight() * 0.5f);
    float right_x = max.x - (warning ? 24.0f : 6.0f);
    bool show_type_badge = c.type != CMD_GROUP;
    char profile_buf[32] = {};
    if (g_profiler_enabled) {
        if (cmd_profile_ready())
            snprintf(profile_buf, sizeof(profile_buf), "%.3f ms", cmd_profile_ms(h));
        else
            snprintf(profile_buf, sizeof(profile_buf), "...");
        ImVec2 ts = ImGui::CalcTextSize(profile_buf);
        dl->AddText(ImVec2(right_x - ts.x, floorf(cy - ts.y * 0.5f)),
            ImGui::GetColorU32(ImVec4(0.54f, 0.56f, 0.60f, 1.0f)), profile_buf);
        right_x -= ts.x + 10.0f;
    }
    ImVec2 badge_min = ImVec2(right_x - badge_w, floorf(cy - badge_h * 0.5f));
    if (show_type_badge && badge_min.x > row_x + 96.0f)
        ui_draw_badge(dl, badge_min, type, ImVec4(0.60f, 0.62f, 0.66f, 1.0f));

    ImU32 name_col = ImGui::GetColorU32(c.enabled ? ImVec4(0.92f, 0.93f, 0.94f, 1.0f) :
        ImVec4(0.48f, 0.49f, 0.51f, 1.0f));
    ImVec2 name_pos = ImVec2(row_x + 28.0f, text_y);
    float clip_right = show_type_badge ? (badge_min.x - 6.0f) : (right_x - 6.0f);
    dl->PushClipRect(name_pos, ImVec2(clip_right, max.y), true);
    dl->AddText(name_pos, name_col, c.name);
    dl->PopClipRect();

    if (warning)
        dl->AddText(ImVec2(max.x - 18.0f, text_y), ImGui::GetColorU32(ImVec4(1.0f, 0.78f, 0.28f, 1.0f)), "!");

    bool moved = false;
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
        ImGui::SetDragDropPayload("LAZY_CMD_ROW", &h, sizeof(h));
        ImGui::TextUnformatted(c.name);
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LAZY_CMD_ROW",
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (payload && payload->DataSize == sizeof(CmdHandle)) {
            CmdHandle src_h = *(const CmdHandle*)payload->Data;
            Command* src = cmd_get(src_h);
            if (src && src_h != h && !ui_command_is_descendant(h, src_h)) {
                bool wants_group_drop = c.type == CMD_GROUP && ImGui::GetIO().MousePos.y > min.y + 5.0f &&
                                        ImGui::GetIO().MousePos.y < max.y - 5.0f;
                if (wants_group_drop) {
                    dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.72f, 0.74f, 0.78f, 0.85f)), 3.0f, 0, 2.0f);
                    if (payload->IsDelivery()) {
                        src->parent = h;
                        c.repeat_expanded = true;
                        g_sel_cmd = src_h;
                        g_sel_res = INVALID_HANDLE;
                        s_cmd_nav = src_h;
                        moved = true;
                    }
                } else if (src->parent == c.parent) {
                    bool after = ImGui::GetIO().MousePos.y > cy;
                    float y = after ? max.y : min.y;
                    dl->AddLine(ImVec2(min.x + indent, y), ImVec2(max.x, y),
                        ImGui::GetColorU32(ImVec4(0.72f, 0.74f, 0.78f, 1.0f)), 2.0f);
                    if (payload->IsDelivery()) {
                        CmdHandle moved_h = cmd_move(src_h, h, after);
                        g_sel_cmd = moved_h;
                        g_sel_res = INVALID_HANDLE;
                        s_cmd_nav = moved_h;
                        moved = true;
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (moved) {
        ImGui::PopID();
        return true;
    }

    if (ImGui::BeginPopupContextItem()) {
        if (c.parent != INVALID_HANDLE) {
            if (ImGui::MenuItem("Detach from Container"))
                c.parent = INVALID_HANDLE;
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
            ui_copy_command_to_clipboard(h);
        if (ImGui::MenuItem(ui_command_is_container(&c) ? "Paste Inside" : "Paste After",
                            "Ctrl+V", false, ui_can_paste_command_clipboard())) {
            CmdHandle pasted_h = ui_paste_command_clipboard(h);
            if (pasted_h != INVALID_HANDLE) {
                g_sel_cmd = pasted_h;
                g_sel_res = INVALID_HANDLE;
                s_cmd_nav = pasted_h;
                moved = true;
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Enabled", nullptr, &c.enabled)) {
            timeline_capture_if_tracked(TIMELINE_TRACK_COMMAND_ENABLED, c.name, RES_NONE);
            app_request_scene_render();
        }
        if (c.type == CMD_REPEAT) {
            if (ImGui::MenuItem("Add Dispatch Child")) {
                char uname[MAX_NAME];
                cmd_make_unique_name("dispatch_0", uname, MAX_NAME);
                CmdHandle child_h = cmd_alloc(uname, CMD_DISPATCH);
                if (Command* child = cmd_get(child_h))
                    child->parent = h;
                c.repeat_expanded = true;
                g_sel_cmd = child_h;
                g_sel_res = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("Add IndirectDispatch Child")) {
                char uname[MAX_NAME];
                cmd_make_unique_name("idisp_0", uname, MAX_NAME);
                CmdHandle child_h = cmd_alloc(uname, CMD_INDIRECT_DISPATCH);
                if (Command* child = cmd_get(child_h))
                    child->parent = h;
                c.repeat_expanded = true;
                g_sel_cmd = child_h;
                g_sel_res = INVALID_HANDLE;
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Rename")) {
            g_sel_cmd = h;
            strncpy(s_rename_buf, c.name, MAX_NAME - 1);
            s_rename_active = true; s_rename_is_cmd = true;
        }
        if (ImGui::MenuItem("Delete")) {
            cmd_free(h);
            if (g_sel_cmd == h) g_sel_cmd = INVALID_HANDLE;
            app_request_scene_render();
            deleted = true;
        }
        ImGui::EndPopup();
    }

    if (moved) {
        ImGui::PopID();
        return true;
    }

    ImGui::PopID();
    return deleted;
}

static int ui_collect_visible_resources(bool builtins, int filter, ResHandle* out, int max_count) {
    int count = 0;
    for (int i = 0; i < MAX_RESOURCES && count < max_count; i++) {
        Resource& r = g_resources[i];
        if (!r.active || r.is_generated || r.is_builtin != builtins || !ui_resource_filter_match(r, filter))
            continue;
        out[count++] = (ResHandle)(i + 1);
    }
    return count;
}

static int ui_find_visible_resource_index(const ResHandle* items, int count, ResHandle h) {
    for (int i = 0; i < count; i++)
        if (items[i] == h)
            return i;
    return -1;
}

static int ui_collect_visible_commands_recursive(CmdHandle parent, CmdHandle* out, int max_count) {
    int count = 0;
    for (int i = 0; i < MAX_COMMANDS && count < max_count; i++) {
        Command& c = g_commands[i];
        if (!c.active || c.parent != parent)
            continue;
        CmdHandle h = (CmdHandle)(i + 1);
        out[count++] = h;
        if ((c.type == CMD_REPEAT || c.type == CMD_GROUP) && c.repeat_expanded)
            count += ui_collect_visible_commands_recursive(h, out + count, max_count - count);
    }
    return count;
}

static int ui_find_visible_command_index(const CmdHandle* items, int count, CmdHandle h) {
    for (int i = 0; i < count; i++)
        if (items[i] == h)
            return i;
    return -1;
}

static void ui_panel_resources(bool embedded = false) {
    if (!embedded) ImGui::Begin("Resources");

    static char s_rt_name[MAX_NAME] = {};
    static int s_rt_w = 512, s_rt_h = 512;
    static DXGI_FORMAT s_rt_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    static bool s_rt_rtv = true, s_rt_srv = true, s_rt_uav = false, s_rt_dsv = false;
    static int s_rt_scene_div = 0;
    static bool s_open_rt_create = false;

    static char s_rt3_name[MAX_NAME] = {};
    static int s_rt3_w = 128, s_rt3_h = 128, s_rt3_d = 16;
    static DXGI_FORMAT s_rt3_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    static bool s_rt3_rtv = false, s_rt3_srv = true, s_rt3_uav = true;
    static bool s_open_rt3_create = false;

    static char s_sb_name[MAX_NAME] = {};
    static int s_sb_stride = 16, s_sb_count = 64;
    static bool s_sb_srv = true, s_sb_uav = true, s_sb_indirect_args = false;
    static bool s_open_sb_create = false;
    static bool s_user_scope_active = true;

    if (s_user_scope_active && ImGui::BeginPopupContextWindow("res_ctx_bg",
        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::BeginMenu("Create")) {
            char uname[MAX_NAME];
            if (ImGui::MenuItem("int")) {
                res_make_unique_name("int_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_INT); g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("int2")) {
                res_make_unique_name("int2_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_INT2); g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("int3")) {
                res_make_unique_name("int3_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_INT3); g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("float")) {
                res_make_unique_name("float_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_FLOAT); g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("float2")) {
                res_make_unique_name("float2_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_FLOAT2); g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("float3")) {
                res_make_unique_name("float3_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_FLOAT3); g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("float4")) {
                res_make_unique_name("float4_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_FLOAT4); g_sel_cmd = INVALID_HANDLE;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("RenderTexture2D...")) {
                res_make_unique_name("rt_0", uname, MAX_NAME);
                strncpy(s_rt_name, uname, MAX_NAME - 1);
                s_rt_name[MAX_NAME - 1] = '\0';
                s_rt_w = 512; s_rt_h = 512;
                s_rt_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
                s_rt_rtv = true; s_rt_srv = true; s_rt_uav = false; s_rt_dsv = false;
                s_rt_scene_div = 0;
                s_open_rt_create = true;
            }
            if (ImGui::MenuItem("RenderTexture3D...")) {
                res_make_unique_name("rt3d_0", uname, MAX_NAME);
                strncpy(s_rt3_name, uname, MAX_NAME - 1);
                s_rt3_name[MAX_NAME - 1] = '\0';
                s_rt3_w = 128; s_rt3_h = 128; s_rt3_d = 16;
                s_rt3_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
                s_rt3_rtv = false; s_rt3_srv = true; s_rt3_uav = true;
                s_open_rt3_create = true;
            }
            if (ImGui::MenuItem("StructuredBuffer...")) {
                res_make_unique_name("sb_0", uname, MAX_NAME);
                strncpy(s_sb_name, uname, MAX_NAME - 1);
                s_sb_name[MAX_NAME - 1] = '\0';
                s_sb_stride = 16; s_sb_count = 64;
                s_sb_srv = true; s_sb_uav = true; s_sb_indirect_args = false;
                s_open_sb_create = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Shader (VS+PS)")) {
                res_make_unique_name("shader_0", uname, MAX_NAME);
                g_sel_res = res_create_shader(uname, "shaders/scene.hlsl", "VSMain", "PSMain");
                g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("Shader (CS)")) {
                res_make_unique_name("cs_0", uname, MAX_NAME);
                g_sel_res = res_create_compute_shader(uname, "", "CSMain");
                g_sel_cmd = INVALID_HANDLE;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load Texture... (set path in Inspector)")) {
                res_make_unique_name("tex_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_TEXTURE2D);
                g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("Load Mesh glTF... (set path in Inspector)")) {
                res_make_unique_name("mesh_0", uname, MAX_NAME);
                g_sel_res = res_alloc(uname, RES_MESH);
                g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::BeginMenu("Mesh Primitive")) {
                if (ImGui::MenuItem("Cube")) {
                    res_make_unique_name("cube_0", uname, MAX_NAME);
                    g_sel_res = res_create_mesh_primitive(uname, MESH_PRIM_CUBE);
                    g_sel_cmd = INVALID_HANDLE;
                }
                if (ImGui::MenuItem("Quad")) {
                    res_make_unique_name("quad_0", uname, MAX_NAME);
                    g_sel_res = res_create_mesh_primitive(uname, MESH_PRIM_QUAD);
                    g_sel_cmd = INVALID_HANDLE;
                }
                if (ImGui::MenuItem("Tetrahedron")) {
                    res_make_unique_name("tetra_0", uname, MAX_NAME);
                    g_sel_res = res_create_mesh_primitive(uname, MESH_PRIM_TETRAHEDRON);
                    g_sel_cmd = INVALID_HANDLE;
                }
                if (ImGui::MenuItem("Sphere")) {
                    res_make_unique_name("sphere_0", uname, MAX_NAME);
                    g_sel_res = res_create_mesh_primitive(uname, MESH_PRIM_SPHERE);
                    g_sel_cmd = INVALID_HANDLE;
                }
                if (ImGui::MenuItem("Fullscreen Triangle")) {
                    res_make_unique_name("fs_tri_0", uname, MAX_NAME);
                    g_sel_res = res_create_mesh_primitive(uname, MESH_PRIM_FULLSCREEN_TRIANGLE);
                    g_sel_cmd = INVALID_HANDLE;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    if (s_open_rt_create) {
        ImGui::OpenPopup("Create RenderTexture2D");
        s_open_rt_create = false;
    }
    if (s_open_rt3_create) {
        ImGui::OpenPopup("Create RenderTexture3D");
        s_open_rt3_create = false;
    }
    if (s_open_sb_create) {
        ImGui::OpenPopup("Create StructuredBuffer");
        s_open_sb_create = false;
    }

    if (ImGui::BeginPopupModal("Create RenderTexture2D", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", s_rt_name, MAX_NAME);
        ui_rt_scene_scale_combo("Resolution", &s_rt_scene_div);
        if (s_rt_scene_div == 0) {
            ImGui::InputInt("Width", &s_rt_w);
            ImGui::InputInt("Height", &s_rt_h);
            if (s_rt_w < 1) s_rt_w = 1;
            if (s_rt_h < 1) s_rt_h = 1;
        } else {
            int preview_w = g_dx.scene_width > 0 ? g_dx.scene_width : s_rt_w;
            int preview_h = g_dx.scene_height > 0 ? g_dx.scene_height : s_rt_h;
            if (s_rt_scene_div > 1) {
                preview_w = (preview_w + s_rt_scene_div - 1) / s_rt_scene_div;
                preview_h = (preview_h + s_rt_scene_div - 1) / s_rt_scene_div;
            }
            ImGui::TextDisabled("Current size: %dx%d", preview_w, preview_h);
        }

        if (ui_rt_format_combo("Format", &s_rt_fmt))
            ui_clamp_rt_flags(s_rt_fmt, &s_rt_rtv, &s_rt_srv, &s_rt_uav, &s_rt_dsv);

        const RTFormatOption* fmt_info = ui_rt_format_info(s_rt_fmt);
        bool is_depth = fmt_info && fmt_info->depth;
        bool supports_uav = !fmt_info || fmt_info->uav;

        if (is_depth) ImGui::BeginDisabled();
        ImGui::Checkbox("RTV", &s_rt_rtv);
        if (is_depth) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("SRV", &s_rt_srv);
        ImGui::SameLine();
        if (is_depth || !supports_uav) ImGui::BeginDisabled();
        ImGui::Checkbox("UAV", &s_rt_uav);
        if (is_depth || !supports_uav) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!is_depth) ImGui::BeginDisabled();
        ImGui::Checkbox("DSV", &s_rt_dsv);
        if (!is_depth) ImGui::EndDisabled();
        ui_clamp_rt_flags(s_rt_fmt, &s_rt_rtv, &s_rt_srv, &s_rt_uav, &s_rt_dsv);

        if (ImGui::Button("Create")) {
            char uname[MAX_NAME] = {};
            res_make_unique_name(s_rt_name[0] ? s_rt_name : "rt_0", uname, MAX_NAME);
            g_sel_res = res_create_render_texture(uname, s_rt_w, s_rt_h, s_rt_fmt,
                                                  s_rt_rtv, s_rt_srv, s_rt_uav, s_rt_dsv, s_rt_scene_div);
            g_sel_cmd = INVALID_HANDLE;
            s_res_nav = g_sel_res;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Create RenderTexture3D", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", s_rt3_name, MAX_NAME);
        ImGui::InputInt("Width", &s_rt3_w);
        ImGui::InputInt("Height", &s_rt3_h);
        ImGui::InputInt("Depth", &s_rt3_d);
        if (s_rt3_w < 1) s_rt3_w = 1;
        if (s_rt3_h < 1) s_rt3_h = 1;
        if (s_rt3_d < 1) s_rt3_d = 1;

        if (ui_rt_format_combo("Format", &s_rt3_fmt))
            ui_clamp_rt3d_flags(s_rt3_fmt, &s_rt3_rtv, &s_rt3_srv, &s_rt3_uav);

        const RTFormatOption* fmt_info = ui_rt_format_info(s_rt3_fmt);
        bool is_depth = fmt_info && fmt_info->depth;
        bool supports_uav = !fmt_info || fmt_info->uav;

        if (is_depth) ImGui::BeginDisabled();
        ImGui::Checkbox("RTV", &s_rt3_rtv);
        if (is_depth) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("SRV", &s_rt3_srv);
        ImGui::SameLine();
        if (is_depth || !supports_uav) ImGui::BeginDisabled();
        ImGui::Checkbox("UAV", &s_rt3_uav);
        if (is_depth || !supports_uav) ImGui::EndDisabled();
        ui_clamp_rt3d_flags(s_rt3_fmt, &s_rt3_rtv, &s_rt3_srv, &s_rt3_uav);
        if (is_depth)
            ImGui::TextDisabled("Depth formats are only supported by RenderTexture2D.");

        if (is_depth) ImGui::BeginDisabled();
        if (ImGui::Button("Create")) {
            char uname[MAX_NAME] = {};
            res_make_unique_name(s_rt3_name[0] ? s_rt3_name : "rt3d_0", uname, MAX_NAME);
            g_sel_res = res_create_render_texture3d(uname, s_rt3_w, s_rt3_h, s_rt3_d, s_rt3_fmt,
                                                    s_rt3_rtv, s_rt3_srv, s_rt3_uav);
            g_sel_cmd = INVALID_HANDLE;
            ImGui::CloseCurrentPopup();
        }
        if (is_depth) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Create StructuredBuffer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", s_sb_name, MAX_NAME);
        ImGui::InputInt("Stride bytes", &s_sb_stride);
        ImGui::InputInt("Elements", &s_sb_count);
        if (s_sb_indirect_args) s_sb_stride = 4;
        if (s_sb_stride < 1) s_sb_stride = 1;
        if (s_sb_count < 1) s_sb_count = 1;
        ImGui::TextDisabled("Total: %d bytes", s_sb_stride * s_sb_count);
        ImGui::Checkbox("SRV", &s_sb_srv);
        ImGui::SameLine();
        ImGui::Checkbox("UAV", &s_sb_uav);
        ImGui::Checkbox("Indirect Args", &s_sb_indirect_args);
        if (s_sb_indirect_args)
            ImGui::TextDisabled("Indirect args are DWORD arrays. Stride is forced to 4 bytes.");
        else
            ImGui::TextDisabled("Enable only for DrawIndirect / DispatchIndirect argument buffers.");

        if (ImGui::Button("Create")) {
            char uname[MAX_NAME] = {};
            res_make_unique_name(s_sb_name[0] ? s_sb_name : "sb_0", uname, MAX_NAME);
            g_sel_res = res_create_structured_buffer(uname, s_sb_stride, s_sb_count,
                                                     s_sb_srv, s_sb_uav, s_sb_indirect_args);
            g_sel_cmd = INVALID_HANDLE;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    static int s_res_filter = 0;
    ui_filter_button("all", 0, &s_res_filter);
    ImGui::SameLine(); ui_filter_button("mesh", 1, &s_res_filter);
    ImGui::SameLine(); ui_filter_button("shader", 2, &s_res_filter);
    ImGui::SameLine(); ui_filter_button("tex", 3, &s_res_filter);
    ImGui::SameLine(); ui_filter_button("buf", 4, &s_res_filter);
    ImGui::SameLine(); ui_filter_button("var", 5, &s_res_filter);
    ImGui::Separator();

    if (ImGui::BeginTabBar("##resource_scope_tabs")) {
        if (ImGui::BeginTabItem("User")) {
            s_user_scope_active = true;
            ResHandle visible_items[MAX_RESOURCES] = {};
            int visible = ui_collect_visible_resources(false, s_res_filter, visible_items, MAX_RESOURCES);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_None) && !ImGui::IsAnyItemActive()) {
                ImGui::SetNextFrameWantCaptureKeyboard(true);
                if (visible > 0) {
                    int nav_index = ui_find_visible_resource_index(visible_items, visible, s_res_nav);
                    if (nav_index < 0)
                        nav_index = ui_find_visible_resource_index(visible_items, visible, g_sel_res);
                    if (nav_index < 0) nav_index = 0;
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && nav_index > 0)
                        nav_index--;
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && nav_index + 1 < visible)
                        nav_index++;
                    s_res_nav = visible_items[nav_index];
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                        g_sel_res = s_res_nav;
                        g_sel_cmd = INVALID_HANDLE;
                    }
                }
            }
            for (int item = 0; item < visible; item++) {
                Resource* r = res_get(visible_items[item]);
                if (!r) continue;
                if (ui_resource_row((int)visible_items[item] - 1, *r))
                    continue;
            }
            if (visible == 0)
                ImGui::TextDisabled("No user resources match this filter.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Built-in")) {
            s_user_scope_active = false;
            ResHandle visible_items[MAX_RESOURCES] = {};
            int visible = ui_collect_visible_resources(true, s_res_filter, visible_items, MAX_RESOURCES);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_None) && !ImGui::IsAnyItemActive()) {
                ImGui::SetNextFrameWantCaptureKeyboard(true);
                if (visible > 0) {
                    int nav_index = ui_find_visible_resource_index(visible_items, visible, s_res_nav);
                    if (nav_index < 0)
                        nav_index = ui_find_visible_resource_index(visible_items, visible, g_sel_res);
                    if (nav_index < 0) nav_index = 0;
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && nav_index > 0)
                        nav_index--;
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && nav_index + 1 < visible)
                        nav_index++;
                    s_res_nav = visible_items[nav_index];
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                        g_sel_res = s_res_nav;
                        g_sel_cmd = INVALID_HANDLE;
                    }
                }
            }
            for (int item = 0; item < visible; item++) {
                Resource* r = res_get(visible_items[item]);
                if (!r) continue;
                ui_resource_row((int)visible_items[item] - 1, *r);
            }
            if (visible == 0)
                ImGui::TextDisabled("No built-in resources match this filter.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
        Resource* r = res_get(g_sel_res);
        if (r && !r->is_builtin) {
            strncpy(s_rename_buf, r->name, MAX_NAME - 1);
            s_rename_active = true; s_rename_is_cmd = false;
        }
    }

    if (!embedded) ImGui::End();
}

// ── commands panel ────────────────────────────────────────────────────────

static float ui_command_tree_row_height(const Command& c) {
    return c.type == CMD_GROUP ? (ImGui::GetTextLineHeight() + 8.0f) : (ImGui::GetTextLineHeight() + 12.0f);
}

static void ui_accumulate_command_subtree_height(CmdHandle parent, int depth, float* total, bool* first) {
    if (depth > 8)
        return;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active || c.parent != parent)
            continue;
        if (!*first)
            *total += ImGui::GetStyle().ItemSpacing.y;
        *total += ui_command_tree_row_height(c);
        *first = false;
        if ((c.type == CMD_REPEAT || c.type == CMD_GROUP) && c.repeat_expanded)
            ui_accumulate_command_subtree_height((CmdHandle)(i + 1), depth + (c.type == CMD_GROUP ? 0 : 1), total, first);
    }
}

static float ui_command_subtree_height(CmdHandle parent, int depth) {
    float total = 0.0f;
    bool first = true;
    ui_accumulate_command_subtree_height(parent, depth, &total, &first);
    return total;
}

static void ui_draw_command_tree(CmdHandle parent, int depth) {
    if (depth > 8) return;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active || c.parent != parent) continue;
        if (ui_command_row(i, c, depth))
            continue;

        CmdHandle h = (CmdHandle)(i + 1);
        if (c.type == CMD_GROUP && c.repeat_expanded) {
            float child_h = ui_command_subtree_height(h, depth);
            if (child_h > 1.0f) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 child_min = ImGui::GetCursorScreenPos();
                float inset_l = 3.0f + (float)depth * 18.0f;
                float inset_r = 2.0f;
                ImVec2 block_min = ImVec2(child_min.x + inset_l, child_min.y + 1.0f);
                ImVec2 block_max = ImVec2(
                    ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - inset_r,
                    child_min.y + child_h - 1.0f);
                dl->AddRectFilled(block_min, block_max,
                    ImGui::GetColorU32(ImVec4(0.245f, 0.188f, 0.118f, 0.58f)), 5.0f);
            }
        }
        if ((c.type == CMD_REPEAT || c.type == CMD_GROUP) && c.repeat_expanded)
            ui_draw_command_tree(h, depth + (c.type == CMD_GROUP ? 0 : 1));
    }
}

static void ui_panel_commands(bool embedded = false) {
    if (!embedded) ImGui::Begin("Commands");

    if (ImGui::BeginPopupContextWindow("cmd_ctx_bg",
        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        char uname[MAX_NAME];
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, ui_can_paste_command_clipboard())) {
            CmdHandle pasted_h = ui_paste_command_clipboard(INVALID_HANDLE);
            if (pasted_h != INVALID_HANDLE) {
                g_sel_cmd = pasted_h;
                g_sel_res = INVALID_HANDLE;
                s_cmd_nav = pasted_h;
            }
        }
        if (ui_can_paste_command_clipboard())
            ImGui::Separator();
        if (ImGui::MenuItem("Group")) {
            cmd_make_unique_name("group_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_GROUP); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        if (ImGui::MenuItem("Repeat")) {
            cmd_make_unique_name("repeat_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_REPEAT); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear")) {
            cmd_make_unique_name("clear_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_CLEAR); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        if (ImGui::MenuItem("DrawMesh")) {
            cmd_make_unique_name("draw_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_DRAW_MESH); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        if (ImGui::MenuItem("DrawInstanced")) {
            cmd_make_unique_name("drawi_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_DRAW_INSTANCED); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        if (ImGui::MenuItem("Dispatch")) {
            cmd_make_unique_name("dispatch_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_DISPATCH); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        if (ImGui::MenuItem("IndirectDraw")) {
            cmd_make_unique_name("idraw_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_INDIRECT_DRAW); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        if (ImGui::MenuItem("IndirectDispatch")) {
            cmd_make_unique_name("idisp_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_INDIRECT_DISPATCH); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        ImGui::EndPopup();
    }

    CmdHandle visible_items[MAX_COMMANDS] = {};
    int visible = ui_collect_visible_commands_recursive(INVALID_HANDLE, visible_items, MAX_COMMANDS);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_None) && !ImGui::IsAnyItemActive()) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextFrameWantCaptureKeyboard(true);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && g_sel_cmd != INVALID_HANDLE)
            ui_copy_command_to_clipboard(g_sel_cmd);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && ui_can_paste_command_clipboard()) {
            CmdHandle pasted_h = ui_paste_command_clipboard(g_sel_cmd);
            if (pasted_h != INVALID_HANDLE) {
                g_sel_cmd = pasted_h;
                g_sel_res = INVALID_HANDLE;
                s_cmd_nav = pasted_h;
            }
        }
        if (visible > 0) {
            int nav_index = ui_find_visible_command_index(visible_items, visible, s_cmd_nav);
            if (nav_index < 0)
                nav_index = ui_find_visible_command_index(visible_items, visible, g_sel_cmd);
            if (nav_index < 0) nav_index = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && nav_index > 0)
                nav_index--;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && nav_index + 1 < visible)
                nav_index++;
            s_cmd_nav = visible_items[nav_index];
            Command* nav_cmd = cmd_get(s_cmd_nav);
            if (nav_cmd && (nav_cmd->type == CMD_REPEAT || nav_cmd->type == CMD_GROUP)) {
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
                    nav_cmd->repeat_expanded = false;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
                    nav_cmd->repeat_expanded = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                g_sel_cmd = s_cmd_nav;
                g_sel_res = INVALID_HANDLE;
            }
        }
    }

    ui_draw_command_tree(INVALID_HANDLE, 0);

    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
        Command* c = cmd_get(g_sel_cmd);
        if (c) {
            strncpy(s_rename_buf, c->name, MAX_NAME - 1);
            s_rename_active = true; s_rename_is_cmd = true;
        }
    }

    if (!embedded) ImGui::End();
}

// ── inspector ─────────────────────────────────────────────────────────────

static void ui_compute_legacy_cascade_splits(float near_z, float far_z, int cascade_count,
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

static void ui_seed_dirlight_cascade_range(Resource* r, int from_index, int cascade_count) {
    if (!r)
        return;

    if (from_index < 0)
        from_index = 0;
    if (cascade_count < 1)
        cascade_count = 1;
    if (cascade_count > MAX_SHADOW_CASCADES)
        cascade_count = MAX_SHADOW_CASCADES;
    if (from_index >= cascade_count)
        return;

    float seeded_splits[MAX_SHADOW_CASCADES] = {};
    float split_far = r->shadow_distance > 0.1f ? r->shadow_distance : g_camera.far_z;
    ui_compute_legacy_cascade_splits(g_camera.near_z, split_far, cascade_count,
                                     r->shadow_split_lambda, seeded_splits);

    float base_extent_x = r->shadow_extent[0] > 0.01f ? r->shadow_extent[0] : 0.01f;
    float base_extent_y = r->shadow_extent[1] > 0.01f ? r->shadow_extent[1] : 0.01f;
    float base_near = r->shadow_near > 0.0001f ? r->shadow_near : 0.0001f;
    float base_far = r->shadow_far > base_near + 0.001f ? r->shadow_far : base_near + 0.001f;
    for (int i = from_index; i < cascade_count; i++) {
        r->shadow_cascade_split[i] = seeded_splits[i];
        r->shadow_cascade_extent[i][0] = base_extent_x;
        r->shadow_cascade_extent[i][1] = base_extent_y;
        r->shadow_cascade_near[i] = base_near;
        r->shadow_cascade_far[i] = base_far;
    }
}

static void ui_validate_dirlight_cascades(Resource* r) {
    if (!r)
        return;

    float prev_split = g_camera.near_z > 0.0001f ? g_camera.near_z : 0.1f;
    float max_split = g_camera.far_z > prev_split + 0.001f ? g_camera.far_z : prev_split + 0.001f;
    for (int i = 0; i < MAX_SHADOW_CASCADES; i++) {
        if (r->shadow_cascade_extent[i][0] < 0.01f) r->shadow_cascade_extent[i][0] = 0.01f;
        if (r->shadow_cascade_extent[i][1] < 0.01f) r->shadow_cascade_extent[i][1] = 0.01f;
        if (r->shadow_cascade_near[i] < 0.0001f) r->shadow_cascade_near[i] = 0.0001f;
        if (r->shadow_cascade_far[i] <= r->shadow_cascade_near[i] + 0.001f)
            r->shadow_cascade_far[i] = r->shadow_cascade_near[i] + 0.001f;

        float min_split = prev_split + 0.001f;
        if (min_split > max_split)
            min_split = max_split;
        if (r->shadow_cascade_split[i] < min_split)
            r->shadow_cascade_split[i] = min_split;
        if (r->shadow_cascade_split[i] > max_split)
            r->shadow_cascade_split[i] = max_split;
        prev_split = r->shadow_cascade_split[i];
    }
}


static void ui_timeline_capture_user_vars_for_resource(ResHandle h) {
    if (h == INVALID_HANDLE)
        return;

    for (int i = 0; i < g_user_cb_count; i++) {
        UserCBEntry& e = g_user_cb_entries[i];
        UserCBSourceKind source_kind = e.source_kind;
        if (source_kind == USER_CB_SOURCE_NONE && e.source != INVALID_HANDLE)
            source_kind = USER_CB_SOURCE_RESOURCE;
        if (source_kind != USER_CB_SOURCE_RESOURCE || e.source != h)
            continue;

        // Resource-backed UserCB entries are copied from the resource lazily.
        // Refresh before capturing so an edited resource value becomes the new
        // key value immediately, matching the transform auto-key behaviour.
        user_cb_refresh_entry(i);
        timeline_capture_if_tracked(TIMELINE_TRACK_USER_VAR, e.name, e.type);
    }
}

static void ui_inspector_resource(Resource* r, ResHandle h) {
    if (r->is_builtin) ImGui::TextDisabled("(built-in — read only name)");

    bool value_changed = false;
    switch (r->type) {
    case RES_INT:    value_changed = ImGui::InputInt("value",   &r->ival[0]);       break;
    case RES_INT2:   value_changed = ImGui::InputInt2("value",   r->ival);          break;
    case RES_INT3:   value_changed = ImGui::InputInt3("value",   r->ival);          break;
    case RES_FLOAT:  value_changed = ImGui::DragFloat("value",  &r->fval[0], 0.01f); break;
    case RES_FLOAT2: value_changed = ImGui::DragFloat2("value",  r->fval,    0.01f); break;
    case RES_FLOAT3: value_changed = ImGui::ColorEdit3("value",  r->fval);          break;
    case RES_FLOAT4: value_changed = ImGui::ColorEdit4("value",  r->fval);          break;

    case RES_RENDER_TEXTURE2D: {
        static ResHandle s_edit_rt = INVALID_HANDLE;
        static int s_w = 1, s_h = 1;
        static DXGI_FORMAT s_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        static bool s_rtv = false, s_srv = false, s_uav = false, s_dsv = false;
        static int s_scene_div = 0;
        if (s_edit_rt != h) {
            s_edit_rt = h;
            s_w = r->width; s_h = r->height; s_fmt = r->tex_fmt;
            s_rtv = r->has_rtv; s_srv = r->has_srv; s_uav = r->has_uav; s_dsv = r->has_dsv;
            s_scene_div = r->scene_scale_divisor;
        }

        ui_rt_scene_scale_combo("Resolution", &s_scene_div);
        if (s_scene_div == 0) {
            ImGui::InputInt("Width", &s_w);
            ImGui::InputInt("Height", &s_h);
            if (s_w < 1) s_w = 1;
            if (s_h < 1) s_h = 1;
        } else {
            ImGui::TextDisabled("Scene-scaled: %s", ui_rt_scene_scale_name(s_scene_div));
        }
        if (ui_rt_format_combo("Format", &s_fmt))
            ui_clamp_rt_flags(s_fmt, &s_rtv, &s_srv, &s_uav, &s_dsv);

        const RTFormatOption* fmt_info = ui_rt_format_info(s_fmt);
        bool is_depth = fmt_info && fmt_info->depth;
        bool supports_uav = !fmt_info || fmt_info->uav;
        if (is_depth) ImGui::BeginDisabled();
        ImGui::Checkbox("RTV", &s_rtv);
        if (is_depth) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("SRV", &s_srv);
        ImGui::SameLine();
        if (is_depth || !supports_uav) ImGui::BeginDisabled();
        ImGui::Checkbox("UAV", &s_uav);
        if (is_depth || !supports_uav) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!is_depth) ImGui::BeginDisabled();
        ImGui::Checkbox("DSV", &s_dsv);
        if (!is_depth) ImGui::EndDisabled();
        ui_clamp_rt_flags(s_fmt, &s_rtv, &s_srv, &s_uav, &s_dsv);

        if (ImGui::Button("Recreate")) {
            if (res_recreate_render_texture(h, s_w, s_h, s_fmt, s_rtv, s_srv, s_uav, s_dsv, s_scene_div))
                log_info("RenderTexture2D recreated: %s (%dx%d)", r->name, s_w, s_h);
        }
        ImGui::Text("Current: %dx%d, %s", r->width, r->height, ui_rt_format_name(r->tex_fmt));
        if (r->scene_scale_divisor > 0)
            ImGui::TextDisabled("Mode: %s", ui_rt_scene_scale_name(r->scene_scale_divisor));
        ImGui::Text("RTV:%s SRV:%s UAV:%s DSV:%s",
            r->has_rtv?"Y":"N", r->has_srv?"Y":"N",
            r->has_uav?"Y":"N", r->has_dsv?"Y":"N");
        ui_image_fit_panel(r->srv, r->width, r->height);
        break;
    }

    case RES_RENDER_TEXTURE3D: {
        static ResHandle s_edit_rt3 = INVALID_HANDLE;
        static int s_w = 1, s_h = 1, s_d = 1;
        static DXGI_FORMAT s_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        static bool s_rtv = false, s_srv = false, s_uav = false;
        static int s_preview_slice = 0;
        if (s_edit_rt3 != h) {
            s_edit_rt3 = h;
            s_w = r->width; s_h = r->height; s_d = r->depth; s_fmt = r->tex_fmt;
            s_rtv = r->has_rtv; s_srv = r->has_srv; s_uav = r->has_uav;
            s_preview_slice = 0;
        }

        ImGui::InputInt("Width", &s_w);
        ImGui::InputInt("Height", &s_h);
        ImGui::InputInt("Depth", &s_d);
        if (s_w < 1) s_w = 1;
        if (s_h < 1) s_h = 1;
        if (s_d < 1) s_d = 1;
        if (ui_rt_format_combo("Format", &s_fmt))
            ui_clamp_rt3d_flags(s_fmt, &s_rtv, &s_srv, &s_uav);

        const RTFormatOption* fmt_info = ui_rt_format_info(s_fmt);
        bool is_depth = fmt_info && fmt_info->depth;
        bool supports_uav = !fmt_info || fmt_info->uav;
        if (is_depth) ImGui::BeginDisabled();
        ImGui::Checkbox("RTV", &s_rtv);
        if (is_depth) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("SRV", &s_srv);
        ImGui::SameLine();
        if (is_depth || !supports_uav) ImGui::BeginDisabled();
        ImGui::Checkbox("UAV", &s_uav);
        if (is_depth || !supports_uav) ImGui::EndDisabled();
        ui_clamp_rt3d_flags(s_fmt, &s_rtv, &s_srv, &s_uav);
        if (is_depth)
            ImGui::TextDisabled("Depth formats are only supported by RenderTexture2D.");

        if (is_depth) ImGui::BeginDisabled();
        if (ImGui::Button("Recreate")) {
            if (res_recreate_render_texture3d(h, s_w, s_h, s_d, s_fmt, s_rtv, s_srv, s_uav))
                log_info("RenderTexture3D recreated: %s (%dx%dx%d)", r->name, s_w, s_h, s_d);
        }
        if (is_depth) ImGui::EndDisabled();

        ImGui::Text("Current: %dx%dx%d, %s", r->width, r->height, r->depth, ui_rt_format_name(r->tex_fmt));
        ImGui::Text("RTV:%s SRV:%s UAV:%s",
            r->has_rtv?"Y":"N", r->has_srv?"Y":"N", r->has_uav?"Y":"N");
        if (!r->srv) {
            ImGui::TextDisabled("Preview requires SRV enabled.");
            break;
        }
        if (!ui_rt3d_preview_supported_format(r->tex_fmt)) {
            ImGui::TextDisabled("Preview is not available for this format yet.");
            break;
        }
        if (s_preview_slice >= r->depth) s_preview_slice = r->depth - 1;
        if (s_preview_slice < 0) s_preview_slice = 0;
        if (r->depth > 1) {
            ImGui::SliderInt("Slice", &s_preview_slice, 0, r->depth - 1);
        } else {
            ImGui::BeginDisabled();
            int slice = 0;
            ImGui::SliderInt("Slice", &slice, 0, 0);
            ImGui::EndDisabled();
        }
        if (ID3D11ShaderResourceView* preview_srv = ui_render_texture3d_preview_slice(r, s_preview_slice))
            ui_image_fit_panel(preview_srv, r->width, r->height);
        else
            ImGui::TextDisabled("3D texture preview is unavailable.");
        break;
    }

    case RES_TEXTURE2D: {
        ImGui::Text("Size: %dx%d", r->width, r->height);
        ImGui::Text("Path: %s", r->path);
        static ResHandle tex_edit = INVALID_HANDLE;
        static char tex_path[MAX_PATH_LEN] = {};
        if (tex_edit != h) {
            tex_edit = h;
            strncpy(tex_path, r->path, MAX_PATH_LEN - 1);
            tex_path[MAX_PATH_LEN - 1] = '\0';
        }
        PathInputResult tex_path_result =
            ui_path_input_ex("Path##tex", tex_path, MAX_PATH_LEN, ".png;.jpg;.jpeg;.tga;.bmp;.hdr");
        if (tex_path_result.file_selected) {
            if (res_reload_texture(r, tex_path)) {
                strncpy(tex_path, r->path, MAX_PATH_LEN - 1);
                tex_path[MAX_PATH_LEN - 1] = '\0';
            }
        }
        if (ImGui::Button(r->tex ? "Reload Texture" : "Load Texture")) {
            if (res_reload_texture(r, tex_path)) {
                strncpy(tex_path, r->path, MAX_PATH_LEN - 1);
                tex_path[MAX_PATH_LEN - 1] = '\0';
            }
        }
        ui_image_fit_panel(r->srv, r->width, r->height);
        break;
    }

    case RES_STRUCTURED_BUFFER: {
        static ResHandle s_edit_sb = INVALID_HANDLE;
        static int s_stride = 16, s_count = 1;
        static bool s_srv = true, s_uav = true, s_indirect_args = false;
        if (s_edit_sb != h) {
            s_edit_sb = h;
            s_stride = r->elem_size;
            s_count = r->elem_count;
            s_srv = r->has_srv;
            s_uav = r->has_uav;
            s_indirect_args = r->indirect_args;
        }

        ImGui::InputInt("Stride bytes", &s_stride);
        ImGui::InputInt("Elements", &s_count);
        if (s_indirect_args) s_stride = 4;
        if (s_stride < 1) s_stride = 1;
        if (s_count < 1) s_count = 1;
        ImGui::TextDisabled("Total: %d bytes", s_stride * s_count);
        ImGui::Checkbox("SRV", &s_srv);
        ImGui::SameLine();
        ImGui::Checkbox("UAV", &s_uav);
        ImGui::Checkbox("Indirect Args", &s_indirect_args);
        if (s_indirect_args)
            ImGui::TextDisabled("Indirect args use typed uint views for D3D11 compatibility.");
        if (ImGui::Button("Recreate")) {
            if (res_recreate_structured_buffer(h, s_stride, s_count, s_srv, s_uav, s_indirect_args))
                log_info("StructuredBuffer recreated: %s (%d x %d bytes)", r->name, s_count, s_stride);
        }

        ImGui::Text("Current: %d x %d bytes = %d bytes",
            r->elem_count, r->elem_size, r->elem_count * r->elem_size);
        ImGui::Text("SRV:%s UAV:%s IndirectArgs:%s",
            r->has_srv?"Y":"N", r->has_uav?"Y":"N", r->indirect_args?"Y":"N");
        break;
    }

    case RES_MESH: {
        ImGui::Text("Vertices: %d", r->vert_count);
        ImGui::Text("Indices:  %d", r->idx_count);
        ImGui::Text("Stride:   %d bytes", r->vert_stride);
        ImGui::Text("Parts:    %d", r->mesh_part_count);
        ImGui::Text("Materials:%d", r->mesh_material_count);
        ImGui::Text("Path: %s", r->path);
        static ResHandle mesh_edit = INVALID_HANDLE;
        static char mesh_path[MAX_PATH_LEN] = {};
        if (mesh_edit != h) {
            mesh_edit = h;
            strncpy(mesh_path, r->path, MAX_PATH_LEN - 1);
            mesh_path[MAX_PATH_LEN - 1] = '\0';
        }
        if (r->using_fallback) {
            ImGui::TextColored({1, 0.35f, 0.3f, 1}, "Status: FALLBACK CUBE");
            if (r->compile_err[0])
                ImGui::TextColored({1, 0.35f, 0.3f, 1}, "%s", r->compile_err);
        } else if (r->vb) {
            ImGui::TextColored({0.35f, 1, 0.45f, 1}, "Status: OK");
        }
        if (r->mesh_part_count > 0) {
            ImGui::Separator();
            ImGui::Text("Parts:");
            for (int pi = 0; pi < r->mesh_part_count; pi++) {
                MeshPart& part = r->mesh_parts[pi];
                ImGui::PushID(pi);
                ImGui::Checkbox("##enabled", &part.enabled);
                ImGui::SameLine();
                ImGui::Text("%s", part.name[0] ? part.name : "(part)");
                if (part.material_index >= 0 && part.material_index < r->mesh_material_count) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("mat %s", r->mesh_materials[part.material_index].name);
                }
                ImGui::PopID();
            }
        }
        if (r->mesh_material_count > 0) {
            ImGui::Separator();
            ImGui::Text("Materials:");
            for (int mi = 0; mi < r->mesh_material_count; mi++) {
                MeshMaterial& mat = r->mesh_materials[mi];
                ImGui::PushID(1000 + mi);
                if (ImGui::TreeNodeEx(mat.name[0] ? mat.name : "(material)", ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (mat.double_sided)
                        ImGui::TextDisabled("Double-sided");
                    else
                        ImGui::TextDisabled("Backface culled by default");
                    for (int slot = 0; slot < MAX_MESH_MATERIAL_TEXTURES; slot++) {
                        ResHandle tex_h = mat.textures[slot];
                        Resource* tr = res_get(tex_h);
                        if (!tex_h)
                            continue;
                        char tex_label[MAX_NAME + 48];
                        snprintf(tex_label, sizeof(tex_label), "%s: %s##meshmat%d_%d",
                            ui_mesh_material_slot_name(slot), tr ? tr->name : "(deleted)", mi, slot);
                        if (ImGui::Selectable(tex_label, false) && tr) {
                            g_sel_res = tex_h;
                            g_sel_cmd = INVALID_HANDLE;
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Use Cube")) {
            res_set_mesh_primitive(r, MESH_PRIM_CUBE);
            r->path[0] = '\0';
            mesh_path[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Quad")) {
            res_set_mesh_primitive(r, MESH_PRIM_QUAD);
            r->path[0] = '\0';
            mesh_path[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Tetrahedron")) {
            res_set_mesh_primitive(r, MESH_PRIM_TETRAHEDRON);
            r->path[0] = '\0';
            mesh_path[0] = '\0';
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Sphere")) {
            res_set_mesh_primitive(r, MESH_PRIM_SPHERE);
            r->path[0] = '\0';
            mesh_path[0] = '\0';
        }
        if (ImGui::Button("Use Fullscreen Triangle")) {
            res_set_mesh_primitive(r, MESH_PRIM_FULLSCREEN_TRIANGLE);
            r->path[0] = '\0';
            mesh_path[0] = '\0';
        }
        if (r->using_fallback) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.35f, 0.06f, 0.05f, 1));
        PathInputResult mesh_path_result =
            ui_path_input_ex("Path##mesh", mesh_path, MAX_PATH_LEN, ".gltf;.glb");
        if (r->using_fallback) ImGui::PopStyleColor();
        if (mesh_path_result.file_selected)
            ui_reload_mesh_resource(r, mesh_path);
        if (ImGui::Button("Load glTF")) {
            ui_reload_mesh_resource(r, mesh_path);
        }
        break;
    }

    case RES_SHADER: {
        ImGui::TextWrapped("Path: %s", r->path);
        if (!r->compiled_ok) {
            ImGui::TextColored({1, 0.25f, 0.2f, 1}, "Status: FALLBACK");
        } else if (r->using_fallback) {
            ImGui::TextColored({1, 0.75f, 0.25f, 1}, "Status: FALLBACK");
        } else {
            ImGui::TextColored({0.35f, 1, 0.45f, 1}, "Status: OK");
        }

        static ResHandle shader_edit = INVALID_HANDLE;
        static char shader_path[MAX_PATH_LEN] = {};
        if (shader_edit != h) {
            shader_edit = h;
            strncpy(shader_path, r->path, MAX_PATH_LEN - 1);
            shader_path[MAX_PATH_LEN - 1] = '\0';
        }
        if (!r->compiled_ok) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.35f, 0.06f, 0.05f, 1));
        PathInputResult shader_path_result =
            ui_path_input_ex("Path##sh", shader_path, MAX_PATH_LEN, ".hlsl;.hlsli;.fx");
        if (!r->compiled_ok) ImGui::PopStyleColor();
        if (shader_path_result.file_selected) {
            strncpy(r->path, shader_path, MAX_PATH_LEN - 1);
            r->path[MAX_PATH_LEN - 1] = '\0';
            ui_recompile_shader_resource(h, r, r->path);
        }

        ui_inspector_section("SOURCE");
        ui_shader_source_viewer(h, r);

        ImGui::Separator();
        if (r->shader_cb.active) {
            ImGui::Text("Reflected cbuffer: %s (b%u, %u bytes)",
                r->shader_cb.name, r->shader_cb.bind_slot, r->shader_cb.size);
            for (int i = 0; i < r->shader_cb.var_count; i++) {
                const ShaderCBVar& v = r->shader_cb.vars[i];
                ImGui::TextDisabled("%s %s @ %u", res_type_str(v.type), v.name, v.offset);
            }
            if (r->object_cb_active)
                ImGui::TextDisabled("ObjectCB slot: b%u", r->object_cb_bind_slot);
        } else {
            ImGui::TextDisabled("No UserCB cbuffer reflected. Recommended: register(b2).");
            if (r->object_cb_active)
                ImGui::TextDisabled("ObjectCB slot: b%u", r->object_cb_bind_slot);
        }
        break;
    }

    case RES_BUILTIN_DIRLIGHT: {
        Vec3 target = v3(r->light_target[0], r->light_target[1], r->light_target[2]);
        Vec3 pos = v3(r->light_pos[0], r->light_pos[1], r->light_pos[2]);
        Vec3 offset = v3_sub(pos, target);
        float distance = sqrtf(v3_dot(offset, offset));
        if (distance < 0.001f) {
            distance = 0.001f;
            offset = v3(0.0f, 1.0f, 0.0f);
        }

        bool light_changed = false;
        float edit_target[3] = { target.x, target.y, target.z };
        if (ImGui::DragFloat3("Target", edit_target, 0.01f)) {
            Vec3 new_target = v3(edit_target[0], edit_target[1], edit_target[2]);
            Vec3 new_pos = v3_add(new_target, offset);
            r->light_target[0] = new_target.x;
            r->light_target[1] = new_target.y;
            r->light_target[2] = new_target.z;
            r->light_pos[0] = new_pos.x;
            r->light_pos[1] = new_pos.y;
            r->light_pos[2] = new_pos.z;
            target = new_target;
            pos = new_pos;
            offset = v3_sub(pos, target);
            light_changed = true;
        }

        if (ImGui::DragFloat("Distance", &distance, 0.01f, 0.001f, 100.0f)) {
            if (distance < 0.001f) distance = 0.001f;
            Vec3 dir = v3_norm(offset);
            if (v3_dot(dir, dir) < 0.0001f) dir = v3(0.0f, 1.0f, 0.0f);
            Vec3 new_pos = v3_add(target, v3_scale(dir, distance));
            r->light_pos[0] = new_pos.x;
            r->light_pos[1] = new_pos.y;
            r->light_pos[2] = new_pos.z;
            light_changed = true;
        }

        light_changed |= ImGui::ColorEdit3("Color", r->light_color);
        light_changed |= ImGui::DragFloat("Intensity", &r->light_intensity, 0.01f, 0.f, 10.f);
        if (light_changed)
            timeline_capture_if_tracked(TIMELINE_TRACK_DIRLIGHT, "dirlight", RES_NONE);
        ImGui::Separator();
        int shadow_size[2] = {
            r->shadow_width > 0 ? r->shadow_width : g_dx.shadow_width,
            r->shadow_height > 0 ? r->shadow_height : g_dx.shadow_height
        };
        if (ImGui::InputInt2("Shadow Texture", shadow_size)) {
            if (shadow_size[0] < 16) shadow_size[0] = 16;
            if (shadow_size[1] < 16) shadow_size[1] = 16;
            if (shadow_size[0] > 8192) shadow_size[0] = 8192;
            if (shadow_size[1] > 8192) shadow_size[1] = 8192;
            r->shadow_width = shadow_size[0];
            r->shadow_height = shadow_size[1];
            dx_create_shadow_map(r->shadow_width, r->shadow_height);
        }
        int cascade_count = r->shadow_cascade_count > 0 ? r->shadow_cascade_count : 1;
        int prev_cascade_count = cascade_count;
        if (ImGui::InputInt("Shadow Cascades", &cascade_count)) {
            if (cascade_count < 1) cascade_count = 1;
            if (cascade_count > MAX_SHADOW_CASCADES) cascade_count = MAX_SHADOW_CASCADES;
            r->shadow_cascade_count = cascade_count;
            if (cascade_count > prev_cascade_count)
                ui_seed_dirlight_cascade_range(r, prev_cascade_count, cascade_count);
        }
        ui_validate_dirlight_cascades(r);
        if (r->shadow_cascade_count > 1) {
            for (int cascade = 0; cascade < r->shadow_cascade_count; cascade++) {
                ImGui::PushID(cascade);
                if (cascade > 0)
                    ImGui::Separator();
                ImGui::TextDisabled("Cascade %d", cascade + 1);
                ImGui::DragFloat("Split Far", &r->shadow_cascade_split[cascade], 0.05f, 0.0f, 1000.0f);
                ImGui::DragFloat2("Ortho Size", r->shadow_cascade_extent[cascade], 0.01f, 0.01f, 100.0f);
                ImGui::DragFloat("Near", &r->shadow_cascade_near[cascade], 0.001f, 0.0001f, 100.0f);
                ImGui::DragFloat("Far", &r->shadow_cascade_far[cascade], 0.01f, 0.001f, 1000.0f);
                ImGui::PopID();
            }
            ui_validate_dirlight_cascades(r);
            ImGui::TextDisabled("Each cascade uses a manual ortho box and manual split distance.");
        } else {
            ImGui::DragFloat2("Shadow Ortho Size", r->shadow_extent, 0.01f, 0.01f, 100.0f);
            ImGui::DragFloat("Shadow Near", &r->shadow_near, 0.001f, 0.0001f, 100.0f);
            ImGui::DragFloat("Shadow Far", &r->shadow_far, 0.01f, 0.001f, 1000.0f);
            if (r->shadow_far <= r->shadow_near + 0.001f)
                r->shadow_far = r->shadow_near + 0.001f;
            ImGui::TextDisabled("Single-cascade mode uses the manual ortho box above.");
        }
        ImGui::Separator();
        if (Resource* shadow_map = res_get(g_builtin_shadow_map)) {
            ImGui::Text("Shadow Atlas Preview (%dx%d, %d cascades)",
                        shadow_map->width, shadow_map->height, r->shadow_cascade_count > 0 ? r->shadow_cascade_count : 1);
            ui_image_fill_panel_width(shadow_map->srv, shadow_map->width, shadow_map->height);
        }
        break;
    }

    case RES_BUILTIN_TIME:
        ImGui::Text("Scene Time = %.3f s", r->fval[0]);
        break;

    case RES_BUILTIN_SCENE_COLOR:
        ImGui::Text("Scene Color (%dx%d)", r->width, r->height);
        ui_image_fit_panel(r->srv, r->width, r->height);
        break;

    case RES_BUILTIN_SCENE_DEPTH:
        ImGui::Text("Scene Depth (%dx%d)", r->width, r->height);
        break;

    case RES_BUILTIN_SHADOW_MAP: {
        ImGui::Text("Shadow Map (%dx%d)", r->width, r->height);
        Resource* dl = res_get(g_builtin_dirlight);
        int shadow_size[2] = {
            dl && dl->shadow_width > 0 ? dl->shadow_width : g_dx.shadow_width,
            dl && dl->shadow_height > 0 ? dl->shadow_height : g_dx.shadow_height
        };
        if (ImGui::InputInt2("Shadow Texture", shadow_size)) {
            if (shadow_size[0] < 16) shadow_size[0] = 16;
            if (shadow_size[1] < 16) shadow_size[1] = 16;
            if (shadow_size[0] > 8192) shadow_size[0] = 8192;
            if (shadow_size[1] > 8192) shadow_size[1] = 8192;
            if (dl) {
                dl->shadow_width = shadow_size[0];
                dl->shadow_height = shadow_size[1];
            }
            dx_create_shadow_map(shadow_size[0], shadow_size[1]);
        }
        if (dl) {
            ImGui::TextDisabled("%d cascades%s",
                                dl->shadow_cascade_count > 0 ? dl->shadow_cascade_count : 1,
                                dl->shadow_cascade_count > 1 ? " (atlas)" : "");
        }
        ui_image_fill_panel_width(r->srv, r->width, r->height);
        break;
    }

    default: break;
    }

    if (value_changed) {
        ui_timeline_capture_user_vars_for_resource(h);
        app_request_scene_render();
    }
}

static bool ui_compute_shader_combo(const char* label, ResHandle* h) {
    Resource* cur = res_get(*h);
    const char* prev = (cur && cur->type == RES_SHADER && cur->cs) ? cur->name : "(none)";
    bool changed = false;
    if (ImGui::BeginCombo(label, prev)) {
        if (ImGui::Selectable("(none)", *h == INVALID_HANDLE)) {
            *h = INVALID_HANDLE;
            changed = true;
        }
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active || r.type != RES_SHADER || !r.cs) continue;
            ResHandle rh = (ResHandle)(i + 1);
            bool sel = *h == rh;
            ImGui::PushID(i);
            if (ImGui::Selectable(r.name, sel)) {
                *h = rh;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static CmdHandle ui_create_repeat_dispatch_child(CmdHandle parent, ResHandle shader_h) {
    char base[MAX_NAME] = "dispatch_0";
    if (Resource* shader = res_get(shader_h))
        snprintf(base, sizeof(base), "%s_dispatch", shader->name);

    char uname[MAX_NAME] = {};
    cmd_make_unique_name(base, uname, MAX_NAME);
    CmdHandle child_h = cmd_alloc(uname, CMD_DISPATCH);
    if (Command* child = cmd_get(child_h)) {
        child->parent = parent;
        child->shader = shader_h;
    }
    if (Command* repeat = cmd_get(parent))
        repeat->repeat_expanded = true;
    return child_h;
}

static bool ui_command_has_transform(const Command* c) {
    return c && (c->type == CMD_DRAW_MESH ||
                 c->type == CMD_DRAW_INSTANCED ||
                 c->type == CMD_INDIRECT_DRAW);
}

static void ui_command_transform_editor(Command* c) {
    if (!c)
        return;

    ui_inspector_section("TRANSFORM");
    bool transform_changed = false;
    transform_changed |= ui_tinted_transform_row(
        "Position", c->pos, 0.001f,
        ImVec4(0.150f, 0.055f, 0.050f, 0.3f),
        ImVec4(0.230f, 0.080f, 0.070f, 0.5f)
    );

    transform_changed |= ui_tinted_transform_row(
        "Rotation", c->rot, 0.01f,
        ImVec4(0.055f, 0.130f, 0.070f, 0.3f),
        ImVec4(0.080f, 0.200f, 0.105f, 0.5f)
    );

    transform_changed |= ui_tinted_transform_row(
        "Scale", c->scale, 0.001f,
        ImVec4(0.050f, 0.075f, 0.150f, 0.3f),
        ImVec4(0.070f, 0.105f, 0.230f, 0.5f)
    );
    if (transform_changed)
        timeline_capture_if_tracked(TIMELINE_TRACK_COMMAND_TRANSFORM, c->name, RES_NONE);
}

static bool ui_user_cb_source_combo(const char* label, char* source_name, ResType type_a, ResType type_b = RES_NONE) {
    if (!source_name)
        return false;

    const UserCBEntry* current = user_cb_get(source_name);
    char preview[160] = {};
    if (current && (current->type == type_a || current->type == type_b)) {
        snprintf(preview, sizeof(preview), "%s", current->name);
    } else if (source_name[0]) {
        snprintf(preview, sizeof(preview), "(missing) %s", source_name);
    } else {
        snprintf(preview, sizeof(preview), "(manual)");
    }

    bool changed = false;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo(label, preview)) {
        bool manual = source_name[0] == '\0';
        if (ImGui::Selectable("(manual)", manual)) {
            source_name[0] = '\0';
            changed = true;
        }
        ImGui::Separator();
        for (int i = 0; i < g_user_cb_count; i++) {
            UserCBEntry& e = g_user_cb_entries[i];
            if (e.type != type_a && e.type != type_b)
                continue;
            bool selected = strcmp(source_name, e.name) == 0;
            ImGui::PushID(i);
            if (ImGui::Selectable(e.name, selected)) {
                strncpy(source_name, e.name, MAX_NAME - 1);
                source_name[MAX_NAME - 1] = '\0';
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void ui_inspector_command(Command* c) {
    if (ImGui::Checkbox("Enabled", &c->enabled)) {
        timeline_capture_if_tracked(TIMELINE_TRACK_COMMAND_ENABLED, c->name, RES_NONE);
        app_request_scene_render();
    }

    if (ui_command_has_transform(c))
        ui_command_transform_editor(c);

    switch (c->type) {
    case CMD_GROUP: {
        ui_inspector_section("GROUP");
        ImGui::Checkbox("Expanded", &c->repeat_expanded);
        ImGui::Spacing();
        for (int i = 0; i < MAX_COMMANDS; i++) {
            Command& child = g_commands[i];
            if (!child.active || child.parent != g_sel_cmd) continue;
            ImGui::PushID(i);
            if (ImGui::Selectable(child.name, false)) {
                g_sel_cmd = (CmdHandle)(i + 1);
                g_sel_res = INVALID_HANDLE;
                s_cmd_nav = g_sel_cmd;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", cmd_type_str(child.type));
            ImGui::PopID();
        }
        break;
    }

    case CMD_REPEAT: {
        ui_inspector_section("REPEAT");
        ImGui::InputInt("Iterations", &c->repeat_count);
        if (c->repeat_count < 1) c->repeat_count = 1;
        ImGui::Checkbox("Expanded", &c->repeat_expanded);

        ui_inspector_section("COMPUTE SHADERS");
        static CmdHandle s_repeat_parent = INVALID_HANDLE;
        static ResHandle s_repeat_shader = INVALID_HANDLE;
        if (s_repeat_parent != g_sel_cmd) {
            s_repeat_parent = g_sel_cmd;
            s_repeat_shader = INVALID_HANDLE;
        }
        CmdHandle repeat_h = g_sel_cmd;
        ui_compute_shader_combo("Add Shader", &s_repeat_shader);
        bool can_add = s_repeat_shader != INVALID_HANDLE;
        if (!can_add) ImGui::BeginDisabled();
        if (ImGui::Button("Add Dispatch")) {
            CmdHandle child_h = ui_create_repeat_dispatch_child(repeat_h, s_repeat_shader);
            g_sel_cmd = child_h;
            g_sel_res = INVALID_HANDLE;
            s_cmd_nav = child_h;
        }
        if (!can_add) ImGui::EndDisabled();

        ImGui::Spacing();
        for (int i = 0; i < MAX_COMMANDS; i++) {
            Command& child = g_commands[i];
            if (!child.active || child.parent != repeat_h) continue;
            ImGui::PushID(i);
            if (ImGui::Selectable(child.name, false)) {
                g_sel_cmd = (CmdHandle)(i + 1);
                g_sel_res = INVALID_HANDLE;
                s_cmd_nav = g_sel_cmd;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", cmd_type_str(child.type));
            ImGui::PopID();
        }
        break;
    }

    case CMD_CLEAR: {
        ui_inspector_section("CLEAR");
        bool clear_changed = false;
        clear_changed |= ImGui::Checkbox("Clear Color", &c->clear_color_enabled);
        if (c->clear_color_enabled) {
            clear_changed |= ui_user_cb_source_combo("Color Source", c->clear_color_source, RES_FLOAT3, RES_FLOAT4);
            if (c->clear_color_source[0]) {
                ImGui::TextWrapped("Clear color is driven by UserCB '%s'.", c->clear_color_source);
            } else {
                clear_changed |= ImGui::ColorEdit4("Clear Color Value", c->clear_color);
            }
        }
        clear_changed |= ImGui::Checkbox("Clear Depth", &c->clear_depth);
        if (c->clear_depth) {
            clear_changed |= ui_user_cb_source_combo("Depth Source", c->clear_depth_source, RES_FLOAT);
            if (c->clear_depth_source[0]) {
                ImGui::TextWrapped("Depth clear is driven by UserCB '%s'.", c->clear_depth_source);
            } else {
                clear_changed |= ImGui::DragFloat("Depth Value", &c->depth_clear_val, 0.01f, 0.f, 1.f);
            }
        }
        if (clear_changed)
            app_request_scene_render();

        ui_inspector_section("TARGETS");
        res_combo_render_target("Render Target##clrt", &c->rt);
        res_combo_depth_target("Depth Buffer##cldp", &c->depth);
        break;
    }

    case CMD_DRAW_MESH:
    case CMD_DRAW_INSTANCED: {
        ui_inspector_section("DRAW SOURCE & SHADER");
        bool source_changed = ui_draw_source_combo("Source##dm", &c->draw_source);
        if (source_changed && c->draw_source == DRAW_SOURCE_PROCEDURAL && c->vertex_count < 1)
            c->vertex_count = 3;
        if (ui_command_uses_procedural_draw(*c)) {
            ui_draw_topology_combo("Topology##dm", &c->draw_topology);
            ImGui::InputInt("Vertex Count", &c->vertex_count);
            if (c->vertex_count < 0) c->vertex_count = 0;
            ImGui::TextDisabled("No mesh is bound. The VS should use SV_VertexID / VS SRVs.");
        } else {
            res_combo("Mesh##dm", &c->mesh, RES_MESH, false);
        }
        res_combo("Shader##dm", &c->shader, RES_SHADER, false);

        ui_inspector_section("SHADER PARAMETERS");
        ui_command_shader_params(c, res_get(c->shader));

        ui_inspector_section("TARGETS");
        res_combo_render_target("Render Target##dm", &c->rt);
        res_combo_depth_target("Depth Buffer##dm", &c->depth);
        ImGui::Text("Additional MRTs (%d / %d):", c->mrt_count, MAX_DRAW_RENDER_TARGETS - 1);
        for (int rt_i = 0; rt_i < c->mrt_count; rt_i++) {
            ImGui::PushID(600 + rt_i);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "RT%d##dm_mrt", rt_i + 1);
            res_combo_render_target(lbl, &c->mrt_handles[rt_i]);
            ImGui::PopID();
        }
        if (c->mrt_count < MAX_DRAW_RENDER_TARGETS - 1 && ImGui::SmallButton("+##dm_mrt")) c->mrt_count++;
        if (c->mrt_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##dm_mrt")) c->mrt_count--; }

        ui_inspector_section("RENDER STATE");
        ImGui::Checkbox("Color Write", &c->color_write);
        ImGui::SameLine(170.0f);
        ImGui::Checkbox("Alpha Blend", &c->alpha_blend);
        ImGui::Checkbox("Depth Test", &c->depth_test);
        ImGui::SameLine(170.0f);
        ImGui::Checkbox("Backface Cull", &c->cull_back);
        if (!c->depth_test) ImGui::BeginDisabled();
        ImGui::Checkbox("Depth Write", &c->depth_write);
        if (!c->depth_test) ImGui::EndDisabled();
        ImGui::SameLine(170.0f);
        ImGui::Checkbox("Shadow Caster", &c->shadow_cast);
        ImGui::Checkbox("Shadow Receiver", &c->shadow_receive);
        if (c->shadow_cast)
            res_combo("Shadow Shader##dm", &c->shadow_shader, RES_SHADER);

        if (c->type == CMD_DRAW_INSTANCED)
            ImGui::InputInt("Instance Count", &c->instance_count);

        ui_inspector_section("TEXTURE BINDINGS");
        ImGui::TextDisabled("Reserved PS t# slots:");
        ImGui::TextDisabled("t0 base, t1 metal-rough, t2 normal, t3 emissive, t4 occlusion");
        ImGui::TextDisabled("t5 env map, t7 shadow map. Manual bindings override mesh materials on the same slot.");
        if (ImGui::TreeNodeEx("Slot Reference##draw_slots", ImGuiTreeNodeFlags_None)) {
            ui_draw_texture_slot_reference("##draw_slot_reference");
            ImGui::TreePop();
        }
        ImGui::Text("Texture Slots (%d / %d):", c->tex_count, MAX_TEX_SLOTS);
        for (int t = 0; t < c->tex_count; t++) {
            ImGui::PushID(t);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "t%d tex", (int)c->tex_slots[t]);
            res_combo(lbl, &c->tex_handles[t], RES_NONE);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.f);
            ImGui::InputScalar("##slot", ImGuiDataType_U32, &c->tex_slots[t]);
            ImGui::PopID();
        }
        if (c->tex_count < MAX_TEX_SLOTS && ImGui::SmallButton("+##t")) c->tex_count++;
        if (c->tex_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##t")) c->tex_count--; }

        ImGui::Spacing();
        ImGui::Text("SRV Slots (%d / %d):", c->srv_count, MAX_SRV_SLOTS);
        for (int s = 0; s < c->srv_count; s++) {
            ImGui::PushID(100 + s);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "s%d srv", (int)c->srv_slots[s]);
            res_combo(lbl, &c->srv_handles[s], RES_NONE);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.f);
            ImGui::InputScalar("##sslot", ImGuiDataType_U32, &c->srv_slots[s]);
            ImGui::PopID();
        }
        if (c->srv_count < MAX_SRV_SLOTS && ImGui::SmallButton("+##s")) c->srv_count++;
        if (c->srv_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##s")) c->srv_count--; }

        ui_inspector_section("PIXEL UAV OUTPUTS");
        UINT draw_rtv_count = ui_draw_command_rtv_count(*c);
        ImGui::TextDisabled("DX11 output slots are shared by RTVs and PS UAVs.");
        ImGui::TextDisabled("With %u RTV%s active, the first valid UAV slot is u%u.",
            draw_rtv_count, draw_rtv_count == 1 ? "" : "s", draw_rtv_count);
        ImGui::Text("UAV Slots (%d / %d):", c->uav_count, MAX_UAV_SLOTS);
        for (int u = 0; u < c->uav_count; u++) {
            ImGui::PushID(700 + u);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "u%d uav", (int)c->uav_slots[u]);
            res_combo(lbl, &c->uav_handles[u], RES_NONE);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.f);
            ImGui::InputScalar("##puavslot", ImGuiDataType_U32, &c->uav_slots[u]);
            ImGui::PopID();
        }
        if (c->uav_count < MAX_UAV_SLOTS && ImGui::SmallButton("+##pu")) c->uav_count++;
        if (c->uav_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##pu")) c->uav_count--; }
        break;
    }

    case CMD_DISPATCH: {
        ui_inspector_section("COMPUTE");
        res_combo("Compute Shader##dp", &c->shader, RES_SHADER, false);
        ImGui::Checkbox("Only On Reset", &c->compute_on_reset);
        res_combo_dispatch_source("Dispatch From##dp", &c->dispatch_size_source);
        if (c->dispatch_size_source != INVALID_HANDLE) {
            ImGui::InputInt3("Divisor XYZ", &c->thread_x);
            ImGui::TextDisabled("Dispatch = ceil(source_size / divisor)");
        } else {
            ImGui::InputInt3("Dispatch XYZ", &c->thread_x);
        }

        ui_inspector_section("SHADER PARAMETERS");
        ui_command_shader_params(c, res_get(c->shader));

        ui_inspector_section("BINDINGS");
        ImGui::Text("Texture/SRV Slots t# (%d / %d):", c->srv_count, MAX_SRV_SLOTS);
        for (int s = 0; s < c->srv_count; s++) {
            ImGui::PushID(200 + s);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "t%d srv", (int)c->srv_slots[s]);
            res_combo(lbl, &c->srv_handles[s], RES_NONE);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.f);
            ImGui::InputScalar("##dss", ImGuiDataType_U32, &c->srv_slots[s]);
            ImGui::PopID();
        }
        if (c->srv_count < MAX_SRV_SLOTS && ImGui::SmallButton("+##ds")) c->srv_count++;
        if (c->srv_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##ds")) c->srv_count--; }

        ImGui::Spacing();
        ImGui::Text("UAV Slots (%d / %d):", c->uav_count, MAX_UAV_SLOTS);
        for (int u = 0; u < c->uav_count; u++) {
            ImGui::PushID(300 + u);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "u%d uav", (int)c->uav_slots[u]);
            res_combo(lbl, &c->uav_handles[u], RES_NONE);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.f);
            ImGui::InputScalar("##dus", ImGuiDataType_U32, &c->uav_slots[u]);
            ImGui::PopID();
        }
        if (c->uav_count < MAX_UAV_SLOTS && ImGui::SmallButton("+##du")) c->uav_count++;
        if (c->uav_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##du")) c->uav_count--; }
        break;
    }

    case CMD_INDIRECT_DRAW: {
        ui_inspector_section("INDIRECT COMMAND");
        bool source_changed = ui_draw_source_combo("Source##id", &c->draw_source);
        if (source_changed && c->draw_source == DRAW_SOURCE_PROCEDURAL)
            c->draw_topology = DRAW_TOPOLOGY_TRIANGLE_LIST;
        Resource* mesh = res_get(c->mesh);
        if (ui_command_uses_procedural_draw(*c)) {
            ui_draw_topology_combo("Topology##id", &c->draw_topology);
            ImGui::TextDisabled("Uses DrawInstancedIndirect args.");
        } else {
            res_combo("Mesh##id", &c->mesh, RES_MESH, false);
            ImGui::TextDisabled("%s",
                (mesh && mesh->ib) ? "Uses DrawIndexedInstancedIndirect args."
                                   : "Uses DrawInstancedIndirect args.");
        }
        res_combo("Shader##id", &c->shader, RES_SHADER, false);
        ui_inspector_section("SHADER PARAMETERS");
        ui_command_shader_params(c, res_get(c->shader));
        ui_inspector_section("TARGETS");
        res_combo_render_target("Render Target##id", &c->rt);
        res_combo_depth_target("Depth Buffer##id", &c->depth);
        ImGui::Text("Additional MRTs (%d / %d):", c->mrt_count, MAX_DRAW_RENDER_TARGETS - 1);
        for (int rt_i = 0; rt_i < c->mrt_count; rt_i++) {
            ImGui::PushID(800 + rt_i);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "RT%d##id_mrt", rt_i + 1);
            res_combo_render_target(lbl, &c->mrt_handles[rt_i]);
            ImGui::PopID();
        }
        if (c->mrt_count < MAX_DRAW_RENDER_TARGETS - 1 && ImGui::SmallButton("+##id_mrt")) c->mrt_count++;
        if (c->mrt_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##id_mrt")) c->mrt_count--; }

        ui_inspector_section("RENDER STATE");
        ImGui::Checkbox("Color Write", &c->color_write);
        ImGui::SameLine(170.0f);
        ImGui::Checkbox("Alpha Blend", &c->alpha_blend);
        ImGui::Checkbox("Depth Test", &c->depth_test);
        ImGui::SameLine(170.0f);
        ImGui::Checkbox("Backface Cull", &c->cull_back);
        if (!c->depth_test) ImGui::BeginDisabled();
        ImGui::Checkbox("Depth Write", &c->depth_write);
        if (!c->depth_test) ImGui::EndDisabled();
        ImGui::SameLine(170.0f);
        ImGui::Checkbox("Shadow Caster", &c->shadow_cast);
        ImGui::Checkbox("Shadow Receiver", &c->shadow_receive);
        if (c->shadow_cast)
            res_combo("Shadow Shader##id", &c->shadow_shader, RES_SHADER);

        ui_inspector_section("TEXTURE BINDINGS");
        ImGui::TextDisabled("Reserved PS t# slots:");
        ImGui::TextDisabled("t0 base, t1 metal-rough, t2 normal, t3 emissive, t4 occlusion");
        ImGui::TextDisabled("t5 env map, t7 shadow map. Manual bindings override mesh materials on the same slot.");
        if (ImGui::TreeNodeEx("Slot Reference##indirect_draw_slots", ImGuiTreeNodeFlags_None)) {
            ui_draw_texture_slot_reference("##indirect_draw_slot_reference");
            ImGui::TreePop();
        }
        ImGui::Text("Texture Slots (%d / %d):", c->tex_count, MAX_TEX_SLOTS);
        for (int t = 0; t < c->tex_count; t++) {
            ImGui::PushID(900 + t);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "t%d tex", (int)c->tex_slots[t]);
            res_combo(lbl, &c->tex_handles[t], RES_NONE);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.f);
            ImGui::InputScalar("##idslot", ImGuiDataType_U32, &c->tex_slots[t]);
            ImGui::PopID();
        }
        if (c->tex_count < MAX_TEX_SLOTS && ImGui::SmallButton("+##id_t")) c->tex_count++;
        if (c->tex_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##id_t")) c->tex_count--; }

        ImGui::Spacing();
        ImGui::Text("SRV Slots (%d / %d):", c->srv_count, MAX_SRV_SLOTS);
        for (int s = 0; s < c->srv_count; s++) {
            ImGui::PushID(1000 + s);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "s%d srv", (int)c->srv_slots[s]);
            res_combo(lbl, &c->srv_handles[s], RES_NONE);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.f);
            ImGui::InputScalar("##idsslot", ImGuiDataType_U32, &c->srv_slots[s]);
            ImGui::PopID();
        }
        if (c->srv_count < MAX_SRV_SLOTS && ImGui::SmallButton("+##id_s")) c->srv_count++;
        if (c->srv_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##id_s")) c->srv_count--; }

        ui_inspector_section("PIXEL UAV OUTPUTS");
        UINT draw_rtv_count = ui_draw_command_rtv_count(*c);
        ImGui::TextDisabled("DX11 output slots are shared by RTVs and PS UAVs.");
        ImGui::TextDisabled("With %u RTV%s active, the first valid UAV slot is u%u.",
            draw_rtv_count, draw_rtv_count == 1 ? "" : "s", draw_rtv_count);
        ImGui::Text("UAV Slots (%d / %d):", c->uav_count, MAX_UAV_SLOTS);
        for (int u = 0; u < c->uav_count; u++) {
            ImGui::PushID(1100 + u);
            char lbl[32]; snprintf(lbl, sizeof(lbl), "u%d uav", (int)c->uav_slots[u]);
            res_combo(lbl, &c->uav_handles[u], RES_NONE);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.f);
            ImGui::InputScalar("##idpuavslot", ImGuiDataType_U32, &c->uav_slots[u]);
            ImGui::PopID();
        }
        if (c->uav_count < MAX_UAV_SLOTS && ImGui::SmallButton("+##id_u")) c->uav_count++;
        if (c->uav_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##id_u")) c->uav_count--; }

        ui_inspector_section("INDIRECT BUFFER");
        res_combo("Indirect Buffer##id", &c->indirect_buf, RES_STRUCTURED_BUFFER, false);
        ImGui::InputScalar("Byte Offset", ImGuiDataType_U32, &c->indirect_offset);
        break;
    }

    case CMD_INDIRECT_DISPATCH: {
        ui_inspector_section("INDIRECT COMMAND");
        res_combo("Shader##id", &c->shader, RES_SHADER, false);
        ImGui::Checkbox("Only On Reset", &c->compute_on_reset);
        ui_inspector_section("SHADER PARAMETERS");
        ui_command_shader_params(c, res_get(c->shader));
        ui_inspector_section("INDIRECT BUFFER");
        res_combo("Indirect Buffer##id", &c->indirect_buf, RES_STRUCTURED_BUFFER, false);
        ImGui::InputScalar("Byte Offset", ImGuiDataType_U32, &c->indirect_offset);
        break;
    }

    default: break;
    }
}

static void ui_panel_inspector(bool embedded = false) {
    if (!embedded) ImGui::Begin("Inspector");
    if (g_sel_res != INVALID_HANDLE) {
        Resource* r = res_get(g_sel_res);
        if (r) ui_inspector_resource(r, g_sel_res);
        else   ImGui::TextDisabled("(stale selection)");
    } else if (g_sel_cmd != INVALID_HANDLE) {
        Command* c = cmd_get(g_sel_cmd);
        if (c) ui_inspector_command(c);
        else   ImGui::TextDisabled("(stale selection)");
    } else {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextDisabled("Right-click Resources or Commands panels to create items.");
    }
    if (!embedded) ImGui::End();
}

// ── user cb panel ─────────────────────────────────────────────────────────

static void ui_binding_row(const char* role, const char* stage, int slot, ResHandle h) {
    Resource* r = res_get(h);
    ImGui::PushID(role);
    ImGui::PushID(stage ? stage : "");
    ImGui::PushID(slot);
    ImGui::PushID((int)h);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.080f, 0.077f, 0.081f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
    ImGui::BeginChild("##binding_card", ImVec2(0.0f, 62.0f), true);

    ImGui::TextUnformatted(role);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", stage ? stage : "-");
    if (slot >= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("slot %d", slot);
    }

    if (r) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_type_color(r->type));
        ImGui::TextUnformatted(ui_resource_display_name(*r));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ui_resource_display_type(*r));
    } else {
        ImGui::TextDisabled("(none)");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
    ImGui::PopID();
}

static void ui_panel_bindings(bool embedded = false) {
    if (!embedded) ImGui::Begin("Bindings");

    if (g_sel_cmd != INVALID_HANDLE) {
        Command* c = cmd_get(g_sel_cmd);
        if (!c) {
            ImGui::TextDisabled("(stale command)");
        } else {
            switch (c->type) {
            case CMD_CLEAR:
                ui_inspector_section("OUTPUTS");
                ui_binding_row("Render Target", "OM", -1, c->rt);
                ui_binding_row("Depth Buffer", "OM", -1, c->depth);
                break;
            case CMD_DRAW_MESH:
            case CMD_DRAW_INSTANCED:
                ui_inspector_section("PIPELINE");
                if (ui_command_uses_procedural_draw(*c))
                    ImGui::TextDisabled("Source: Procedural (%s)", ui_draw_topology_name(c->draw_topology));
                else
                    ui_binding_row("Mesh", "IA", -1, c->mesh);
                ui_binding_row("Shader", "VS/PS", -1, c->shader);
                if (c->shadow_cast)
                    ui_binding_row("Shadow Shader", "VS", -1, c->shadow_shader);
                ui_inspector_section("OUTPUTS");
                ui_binding_row("Render Target 0", "OM", 0, c->rt);
                for (int i = 0; i < c->mrt_count; i++) {
                    char role[32];
                    snprintf(role, sizeof(role), "Render Target %d", i + 1);
                    ui_binding_row(role, "OM", i + 1, c->mrt_handles[i]);
                }
                ui_binding_row("Depth Buffer", "OM", -1, c->depth);
                for (int i = 0; i < c->uav_count; i++)
                    ui_binding_row("UAV", "OM/PS", (int)c->uav_slots[i], c->uav_handles[i]);
                if (c->tex_count > 0 || c->srv_count > 0)
                    ui_inspector_section("SHADER RESOURCES");
                for (int i = 0; i < c->tex_count; i++)
                    ui_binding_row("Texture", "PS", (int)c->tex_slots[i], c->tex_handles[i]);
                for (int i = 0; i < c->srv_count; i++)
                    ui_binding_row("SRV", "VS", (int)c->srv_slots[i], c->srv_handles[i]);
                break;
            case CMD_DISPATCH:
                ui_inspector_section("COMPUTE");
                ui_binding_row("Compute Shader", "CS", -1, c->shader);
                if (c->srv_count > 0)
                    ui_inspector_section("INPUTS");
                for (int i = 0; i < c->srv_count; i++)
                    ui_binding_row("SRV", "CS", (int)c->srv_slots[i], c->srv_handles[i]);
                if (c->uav_count > 0)
                    ui_inspector_section("OUTPUTS");
                for (int i = 0; i < c->uav_count; i++)
                    ui_binding_row("UAV", "CS", (int)c->uav_slots[i], c->uav_handles[i]);
                break;
            case CMD_INDIRECT_DRAW:
                ui_inspector_section("PIPELINE");
                if (ui_command_uses_procedural_draw(*c))
                    ImGui::TextDisabled("Source: Procedural (%s)", ui_draw_topology_name(c->draw_topology));
                else
                    ui_binding_row("Mesh", "IA", -1, c->mesh);
                ui_binding_row("Shader", "VS/PS", -1, c->shader);
                if (c->shadow_cast)
                    ui_binding_row("Shadow Shader", "VS", -1, c->shadow_shader);
                ui_inspector_section("OUTPUTS");
                ui_binding_row("Render Target 0", "OM", 0, c->rt);
                for (int i = 0; i < c->mrt_count; i++) {
                    char role[32];
                    snprintf(role, sizeof(role), "Render Target %d", i + 1);
                    ui_binding_row(role, "OM", i + 1, c->mrt_handles[i]);
                }
                ui_binding_row("Depth Buffer", "OM", -1, c->depth);
                for (int i = 0; i < c->uav_count; i++)
                    ui_binding_row("UAV", "OM/PS", (int)c->uav_slots[i], c->uav_handles[i]);
                if (c->tex_count > 0 || c->srv_count > 0)
                    ui_inspector_section("SHADER RESOURCES");
                for (int i = 0; i < c->tex_count; i++)
                    ui_binding_row("Texture", "PS", (int)c->tex_slots[i], c->tex_handles[i]);
                for (int i = 0; i < c->srv_count; i++)
                    ui_binding_row("SRV", "VS", (int)c->srv_slots[i], c->srv_handles[i]);
                ui_inspector_section("ARGUMENTS");
                ui_binding_row("Indirect Buffer", "ARG", -1, c->indirect_buf);
                break;
            case CMD_INDIRECT_DISPATCH:
                ui_inspector_section("COMPUTE");
                ui_binding_row("Compute Shader", "CS", -1, c->shader);
                ui_inspector_section("ARGUMENTS");
                ui_binding_row("Indirect Buffer", "ARG", -1, c->indirect_buf);
                break;
            default:
                break;
            }
        }
    } else if (g_sel_res != INVALID_HANDLE) {
        Resource* r = res_get(g_sel_res);
        if (!r) {
            ImGui::TextDisabled("(stale resource)");
        } else if (r->type == RES_SHADER && r->shader_cb.active) {
            ui_inspector_section("REFLECTED CBUFFER");
            ImGui::TextDisabled("%s: register(b%u), %u bytes",
                r->shader_cb.name, r->shader_cb.bind_slot, r->shader_cb.size);
            for (int i = 0; i < r->shader_cb.var_count; i++) {
                const ShaderCBVar& v = r->shader_cb.vars[i];
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.080f, 0.077f, 0.081f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
                ImGui::BeginChild("##shader_var_card", ImVec2(0.0f, 58.0f), true);
                ImGui::TextUnformatted(v.name);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", res_type_str(v.type));
                ImGui::TextDisabled("offset %u  size %u", v.offset, v.size);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        } else if (r->type == RES_MESH && (r->mesh_part_count > 0 || r->mesh_material_count > 0)) {
            if (r->mesh_part_count > 0) {
                ui_inspector_section("MESH PARTS");
                for (int i = 0; i < r->mesh_part_count; i++) {
                    MeshPart& part = r->mesh_parts[i];
                    ImGui::Text("%s [%s]", part.name[0] ? part.name : "(part)", part.enabled ? "on" : "off");
                }
            }
            if (r->mesh_material_count > 0) {
                ui_inspector_section("MESH MATERIALS");
                for (int mi = 0; mi < r->mesh_material_count; mi++) {
                    MeshMaterial& mat = r->mesh_materials[mi];
                    ImGui::TextDisabled("%s", mat.name[0] ? mat.name : "(material)");
                    for (int slot = 0; slot < MAX_MESH_MATERIAL_TEXTURES; slot++) {
                        if (mat.textures[slot] != INVALID_HANDLE)
                            ui_key_value_handle(ui_mesh_material_slot_name(slot), mat.textures[slot]);
                    }
                }
            }
        } else {
            ui_inspector_section("GPU BIND FLAGS");
            ImGui::Text("RTV: %s", r->has_rtv ? "yes" : "no");
            ImGui::Text("SRV: %s", r->has_srv ? "yes" : "no");
            ImGui::Text("UAV: %s", r->has_uav ? "yes" : "no");
            ImGui::Text("DSV: %s", r->has_dsv ? "yes" : "no");
        }
    } else {
        ImGui::TextDisabled("Nothing selected.");
    }

    if (!embedded) ImGui::End();
}

static void ui_panel_selection_state(bool embedded = false) {
    if (!embedded) ImGui::Begin("State");

    if (g_sel_cmd != INVALID_HANDLE) {
        Command* c = cmd_get(g_sel_cmd);
        if (!c) {
            ImGui::TextDisabled("(stale command)");
        } else {
            ui_inspector_section("COMMAND STATE");
            ImGui::Text("Enabled: %s", c->enabled ? "yes" : "no");
            ImGui::Text("Warning: %s", ui_command_has_warning(*c) ? "yes" : "no");
            ImGui::Text("Type: %s", cmd_type_str(c->type));
            if (c->type == CMD_DRAW_MESH || c->type == CMD_DRAW_INSTANCED || c->type == CMD_INDIRECT_DRAW) {
                ui_inspector_section("DRAW STATE");
                ImGui::Text("Source: %s", ui_draw_source_name(c->draw_source));
                if (ui_command_uses_procedural_draw(*c)) {
                    ImGui::Text("Topology: %s", ui_draw_topology_name(c->draw_topology));
                    if (c->type != CMD_INDIRECT_DRAW)
                        ImGui::Text("Vertex Count: %d", c->vertex_count);
                }
                ImGui::Text("Color Write: %s", c->color_write ? "yes" : "no");
                ImGui::Text("Depth Test: %s", c->depth_test ? "yes" : "no");
                ImGui::Text("Depth Write: %s", c->depth_write ? "yes" : "no");
                ImGui::Text("Alpha Blend: %s", c->alpha_blend ? "yes" : "no");
                ImGui::Text("RTV Count: %u", ui_draw_command_rtv_count(*c));
                ImGui::Text("Pixel UAV Count: %d", c->uav_count);
                ImGui::Text("Cull Back: %s", c->cull_back ? "yes" : "no");
                ImGui::Text("Shadow Cast: %s", c->shadow_cast ? "yes" : "no");
                ImGui::Text("Shadow Receive: %s", c->shadow_receive ? "yes" : "no");
            }
            if (c->type == CMD_DISPATCH) {
                ui_inspector_section("DISPATCH");
                ImGui::Text("Threads: %d, %d, %d", c->thread_x, c->thread_y, c->thread_z);
            }
        }
    } else if (g_sel_res != INVALID_HANDLE) {
        Resource* r = res_get(g_sel_res);
        if (!r) {
            ImGui::TextDisabled("(stale resource)");
        } else {
            ui_inspector_section("RESOURCE STATE");
            ImGui::Text("Type: %s", ui_resource_display_type(*r));
            ImGui::Text("Built-in: %s", r->is_builtin ? "yes" : "no");
            ImGui::Text("Warning: %s", ui_resource_has_warning(*r) ? "yes" : "no");
            if (r->path[0])
                ImGui::TextWrapped("Path: %s", r->path);
            if (r->type == RES_SHADER || r->type == RES_MESH) {
                ImGui::Text("Compiled OK: %s", r->compiled_ok ? "yes" : "no");
                ImGui::Text("Fallback: %s", r->using_fallback ? "yes" : "no");
            }
            if (r->width > 0 || r->height > 0) {
                if (r->depth > 1)
                    ImGui::Text("Size: %dx%dx%d", r->width, r->height, r->depth);
                else
                    ImGui::Text("Size: %dx%d", r->width, r->height);
            }
            if (r->elem_count > 0)
                ImGui::Text("Elements: %d x %d bytes", r->elem_count, r->elem_size);
            if (r->vert_count > 0)
                ImGui::Text("Geometry: %d verts, %d indices", r->vert_count, r->idx_count);
        }
    } else {
        ImGui::TextDisabled("Nothing selected.");
    }

    if (!embedded) ImGui::End();
}

static void ui_panel_user_cb() {
    ImGui::Begin("User CB (b2)");
    user_cb_enforce_unique_names();

    ImGui::TextDisabled("Slot = 16 bytes (float4). Recommended: cbuffer UserCB : register(b2).");
    ImGui::Separator();

    if (ImGui::BeginTable("ucb", 6,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Slot",   ImGuiTableColumnFlags_WidthFixed,   36);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed,   56);
        ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed,   62);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableHeadersRow();

        static char s_ucb_name_edit[MAX_USER_CB_VARS][MAX_NAME] = {};
        static int s_ucb_name_editing = -1;
        for (int i = 0; i < g_user_cb_count; i++) {
            UserCBEntry& e = g_user_cb_entries[i];
            user_cb_refresh_entry(i);
            Resource* src = e.source_kind == USER_CB_SOURCE_RESOURCE ? res_get(e.source) : nullptr;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("c%d", i);
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("+%d", user_cb_slot_offset(i));
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1.f);
            if (s_ucb_name_editing != i) {
                strncpy(s_ucb_name_edit[i], e.name, MAX_NAME - 1);
                s_ucb_name_edit[i][MAX_NAME - 1] = '\0';
            }
            bool rename_enter = ImGui::InputText("##name", s_ucb_name_edit[i], MAX_NAME,
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemActivated())
                s_ucb_name_editing = i;
            bool rename_commit = s_ucb_name_editing == i &&
                (rename_enter || ImGui::IsItemDeactivatedAfterEdit());
            if (rename_commit) {
                user_cb_rename(i, s_ucb_name_edit[i]);
                strncpy(s_ucb_name_edit[i], e.name, MAX_NAME - 1);
                s_ucb_name_edit[i][MAX_NAME - 1] = '\0';
                s_ucb_name_editing = -1;
            } else if (s_ucb_name_editing == i && ImGui::IsItemDeactivated()) {
                strncpy(s_ucb_name_edit[i], e.name, MAX_NAME - 1);
                s_ucb_name_edit[i][MAX_NAME - 1] = '\0';
                s_ucb_name_editing = -1;
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s", res_type_str(e.type));
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-1.f);
            bool user_changed = false;
            char source_label[160] = {};
            if (e.source_kind == USER_CB_SOURCE_RESOURCE && src) {
                snprintf(source_label, sizeof(source_label), "%s", ui_resource_display_name(*src));
            } else if (e.source_kind == USER_CB_SOURCE_COMMAND_POSITION) {
                snprintf(source_label, sizeof(source_label), "%s Position", e.source_target);
            } else if (e.source_kind == USER_CB_SOURCE_COMMAND_ROTATION) {
                snprintf(source_label, sizeof(source_label), "%s Rotation", e.source_target);
            } else if (e.source_kind == USER_CB_SOURCE_COMMAND_SCALE) {
                snprintf(source_label, sizeof(source_label), "%s Scale", e.source_target);
            } else if (e.source_kind == USER_CB_SOURCE_CAMERA_POSITION) {
                snprintf(source_label, sizeof(source_label), "Camera Position");
            } else if (e.source_kind == USER_CB_SOURCE_CAMERA_ROTATION) {
                snprintf(source_label, sizeof(source_label), "Camera Rotation");
            } else if (e.source_kind == USER_CB_SOURCE_DIRLIGHT_POSITION) {
                snprintf(source_label, sizeof(source_label), "Dir Light Position");
            } else if (e.source_kind == USER_CB_SOURCE_DIRLIGHT_TARGET) {
                snprintf(source_label, sizeof(source_label), "Dir Light Target");
            } else {
                snprintf(source_label, sizeof(source_label), "(hardcoded)");
            }
            if (ImGui::BeginCombo("##source", source_label)) {
                if (ImGui::Selectable("(hardcoded)", e.source_kind == USER_CB_SOURCE_NONE)) {
                    user_cb_set_source(i, INVALID_HANDLE);
                    user_changed = true;
                }
                ImGui::Separator();
                ImGui::TextDisabled("Resources");
                for (int r_i = 0; r_i < MAX_RESOURCES; r_i++) {
                    Resource& r = g_resources[r_i];
                    if (!r.active || r.is_builtin || r.type != e.type) continue;
                    ResHandle h = (ResHandle)(r_i + 1);
                    bool sel = (e.source_kind == USER_CB_SOURCE_RESOURCE && e.source == h);
                    ImGui::PushID(r_i);
                    if (ImGui::Selectable(ui_resource_display_name(r), sel)) {
                        user_cb_set_source(i, h);
                        user_changed = true;
                    }
                    ImGui::PopID();
                }
                if (e.type == RES_FLOAT3 || e.type == RES_FLOAT4) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Scene transforms");
                    if (ImGui::Selectable("Camera Position", e.source_kind == USER_CB_SOURCE_CAMERA_POSITION)) {
                        user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_CAMERA_POSITION, "camera");
                    }
                    if (ImGui::Selectable("Camera Rotation", e.source_kind == USER_CB_SOURCE_CAMERA_ROTATION)) {
                        user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_CAMERA_ROTATION, "camera");
                    }
                    if (ImGui::Selectable("Dir Light Position", e.source_kind == USER_CB_SOURCE_DIRLIGHT_POSITION)) {
                        user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_DIRLIGHT_POSITION, "dirlight");
                    }
                    if (ImGui::Selectable("Dir Light Target", e.source_kind == USER_CB_SOURCE_DIRLIGHT_TARGET)) {
                        user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_DIRLIGHT_TARGET, "dirlight");
                    }
                    for (int c_i = 0; c_i < MAX_COMMANDS; c_i++) {
                        Command& c = g_commands[c_i];
                        if (!c.active || !ui_command_has_transform(&c))
                            continue;
                        ImGui::PushID(10000 + c_i);
                        char item[128] = {};
                        snprintf(item, sizeof(item), "%s Position", c.name);
                        if (ImGui::Selectable(item, e.source_kind == USER_CB_SOURCE_COMMAND_POSITION &&
                                                    strcmp(e.source_target, c.name) == 0)) {
                            user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_COMMAND_POSITION, c.name);
                        }
                        snprintf(item, sizeof(item), "%s Rotation", c.name);
                        if (ImGui::Selectable(item, e.source_kind == USER_CB_SOURCE_COMMAND_ROTATION &&
                                                    strcmp(e.source_target, c.name) == 0)) {
                            user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_COMMAND_ROTATION, c.name);
                        }
                        snprintf(item, sizeof(item), "%s Scale", c.name);
                        if (ImGui::Selectable(item, e.source_kind == USER_CB_SOURCE_COMMAND_SCALE &&
                                                    strcmp(e.source_target, c.name) == 0)) {
                            user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_COMMAND_SCALE, c.name);
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(5);
            src = e.source_kind == USER_CB_SOURCE_RESOURCE ? res_get(e.source) : nullptr;
            if (e.source_kind != USER_CB_SOURCE_NONE)
                ImGui::TextDisabled("(source driven)");
            else
                user_changed |= ui_user_cb_value_editor(e.type, e.ival, e.fval);
            if (user_changed)
                timeline_capture_if_tracked(TIMELINE_TRACK_USER_VAR, e.name, e.type);
            ImGui::SameLine();
            if (ImGui::SmallButton("^") && i > 0) {
                s_ucb_name_editing = -1;
                user_cb_move(i, i - 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("v") && i < g_user_cb_count - 1) {
                s_ucb_name_editing = -1;
                user_cb_move(i, i + 1);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                s_ucb_name_editing = -1;
                user_cb_remove(i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Create a cbuffer variable with its own hardcoded value.
    ImGui::Separator();
    static ResType s_create_type = RES_FLOAT;
    static char s_create_name[MAX_NAME] = {};
    const ResType create_types[] = {
        RES_FLOAT, RES_FLOAT2, RES_FLOAT3, RES_FLOAT4, RES_INT, RES_INT2, RES_INT3
    };

    ImGui::SetNextItemWidth(110.f);
    if (ImGui::BeginCombo("Type##ucb_create_type", res_type_str(s_create_type))) {
        for (int i = 0; i < (int)(sizeof(create_types) / sizeof(create_types[0])); i++) {
            ResType type = create_types[i];
            bool sel = (s_create_type == type);
            if (ImGui::Selectable(res_type_str(type), sel))
                s_create_type = type;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.f);
    ImGui::InputText("Name##ucb_create_name", s_create_name, MAX_NAME);
    ImGui::SameLine();
    if (ImGui::Button("Create Var")) {
        const char* name = s_create_name[0] ? s_create_name : user_cb_default_base_name(s_create_type);
        if (user_cb_add_var(name, s_create_type))
            s_create_name[0] = '\0';
    }

    ImGui::Spacing();
    static ResHandle s_add = INVALID_HANDLE;
    {
        Resource* cur = res_get(s_add);
        ImGui::SetNextItemWidth(-60.f);
        if (ImGui::BeginCombo("##ucb_add", cur ? ui_resource_display_name(*cur) : "(select resource)")) {
            for (int i = 0; i < MAX_RESOURCES; i++) {
                Resource& r = g_resources[i];
                if (!r.active || r.is_builtin) continue;
                if (!user_cb_type_supported(r.type)) continue;
                bool sel = (s_add == (ResHandle)(i + 1));
                ImGui::PushID(i);
                if (ImGui::Selectable(ui_resource_display_name(r), sel)) s_add = (ResHandle)(i + 1);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Link") && s_add != INVALID_HANDLE) {
        user_cb_add_from_resource(s_add);
        s_add = INVALID_HANDLE;
    }

    // Generated HLSL snippet
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("HLSL snippet");
    if (ImGui::BeginChild("ucb_hlsl", {0, 140}, true)) {
        ImGui::TextDisabled("cbuffer UserCB : register(b2)");
        ImGui::TextDisabled("{");
        for (int i = 0; i < g_user_cb_count; i++) {
            UserCBEntry& e = g_user_cb_entries[i];
            ImGui::TextDisabled("    %-8s %-24s: packoffset(c%d);", user_cb_hlsl_type(e.type), e.name, i);
        }
        ImGui::TextDisabled("};");
    }
    ImGui::EndChild();

    ImGui::End();
}

// ── log panel ─────────────────────────────────────────────────────────────

static void ui_format_bytes(uint64_t bytes, char* out, int out_sz);
static uint64_t ui_process_memory_bytes();
static uint64_t ui_estimated_gpu_memory_bytes();
static void ui_reset_camera_view();

static void ui_panel_general(bool embedded = false) {
    if (!embedded) ImGui::Begin("General");
    bool settings_dirty = false;

    if (ImGui::CollapsingHeader("Interface", ImGuiTreeNodeFlags_DefaultOpen)) {
        float ui_scale = ui_global_scale();
        if (ImGui::SliderFloat("Global Scale", &ui_scale, k_ui_scale_min, k_ui_scale_max, "%.2fx")) {
            ui_set_global_scale(ui_scale);
            settings_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##ui_scale")) {
            ui_set_global_scale(k_ui_scale_default);
            settings_dirty = true;
        }
        ImGui::TextDisabled("Scales fonts and layout globally without changing project data.");
    }

    if (ImGui::CollapsingHeader("Shader Editor", ImGuiTreeNodeFlags_DefaultOpen)) {
        float font_size = ui_code_font_size();
        if (ImGui::SliderFloat("Code Font Size", &font_size, 10.0f, 28.0f, "%.0f px")) {
            ui_set_code_font_size(font_size);
            settings_dirty = true;
        }
        bool auto_save_compile = ui_shader_auto_save_compile();
        if (ImGui::Checkbox("Auto Save + Compile", &auto_save_compile)) {
            ui_set_shader_auto_save_compile(auto_save_compile);
            settings_dirty = true;
        }
        ImGui::TextDisabled("Shader source editor defaults. Saved outside project files.");
    }

    if (ImGui::CollapsingHeader("Application", ImGuiTreeNodeFlags_DefaultOpen)) {
        settings_dirty |= ImGui::Checkbox("VSync", &g_dx.vsync);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_dx.vsync ? "Present interval 1" : "Present immediate");
    }

    if (ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Show Grid", &g_dx.scene_grid_enabled)) {
            settings_dirty = true;
            app_request_scene_render();
        }
        if (ImGui::ColorEdit4("Grid Color", g_dx.scene_grid_color,
            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)) {
            settings_dirty = true;
            app_request_scene_render();
        }
        ImGui::TextDisabled("Infinite grid overlay on y=0. Alpha controls overall intensity.");
    }

    if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        settings_dirty |= ImGui::Checkbox("D3D11 Runtime Validation", &g_dx.d3d11_validation);
        if (g_dx.d3d11_validation_active) {
            ImGui::TextDisabled("Debug layer active in this session. Adds overhead.");
        } else if (g_dx.d3d11_validation) {
            if (!g_dx.d3d11_validation_supported)
                ImGui::TextDisabled("Requested, but the D3D11 debug layer is unavailable on this system.");
            else
                ImGui::TextDisabled("Takes effect on next launch.");
        } else {
            ImGui::TextDisabled("Disabled. Enable and restart to capture D3D11 runtime warnings.");
        }

        settings_dirty |= ImGui::Checkbox("Shader Binding Warnings", &g_dx.shader_validation_warnings);
        ImGui::TextDisabled("Warn once when a shader expects SRV/UAV bindings that a command does not provide.");

        bool can_flush_d3d11 = g_dx.d3d11_validation_active && g_dx.info_queue;
        if (!can_flush_d3d11)
            ImGui::BeginDisabled();
        if (ImGui::Button("Flush D3D11 Messages"))
            dx_debug_log_messages();
        if (!can_flush_d3d11)
            ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Profiler", ImGuiTreeNodeFlags_DefaultOpen)) {
        settings_dirty |= ImGui::Checkbox("Enable profiling", &g_profiler_enabled);
        if (g_profiler_enabled) {
            char app_mem[32] = {};
            char gpu_mem[32] = {};
            char project_gpu_mem[32] = {};
            ui_format_bytes(ui_process_memory_bytes(), app_mem, sizeof(app_mem));
            ui_format_bytes(ui_estimated_gpu_memory_bytes(), gpu_mem, sizeof(gpu_mem));
            ui_format_bytes(res_estimate_gpu_total(false), project_gpu_mem, sizeof(project_gpu_mem));
            if (cmd_profile_total_ready())
                ImGui::Text("Frame GPU time: %.3f ms", cmd_profile_total_frame_ms());
            else
                ImGui::TextDisabled("Frame GPU time: warming up...");
            if (cmd_profile_ready())
                ImGui::Text("Command GPU time: %.3f ms", cmd_profile_frame_ms());
            else
                ImGui::TextDisabled("Command GPU time: warming up...");
            ImGui::Text("Application memory: %s", app_mem);
            ImGui::Text("Estimated GPU memory: %s", gpu_mem);
            ImGui::TextDisabled("Project GPU resources: %s", project_gpu_mem);
        }
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        settings_dirty |= ImGui::Checkbox("Enabled", &g_camera_controls.enabled);
        settings_dirty |= ImGui::Checkbox("Mouse Look", &g_camera_controls.mouse_look);
        ImGui::SameLine();
        settings_dirty |= ImGui::Checkbox("Invert Y", &g_camera_controls.invert_y);

        settings_dirty |= ImGui::DragFloat("Move Speed", &g_camera_controls.move_speed, 0.01f, 0.001f, 100.0f);
        settings_dirty |= ImGui::DragFloat("Fast Mult", &g_camera_controls.fast_mult, 0.05f, 1.0f, 20.0f);
        settings_dirty |= ImGui::DragFloat("Slow Mult", &g_camera_controls.slow_mult, 0.01f, 0.01f, 1.0f);
        settings_dirty |= ImGui::DragFloat("Mouse Sensitivity", &g_camera_controls.mouse_sensitivity, 0.0001f, 0.0001f, 0.05f, "%.4f");

        ImGui::Separator();
        bool camera_changed = false;
        camera_changed |= ImGui::DragFloat3("Position", g_camera.position, 0.01f);
        camera_changed |= ImGui::DragFloat("Yaw", &g_camera.yaw, 0.01f);
        camera_changed |= ImGui::DragFloat("Pitch", &g_camera.pitch, 0.01f, -1.50f, 1.50f);
        camera_changed |= ImGui::DragFloat("FOV", &g_camera.fov_y, 0.01f, 0.10f, 2.80f);
        camera_changed |= ImGui::DragFloat("Near Plane", &g_camera.near_z, 0.001f, 0.0001f, 100.0f);
        camera_changed |= ImGui::DragFloat("Far Plane", &g_camera.far_z, 0.05f, 0.001f, 10000.0f);

        g_camera.pitch = clampf(g_camera.pitch, -1.50f, 1.50f);
        if (g_camera.fov_y < 0.10f) g_camera.fov_y = 0.10f;
        if (g_camera.fov_y > 2.80f) g_camera.fov_y = 2.80f;
        if (g_camera.near_z < 0.0001f) g_camera.near_z = 0.0001f;
        if (g_camera.far_z <= g_camera.near_z + 0.001f)
            g_camera.far_z = g_camera.near_z + 0.001f;
        if (camera_changed)
            timeline_capture_if_tracked(TIMELINE_TRACK_CAMERA, "camera", RES_NONE);

        if (ImGui::Button("Reset Camera")) {
            ui_reset_camera_view();
            timeline_capture_if_tracked(TIMELINE_TRACK_CAMERA, "camera", RES_NONE);
        }
    }

    if (settings_dirty)
        app_settings_save();

    if (!embedded) ImGui::End();
}

static void ui_panel_log(bool embedded = false) {
    if (!embedded) ImGui::Begin("Log");

    if (ImGui::SmallButton("Clear")) memset(&g_log, 0, sizeof(g_log));
    ImGui::SameLine();
    ImGui::TextDisabled("%d entries", g_log.count);
    ImGui::Separator();

    ImGui::BeginChild("log_scroll", {0, 0}, false, ImGuiWindowFlags_HorizontalScrollbar);

    int total = g_log.count < LOG_MAX_ENTRIES ? g_log.count : LOG_MAX_ENTRIES;
    int start = g_log.count < LOG_MAX_ENTRIES ? 0 : g_log.head;

    for (int i = 0; i < total; i++) {
        const LogEntry& e = g_log.entries[(start + i) % LOG_MAX_ENTRIES];
        ImVec4 col = {0.85f, 0.85f, 0.85f, 1.f};
        const char* prefix = "   ";
        if (e.level == LOG_WARN)  { col = {1.0f, 0.85f, 0.2f, 1.f};  prefix = "[W]"; }
        if (e.level == LOG_ERROR) { col = {1.0f, 0.35f, 0.3f, 1.f};  prefix = "[E]"; }
        ImGui::TextColored(col, "[%s] %s %s", e.time[0] ? e.time : "--:--:--", prefix, e.msg);
    }

    if (g_log.scroll_to_bottom) {
        ImGui::SetScrollHereY(1.0f);
        g_log.scroll_to_bottom = false;
    }
    ImGui::EndChild();
    if (!embedded) ImGui::End();
}

// ── scene viewport ────────────────────────────────────────────────────────

static bool ui_selected_command_supports_gizmo() {
    Command* c = cmd_get(g_sel_cmd);
    return c && ui_command_supports_gizmo_type(c->type);
}

static Command* ui_selected_gizmo_command() {
    return ui_selected_command_supports_gizmo() ? cmd_get(g_sel_cmd) : nullptr;
}

static void ui_cancel_viewport_gizmo_drag() {
    memset(&s_viewport_gizmo_drag, 0, sizeof(s_viewport_gizmo_drag));
}

static void ui_set_viewport_gizmo_mode(UiViewportGizmoMode mode) {
    if (s_viewport_gizmo_mode == mode)
        mode = UI_GIZMO_NONE;
    s_viewport_gizmo_mode = mode;
    ui_cancel_viewport_gizmo_drag();
}

static Mat4 ui_mat4_from_raw(const float raw[16]) {
    Mat4 m = {};
    if (raw)
        memcpy(m.m, raw, sizeof(m.m));
    return m;
}

static Vec3 ui_gizmo_axis_dir_from_rotation(const Mat4& rot, int axis) {
    switch (axis) {
    case 0: return v3_norm(v3(rot.m[0], rot.m[1], rot.m[2]));
    case 1: return v3_norm(v3(rot.m[4], rot.m[5], rot.m[6]));
    default: return v3_norm(v3(rot.m[8], rot.m[9], rot.m[10]));
    }
}

static void ui_mul_world_point(const Mat4& m, Vec3 p, float* x, float* y, float* z, float* w) {
    if (x) *x = p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8]  + m.m[12];
    if (y) *y = p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9]  + m.m[13];
    if (z) *z = p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14];
    if (w) *w = p.x * m.m[3] + p.y * m.m[7] + p.z * m.m[11] + m.m[15];
}

static bool ui_project_world_to_screen(const Mat4& view_proj, ImVec2 rect_min, ImVec2 rect_max,
                                       Vec3 world, ImVec2* out_screen)
{
    float clip_x = 0.0f, clip_y = 0.0f, clip_z = 0.0f, clip_w = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float w = rect_max.x - rect_min.x;
    float h = rect_max.y - rect_min.y;
    if (!out_screen || w <= 1.0f || h <= 1.0f)
        return false;

    ui_mul_world_point(view_proj, world, &clip_x, &clip_y, &clip_z, &clip_w);
    if (clip_w <= 0.0001f)
        return false;

    nx = clip_x / clip_w;
    ny = clip_y / clip_w;
    out_screen->x = rect_min.x + (nx * 0.5f + 0.5f) * w;
    out_screen->y = rect_min.y + (1.0f - (ny * 0.5f + 0.5f)) * h;
    return true;
}

static float ui_imvec2_dot(ImVec2 a, ImVec2 b) {
    return a.x * b.x + a.y * b.y;
}

static ImVec2 ui_imvec2_sub(ImVec2 a, ImVec2 b) {
    return ImVec2(a.x - b.x, a.y - b.y);
}

static ImVec2 ui_imvec2_add(ImVec2 a, ImVec2 b) {
    return ImVec2(a.x + b.x, a.y + b.y);
}

static ImVec2 ui_imvec2_scale(ImVec2 a, float s) {
    return ImVec2(a.x * s, a.y * s);
}

static float ui_imvec2_len(ImVec2 a) {
    return sqrtf(a.x * a.x + a.y * a.y);
}

static ImVec2 ui_imvec2_norm(ImVec2 a) {
    float len = ui_imvec2_len(a);
    if (len <= 0.0001f)
        return ImVec2(1.0f, 0.0f);
    return ImVec2(a.x / len, a.y / len);
}

static float ui_imvec2_distance_sq(ImVec2 a, ImVec2 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static float ui_distance_sq_to_segment(ImVec2 p, ImVec2 a, ImVec2 b, float* out_t) {
    ImVec2 ab = ui_imvec2_sub(b, a);
    ImVec2 ap = ui_imvec2_sub(p, a);
    float ab_len_sq = ui_imvec2_dot(ab, ab);
    float t = 0.0f;
    ImVec2 closest = a;
    if (ab_len_sq > 0.0001f) {
        t = clampf(ui_imvec2_dot(ap, ab) / ab_len_sq, 0.0f, 1.0f);
        closest = ui_imvec2_add(a, ui_imvec2_scale(ab, t));
    }
    if (out_t)
        *out_t = t;
    return ui_imvec2_distance_sq(p, closest);
}

static float ui_distance_sq_to_polyline(ImVec2 p, const ImVec2* pts, int count) {
    float best = 1e30f;
    if (!pts || count < 2)
        return best;
    for (int i = 0; i < count - 1; i++) {
        float hit = ui_distance_sq_to_segment(p, pts[i], pts[i + 1], nullptr);
        if (hit < best)
            best = hit;
    }
    return best;
}

static ImVec4 ui_gizmo_axis_color(int axis, bool active) {
    ImVec4 color = ImVec4(0.82f, 0.82f, 0.82f, 1.0f);
    if (axis == 0) color = ImVec4(0.92f, 0.30f, 0.28f, 1.0f);
    if (axis == 1) color = ImVec4(0.32f, 0.84f, 0.42f, 1.0f);
    if (axis == 2) color = ImVec4(0.35f, 0.58f, 0.96f, 1.0f);
    if (active) {
        color.x = clampf(color.x + 0.12f, 0.0f, 1.0f);
        color.y = clampf(color.y + 0.12f, 0.0f, 1.0f);
        color.z = clampf(color.z + 0.12f, 0.0f, 1.0f);
    }
    return color;
}

static float ui_viewport_gizmo_world_axis_len(Vec3 origin, float viewport_h) {
    Vec3 eye = camera_eye(g_camera);
    Vec3 delta = v3_sub(origin, eye);
    float dist = sqrtf(v3_dot(delta, delta));
    float world_per_pixel = 0.0f;
    if (dist < 0.05f) dist = 0.05f;
    if (viewport_h < 8.0f) viewport_h = 8.0f;
    world_per_pixel = (2.0f * dist * tanf(g_camera.fov_y * 0.5f)) / viewport_h;
    return clampf(world_per_pixel * 78.0f, 0.05f, 1000.0f);
}

static Mat4 ui_gizmo_axis_rotation_matrix(int axis, float angle) {
    Vec3 euler = v3(0.0f, 0.0f, 0.0f);
    if (axis == 0) euler.x = angle;
    if (axis == 1) euler.y = angle;
    if (axis == 2) euler.z = angle;
    return mat4_rotation_xyz(euler);
}

static float ui_wrap_angle_near(float angle, float target) {
    float two_pi = 6.28318530718f;
    while (target - angle > 3.14159265359f)
        target -= two_pi;
    while (target - angle < -3.14159265359f)
        target += two_pi;
    return target;
}

static void ui_extract_rotation_xyz(const Mat4& rot, const float reference[3], float out[3]) {
    float sy = -clampf(rot.m[2], -1.0f, 1.0f);
    float y = asinf(sy);
    float cy = cosf(y);
    float x = 0.0f;
    float z = 0.0f;

    if (fabsf(cy) > 1e-5f) {
        x = atan2f(rot.m[6], rot.m[10]);
        z = atan2f(rot.m[1], rot.m[0]);
    } else {
        y = sy >= 0.0f ? 1.57079632679f : -1.57079632679f;
        z = 0.0f;
        x = sy >= 0.0f ? atan2f(rot.m[4], rot.m[5]) : atan2f(-rot.m[4], rot.m[5]);
    }

    if (reference) {
        x = ui_wrap_angle_near(reference[0], x);
        y = ui_wrap_angle_near(reference[1], y);
        z = ui_wrap_angle_near(reference[2], z);
    }

    out[0] = x;
    out[1] = y;
    out[2] = z;
}

static void ui_draw_translate_axis(ImDrawList* dl, ImVec2 origin, ImVec2 end, ImU32 col, float thickness) {
    ImVec2 dir = ui_imvec2_norm(ui_imvec2_sub(end, origin));
    ImVec2 tangent = ImVec2(-dir.y, dir.x);
    ImVec2 head = ui_imvec2_scale(dir, 9.0f);
    ImVec2 wing = ui_imvec2_scale(tangent, 4.0f);
    dl->AddLine(origin, end, col, thickness);
    dl->AddTriangleFilled(
        end,
        ui_imvec2_sub(end, ui_imvec2_add(head, wing)),
        ui_imvec2_sub(end, ui_imvec2_sub(head, wing)),
        col);
}

static void ui_draw_scale_axis(ImDrawList* dl, ImVec2 origin, ImVec2 end, ImU32 col, float thickness) {
    float box_r = 5.0f;
    dl->AddLine(origin, end, col, thickness);
    dl->AddRectFilled(ImVec2(end.x - box_r, end.y - box_r), ImVec2(end.x + box_r, end.y + box_r), col, 2.0f);
}

static void ui_draw_rotate_axis(ImDrawList* dl, ImVec2 origin, ImVec2 end, ImU32 col, float thickness) {
    dl->AddLine(origin, end, col, thickness);
    dl->AddCircle(end, 6.0f, col, 24, thickness);
}

static void ui_gizmo_rotation_plane_basis(Vec3 axis_world[3], int axis, Vec3* out_u, Vec3* out_v) {
    if (!out_u || !out_v)
        return;
    if (axis == 0) {
        *out_u = axis_world[1];
        *out_v = axis_world[2];
    } else if (axis == 1) {
        *out_u = axis_world[2];
        *out_v = axis_world[0];
    } else {
        *out_u = axis_world[0];
        *out_v = axis_world[1];
    }
}

static int ui_project_rotation_ring(const Mat4& view_proj, ImVec2 rect_min, ImVec2 rect_max,
                                    Vec3 origin, Vec3 basis_u, Vec3 basis_v, float radius,
                                    ImVec2* out_pts, int max_pts)
{
    const int segments = 48;
    int count = 0;
    if (!out_pts || max_pts < segments + 1)
        return 0;

    for (int i = 0; i <= segments; i++) {
        float t = (6.28318530718f * (float)i) / (float)segments;
        Vec3 world_pt =
            v3_add(origin,
                   v3_add(v3_scale(basis_u, cosf(t) * radius),
                          v3_scale(basis_v, sinf(t) * radius)));
        if (!ui_project_world_to_screen(view_proj, rect_min, rect_max, world_pt, &out_pts[count]))
            return 0;
        count++;
    }
    return count;
}

static float ui_gizmo_ring_angle(ImVec2 mouse_vec, ImVec2 basis_u, ImVec2 basis_v) {
    float det = basis_u.x * basis_v.y - basis_u.y * basis_v.x;
    if (fabsf(det) < 1e-4f)
        return atan2f(mouse_vec.y, mouse_vec.x);

    float a = (mouse_vec.x * basis_v.y - mouse_vec.y * basis_v.x) / det;
    float b = (basis_u.x * mouse_vec.y - basis_u.y * mouse_vec.x) / det;
    return atan2f(b, a);
}

static void ui_apply_gizmo_drag(Command* c, const UiViewportGizmoDrag* drag, Vec3 axis_world, ImVec2 mouse_pos) {
    ImVec2 axis_screen = {};
    ImVec2 axis_dir_2d = {};
    ImVec2 mouse_delta = {};
    float axis_motion = 0.0f;
    float screen_len = 1.0f;

    if (!c || !drag)
        return;

    axis_screen = ui_imvec2_sub(drag->axis_end_screen, drag->origin_screen);
    axis_dir_2d = ui_imvec2_norm(axis_screen);
    mouse_delta = ui_imvec2_sub(mouse_pos, drag->mouse_start);
    axis_motion = ui_imvec2_dot(mouse_delta, axis_dir_2d);
    screen_len = drag->axis_screen_len > 1.0f ? drag->axis_screen_len : 1.0f;

    if (drag->mode == UI_GIZMO_TRANSLATE) {
        float world_delta = (axis_motion / screen_len) * drag->axis_world_len;
        c->pos[0] = drag->initial_pos[0] + axis_world.x * world_delta;
        c->pos[1] = drag->initial_pos[1] + axis_world.y * world_delta;
        c->pos[2] = drag->initial_pos[2] + axis_world.z * world_delta;
    } else if (drag->mode == UI_GIZMO_ROTATE) {
        float current_angle = ui_gizmo_ring_angle(
            ui_imvec2_sub(mouse_pos, drag->origin_screen),
            drag->ring_basis_u_screen,
            drag->ring_basis_v_screen);
        float delta_angle = ui_wrap_angle_near(0.0f, current_angle - drag->ring_start_angle);
        Mat4 delta_rot = ui_gizmo_axis_rotation_matrix(drag->axis, delta_angle);
        Mat4 final_rot = mat4_mul(delta_rot, drag->initial_rot_matrix);
        float euler[3] = {};
        ui_extract_rotation_xyz(final_rot, drag->initial_rot, euler);
        c->rot[0] = euler[0];
        c->rot[1] = euler[1];
        c->rot[2] = euler[2];
    } else if (drag->mode == UI_GIZMO_SCALE) {
        c->scale[drag->axis] = drag->initial_scale[drag->axis] + axis_motion / screen_len;
        if (c->scale[drag->axis] < 0.001f)
            c->scale[drag->axis] = 0.001f;
    }
}

static void ui_handle_viewport_gizmo_hotkeys(bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    bool text_blocked = io.WantTextInput || ImGui::IsAnyItemActive();
    if (!text_blocked && s_viewport_gizmo_mode != UI_GIZMO_NONE &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ui_set_viewport_gizmo_mode(UI_GIZMO_NONE);
        return;
    }
    if (!hovered || text_blocked)
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_1, false))
        ui_set_viewport_gizmo_mode(UI_GIZMO_TRANSLATE);
    if (ImGui::IsKeyPressed(ImGuiKey_2, false))
        ui_set_viewport_gizmo_mode(UI_GIZMO_ROTATE);
    if (ImGui::IsKeyPressed(ImGuiKey_3, false))
        ui_set_viewport_gizmo_mode(UI_GIZMO_SCALE);
}

static void ui_draw_viewport_gizmo(ImVec2 rect_min, ImVec2 rect_max, bool hovered) {
    Command* c = ui_selected_gizmo_command();
    Mat4 view_proj = {};
    Mat4 rot = {};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Vec3 origin_world = {};
    Vec3 axis_world[3] = {};
    ImVec2 origin_screen = {};
    ImVec2 axis_end[3] = {};
    float axis_screen_len[3] = {};
    float axis_world_len = 0.0f;
    float ring_world_radius = 0.0f;
    int hovered_axis = -1;
    int active_axis = -1;
    float best_hit = 1e30f;
    bool dragging = s_viewport_gizmo_drag.active;
    ImVec2 ring_pts[3][49] = {};
    int ring_count[3] = {};

    if (!c || s_viewport_gizmo_mode == UI_GIZMO_NONE) {
        ui_cancel_viewport_gizmo_drag();
        return;
    }

    origin_world = v3(c->pos[0], c->pos[1], c->pos[2]);
    view_proj = ui_mat4_from_raw(g_dx.scene_cb_data.view_proj);
    rot = mat4_rotation_xyz(v3(c->rot[0], c->rot[1], c->rot[2]));
    axis_world[0] = ui_gizmo_axis_dir_from_rotation(rot, 0);
    axis_world[1] = ui_gizmo_axis_dir_from_rotation(rot, 1);
    axis_world[2] = ui_gizmo_axis_dir_from_rotation(rot, 2);
    axis_world_len = ui_viewport_gizmo_world_axis_len(origin_world, rect_max.y - rect_min.y);
    ring_world_radius = axis_world_len * 0.82f;

    if (!ui_project_world_to_screen(view_proj, rect_min, rect_max, origin_world, &origin_screen)) {
        ui_cancel_viewport_gizmo_drag();
        return;
    }

    for (int axis = 0; axis < 3; axis++) {
        Vec3 axis_tip_world = v3_add(origin_world, v3_scale(axis_world[axis], axis_world_len));
        if (!ui_project_world_to_screen(view_proj, rect_min, rect_max, axis_tip_world, &axis_end[axis]))
            axis_end[axis] = origin_screen;
        axis_screen_len[axis] = ui_imvec2_len(ui_imvec2_sub(axis_end[axis], origin_screen));
        if (s_viewport_gizmo_mode == UI_GIZMO_ROTATE) {
            Vec3 ring_u = {};
            Vec3 ring_v = {};
            ui_gizmo_rotation_plane_basis(axis_world, axis, &ring_u, &ring_v);
            ring_count[axis] = ui_project_rotation_ring(
                view_proj, rect_min, rect_max, origin_world, ring_u, ring_v,
                ring_world_radius, ring_pts[axis],
                (int)(sizeof(ring_pts[axis]) / sizeof(ring_pts[axis][0])));
        }
    }

    if (dragging) {
        if (s_viewport_gizmo_drag.cmd != g_sel_cmd || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ui_cancel_viewport_gizmo_drag();
            dragging = false;
        } else {
            active_axis = s_viewport_gizmo_drag.axis;
            ImGui::SetNextFrameWantCaptureMouse(true);
            ui_apply_gizmo_drag(c, &s_viewport_gizmo_drag, axis_world[active_axis], ImGui::GetIO().MousePos);
            timeline_capture_if_tracked(TIMELINE_TRACK_COMMAND_TRANSFORM, c->name, RES_NONE);
        }
    }

    if (!dragging && hovered) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        for (int axis = 0; axis < 3; axis++) {
            float hit = 1e30f;
            if (s_viewport_gizmo_mode == UI_GIZMO_ROTATE) {
                hit = ui_distance_sq_to_polyline(mouse, ring_pts[axis], ring_count[axis]);
                if (ring_count[axis] > 1 && hit <= 8.0f * 8.0f && hit < best_hit) {
                    hovered_axis = axis;
                    best_hit = hit;
                }
            } else {
                float t = 0.0f;
                hit = ui_distance_sq_to_segment(mouse, origin_screen, axis_end[axis], &t);
                if ((hit <= 9.0f * 9.0f && t >= 0.12f) ||
                    ui_imvec2_distance_sq(mouse, axis_end[axis]) <= 12.0f * 12.0f) {
                    if (hit < best_hit) {
                        hovered_axis = axis;
                        best_hit = hit;
                    }
                }
            }
        }

        if (hovered_axis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            s_viewport_gizmo_drag.active = true;
            s_viewport_gizmo_drag.mode = s_viewport_gizmo_mode;
            s_viewport_gizmo_drag.cmd = g_sel_cmd;
            s_viewport_gizmo_drag.axis = hovered_axis;
            memcpy(s_viewport_gizmo_drag.initial_pos, c->pos, sizeof(c->pos));
            memcpy(s_viewport_gizmo_drag.initial_rot, c->rot, sizeof(c->rot));
            memcpy(s_viewport_gizmo_drag.initial_scale, c->scale, sizeof(c->scale));
            s_viewport_gizmo_drag.axis_world_len = axis_world_len;
            s_viewport_gizmo_drag.axis_screen_len = axis_screen_len[hovered_axis];
            s_viewport_gizmo_drag.mouse_start = ImGui::GetIO().MousePos;
            s_viewport_gizmo_drag.origin_screen = origin_screen;
            s_viewport_gizmo_drag.axis_end_screen = axis_end[hovered_axis];
            s_viewport_gizmo_drag.initial_rot_matrix = rot;
            if (s_viewport_gizmo_mode == UI_GIZMO_ROTATE) {
                Vec3 ring_u = {};
                Vec3 ring_v = {};
                ImVec2 ring_u_screen = origin_screen;
                ImVec2 ring_v_screen = origin_screen;
                ui_gizmo_rotation_plane_basis(axis_world, hovered_axis, &ring_u, &ring_v);
                ui_project_world_to_screen(
                    view_proj, rect_min, rect_max,
                    v3_add(origin_world, v3_scale(ring_u, ring_world_radius)),
                    &ring_u_screen);
                ui_project_world_to_screen(
                    view_proj, rect_min, rect_max,
                    v3_add(origin_world, v3_scale(ring_v, ring_world_radius)),
                    &ring_v_screen);
                s_viewport_gizmo_drag.ring_basis_u_screen = ui_imvec2_sub(ring_u_screen, origin_screen);
                s_viewport_gizmo_drag.ring_basis_v_screen = ui_imvec2_sub(ring_v_screen, origin_screen);
                s_viewport_gizmo_drag.ring_start_angle = ui_gizmo_ring_angle(
                    ui_imvec2_sub(ImGui::GetIO().MousePos, origin_screen),
                    s_viewport_gizmo_drag.ring_basis_u_screen,
                    s_viewport_gizmo_drag.ring_basis_v_screen);
            }
            active_axis = hovered_axis;
            dragging = true;
            ImGui::SetNextFrameWantCaptureMouse(true);
        }
    }

    dl->AddCircleFilled(origin_screen, 4.0f, ImGui::GetColorU32(ImVec4(0.95f, 0.95f, 0.95f, 0.95f)), 16);
    dl->AddCircle(origin_screen, 7.0f, ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.08f, 0.90f)), 16, 2.0f);
    for (int axis = 0; axis < 3; axis++) {
        bool active = axis == active_axis || axis == hovered_axis;
        ImU32 col = ImGui::GetColorU32(ui_gizmo_axis_color(axis, active));
        float thickness = active ? 3.2f : 2.2f;
        if (s_viewport_gizmo_mode == UI_GIZMO_TRANSLATE)
            ui_draw_translate_axis(dl, origin_screen, axis_end[axis], col, thickness);
        else if (s_viewport_gizmo_mode == UI_GIZMO_ROTATE) {
            if (ring_count[axis] > 1)
                dl->AddPolyline(ring_pts[axis], ring_count[axis], col, ImDrawFlags_None, thickness);
        }
        else
            ui_draw_scale_axis(dl, origin_screen, axis_end[axis], col, thickness);
    }
}

static void ui_panel_scene(bool embedded = false) {
    if (!embedded) ImGui::Begin("Scene");
    bool hovered = false;
    ImVec2 image_min = {};
    ImVec2 image_max = {};
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x > 4 && avail.y > 4) {
        int new_w = (int)avail.x;
        int new_h = (int)avail.y;
        if (s_scene_surface_resize_armed) {
            app_request_scene_surface_resize(new_w, new_h);
            s_scene_surface_resize_armed = false;
        }
        if (g_dx.scene_srv) {
            ImGui::Image((ImTextureID)g_dx.scene_srv, avail);
            hovered = ImGui::IsItemHovered();
            image_min = ImGui::GetItemRectMin();
            image_max = ImGui::GetItemRectMax();
        }
    }
    ui_handle_viewport_gizmo_hotkeys(hovered);
    g_scene_view_hovered = hovered;
    if (g_dx.scene_srv && image_max.x > image_min.x && image_max.y > image_min.y)
        ui_draw_viewport_gizmo(image_min, image_max, hovered);
    if (!embedded) ImGui::End();
}

// ── public ────────────────────────────────────────────────────────────────

static float ui_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

enum UiPanelTone {
    UI_PANEL_DEFAULT = 0,
    UI_PANEL_PIPELINE,
    UI_PANEL_RESOURCES,
    UI_PANEL_VIEWPORT,
    UI_PANEL_LOG,
    UI_PANEL_INSPECTOR,
    UI_PANEL_GENERAL
};

static UiPanelTone s_panel_tone_stack[16] = {};
static int s_panel_tone_count = 0;

static ImVec4 ui_panel_bg(UiPanelTone tone) {
    (void)tone;
    return ImVec4(0.090f, 0.087f, 0.092f, 1.0f);
}

static ImVec4 ui_panel_accent(UiPanelTone tone) {
    (void)tone;
    return ImVec4(0.78f, 0.42f, 0.32f, 1.0f);
}

static ImVec4 ui_with_alpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

static UiPanelTone ui_current_panel_tone() {
    return s_panel_tone_count > 0 ? s_panel_tone_stack[s_panel_tone_count - 1] : UI_PANEL_DEFAULT;
}

static void ui_lock_current_window_scroll_x() {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window)
        return;
    window->Scroll.x = 0.0f;
    window->ScrollMax.x = 0.0f;
    window->ScrollTarget.x = FLT_MAX;
    window->ScrollbarX = false;
    ImGui::SetScrollX(0.0f);
}

static void ui_apply_gray_tool_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(8.0f, 8.0f);
    s.FramePadding = ImVec2(7.0f, 4.0f);
    s.CellPadding = ImVec2(6.0f, 4.0f);
    s.ItemSpacing = ImVec2(7.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 8.0f;
    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.875f, 0.865f, 0.858f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.51f, 0.49f, 0.49f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.060f, 0.057f, 0.060f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.090f, 0.087f, 0.092f, 1.00f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.100f, 0.096f, 0.100f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.220f, 0.205f, 0.200f, 1.00f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.112f, 0.108f, 0.112f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.140f, 0.128f, 0.128f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.176f, 0.124f, 0.114f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.074f, 0.071f, 0.074f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.112f, 0.108f, 0.112f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.064f, 0.061f, 0.064f, 1.00f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.086f, 0.082f, 0.085f, 1.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.064f, 0.061f, 0.064f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.285f, 0.260f, 0.252f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.370f, 0.338f, 0.326f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.455f, 0.410f, 0.392f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.78f, 0.42f, 0.32f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.52f, 0.36f, 0.32f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.72f, 0.42f, 0.32f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.128f, 0.122f, 0.124f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.165f, 0.142f, 0.138f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.205f, 0.138f, 0.124f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.142f, 0.126f, 0.124f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.184f, 0.138f, 0.128f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.230f, 0.150f, 0.132f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.215f, 0.200f, 0.198f, 1.00f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.360f, 0.330f, 0.320f, 1.00f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.490f, 0.440f, 0.420f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.245f, 0.225f, 0.220f, 0.70f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.395f, 0.358f, 0.345f, 0.90f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.520f, 0.470f, 0.448f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.112f, 0.108f, 0.112f, 1.00f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.185f, 0.142f, 0.134f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.150f, 0.120f, 0.116f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.096f, 0.092f, 0.096f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.140f, 0.132f, 0.132f, 1.00f);
    c[ImGuiCol_DockingPreview]        = ImVec4(0.700f, 0.540f, 0.460f, 0.28f);
    c[ImGuiCol_TableHeaderBg]         = ImVec4(0.134f, 0.126f, 0.128f, 1.00f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(0.230f, 0.212f, 0.206f, 1.00f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(0.175f, 0.164f, 0.162f, 1.00f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.158f, 0.146f, 0.144f, 0.28f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.390f, 0.305f, 0.285f, 0.48f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.700f, 0.500f, 0.420f, 0.42f);
}

enum UiIconKind {
    UI_ICON_NONE = 0,
    UI_ICON_PLAY,
    UI_ICON_PAUSE,
    UI_ICON_HELP,
    UI_ICON_RESTART,
    UI_ICON_GIZMO_MOVE,
    UI_ICON_GIZMO_ROTATE,
    UI_ICON_GIZMO_SCALE,
    UI_ICON_WIREFRAME,
    UI_ICON_GRID,
    UI_ICON_FULLSCREEN,
    UI_ICON_FULLSCREEN_EXIT,
    UI_ICON_MAXIMIZE_SQUARE,
    UI_ICON_MINIMIZE,
    UI_ICON_CLOSE,
    UI_ICON_TIMELINE,
    UI_ICON_COMPILE,
    UI_ICON_EXPORT_EXE
};

static const char* ui_svg_icon_path(UiIconKind icon) {
    switch (icon) {
    case UI_ICON_PLAY:             return "assets/icons/lucide-play.svg";
    case UI_ICON_PAUSE:            return "assets/icons/lucide-pause.svg";
    case UI_ICON_HELP:             return "assets/icons/lucide-badge-question-mark.svg";
    case UI_ICON_RESTART:          return "assets/icons/lucide-rotate-ccw.svg";
    case UI_ICON_GIZMO_MOVE:       return "assets/icons/lucide-move-3d.svg";
    case UI_ICON_GIZMO_ROTATE:     return "assets/icons/lucide-rotate-3d.svg";
    case UI_ICON_GIZMO_SCALE:      return "assets/icons/lucide-scale-3d.svg";
    case UI_ICON_WIREFRAME:        return "assets/icons/lucide-box.svg";
    case UI_ICON_GRID:             return "assets/icons/lucide-grid-3x3.svg";
    case UI_ICON_FULLSCREEN:       return "assets/icons/lucide-maximize.svg";
    case UI_ICON_FULLSCREEN_EXIT:  return "assets/icons/lucide-minimize.svg";
    case UI_ICON_MAXIMIZE_SQUARE:  return "assets/icons/lucide-square.svg";
    case UI_ICON_MINIMIZE:         return "assets/icons/lucide-minus.svg";
    case UI_ICON_CLOSE:            return "assets/icons/lucide-x.svg";
    case UI_ICON_TIMELINE:         return "assets/icons/lucide-chart-no-axes-gantt.svg";
    case UI_ICON_COMPILE:          return "assets/icons/lucide-hammer.svg";
    case UI_ICON_EXPORT_EXE:       return "assets/icons/lucide-file-down.svg";
    default:                       return nullptr;
    }
}

static NSVGimage* ui_svg_icon(UiIconKind icon) {
    struct CacheEntry {
        UiIconKind icon;
        NSVGimage* image;
        bool tried;
    };
    static CacheEntry cache[] = {
        { UI_ICON_PLAY, nullptr, false },
        { UI_ICON_PAUSE, nullptr, false },
        { UI_ICON_HELP, nullptr, false },
        { UI_ICON_RESTART, nullptr, false },
        { UI_ICON_GIZMO_MOVE, nullptr, false },
        { UI_ICON_GIZMO_ROTATE, nullptr, false },
        { UI_ICON_GIZMO_SCALE, nullptr, false },
        { UI_ICON_WIREFRAME, nullptr, false },
        { UI_ICON_GRID, nullptr, false },
        { UI_ICON_FULLSCREEN, nullptr, false },
        { UI_ICON_FULLSCREEN_EXIT, nullptr, false },
        { UI_ICON_MAXIMIZE_SQUARE, nullptr, false },
        { UI_ICON_MINIMIZE, nullptr, false },
        { UI_ICON_CLOSE, nullptr, false },
        { UI_ICON_TIMELINE, nullptr, false },
        { UI_ICON_COMPILE, nullptr, false },
        { UI_ICON_EXPORT_EXE, nullptr, false },
    };

    for (int i = 0; i < (int)(sizeof(cache) / sizeof(cache[0])); i++) {
        CacheEntry& e = cache[i];
        if (e.icon != icon) continue;
        if (!e.tried) {
            e.tried = true;
            const char* path = ui_svg_icon_path(icon);
            if (path)
                e.image = nsvgParseFromFile(path, "px", 96.0f);
            if (!e.image)
                log_warn("Icon SVG load failed: %s", path ? path : "(none)");
        }
        return e.image;
    }
    return nullptr;
}

static ImVec2 ui_svg_point(const float* p, ImVec2 origin, float scale) {
    return ImVec2(origin.x + p[0] * scale, origin.y + p[1] * scale);
}

static void ui_draw_icon_shape(UiIconKind icon, ImVec2 min, ImVec2 max, ImU32 col) {
    NSVGimage* image = ui_svg_icon(icon);
    if (!image || image->width <= 0.0f || image->height <= 0.0f)
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = max.x - min.x;
    float h = max.y - min.y;
    float box = w < h ? w : h;
    float icon_box = box * 0.82f;
    float scale = icon_box / (image->width > image->height ? image->width : image->height);
    ImVec2 origin = ImVec2(
        min.x + (w - image->width * scale) * 0.5f,
        min.y + (h - image->height * scale) * 0.5f);

    for (NSVGshape* shape = image->shapes; shape; shape = shape->next) {
        float thickness = shape->strokeWidth > 0.0f ? shape->strokeWidth * scale : 1.5f;
        if (thickness < 1.0f) thickness = 1.0f;
        for (NSVGpath* path = shape->paths; path; path = path->next) {
            if (!path->pts || path->npts < 1) continue;
            dl->PathClear();
            float* start = &path->pts[0];
            dl->PathLineTo(ui_svg_point(start, origin, scale));
            for (int i = 0; i < path->npts - 1; i += 3) {
                float* p = &path->pts[i * 2];
                dl->PathBezierCubicCurveTo(
                    ui_svg_point(p + 2, origin, scale),
                    ui_svg_point(p + 4, origin, scale),
                    ui_svg_point(p + 6, origin, scale),
                    0);
            }
            dl->PathStroke(col, path->closed ? ImDrawFlags_Closed : 0, thickness);
        }
    }
}

static bool ui_icon_button(const char* id, UiIconKind icon, ImVec2 size, const char* tooltip = nullptr) {
    if (size.y <= 0.0f)
        size.y = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    bool clicked = ImGui::Button("##icon", size);
    ImU32 col = ImGui::GetColorU32(ImGui::IsItemActive() ? ImVec4(0.98f, 0.98f, 1.0f, 1.0f) :
        (ImGui::IsItemHovered() ? ImVec4(0.92f, 0.93f, 0.95f, 1.0f) : ImVec4(0.70f, 0.72f, 0.75f, 1.0f)));
    ui_draw_icon_shape(icon, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), col);
    if (tooltip && tooltip[0] && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

static bool ui_icon_button_pressed(const char* id, UiIconKind icon, ImVec2 size, const char* tooltip = nullptr) {
    if (size.y <= 0.0f)
        size.y = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    ImGui::Button("##icon", size);
    bool pressed = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImU32 col = ImGui::GetColorU32(ImGui::IsItemActive() ? ImVec4(0.98f, 0.98f, 1.0f, 1.0f) :
        (ImGui::IsItemHovered() ? ImVec4(0.92f, 0.93f, 0.95f, 1.0f) : ImVec4(0.70f, 0.72f, 0.75f, 1.0f)));
    ui_draw_icon_shape(icon, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), col);
    if (tooltip && tooltip[0] && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

static void ui_store_window_control_rect(int index) {
    if (index < 0 || index >= 3)
        return;
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    s_ui_window_control_screen_rects[index].left = (LONG)floorf(min.x);
    s_ui_window_control_screen_rects[index].top = (LONG)floorf(min.y);
    s_ui_window_control_screen_rects[index].right = (LONG)ceilf(max.x);
    s_ui_window_control_screen_rects[index].bottom = (LONG)ceilf(max.y);
    s_ui_window_control_screen_rects_valid[index] = true;
}

static UiIconKind ui_icon_for_action(const char* action, const char** tooltip) {
    if (tooltip) *tooltip = nullptr;
    if (!action) return UI_ICON_NONE;
    if (strncmp(action, "GizmoMove", 9) == 0) {
        if (tooltip) *tooltip = "Move gizmo (1)";
        return UI_ICON_GIZMO_MOVE;
    }
    if (strncmp(action, "GizmoRotate", 11) == 0) {
        if (tooltip) *tooltip = "Rotate gizmo (2)";
        return UI_ICON_GIZMO_ROTATE;
    }
    if (strncmp(action, "GizmoScale", 10) == 0) {
        if (tooltip) *tooltip = "Scale gizmo (3)";
        return UI_ICON_GIZMO_SCALE;
    }
    if (strncmp(action, "Wireframe", 9) == 0) {
        if (tooltip) *tooltip = g_dx.scene_wireframe ? "Disable wireframe" : "Enable wireframe";
        return UI_ICON_WIREFRAME;
    }
    if (strncmp(action, "Grid", 4) == 0) {
        if (tooltip) *tooltip = g_dx.scene_grid_enabled ? "Hide grid" : "Show grid";
        return UI_ICON_GRID;
    }
    if (strncmp(action, "Pause", 5) == 0) {
        if (tooltip) *tooltip = "Pause scene";
        return UI_ICON_PAUSE;
    }
    if (strncmp(action, "Resume", 6) == 0) {
        if (tooltip) *tooltip = "Resume scene";
        return UI_ICON_PLAY;
    }
    if (strncmp(action, "Restart", 7) == 0) {
        if (tooltip) *tooltip = "Restart scene";
        return UI_ICON_RESTART;
    }
    if (strncmp(action, "Fullscreen", 10) == 0) {
        if (tooltip) *tooltip = "Fullscreen viewport";
        return UI_ICON_FULLSCREEN;
    }
    if (strncmp(action, "Exit fullscreen", 15) == 0) {
        if (tooltip) *tooltip = "Exit fullscreen";
        return UI_ICON_FULLSCREEN_EXIT;
    }
    return UI_ICON_NONE;
}

static UiViewportGizmoMode ui_gizmo_mode_for_action(const char* action) {
    if (!action) return UI_GIZMO_NONE;
    if (strncmp(action, "GizmoMove", 9) == 0) return UI_GIZMO_TRANSLATE;
    if (strncmp(action, "GizmoRotate", 11) == 0) return UI_GIZMO_ROTATE;
    if (strncmp(action, "GizmoScale", 10) == 0) return UI_GIZMO_SCALE;
    return UI_GIZMO_NONE;
}

static bool ui_header_action_is_separator(const char* action) {
    return action && strncmp(action, "Separator", 9) == 0;
}

static float ui_header_action_width(const char* action, float button_size) {
    if (!action || !action[0])
        return 0.0f;
    if (ui_header_action_is_separator(action))
        return ui_margin_px(10.0f);
    return button_size;
}

static bool ui_header_action_button(const char* action) {
    if (ui_header_action_is_separator(action))
        return false;

    const char* tooltip = nullptr;
    UiIconKind icon = ui_icon_for_action(action, &tooltip);
    if (icon != UI_ICON_NONE) {
        bool warm = icon == UI_ICON_PLAY;
        bool danger = icon == UI_ICON_PAUSE;
        UiViewportGizmoMode gizmo_mode = ui_gizmo_mode_for_action(action);
        bool gizmo_active = gizmo_mode != UI_GIZMO_NONE && s_viewport_gizmo_mode == gizmo_mode;
        bool wireframe_active = strncmp(action, "Wireframe", 9) == 0 && g_dx.scene_wireframe;
        bool grid_active = strncmp(action, "Grid", 4) == 0 && g_dx.scene_grid_enabled;
        if (warm) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.34f, 0.20f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.44f, 0.26f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.31f, 0.15f, 1.0f));
        } else if (danger) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.11f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.14f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.48f, 0.18f, 0.18f, 1.0f));
        } else if (gizmo_active || wireframe_active || grid_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.33f, 0.18f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.24f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.30f, 0.14f, 1.0f));
        }
        float icon_size = ui_px(22.0f);
        bool clicked = ui_icon_button(action, icon, ImVec2(icon_size, icon_size), tooltip);
        if (warm || danger || gizmo_active || wireframe_active || grid_active)
            ImGui::PopStyleColor(3);
        if (clicked && gizmo_mode != UI_GIZMO_NONE) {
            ui_set_viewport_gizmo_mode(gizmo_mode);
            return true;
        }
        if (clicked && strncmp(action, "Wireframe", 9) == 0) {
            g_dx.scene_wireframe = !g_dx.scene_wireframe;
            app_request_scene_render();
            return true;
        }
        if (clicked && strncmp(action, "Grid", 4) == 0) {
            g_dx.scene_grid_enabled = !g_dx.scene_grid_enabled;
            app_settings_save();
            app_request_scene_render();
            return true;
        }
        return clicked;
    }
    return ImGui::SmallButton(action);
}

static void ui_fit_text_ellipsis(const char* text, float max_w, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = '\0';
    if (!text || !text[0] || max_w <= 0.0f)
        return;

    snprintf(out, out_sz, "%s", text);
    if (ImGui::CalcTextSize(out).x <= max_w)
        return;

    const char* ellipsis = "...";
    float ellipsis_w = ImGui::CalcTextSize(ellipsis).x;
    if (max_w <= ellipsis_w) {
        snprintf(out, out_sz, "%s", ellipsis);
        return;
    }

    int len = (int)strlen(out);
    while (len > 0) {
        out[--len] = '\0';
        char tmp[MAX_NAME * 2 + 32] = {};
        snprintf(tmp, sizeof(tmp), "%s%s", out, ellipsis);
        if (ImGui::CalcTextSize(tmp).x <= max_w) {
            snprintf(out, out_sz, "%s", tmp);
            return;
        }
    }
    snprintf(out, out_sz, "%s", ellipsis);
}

static void ui_panel_header(const char* title, const char* detail = nullptr,
                            const char* action_a = nullptr, bool* clicked_a = nullptr,
                            const char* action_b = nullptr, bool* clicked_b = nullptr,
                            const char* action_c = nullptr, bool* clicked_c = nullptr,
                            const char* action_d = nullptr, bool* clicked_d = nullptr,
                            const char* action_e = nullptr, bool* clicked_e = nullptr,
                            const char* action_f = nullptr, bool* clicked_f = nullptr,
                            const char* action_g = nullptr, bool* clicked_g = nullptr,
                            const char* action_h = nullptr, bool* clicked_h = nullptr) {
    const char* actions[] = { action_a, action_b, action_c, action_d, action_e, action_f, action_g, action_h };
    bool* clicks[] = { clicked_a, clicked_b, clicked_c, clicked_d, clicked_e, clicked_f, clicked_g, clicked_h };
    const int action_count = (int)(sizeof(actions) / sizeof(actions[0]));
    for (int i = 0; i < action_count; i++) {
        if (clicks[i])
            *clicks[i] = false;
    }

    UiPanelTone tone = ui_current_panel_tone();
    ImVec4 accent = ui_panel_accent(tone);
    ImVec2 header_pos = ImGui::GetCursorScreenPos();
    float header_h = ImGui::GetTextLineHeight() + ui_margin_px(10.0f);
    ImDrawList* header_dl = ImGui::GetWindowDrawList();
    float header_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    header_dl->AddRectFilled(ImVec2(header_pos.x - ui_margin_px(3.0f), header_pos.y - ui_margin_px(2.0f)),
        ImVec2(header_right, header_pos.y + header_h + ui_margin_px(2.0f)),
        ImGui::GetColorU32(ui_with_alpha(accent, 0.040f)), 3.0f);
    header_dl->AddRectFilled(ImVec2(header_pos.x - ui_margin_px(3.0f), header_pos.y - ui_margin_px(2.0f)),
        ImVec2(header_pos.x, header_pos.y + header_h + ui_margin_px(2.0f)),
        ImGui::GetColorU32(ui_with_alpha(accent, 0.62f)), 1.5f);

    float button_size = ui_px(22.0f);
    float button_spacing = ui_margin_px(4.0f);
    float buttons_w = 0.0f;
    bool has_action = false;
    for (int i = 0; i < action_count; i++) {
        if (actions[i] && actions[i][0])
        {
            if (has_action)
                buttons_w += button_spacing;
            buttons_w += ui_header_action_width(actions[i], button_size);
            has_action = true;
        }
    }
    float buttons_x = header_right - buttons_w;
    float buttons_y = header_pos.y + floorf((header_h - button_size) * 0.5f);

    float text_left = header_pos.x + ui_margin_px(8.0f);
    float text_right = buttons_w > 0.0f ? (buttons_x - ui_margin_px(8.0f)) : (header_right - ui_margin_px(8.0f));
    if (text_right < text_left)
        text_right = text_left;

    // The header is drawn manually so every panel gets the same vertical rhythm:
    // title on the left, metadata on the right, compact actions aligned to the
    // same center line. This keeps viewport, inspector and popup headers visually
    // consistent without special-case layout code for each panel.
    float text_y = header_pos.y + floorf((header_h - ImGui::GetTextLineHeight()) * 0.5f);
    header_dl->AddText(ImVec2(text_left, text_y),
        ImGui::GetColorU32(ui_with_alpha(accent, 0.95f)), title);

    float title_w = ImGui::CalcTextSize(title).x;
    if (detail && detail[0]) {
        float detail_min_x = text_left + title_w + ui_margin_px(12.0f);
        float detail_max_w = text_right - detail_min_x;
        if (detail_max_w > 4.0f) {
            char fitted[MAX_NAME * 2 + 32] = {};
            ui_fit_text_ellipsis(detail, detail_max_w, fitted, sizeof(fitted));
            float detail_w = ImGui::CalcTextSize(fitted).x;
            float detail_x = text_right - detail_w;
            if (detail_x < detail_min_x)
                detail_x = detail_min_x;
            header_dl->AddText(ImVec2(detail_x, text_y),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), fitted);
        }
    }

    for (int i = 0; i < action_count; i++) {
        if (!actions[i] || !actions[i][0])
            continue;
        float action_w = ui_header_action_width(actions[i], button_size);
        if (ui_header_action_is_separator(actions[i])) {
            float line_x = buttons_x + floorf(action_w * 0.5f);
            float line_y0 = header_pos.y + ui_margin_px(5.0f);
            float line_y1 = header_pos.y + header_h - ui_margin_px(5.0f);
            header_dl->AddLine(
                ImVec2(line_x, line_y0),
                ImVec2(line_x, line_y1),
                ImGui::GetColorU32(ImVec4(0.28f, 0.25f, 0.24f, 0.85f)), 1.0f);
        } else {
            ImGui::SetCursorScreenPos(ImVec2(buttons_x, buttons_y));
            if (ui_header_action_button(actions[i]) && clicks[i])
                *clicks[i] = true;
        }
        buttons_x += action_w + button_spacing;
    }

    ImGui::SetCursorScreenPos(ImVec2(header_pos.x, header_pos.y));
    ImGui::Dummy(ImVec2(0.0f, header_h));
    ImGui::Separator();
}

static bool ui_begin_tool_panel(const char* id, const char* title, const char* detail, ImVec2 size,
                                UiPanelTone tone = UI_PANEL_DEFAULT,
                                const char* action_a = nullptr, bool* clicked_a = nullptr,
                                const char* action_b = nullptr, bool* clicked_b = nullptr,
                                const char* action_c = nullptr, bool* clicked_c = nullptr,
                                const char* action_d = nullptr, bool* clicked_d = nullptr,
                                const char* action_e = nullptr, bool* clicked_e = nullptr,
                                const char* action_f = nullptr, bool* clicked_f = nullptr,
                                const char* action_g = nullptr, bool* clicked_g = nullptr,
                                const char* action_h = nullptr, bool* clicked_h = nullptr) {
    if (s_panel_tone_count < (int)(sizeof(s_panel_tone_stack) / sizeof(s_panel_tone_stack[0])))
        s_panel_tone_stack[s_panel_tone_count++] = tone;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_panel_bg(tone));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.205f, 0.200f, 1.0f));
    float child_w = size.x > 0.0f ? size.x : ImGui::GetContentRegionAvail().x;
    float content_w = child_w - ui_margin_px(16.0f);
    if (content_w < ui_px(1.0f))
        content_w = ui_px(1.0f);
    ImGui::SetNextWindowContentSize(ImVec2(content_w, 0.0f));
    bool open = ImGui::BeginChild(id, size, true);
    ui_lock_current_window_scroll_x();
    ui_panel_header(title, detail,
                    action_a, clicked_a, action_b, clicked_b, action_c, clicked_c,
                    action_d, clicked_d, action_e, clicked_e, action_f, clicked_f,
                    action_g, clicked_g, action_h, clicked_h);
    return open;
}

static void ui_end_tool_panel() {
    ui_lock_current_window_scroll_x();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (s_panel_tone_count > 0)
        s_panel_tone_count--;
}

static bool ui_header_only_panel(const char* id, const char* title, const char* detail,
                                 ImVec2 size, UiPanelTone tone = UI_PANEL_DEFAULT) {
    bool clicked = false;
    if (s_panel_tone_count < (int)(sizeof(s_panel_tone_stack) / sizeof(s_panel_tone_stack[0])))
        s_panel_tone_stack[s_panel_tone_count++] = tone;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_panel_bg(tone));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.205f, 0.200f, 1.0f));
    if (ImGui::BeginChild(id, size, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ui_lock_current_window_scroll_x();
        ui_panel_header(title, detail);
        clicked = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (s_panel_tone_count > 0)
        s_panel_tone_count--;
    return clicked;
}

static int ui_active_command_count() {
    int n = 0;
    for (int i = 0; i < MAX_COMMANDS; i++)
        if (g_commands[i].active) n++;
    return n;
}

static const char* ui_inspector_header_detail() {
    static char detail[MAX_NAME * 2 + 32];
    detail[0] = '\0';

    if (g_sel_res != INVALID_HANDLE) {
        Resource* r = res_get(g_sel_res);
        if (r)
            snprintf(detail, sizeof(detail), "%s  %s",
                ui_resource_display_name(*r), ui_resource_display_type(*r));
    } else if (g_sel_cmd != INVALID_HANDLE) {
        Command* c = cmd_get(g_sel_cmd);
        if (c)
            snprintf(detail, sizeof(detail), "%s  %s", c->name, cmd_type_str(c->type));
    }

    return detail[0] ? detail : nullptr;
}

static void ui_format_bytes(uint64_t bytes, char* out, int out_sz) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = (double)bytes;
    int unit = 0;
    while (v >= 1024.0 && unit < 3) {
        v /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(out, out_sz, "%llu %s", (unsigned long long)bytes, units[unit]);
    else
        snprintf(out, out_sz, "%.1f %s", v, units[unit]);
}

static uint64_t ui_process_memory_bytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        return (uint64_t)pmc.PrivateUsage;
    return 0;
}

static uint64_t ui_estimated_gpu_memory_bytes() {
    uint64_t total = res_estimate_gpu_total(true);
    total += (uint64_t)(g_dx.width > 0 ? g_dx.width : 0) *
             (uint64_t)(g_dx.height > 0 ? g_dx.height : 0) * 4ull; // swapchain backbuffer
    total += (uint64_t)((sizeof(SceneCBData) + 15) & ~15);
    total += (uint64_t)((sizeof(ObjectCBData) + 15) & ~15);
    total += (uint64_t)((sizeof(UserCBData) + 15) & ~15);
    total += (uint64_t)((sizeof(UserCBData) + 15) & ~15);
    return total;
}

static void ui_viewport_detail(char* out, int out_sz) {
    snprintf(out, out_sz, "frame %llu  %.2fs  %dx%d",
        (unsigned long long)app_scene_frame(), app_scene_time(),
        g_dx.scene_width, g_dx.scene_height);
}

static void ui_reset_camera_view() {
    project_reset_camera_defaults();
}

static void ui_draw_window_controls(float host_h) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(4.0f)));
    ImGui::SetCursorPosY(floorf((host_h - ImGui::GetFrameHeight()) * 0.5f));
    if (ui_icon_button_pressed("##winmin", UI_ICON_MINIMIZE, ImVec2(ui_px(26.0f), 0.0f), "Minimize")) {
        ShowWindowAsync(g_dx.hwnd, SW_MINIMIZE);
    }
    ui_store_window_control_rect(0);
    ImGui::SameLine(0.0f, ui_margin_px(3.0f));
    if (ui_icon_button_pressed("##winmax", UI_ICON_MAXIMIZE_SQUARE, ImVec2(ui_px(26.0f), 0.0f), "Maximize")) {
        bool zoomed = IsZoomed(g_dx.hwnd) != FALSE;
        ShowWindowAsync(g_dx.hwnd, zoomed ? SW_RESTORE : SW_MAXIMIZE);
    }
    ui_store_window_control_rect(1);
    ImGui::SameLine(0.0f, ui_margin_px(3.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.16f, 0.16f, 1.0f));
    if (ui_icon_button_pressed("##winclose", UI_ICON_CLOSE, ImVec2(ui_px(26.0f), 0.0f), "Close")) {
        DestroyWindow(g_dx.hwnd);
    }
    ui_store_window_control_rect(2);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

static float ui_window_controls_width() {
    return ui_px(26.0f) * 3.0f + ui_margin_px(3.0f) * 2.0f;
}

int ui_top_toolbar_height_px() {
    return (int)ui_px(40.0f);
}

bool ui_hit_test_client_area_screen(int x, int y) {
    if (s_ui_top_toolbar_screen_rect_valid) {
        if (x >= s_ui_top_toolbar_screen_rect.left && x < s_ui_top_toolbar_screen_rect.right &&
            y >= s_ui_top_toolbar_screen_rect.top && y < s_ui_top_toolbar_screen_rect.bottom)
            return true;
    }
    return false;
}

UiWindowControlHit ui_hit_test_window_control_screen(int x, int y) {
    if (!s_ui_top_toolbar_screen_rect_valid)
        return UI_WINDOW_CONTROL_NONE;

    if (y < s_ui_top_toolbar_screen_rect.top || y >= s_ui_top_toolbar_screen_rect.bottom)
        return UI_WINDOW_CONTROL_NONE;

    float pad_right = ui_margin_px(8.0f);
    float gap = ui_margin_px(3.0f);
    float button_w = ui_px(26.0f);
    float right = (float)s_ui_top_toolbar_screen_rect.right - pad_right;

    RECT close_rc = {};
    close_rc.left = (LONG)floorf(right - button_w);
    close_rc.top = s_ui_top_toolbar_screen_rect.top;
    close_rc.right = (LONG)ceilf(right);
    close_rc.bottom = s_ui_top_toolbar_screen_rect.bottom;

    right = (float)close_rc.left - gap;
    RECT max_rc = {};
    max_rc.left = (LONG)floorf(right - button_w);
    max_rc.top = s_ui_top_toolbar_screen_rect.top;
    max_rc.right = (LONG)ceilf(right);
    max_rc.bottom = s_ui_top_toolbar_screen_rect.bottom;

    right = (float)max_rc.left - gap;
    RECT min_rc = {};
    min_rc.left = (LONG)floorf(right - button_w);
    min_rc.top = s_ui_top_toolbar_screen_rect.top;
    min_rc.right = (LONG)ceilf(right);
    min_rc.bottom = s_ui_top_toolbar_screen_rect.bottom;

    if (x >= min_rc.left && x < min_rc.right)
        return UI_WINDOW_CONTROL_MINIMIZE;
    if (x >= max_rc.left && x < max_rc.right)
        return UI_WINDOW_CONTROL_MAXIMIZE;
    if (x >= close_rc.left && x < close_rc.right)
        return UI_WINDOW_CONTROL_CLOSE;
    return UI_WINDOW_CONTROL_NONE;
}

UiWindowControlHit ui_hit_test_window_control_client(int x, int y, int client_w) {
    if (y < 0 || y >= ui_top_toolbar_height_px())
        return UI_WINDOW_CONTROL_NONE;
    if (client_w <= 0)
        return UI_WINDOW_CONTROL_NONE;

    float pad_right = ui_margin_px(8.0f);
    float gap = ui_margin_px(3.0f);
    float button_w = ui_px(26.0f);
    float right = (float)client_w - pad_right;

    int close_left = (int)floorf(right - button_w);
    int close_right = (int)ceilf(right);
    right = (float)close_left - gap;

    int max_left = (int)floorf(right - button_w);
    int max_right = (int)ceilf(right);
    right = (float)max_left - gap;

    int min_left = (int)floorf(right - button_w);
    int min_right = (int)ceilf(right);

    if (x >= min_left && x < min_right)
        return UI_WINDOW_CONTROL_MINIMIZE;
    if (x >= max_left && x < max_right)
        return UI_WINDOW_CONTROL_MAXIMIZE;
    if (x >= close_left && x < close_right)
        return UI_WINDOW_CONTROL_CLOSE;
    return UI_WINDOW_CONTROL_NONE;
}

static void ui_draw_shortcuts_popup() {
    if (!s_shortcuts_popup_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(492.0f, 430.0f), ImGuiCond_Appearing);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_panel_bg(UI_PANEL_GENERAL));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.205f, 0.200f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    if (ImGui::Begin("Shortcuts", &s_shortcuts_popup_open,
        ImGuiWindowFlags_NoCollapse))
    {
        ui_panel_header("SHORTCUTS", "F1");
        ImGui::TextDisabled("Viewport, runtime and editor controls in one place.");
        ImGui::Spacing();

        ImGuiTableFlags table_flags =
            ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_PadOuterX;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 5.0f));

        if (ui_begin_shortcut_section("##shortcuts_execution", "EXECUTION", table_flags)) {
            ui_draw_shortcut_row("Space", "Pause / resume scene execution");
            ui_draw_shortcut_row("F6", "Restart scene from frame 0");
            ui_draw_shortcut_row("F11", "Toggle viewport fullscreen");
            ImGui::EndTable();
        }

        if (ui_begin_shortcut_section("##shortcuts_project", "PROJECT", table_flags)) {
            ui_draw_shortcut_row("F5", "Compile all shaders");
            ui_draw_shortcut_row("Ctrl+D", "Compile selected shader");
            ui_draw_shortcut_row("Ctrl+S", "Save shader source or project");
            ui_draw_shortcut_row("F1", "Toggle this shortcuts panel");
            ImGui::EndTable();
        }

        if (ui_begin_shortcut_section("##shortcuts_timeline", "TIMELINE", table_flags)) {
            ui_draw_shortcut_row("Click", "Select slot and move current frame");
            ui_draw_shortcut_row("Arrows", "Move selected slot / current frame");
            ui_draw_shortcut_row("Shift+Arrows", "Move selected slot by 10 frames");
            ui_draw_shortcut_row("I", "Insert or update key on selected slot");
            ui_draw_shortcut_row("Delete", "Delete key on selected slot");
            ui_draw_shortcut_row("Ctrl+C", "Copy selected key");
            ui_draw_shortcut_row("Ctrl+X", "Cut selected key");
            ui_draw_shortcut_row("Ctrl+V", "Paste key into compatible selected slot");
            ImGui::EndTable();
        }

        if (ui_begin_shortcut_section("##shortcuts_selection", "SELECTION", table_flags)) {
            ui_draw_shortcut_row("Arrows", "Move in Resources / Commands");
            ui_draw_shortcut_row("Enter", "Select focused item");
            ui_draw_shortcut_row("F2", "Rename selected resource / command");
            ui_draw_shortcut_row("Delete", "Remove selected item");
            ui_draw_shortcut_row("X", "Toggle selected command enabled");
            ui_draw_shortcut_row("Ctrl+C", "Copy the selected command subtree");
            ui_draw_shortcut_row("Ctrl+V", "Paste commands after the selection or inside a selected container");
            ui_draw_shortcut_row("1 / 2 / 3", "Toggle Move / Rotate / Scale gizmo while hovering the viewport");
            ui_draw_shortcut_row("Esc", "Disable the active viewport gizmo");
            ImGui::EndTable();
        }

        if (ui_begin_shortcut_section("##shortcuts_camera", "CAMERA", table_flags)) {
            ui_draw_shortcut_row("RMB", "Mouse look");
            ui_draw_shortcut_row("WASD", "Move horizontally");
            ui_draw_shortcut_row("Q / E", "Move down / up");
            ui_draw_shortcut_row("Shift", "Faster movement");
            ui_draw_shortcut_row("Ctrl", "Slower movement");
            ui_draw_shortcut_row("L", "Orbit directional light");
            ImGui::EndTable();
        }

        ui_inspector_section("DRAW TEXTURE SLOTS");
        ImGui::TextDisabled("Common pixel shader t# convention used by mesh materials and PBR shaders.");
        ui_draw_texture_slot_reference("##shortcuts_draw_slots");
        ImGui::TextDisabled("Manual Texture Bindings in a draw command override mesh material textures on the same slot.");

        ImGui::PopStyleVar();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

static bool ui_begin_shortcut_section(const char* id, const char* title, ImGuiTableFlags table_flags) {
    ui_inspector_section(title);
    if (!ImGui::BeginTable(id, 2, table_flags))
        return false;
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 108.0f);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

static void ui_draw_shortcut_row(const char* key, const char* desc) {
    char badge_id[64] = {};
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    snprintf(badge_id, sizeof(badge_id), "##shortcut_%s", key);
    ui_inline_badge(badge_id, key, ImVec4(0.74f, 0.53f, 0.42f, 1.0f));
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(desc);
}

static void ui_align_frame_row(float row_y) {
    ImGui::SetCursorPosY(row_y);
}

static void ui_align_text_row(float row_y) {
    ImGui::SetCursorPosY(row_y);
    ImGui::AlignTextToFramePadding();
}

struct TimelineSlotSelection {
    bool valid;
    int  track_index;
    int  frame;
};

struct TimelineSlotClipboard {
    bool              valid;
    TimelineTrackKind kind;
    ResType           value_type;
    TimelineKey       key;
};

static TimelineSlotSelection s_timeline_slot_selection = {};
static TimelineSlotClipboard s_timeline_slot_clipboard = {};

static bool ui_timeline_slot_selection_valid() {
    if (!s_timeline_slot_selection.valid)
        return false;
    int track_index = s_timeline_slot_selection.track_index;
    if (track_index < 0 || track_index >= g_timeline_track_count)
        return false;
    if (!g_timeline_tracks[track_index].active)
        return false;
    int frame = s_timeline_slot_selection.frame;
    return frame >= 0 && frame < timeline_length_frames();
}

static bool ui_timeline_slot_selected(int track_index, int frame) {
    return ui_timeline_slot_selection_valid() &&
           s_timeline_slot_selection.track_index == track_index &&
           s_timeline_slot_selection.frame == frame;
}

static void ui_timeline_select_slot(int track_index, int frame) {
    if (track_index < 0 || track_index >= g_timeline_track_count)
        return;
    s_timeline_slot_selection.valid = true;
    s_timeline_slot_selection.track_index = track_index;
    s_timeline_slot_selection.frame = frame;
}

static int ui_timeline_next_active_track(int start, int dir) {
    int i = start + dir;
    while (i >= 0 && i < g_timeline_track_count) {
        if (g_timeline_tracks[i].active)
            return i;
        i += dir;
    }
    return start;
}

static bool ui_timeline_clipboard_compatible(const TimelineTrack& track) {
    if (!s_timeline_slot_clipboard.valid || !track.active)
        return false;
    if (track.kind != s_timeline_slot_clipboard.kind)
        return false;
    if (track.kind == TIMELINE_TRACK_USER_VAR)
        return track.value_type == s_timeline_slot_clipboard.value_type;
    return true;
}

static void ui_timeline_copy_selected_slot() {
    if (!ui_timeline_slot_selection_valid())
        return;
    TimelineTrack& track = g_timeline_tracks[s_timeline_slot_selection.track_index];
    int key_index = timeline_find_key_index(track, s_timeline_slot_selection.frame);
    if (key_index < 0)
        return;
    s_timeline_slot_clipboard.valid = true;
    s_timeline_slot_clipboard.kind = track.kind;
    s_timeline_slot_clipboard.value_type = track.value_type;
    s_timeline_slot_clipboard.key = track.keys[key_index];
}

static void ui_timeline_paste_selected_slot() {
    if (!ui_timeline_slot_selection_valid())
        return;
    TimelineTrack& track = g_timeline_tracks[s_timeline_slot_selection.track_index];
    if (!ui_timeline_clipboard_compatible(track))
        return;
    TimelineKey* key = timeline_set_key(s_timeline_slot_selection.track_index, s_timeline_slot_selection.frame);
    if (!key)
        return;
    *key = s_timeline_slot_clipboard.key;
    key->frame = s_timeline_slot_selection.frame;
    app_request_scene_render();
}

static void ui_timeline_cut_selected_slot() {
    if (!ui_timeline_slot_selection_valid())
        return;
    TimelineTrack& track = g_timeline_tracks[s_timeline_slot_selection.track_index];
    if (timeline_find_key_index(track, s_timeline_slot_selection.frame) < 0)
        return;
    ui_timeline_copy_selected_slot();
    timeline_delete_key(s_timeline_slot_selection.track_index, s_timeline_slot_selection.frame);
}

static void ui_timeline_delete_selected_slot() {
    if (!ui_timeline_slot_selection_valid())
        return;
    timeline_delete_key(s_timeline_slot_selection.track_index, s_timeline_slot_selection.frame);
}

static void ui_timeline_insert_selected_slot() {
    if (!ui_timeline_slot_selection_valid())
        return;
    if (!timeline_capture_key(s_timeline_slot_selection.track_index, s_timeline_slot_selection.frame))
        log_warn("Timeline: could not capture key.");
}

static void ui_timeline_move_selection(int frame_delta, int track_delta) {
    if (!ui_timeline_slot_selection_valid()) {
        if (g_timeline_track_count > 0)
            ui_timeline_select_slot(0, timeline_current_frame());
        return;
    }
    int track_index = s_timeline_slot_selection.track_index;
    if (track_delta != 0)
        track_index = ui_timeline_next_active_track(track_index, track_delta);
    int frame = s_timeline_slot_selection.frame + frame_delta;
    if (frame < 0) frame = 0;
    if (frame >= timeline_length_frames()) frame = timeline_length_frames() - 1;
    ui_timeline_select_slot(track_index, frame);
    timeline_set_current_frame(frame);
    s_timeline_ensure_current_visible = true;
}

static void ui_timeline_add_track_button(const char* label, TimelineTrackKind kind,
                                         const char* target, ResType value_type) {
    if (ImGui::Button(label)) {
        int idx = timeline_add_track(kind, target ? target : "", value_type);
        if (idx < 0)
            log_warn("Timeline: could not add track.");
    }
}

static int ui_timeline_user_cb_index_for_source(ResHandle h) {
    if (h == INVALID_HANDLE)
        return -1;
    for (int i = 0; i < g_user_cb_count; i++) {
        if (g_user_cb_entries[i].source == h)
            return i;
    }
    return -1;
}

static void ui_timeline_add_parameter_track(ResHandle h) {
    Resource* r = res_get(h);
    if (!r || r->is_builtin || r->is_generated || !user_cb_type_supported(r->type))
        return;

    int entry_idx = ui_timeline_user_cb_index_for_source(h);
    if (entry_idx < 0) {
        if (user_cb_add_from_resource(h))
            entry_idx = g_user_cb_count - 1;
    }
    if (entry_idx < 0) {
        log_warn("Timeline: could not add parameter to User CB.");
        return;
    }

    UserCBEntry& e = g_user_cb_entries[entry_idx];
    int idx = timeline_add_track(TIMELINE_TRACK_USER_VAR, e.name, e.type);
    if (idx < 0)
        log_warn("Timeline: could not add parameter track.");
}

static void ui_timeline_add_tracks() {
    ui_inspector_section("ADD TRACK");

    ui_timeline_add_track_button("Camera", TIMELINE_TRACK_CAMERA, "camera", RES_NONE);
    ImGui::SameLine();
    ui_timeline_add_track_button("Dir Light", TIMELINE_TRACK_DIRLIGHT, "dirlight", RES_NONE);

    Command* selected_cmd = cmd_get(g_sel_cmd);
    if (selected_cmd) {
        bool has_transform = selected_cmd->type == CMD_DRAW_MESH ||
                             selected_cmd->type == CMD_DRAW_INSTANCED ||
                             selected_cmd->type == CMD_INDIRECT_DRAW;
        if (has_transform) {
            ImGui::SameLine();
            ui_timeline_add_track_button("Selected Transform", TIMELINE_TRACK_COMMAND_TRANSFORM,
                                         selected_cmd->name, RES_NONE);
        }
        ImGui::SameLine();
        ui_timeline_add_track_button("Selected Enable", TIMELINE_TRACK_COMMAND_ENABLED,
                                     selected_cmd->name, RES_NONE);
    }

    Resource* selected_res = res_get(g_sel_res);
    if (selected_res && !selected_res->is_builtin && !selected_res->is_generated &&
        user_cb_type_supported(selected_res->type)) {
        ImGui::SameLine();
        if (ImGui::Button("Add Parameter"))
            ui_timeline_add_parameter_track(g_sel_res);
    }
}

static void ui_timeline_track_label(const TimelineTrack& track, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    switch (track.kind) {
    case TIMELINE_TRACK_USER_VAR:
        snprintf(out, out_sz, "%s", track.target);
        break;
    case TIMELINE_TRACK_COMMAND_TRANSFORM:
        snprintf(out, out_sz, "Transform  %s", track.target);
        break;
    case TIMELINE_TRACK_COMMAND_ENABLED:
        snprintf(out, out_sz, "Enable  %s", track.target);
        break;
    case TIMELINE_TRACK_CAMERA:
        snprintf(out, out_sz, "Camera");
        break;
    case TIMELINE_TRACK_DIRLIGHT:
        snprintf(out, out_sz, "Dir Light");
        break;
    default:
        snprintf(out, out_sz, "Track");
        break;
    }
}

static bool ui_timeline_track_enable_checkbox(const char* id, bool* value, float size) {
    if (!value)
        return false;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    bool changed = false;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        *value = !*value;
        changed = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImRect r(p, ImVec2(p.x + size, p.y + size));
    bool hovered = ImGui::IsItemHovered();
    ImU32 bg = ImGui::GetColorU32(*value ? ImVec4(0.24f, 0.13f, 0.08f, 0.94f)
                                         : ImVec4(0.10f, 0.10f, 0.11f, 0.95f));
    ImU32 border = ImGui::GetColorU32(hovered ? ImVec4(0.95f, 0.47f, 0.18f, 0.95f)
                                              : ImVec4(0.55f, 0.28f, 0.17f, 0.82f));
    dl->AddRectFilled(r.Min, r.Max, bg, ui_px(3.0f));
    dl->AddRect(r.Min, r.Max, border, ui_px(3.0f), 0, ui_px(1.0f));

    if (*value) {
        ImVec2 a = ImVec2(r.Min.x + size * 0.24f, r.Min.y + size * 0.53f);
        ImVec2 b = ImVec2(r.Min.x + size * 0.43f, r.Min.y + size * 0.72f);
        ImVec2 c = ImVec2(r.Min.x + size * 0.78f, r.Min.y + size * 0.28f);
        ImU32 check_col = ImGui::GetColorU32(ImVec4(0.93f, 0.43f, 0.22f, 1.0f));
        dl->AddLine(a, b, check_col, ui_px(2.2f));
        dl->AddLine(b, c, check_col, ui_px(2.2f));
    }

    return changed;
}

static void ui_timeline_draw_slot(int track_index, int frame, ImVec2 slot_size) {
    TimelineTrack& track = g_timeline_tracks[track_index];
    bool has_key = timeline_find_key_index(track, frame) >= 0;
    bool selected = ui_timeline_slot_selected(track_index, frame);

    // Keep the interactive item constrained to the slot size. Using
    // TableGetCellBgRect() here makes the button inherit the current table-row
    // background height before the row has been finalized, which can inflate
    // the timeline rows dramatically when scrolling is enabled.
    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
    ImRect cell_rect(cursor_pos,
                     ImVec2(cursor_pos.x + slot_size.x, cursor_pos.y + slot_size.y));
    cell_rect.Min.x += ui_px(0.5f);
    cell_rect.Max.x -= ui_px(0.5f);
    cell_rect.Min.y += ui_px(0.5f);
    cell_rect.Max.y -= ui_px(0.5f);

    ImGui::SetCursorScreenPos(cell_rect.Min);
    ImGui::InvisibleButton("##slot", cell_rect.GetSize());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
        (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        ui_timeline_select_slot(track_index, frame);
        timeline_set_current_frame(frame);
        s_timeline_ensure_current_visible = true;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        ui_timeline_select_slot(track_index, frame);

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem(has_key ? "Update Key" : "Add Key")) {
            if (!timeline_capture_key(track_index, frame))
                log_warn("Timeline: could not capture key.");
        }
        if (has_key && ImGui::MenuItem("Copy Key", "Ctrl+C"))
            ui_timeline_copy_selected_slot();
        if (has_key && ImGui::MenuItem("Cut Key", "Ctrl+X"))
            ui_timeline_cut_selected_slot();
        bool can_paste = ui_timeline_clipboard_compatible(track);
        if (ImGui::MenuItem("Paste Key", "Ctrl+V", false, can_paste))
            ui_timeline_paste_selected_slot();
        if (has_key && ImGui::MenuItem("Delete Key"))
            timeline_delete_key(track_index, frame);
        ImGui::EndPopup();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = cell_rect.Min;
    ImVec2 q = cell_rect.Max;
    bool hovered = ImGui::IsItemHovered();
    if (hovered)
        dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(0.31f, 0.17f, 0.08f, 0.46f)),
            ui_px(2.0f));
    if (selected) {
        dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(0.56f, 0.25f, 0.07f, 0.70f)),
            ui_px(2.0f));
        dl->AddRect(p, q, ImGui::GetColorU32(ImVec4(0.95f, 0.48f, 0.13f, 1.0f)),
            ui_px(2.0f), 0, ui_px(1.8f));
    }
    if (hovered && !selected) {
        dl->AddRect(p, q, ImGui::GetColorU32(ImVec4(0.48f, 0.49f, 0.52f, 0.65f)),
            ui_px(2.0f), 0, ui_px(1.0f));
    }
    if (has_key) {
        ImVec2 c = ImVec2((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
        float r = ui_px(5.0f);
        ImVec2 pts[4] = {
            ImVec2(c.x, c.y - r),
            ImVec2(c.x + r, c.y),
            ImVec2(c.x, c.y + r),
            ImVec2(c.x - r, c.y)
        };
        dl->AddConvexPolyFilled(pts, 4, ImGui::GetColorU32(ImVec4(0.83f, 0.55f, 0.22f, 1.0f)));
        dl->AddPolyline(pts, 4, ImGui::GetColorU32(ImVec4(0.18f, 0.12f, 0.06f, 1.0f)),
            true, ui_px(1.0f));
    }
}

static bool ui_timeline_scrollbar(const char* id, int* first_frame, int max_first_frame,
                                  int visible_count, int total_frames, float left_offset) {
    if (!first_frame)
        return false;

    float width = ImGui::GetContentRegionAvail().x;
    if (width < 1.0f)
        width = 1.0f;

    const float bar_h = ui_px(9.0f);
    const float track_h = ui_px(4.0f);
    const float min_thumb_w = ui_px(22.0f);

    ImGui::InvisibleButton(id, ImVec2(width, bar_h));
    ImRect outer(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

    float track_left = outer.Min.x + left_offset;
    if (track_left > outer.Max.x - min_thumb_w)
        track_left = outer.Min.x;
    ImRect track(ImVec2(track_left, floorf((outer.Min.y + outer.Max.y - track_h) * 0.5f)),
                 ImVec2(outer.Max.x, floorf((outer.Min.y + outer.Max.y + track_h) * 0.5f)));

    float track_w = track.GetWidth();
    if (track_w < 1.0f)
        track_w = 1.0f;

    float thumb_w = track_w;
    if (total_frames > 0 && visible_count < total_frames) {
        thumb_w = track_w * ((float)visible_count / (float)total_frames);
        if (thumb_w < min_thumb_w) thumb_w = min_thumb_w;
        if (thumb_w > track_w) thumb_w = track_w;
    }

    float range_w = track_w - thumb_w;
    float t = (max_first_frame > 0) ? ((float)(*first_frame) / (float)max_first_frame) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    ImRect thumb(ImVec2(track.Min.x + range_w * t, track.Min.y),
                 ImVec2(track.Min.x + range_w * t + thumb_w, track.Max.y));

    ImGuiIO& io = ImGui::GetIO();
    static float s_drag_offset = 0.0f;
    if (ImGui::IsItemActivated()) {
        s_drag_offset = ImGui::IsMouseHoveringRect(thumb.Min, thumb.Max) ?
                        (io.MousePos.x - thumb.Min.x) : thumb_w * 0.5f;
    }

    bool changed = false;
    if (ImGui::IsItemActive() && max_first_frame > 0 && range_w > 0.0f) {
        float new_thumb_x = io.MousePos.x - s_drag_offset;
        float new_t = (new_thumb_x - track.Min.x) / range_w;
        if (new_t < 0.0f) new_t = 0.0f;
        if (new_t > 1.0f) new_t = 1.0f;
        int new_first = (int)(new_t * (float)max_first_frame + 0.5f);
        if (new_first < 0) new_first = 0;
        if (new_first > max_first_frame) new_first = max_first_frame;
        if (*first_frame != new_first) {
            *first_frame = new_first;
            changed = true;
        }

        t = (float)(*first_frame) / (float)max_first_frame;
        thumb.Min.x = track.Min.x + range_w * t;
        thumb.Max.x = thumb.Min.x + thumb_w;
    }

    bool hovered = ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(thumb.Min, thumb.Max);
    ImU32 track_col = ImGui::GetColorU32(ImGuiCol_ScrollbarBg);
    ImU32 thumb_col = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ScrollbarGrabActive :
                                         (hovered ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(track.Min, track.Max, track_col, track_h * 0.5f);
    dl->AddRectFilled(thumb.Min, thumb.Max, thumb_col, track_h * 0.5f);

    if (hovered) {
        int last = *first_frame + visible_count - 1;
        if (last >= total_frames) last = total_frames - 1;
        ImGui::SetTooltip("Frames %d - %d", *first_frame, last);
    }

    return changed;
}

static void ui_draw_timeline_window() {
    if (!s_timeline_window_open) {
        s_timeline_keyboard_focus = false;
        return;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowSize(ImVec2(ui_px(920.0f), ui_px(420.0f)), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_panel_bg(UI_PANEL_DEFAULT));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    bool timeline_window_open = ImGui::Begin("Timeline", nullptr, window_flags);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (!timeline_window_open) {
        ImGui::End();
        return;
    }

    char detail[128] = {};
    snprintf(detail, sizeof(detail), "%s  frame %d / %d  %.2fs",
             timeline_enabled() ? "active" : "disabled",
             timeline_current_frame(), timeline_length_frames() - 1, app_scene_time());
    if (s_panel_tone_count < (int)(sizeof(s_panel_tone_stack) / sizeof(s_panel_tone_stack[0])))
        s_panel_tone_stack[s_panel_tone_count++] = UI_PANEL_DEFAULT;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_panel_bg(UI_PANEL_DEFAULT));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.205f, 0.200f, 1.0f));
    float timeline_child_w = ImGui::GetContentRegionAvail().x;
    float timeline_content_w = timeline_child_w - ui_margin_px(16.0f);
    if (timeline_content_w < ui_px(1.0f))
        timeline_content_w = ui_px(1.0f);
    ImGui::SetNextWindowContentSize(ImVec2(timeline_content_w, 0.0f));
    if (!ImGui::BeginChild("##timeline_panel", ImGui::GetContentRegionAvail(), true)) {
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (s_panel_tone_count > 0)
            s_panel_tone_count--;
        ImGui::End();
        return;
    }
    ui_lock_current_window_scroll_x();
    ui_panel_header("TIMELINE", detail);

    ImGuiIO& io = ImGui::GetIO();
    bool track_enabled = timeline_enabled();
    bool timeline_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    bool timeline_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    s_timeline_keyboard_focus = timeline_focused;
    bool timeline_nav_active = track_enabled && timeline_focused &&
                               !io.WantTextInput && !ImGui::IsAnyItemActive() &&
                               !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    bool timeline_mouse_nav_active = track_enabled && timeline_hovered &&
                                     !io.WantTextInput && !ImGui::IsAnyItemActive() &&
                                     !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (timeline_nav_active) {
        int step = io.KeyShift ? 10 : 1;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
            ui_timeline_copy_selected_slot();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false))
            ui_timeline_cut_selected_slot();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
            ui_timeline_paste_selected_slot();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            ui_timeline_delete_selected_slot();
        if (ImGui::IsKeyPressed(ImGuiKey_I, false))
            ui_timeline_insert_selected_slot();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) {
            if (ui_timeline_slot_selection_valid())
                ui_timeline_move_selection(-step, 0);
            else {
                timeline_set_current_frame(timeline_current_frame() - step);
                s_timeline_ensure_current_visible = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) {
            if (ui_timeline_slot_selection_valid())
                ui_timeline_move_selection(step, 0);
            else {
                timeline_set_current_frame(timeline_current_frame() + step);
                s_timeline_ensure_current_visible = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
            ui_timeline_move_selection(0, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
            ui_timeline_move_selection(0, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            if (ui_timeline_slot_selection_valid())
                ui_timeline_move_selection(-timeline_length_frames(), 0);
            timeline_set_current_frame(0);
            s_timeline_ensure_current_visible = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            if (ui_timeline_slot_selection_valid())
                ui_timeline_move_selection(timeline_length_frames(), 0);
            timeline_set_current_frame(timeline_length_frames() - 1);
            s_timeline_ensure_current_visible = true;
        }
    }

    int fps = timeline_fps();
    int length = timeline_length_frames();
    static float s_timeline_slot_zoom = 1.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui_margin_px(10.0f), ui_margin_px(4.0f)));
    if (track_enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.33f, 0.18f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.24f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.30f, 0.14f, 1.0f));
    }
    if (ImGui::Button(track_enabled ? "Track" : "No Track")) {
        timeline_set_enabled(!track_enabled);
        app_request_scene_render();
    }
    if (track_enabled)
        ImGui::PopStyleColor(3);
    ImGui::SameLine(0.0f, ui_margin_px(18.0f));
    if (!track_enabled)
        ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ui_px(62.0f));
    if (ImGui::InputInt("fps", &fps, 0, 0))
        timeline_set_fps(fps);
    ImGui::SameLine(0.0f, ui_margin_px(18.0f));
    ImGui::SetNextItemWidth(ui_px(76.0f));
    if (ImGui::InputInt("frames", &length, 0, 0))
        timeline_set_length_frames(length);
    ImGui::SameLine(0.0f, ui_margin_px(10.0f));
    bool loop = timeline_loop();
    if (loop) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.33f, 0.18f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.24f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.30f, 0.14f, 1.0f));
    }
    if (ui_icon_button("##timeline_loop", UI_ICON_RESTART, ImVec2(ui_px(26.0f), 0.0f),
                       loop ? "Loop on" : "Loop off"))
        timeline_set_loop(!loop);
    if (loop)
        ImGui::PopStyleColor(3);
    ImGui::SameLine(0.0f, ui_margin_px(10.0f));
    bool interpolate_frames = timeline_interpolate_frames();
    if (interpolate_frames) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.33f, 0.18f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.24f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.30f, 0.14f, 1.0f));
    }
    if (ImGui::Button(interpolate_frames ? "Linear" : "Step"))
        timeline_set_interpolate_frames(!interpolate_frames);
    if (interpolate_frames)
        ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(interpolate_frames ?
                          "Linear interpolation between real timeline frames" :
                          "Stepped playback: hold each real timeline frame");
    ImGui::SameLine(0.0f, ui_margin_px(18.0f));
    ImGui::SetNextItemWidth(ui_px(112.0f));
    ImGui::SliderFloat("zoom", &s_timeline_slot_zoom, 0.55f, 1.60f, "%.2f");
    ImGui::SameLine(0.0f, ui_margin_px(18.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Current frame %d / %d  -  scene %.2fs",
                        timeline_current_frame(), timeline_length_frames() - 1, app_scene_time());
    ImGui::PopStyleVar();

    ui_timeline_add_tracks();
    ImGui::Separator();

    const float row_h = ImGui::GetFrameHeight();
    const float ruler_h = ui_px(32.0f);
    const float scrollbar_h = ui_px(14.0f);
    ImVec2 slot_size = ImVec2(ui_px(22.0f * s_timeline_slot_zoom), row_h);
    const float col_w = slot_size.x + ui_px(2.0f);
    const float track_col_w = ui_px(230.0f);

    // Paint only the frame columns that physically fit in the current window.
    // Column width is fixed by zoom only; resizing the timeline changes how many
    // frame slots are shown, not their size.
    const float timeline_avail_w = ImGui::GetContentRegionAvail().x;
    int fit_visible_frames = (int)((timeline_avail_w - track_col_w) / col_w);
    if (fit_visible_frames < 1) fit_visible_frames = 1;
    int visible_count = timeline_length_frames();
    if (visible_count > fit_visible_frames)
        visible_count = fit_visible_frames;
    const float timeline_table_w = track_col_w + (float)visible_count * col_w;
    int max_first_frame = timeline_length_frames() - visible_count;
    if (max_first_frame < 0) max_first_frame = 0;
    if (s_timeline_ensure_current_visible) {
        int current = timeline_current_frame();
        if (current < s_timeline_visible_first_frame)
            s_timeline_visible_first_frame = current;
        else if (current >= s_timeline_visible_first_frame + visible_count)
            s_timeline_visible_first_frame = current - visible_count + 1;
        s_timeline_ensure_current_visible = false;
    }
    if (s_timeline_visible_first_frame < 0) s_timeline_visible_first_frame = 0;
    if (s_timeline_visible_first_frame > max_first_frame) s_timeline_visible_first_frame = max_first_frame;
    if (timeline_mouse_nav_active) {
        float wheel = io.MouseWheelH;
        if (io.KeyShift)
            wheel += io.MouseWheel;
        if (wheel != 0.0f) {
            int wheel_step = io.KeyCtrl ? 12 : 5;
            s_timeline_visible_first_frame -= (int)(wheel * (float)wheel_step);
            if (s_timeline_visible_first_frame < 0) s_timeline_visible_first_frame = 0;
            if (s_timeline_visible_first_frame > max_first_frame) s_timeline_visible_first_frame = max_first_frame;
        }
    }
    int visible_first = s_timeline_visible_first_frame;
    int visible_last = visible_first + visible_count;

    int pending_length_frames = -1;
    ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ImVec4(0.18f, 0.17f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(0.14f, 0.14f, 0.15f, 1.0f));

    float table_h = ImGui::GetContentRegionAvail().y - scrollbar_h - ui_margin_px(3.0f);
    if (table_h < ruler_h + row_h * 2.0f)
        table_h = ruler_h + row_h * 2.0f;

    // The timeline grid is intentionally drawn manually inside a two-column
    // table. Track labels stay in one fixed column, while frame slots keep a
    // constant width and never get squeezed by ImGui table layout.
    if (ImGui::BeginTable("##timeline_table", 2, flags, ImVec2(timeline_table_w, table_h))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Track", ImGuiTableColumnFlags_WidthFixed, track_col_w);
        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, (float)visible_count * col_w);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 header_bg = ImGui::GetColorU32(ImVec4(0.105f, 0.102f, 0.108f, 1.0f));
        const ImU32 header_bg_2 = ImGui::GetColorU32(ImVec4(0.088f, 0.086f, 0.092f, 1.0f));
        const ImU32 track_bg = ImGui::GetColorU32(ImVec4(0.070f, 0.069f, 0.073f, 1.0f));
        const ImU32 track_bg_alt = ImGui::GetColorU32(ImVec4(0.088f, 0.085f, 0.088f, 1.0f));
        const ImU32 grid_bg = ImGui::GetColorU32(ImVec4(0.060f, 0.060f, 0.064f, 1.0f));
        const ImU32 grid_bg_alt = ImGui::GetColorU32(ImVec4(0.074f, 0.073f, 0.077f, 1.0f));
        const ImU32 grid_line_col = ImGui::GetColorU32(ImVec4(0.18f, 0.18f, 0.19f, 0.72f));
        const ImU32 grid_line_major_col = ImGui::GetColorU32(ImVec4(0.25f, 0.24f, 0.24f, 0.85f));
        const ImU32 row_line_col = ImGui::GetColorU32(ImVec4(0.12f, 0.12f, 0.13f, 1.0f));
        const ImU32 current_band_col = ImGui::GetColorU32(ImVec4(0.37f, 0.16f, 0.06f, 0.48f));
        const ImU32 current_line_col = ImGui::GetColorU32(ImVec4(0.95f, 0.38f, 0.10f, 1.0f));
        const ImU32 subtle_text_col = ImGui::GetColorU32(ImVec4(0.58f, 0.55f, 0.52f, 1.0f));

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, ruler_h);
        ImGui::TableSetColumnIndex(0);
        ImVec2 track_header_min = ImGui::GetCursorScreenPos();
        ImVec2 track_header_max = ImVec2(track_header_min.x + track_col_w, track_header_min.y + ruler_h);
        dl->AddRectFilled(track_header_min, track_header_max, header_bg, ui_px(2.0f));
        dl->AddLine(ImVec2(track_header_min.x, track_header_max.y - 1.0f),
                    ImVec2(track_header_max.x, track_header_max.y - 1.0f),
                    row_line_col, ui_px(1.0f));
        ImGui::SetCursorScreenPos(ImVec2(track_header_min.x + ui_margin_px(10.0f),
                                         track_header_min.y + floorf((ruler_h - ImGui::GetTextLineHeight()) * 0.5f)));
        ImGui::TextDisabled("Track");

        ImGui::TableSetColumnIndex(1);
        ImVec2 grid_origin = ImGui::GetCursorScreenPos();
        ImVec2 grid_min = grid_origin;
        ImVec2 grid_max = ImVec2(grid_origin.x + (float)visible_count * col_w,
                                 grid_origin.y + ruler_h);
        dl->AddRectFilled(grid_min, grid_max, header_bg_2, ui_px(2.0f));
        dl->AddLine(ImVec2(grid_min.x, grid_max.y - 1.0f), ImVec2(grid_max.x, grid_max.y - 1.0f),
                    row_line_col, ui_px(1.0f));

        for (int i = 0; i <= visible_count; i++) {
            int f = visible_first + i;
            float x = grid_min.x + (float)i * col_w;
            bool major = (f % 5) == 0;
            float tick_h = major ? ui_px(12.0f) : ui_px(6.0f);
            dl->AddLine(ImVec2(x, grid_max.y - tick_h), ImVec2(x, grid_max.y),
                        major ? grid_line_major_col : grid_line_col, ui_px(1.0f));
            if (i < visible_count)
                dl->AddLine(ImVec2(x, grid_min.y), ImVec2(x, grid_max.y),
                            major ? grid_line_major_col : grid_line_col, ui_px(1.0f));
        }
        for (int f = visible_first; f < visible_last; f++) {
            int frame_index = f - visible_first;
            ImVec2 slot_min = ImVec2(grid_min.x + (float)frame_index * col_w, grid_min.y);
            ImVec2 slot_max = ImVec2(slot_min.x + slot_size.x, slot_min.y + ruler_h);
            ImGui::PushID(f);
            ImGui::SetCursorScreenPos(slot_min);
            ImGui::InvisibleButton("##frame_header", ImVec2(slot_size.x, ruler_h));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                pending_length_frames = f + 1;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Set timeline length to %d frames", f + 1);
            if (f % 5 == 0) {
                char label[16] = {};
                snprintf(label, sizeof(label), "%d", f);
                ImVec2 label_sz = ImGui::CalcTextSize(label);
                dl->AddText(ImVec2(floorf(slot_min.x + (slot_size.x - label_sz.x) * 0.5f),
                                   floorf(grid_min.y + (ruler_h - label_sz.y) * 0.5f)),
                            subtle_text_col, label);
            }
            ImGui::PopID();
        }

        if (timeline_current_frame() >= visible_first && timeline_current_frame() < visible_last) {
            int current_index = timeline_current_frame() - visible_first;
            float playhead_x = grid_min.x + (float)current_index * col_w + col_w * 0.5f;
            float band_min_x = grid_min.x + (float)current_index * col_w;
            float band_max_x = band_min_x + col_w;
            dl->AddRectFilled(ImVec2(band_min_x, grid_min.y), ImVec2(band_max_x, grid_max.y),
                              current_band_col);
            dl->AddLine(ImVec2(playhead_x, grid_min.y), ImVec2(playhead_x, grid_max.y),
                        current_line_col, ui_px(2.0f));

            char cur_label[16] = {};
            snprintf(cur_label, sizeof(cur_label), "%d", timeline_current_frame());
            ImVec2 label_sz = ImGui::CalcTextSize(cur_label);
            float badge_w = label_sz.x + ui_margin_px(12.0f);
            float badge_h = ui_px(18.0f);
            float badge_x = playhead_x - badge_w * 0.5f;
            if (badge_x < grid_min.x + ui_px(2.0f)) badge_x = grid_min.x + ui_px(2.0f);
            if (badge_x + badge_w > grid_max.x - ui_px(2.0f)) badge_x = grid_max.x - ui_px(2.0f) - badge_w;
            ImVec2 badge_min = ImVec2(floorf(badge_x), floorf(grid_min.y + ui_px(1.0f)));
            ImVec2 badge_max = ImVec2(badge_min.x + badge_w, badge_min.y + badge_h);
            dl->AddRectFilled(badge_min, badge_max,
                              ImGui::GetColorU32(ImVec4(0.58f, 0.26f, 0.10f, 1.0f)), ui_px(3.0f));
            dl->AddRect(badge_min, badge_max,
                        ImGui::GetColorU32(ImVec4(0.95f, 0.45f, 0.12f, 1.0f)), ui_px(3.0f), 0, ui_px(1.0f));
            dl->AddText(ImVec2(floorf(badge_min.x + (badge_w - label_sz.x) * 0.5f),
                               floorf(badge_min.y + (badge_h - label_sz.y) * 0.5f)),
                        ImGui::GetColorU32(ImVec4(0.98f, 0.90f, 0.82f, 1.0f)), cur_label);
        }

        int visible_track_row = 0;
        for (int t = 0; t < g_timeline_track_count; t++) {
            TimelineTrack& track = g_timeline_tracks[t];
            if (!track.active)
                continue;

            ImGui::PushID(t);
            ImGui::TableNextRow(0, row_h);
            ImU32 left_row_bg = (visible_track_row & 1) ? track_bg_alt : track_bg;
            ImU32 right_row_bg = (visible_track_row & 1) ? grid_bg_alt : grid_bg;

            ImGui::TableSetColumnIndex(0);
            ImVec2 row_min = ImGui::GetCursorScreenPos();
            ImVec2 row_max = ImVec2(row_min.x + track_col_w, row_min.y + row_h);
            bool row_hovered = ImGui::IsMouseHoveringRect(row_min, row_max);
            dl->AddRectFilled(row_min, row_max,
                              row_hovered ? ImGui::GetColorU32(ImVec4(0.12f, 0.11f, 0.11f, 1.0f)) : left_row_bg);
            dl->AddLine(ImVec2(row_min.x, row_max.y - 1.0f), ImVec2(row_max.x, row_max.y - 1.0f),
                        row_line_col, ui_px(1.0f));

            float checkbox_size = ui_px(18.0f);
            ImGui::SetCursorScreenPos(ImVec2(row_min.x + ui_margin_px(9.0f),
                                             row_min.y + floorf((row_h - checkbox_size) * 0.5f)));
            if (ui_timeline_track_enable_checkbox("##track_enabled", &track.enabled, checkbox_size))
                app_request_scene_render();
            char label[128] = {};
            ui_timeline_track_label(track, label, sizeof(label));
            bool missing = !timeline_track_target_exists(track);
            float label_left = row_min.x + ui_margin_px(9.0f) + checkbox_size + ui_margin_px(7.0f);
            float label_right = row_max.x - ui_margin_px(8.0f);
            char fitted_label[128] = {};
            ui_fit_text_ellipsis(label, label_right - label_left, fitted_label, sizeof(fitted_label));
            ImVec2 label_sz = ImGui::CalcTextSize(fitted_label);
            float label_x = label_left;
            float label_y = row_min.y + floorf((row_h - label_sz.y) * 0.5f);
            dl->PushClipRect(ImVec2(label_left, row_min.y), ImVec2(label_right, row_max.y), true);
            dl->AddText(ImVec2(floorf(label_x), floorf(label_y)),
                        ImGui::GetColorU32((missing || !track.enabled) ? ImGuiCol_TextDisabled : ImGuiCol_Text),
                        fitted_label);
            dl->PopClipRect();

            if (row_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##track_menu");
            if (ImGui::BeginPopup("##track_menu")) {
                if (ImGui::MenuItem("Delete Track")) {
                    if (s_timeline_slot_selection.valid) {
                        if (s_timeline_slot_selection.track_index == t)
                            s_timeline_slot_selection.valid = false;
                        else if (s_timeline_slot_selection.track_index > t)
                            s_timeline_slot_selection.track_index--;
                    }
                    timeline_delete_track(t);
                    ImGui::EndPopup();
                    ImGui::PopID();
                    t--;
                    continue;
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            grid_origin = ImGui::GetCursorScreenPos();
            grid_min = grid_origin;
            grid_max = ImVec2(grid_origin.x + (float)visible_count * col_w,
                              grid_origin.y + row_h);
            dl->AddRectFilled(grid_min, grid_max, right_row_bg);
            dl->AddLine(ImVec2(grid_min.x, grid_max.y - 1.0f), ImVec2(grid_max.x, grid_max.y - 1.0f),
                        row_line_col, ui_px(1.0f));

            for (int i = 0; i <= visible_count; i++) {
                int f = visible_first + i;
                float x = grid_min.x + (float)i * col_w;
                bool major = (f % 5) == 0;
                dl->AddLine(ImVec2(x, grid_min.y), ImVec2(x, grid_max.y),
                            major ? grid_line_major_col : grid_line_col, ui_px(1.0f));
            }
            if (timeline_current_frame() >= visible_first && timeline_current_frame() < visible_last) {
                int current_index = timeline_current_frame() - visible_first;
                float band_min_x = grid_min.x + (float)current_index * col_w;
                float band_max_x = band_min_x + col_w;
                float playhead_x = band_min_x + col_w * 0.5f;
                dl->AddRectFilled(ImVec2(band_min_x, grid_min.y), ImVec2(band_max_x, grid_max.y),
                                  ImGui::GetColorU32(ImVec4(0.32f, 0.14f, 0.05f, 0.38f)));
                dl->AddLine(ImVec2(playhead_x, grid_min.y), ImVec2(playhead_x, grid_max.y),
                            current_line_col, ui_px(2.0f));
            }

            for (int f = visible_first; f < visible_last; f++) {
                int frame_index = f - visible_first;
                ImGui::PushID(f);
                ImGui::SetCursorScreenPos(ImVec2(grid_min.x + (float)frame_index * col_w, grid_min.y));
                ui_timeline_draw_slot(t, f, slot_size);
                ImGui::PopID();
            }
            ImGui::PopID();
            visible_track_row++;
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    ui_timeline_scrollbar("##timeline_scroll", &s_timeline_visible_first_frame,
                          max_first_frame, visible_count, timeline_length_frames(),
                          track_col_w + ui_px(2.0f));
    if (s_timeline_visible_first_frame < 0) s_timeline_visible_first_frame = 0;
    if (s_timeline_visible_first_frame > max_first_frame) s_timeline_visible_first_frame = max_first_frame;

    if (pending_length_frames > 0)
        timeline_set_length_frames(pending_length_frames);

    if (!track_enabled)
        ImGui::EndDisabled();

    ui_lock_current_window_scroll_x();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (s_panel_tone_count > 0)
        s_panel_tone_count--;
    ImGui::End();
}

static void ui_top_bar() {
    for (int i = 0; i < 3; i++)
        s_ui_window_control_screen_rects_valid[i] = false;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.080f, 0.076f, 0.080f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), 0.0f));
    float toolbar_h = ui_px(40.0f);
    ImGui::BeginChild("##top_toolbar", ImVec2(0.0f, toolbar_h), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImVec2 bar_min = ImGui::GetWindowPos();
        ImVec2 bar_max = ImVec2(bar_min.x + ImGui::GetWindowSize().x, bar_min.y + ImGui::GetWindowSize().y);
        s_ui_top_toolbar_screen_rect.left = (LONG)floorf(bar_min.x);
        s_ui_top_toolbar_screen_rect.top = (LONG)floorf(bar_min.y);
        s_ui_top_toolbar_screen_rect.right = (LONG)ceilf(bar_max.x);
        s_ui_top_toolbar_screen_rect.bottom = (LONG)ceilf(bar_max.y);
        s_ui_top_toolbar_screen_rect_valid = true;
    }

    float row_h = ImGui::GetFrameHeight();
    float row_y = floorf((toolbar_h - row_h) * 0.5f);
    ui_align_text_row(row_y);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.92f, 0.91f, 1.0f));
    ImGui::TextUnformatted("lazyTool");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, ui_margin_px(8.0f));
    ui_align_text_row(row_y);
    ImGui::TextDisabled("workspace");
    ImGui::SameLine(0.0f, ui_margin_px(8.0f));
    ui_align_frame_row(row_y);
    ui_inline_badge("##project_name_badge", project_current_name() ? project_current_name() : "untitled",
                    ImVec4(0.74f, 0.53f, 0.42f, 1.0f), row_h);
    ImGui::SameLine(0.0f, ui_margin_px(12.0f));
    ui_align_frame_row(row_y);

    if (ImGui::Button("New"))
        project_new_default();
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    if (ImGui::Button("Save")) {
        s_project_file_mode = PROJECT_FILE_SAVE;
        s_project_path_focus = true;
    }
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    if (ImGui::Button("Load")) {
        s_project_file_mode = PROJECT_FILE_LOAD;
        s_project_path_focus = true;
    }
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    if (ui_icon_button("##compile_button", UI_ICON_COMPILE, ImVec2(ui_px(28.0f), 0.0f), "Compile shaders"))
        ui_recompile_all_shaders();
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    bool timeline_was_open = s_timeline_window_open;
    if (timeline_was_open) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.33f, 0.18f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.24f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.30f, 0.14f, 1.0f));
    }
    if (ui_icon_button("##timeline_button", UI_ICON_TIMELINE, ImVec2(ui_px(28.0f), 0.0f), "Timeline"))
        s_timeline_window_open = !s_timeline_window_open;
    if (timeline_was_open)
        ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    if (ui_icon_button("##export_exe_button", UI_ICON_EXPORT_EXE, ImVec2(ui_px(28.0f), 0.0f), "Export EXE"))
        ui_export_current_project_single_exe();
    ImGui::SameLine(0.0f, ui_margin_px(6.0f));
    ui_align_frame_row(row_y);
    if (ui_icon_button("##shortcuts_button", UI_ICON_HELP, ImVec2(ui_px(28.0f), 0.0f), "Shortcuts"))
        s_shortcuts_popup_open = !s_shortcuts_popup_open;

    char summary[256] = {};
    float frame_ms = ImGui::GetIO().DeltaTime * 1000.0f;
    if (g_profiler_enabled) {
        char app_mem[32] = {};
        char gpu_mem[32] = {};
        ui_format_bytes(ui_process_memory_bytes(), app_mem, sizeof(app_mem));
        ui_format_bytes(ui_estimated_gpu_memory_bytes(), gpu_mem, sizeof(gpu_mem));
        if (cmd_profile_total_ready() && cmd_profile_ready()) {
            snprintf(summary, sizeof(summary), "dt %.2f ms  frame gpu %.3f ms  cmds gpu %.3f ms  app %s  vram %s  %s",
                frame_ms, cmd_profile_total_frame_ms(), cmd_profile_frame_ms(), app_mem, gpu_mem,
                g_dx.vsync ? "vsync" : "no-vsync");
        } else if (cmd_profile_total_ready()) {
            snprintf(summary, sizeof(summary), "dt %.2f ms  frame gpu %.3f ms  cmds gpu ...  app %s  vram %s  %s",
                frame_ms, cmd_profile_total_frame_ms(), app_mem, gpu_mem, g_dx.vsync ? "vsync" : "no-vsync");
        } else if (cmd_profile_ready()) {
            snprintf(summary, sizeof(summary), "dt %.2f ms  frame gpu ...  cmds gpu %.3f ms  app %s  vram %s  %s",
                frame_ms, cmd_profile_frame_ms(), app_mem, gpu_mem, g_dx.vsync ? "vsync" : "no-vsync");
        } else {
            snprintf(summary, sizeof(summary), "dt %.2f ms  frame gpu ...  cmds gpu ...  app %s  vram %s  %s",
                frame_ms, app_mem, gpu_mem, g_dx.vsync ? "vsync" : "no-vsync");
        }
    } else {
        snprintf(summary, sizeof(summary), "dt %.2f ms  cmds %d  %s",
            frame_ms, ui_active_command_count(), g_dx.vsync ? "vsync" : "no-vsync");
    }

    float content_max_x = ImGui::GetWindowContentRegionMax().x;
    float controls_w = ui_window_controls_width();
    float controls_x = content_max_x - controls_w;
    float summary_gap = ui_margin_px(12.0f);
    float drag_gap = ui_margin_px(8.0f);
    float left_end_x = ImGui::GetCursorPosX();
    float summary_max_w = controls_x - left_end_x - summary_gap - drag_gap;
    char summary_fit[256] = {};
    float summary_w = 0.0f;
    float summary_x = controls_x - summary_gap;
    if (summary_max_w > ui_margin_px(32.0f)) {
        ui_fit_text_ellipsis(summary, summary_max_w, summary_fit, sizeof(summary_fit));
        summary_w = ImGui::CalcTextSize(summary_fit).x;
        summary_x = controls_x - summary_gap - summary_w;
    }

    float drag_w = summary_x - left_end_x - drag_gap;
    if (drag_w > ui_margin_px(24.0f)) {
        ImGui::SameLine();
        ui_align_frame_row(row_y);
        ImGui::InvisibleButton("##title_drag", ImVec2(drag_w, row_h));
        if (ImGui::IsItemActivated()) {
            ReleaseCapture();
            SendMessageW(g_dx.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
    }

    if (summary_w > 0.0f) {
        ImGui::SetCursorPosX(summary_x);
        ui_align_text_row(row_y);
        ImGui::TextDisabled("%s", summary_fit);
    }

    ImGui::SetCursorPosX(controls_x);
    ui_draw_window_controls(toolbar_h);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static void ui_workspace_layout() {
    if (s_scene_surface_host_w != g_dx.width || s_scene_surface_host_h != g_dx.height ||
        s_scene_surface_fullscreen != s_viewport_fullscreen) {
        s_scene_surface_host_w = g_dx.width;
        s_scene_surface_host_h = g_dx.height;
        s_scene_surface_fullscreen = s_viewport_fullscreen;
        s_scene_surface_resize_armed = true;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 640.0f || avail.y < 360.0f) {
        ui_panel_scene(true);
        return;
    }

    if (s_viewport_fullscreen) {
        char detail[96] = {};
        ui_viewport_detail(detail, sizeof(detail));
        bool restart_clicked = false;
        bool pause_clicked = false;
        bool exit_clicked = false;
        if (ui_begin_tool_panel("##viewport_panel_full", "VIEWPORT", detail, avail, UI_PANEL_VIEWPORT,
                                "GizmoMove##viewport_gizmo_move_full", nullptr,
                                "GizmoRotate##viewport_gizmo_rotate_full", nullptr,
                                "GizmoScale##viewport_gizmo_scale_full", nullptr,
                                "Wireframe##viewport_wireframe_full", nullptr,
                                "Grid##viewport_grid_full", nullptr,
                                "Restart##viewport_restart_full", &restart_clicked,
                                app_scene_paused() ? "Resume##viewport_resume_full" : "Pause##viewport_pause_full", &pause_clicked,
                                "Exit fullscreen##viewport_fullscreen_exit", &exit_clicked)) {
            if (restart_clicked)
                app_request_scene_restart();
            if (pause_clicked)
                app_set_scene_paused(!app_scene_paused());
            if (exit_clicked)
                s_viewport_fullscreen = false;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("##viewport_frame_full", ImVec2(0.0f, 0.0f), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ui_panel_scene(true);
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
        ui_end_tool_panel();
        return;
    }

    float left_w = ui_clampf(avail.x * 0.23f, 300.0f, 390.0f);
    float right_w = ui_clampf(avail.x * 0.32f, 430.0f, 560.0f);
    if (avail.x - left_w - right_w < 520.0f) {
        left_w = ui_clampf(avail.x * 0.22f, 270.0f, 340.0f);
        right_w = ui_clampf(avail.x * 0.30f, 380.0f, 480.0f);
    }
    const float col_gap = ui_margin_px(4.0f);
    float center_w = avail.x - left_w - right_w - col_gap * 2.0f;
    if (center_w < 260.0f) center_w = 260.0f;

    float bottom_h = ui_clampf(avail.y * 0.22f, 130.0f, 220.0f);
    float viewport_h = avail.y - bottom_h - 6.0f;
    float cmd_h = ui_clampf(avail.y * 0.60f, 250.0f, avail.y - 170.0f);
    if (cmd_h < 160.0f) cmd_h = avail.y * 0.5f;

    ImGui::BeginGroup();
    if (ui_begin_tool_panel("##pipeline_panel", "COMMAND PIPELINE", "right click to create", ImVec2(left_w, cmd_h), UI_PANEL_PIPELINE)) {
        ui_panel_commands(true);
    }
    ui_end_tool_panel();

    if (ui_begin_tool_panel("##resources_panel", "RESOURCES", "right click to create", ImVec2(left_w, 0.0f), UI_PANEL_RESOURCES)) {
        ui_panel_resources(true);
    }
    ui_end_tool_panel();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, col_gap);
    ImGui::BeginGroup();
    char viewport_detail[96] = {};
    ui_viewport_detail(viewport_detail, sizeof(viewport_detail));
    bool restart_clicked = false;
    bool pause_clicked = false;
    bool fullscreen_clicked = false;
    if (ui_begin_tool_panel("##viewport_panel", "VIEWPORT", viewport_detail, ImVec2(center_w, viewport_h), UI_PANEL_VIEWPORT,
                            "GizmoMove##viewport_gizmo_move", nullptr,
                            "GizmoRotate##viewport_gizmo_rotate", nullptr,
                            "GizmoScale##viewport_gizmo_scale", nullptr,
                            "Wireframe##viewport_wireframe", nullptr,
                            "Grid##viewport_grid", nullptr,
                            "Restart##viewport_restart", &restart_clicked,
                            app_scene_paused() ? "Resume##viewport_resume" : "Pause##viewport_pause", &pause_clicked,
                            "Fullscreen##viewport_fullscreen", &fullscreen_clicked)) {
        if (restart_clicked)
            app_request_scene_restart();
        if (pause_clicked)
            app_set_scene_paused(!app_scene_paused());
        if (fullscreen_clicked)
            s_viewport_fullscreen = true;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##viewport_frame", ImVec2(0.0f, 0.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ui_panel_scene(true);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ui_end_tool_panel();

    if (ui_begin_tool_panel("##log_panel", "LOG", nullptr, ImVec2(center_w, 0.0f), UI_PANEL_LOG)) {
        ui_panel_log(true);
    }
    ui_end_tool_panel();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, col_gap);
    ImGui::BeginGroup();
    {
        float collapsed_h = ui_margin_px(38.0f);
        float expanded_h = avail.y - collapsed_h - col_gap;
        if (expanded_h < 180.0f) {
            collapsed_h = 0.0f;
            expanded_h = avail.y;
        }

        if (s_right_panel_general_open) {
            if (collapsed_h > 0.0f) {
                if (ui_header_only_panel("##inspector_collapsed_panel", "INSPECTOR", ui_inspector_header_detail(),
                                         ImVec2(0.0f, collapsed_h), UI_PANEL_INSPECTOR))
                    s_right_panel_general_open = false;
            }

            if (collapsed_h > 0.0f)
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + col_gap);

            if (ui_begin_tool_panel("##general_panel", "GENERAL", nullptr,
                                    ImVec2(0.0f, expanded_h), UI_PANEL_GENERAL)) {
                ui_panel_general(true);
            }
            ui_end_tool_panel();
        } else {
            if (ui_begin_tool_panel("##inspector_panel", "INSPECTOR", ui_inspector_header_detail(),
                                    ImVec2(0.0f, expanded_h), UI_PANEL_INSPECTOR)) {
                if (ImGui::BeginTabBar("##inspector_tabs")) {
                    if (ImGui::BeginTabItem("Properties")) {
                        ui_panel_inspector(true);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Bindings")) {
                        ui_panel_bindings(true);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ui_end_tool_panel();

            if (collapsed_h > 0.0f) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + col_gap);
                if (ui_header_only_panel("##general_collapsed_panel", "GENERAL", nullptr,
                                         ImVec2(0.0f, collapsed_h), UI_PANEL_GENERAL))
                    s_right_panel_general_open = true;
            }
        }
    }
    ImGui::EndGroup();
}

static void ui_delete_selection() {
    if (g_sel_cmd != INVALID_HANDLE) {
        cmd_free(g_sel_cmd);
        s_cmd_nav = INVALID_HANDLE;
        g_sel_cmd = INVALID_HANDLE;
        app_request_scene_render();
        return;
    }

    if (g_sel_res != INVALID_HANDLE) {
        Resource* r = res_get(g_sel_res);
        if (!r || r->is_builtin || r->is_generated)
            return;
        res_free(g_sel_res);
        s_res_nav = INVALID_HANDLE;
        g_sel_res = INVALID_HANDLE;
        app_request_scene_render();
    }
}

static void ui_toggle_selected_command_enabled() {
    if (g_sel_cmd == INVALID_HANDLE)
        return;
    if (Command* c = cmd_get(g_sel_cmd)) {
        c->enabled = !c->enabled;
        timeline_capture_if_tracked(TIMELINE_TRACK_COMMAND_ENABLED, c->name, RES_NONE);
        app_request_scene_render();
    }
}

void ui_set_global_scale(float scale) {
    s_ui_global_scale = ui_clamp_global_scale(scale);
    s_ui_scale_dirty = true;
}

float ui_global_scale() {
    return s_ui_global_scale;
}

void ui_set_code_font_size(float size) {
    s_code_font_size = clampf(size, 10.0f, 28.0f);
}

float ui_code_font_size() {
    return s_code_font_size;
}

void ui_set_shader_auto_save_compile(bool enabled) {
    s_shader_editor_auto_save_compile = enabled;
}

bool ui_shader_auto_save_compile() {
    return s_shader_editor_auto_save_compile;
}

void ui_init() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImFont* ui_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    if (ui_font)
        io.FontDefault = ui_font;
    else
        io.Fonts->AddFontDefault();
    ImFontConfig code_cfg = {};
    code_cfg.SizePixels = s_code_font_size;
    code_cfg.PixelSnapH = true;
    s_code_font = io.Fonts->AddFontDefaultBitmap(&code_cfg);

    ui_apply_gray_tool_style();
    s_ui_base_style = ImGui::GetStyle();
    s_ui_base_style_valid = true;
    s_ui_global_scale = k_ui_scale_default;
    ui_apply_global_scale_now();
    s_ui_scale_dirty = false;

    ImGui_ImplWin32_Init(g_dx.hwnd);
    ImGui_ImplDX11_Init(g_dx.dev, g_dx.ctx);
    ui_init_rt3d_preview_pipeline();
}

// Draw one full editor frame on top of the already-rendered scene texture.
void ui_draw() {
    if (s_ui_scale_dirty) {
        ui_apply_global_scale_now();
        s_ui_scale_dirty = false;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    bool shader_editor_was_focused = s_shader_source_editor_focused;
    s_shader_source_editor_focused = false;
    bool hotkeys_ok = !io.WantTextInput && !ImGui::IsAnyItemActive();
    bool editor_selection_hotkeys_ok = hotkeys_ok && !s_timeline_keyboard_focus;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
        ui_recompile_all_shaders();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
        ui_recompile_selected_shader();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !shader_editor_was_focused)
        s_project_file_mode = PROJECT_FILE_SAVE;
    if (hotkeys_ok && ImGui::IsKeyPressed(ImGuiKey_F1, false))
        s_shortcuts_popup_open = !s_shortcuts_popup_open;
    if (hotkeys_ok && ImGui::IsKeyPressed(ImGuiKey_Space, false))
        app_set_scene_paused(!app_scene_paused());
    if (hotkeys_ok && ImGui::IsKeyPressed(ImGuiKey_F6, false))
        app_request_scene_restart();
    if (hotkeys_ok && ImGui::IsKeyPressed(ImGuiKey_F11, false))
        s_viewport_fullscreen = !s_viewport_fullscreen;
    if (editor_selection_hotkeys_ok && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        ui_delete_selection();
    if (editor_selection_hotkeys_ok && ImGui::IsKeyPressed(ImGuiKey_X, false))
        ui_toggle_selected_command_enabled();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   {0.f, 0.f});
    ImGui::Begin("##dock_root", nullptr,
        ImGuiWindowFlags_NoDocking      | ImGuiWindowFlags_NoTitleBar   |
        ImGuiWindowFlags_NoCollapse     | ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoScrollbar    | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleVar(3);

    ui_top_bar();
    ui_project_file_bar();

    ImVec2 workspace_size = ImGui::GetContentRegionAvail();
    if (workspace_size.x < 1.0f) workspace_size.x = 1.0f;
    if (workspace_size.y < 1.0f) workspace_size.y = 1.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
    ImGui::BeginChild("##workspace_root", workspace_size, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ui_workspace_layout();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ui_draw_shortcuts_popup();
    ImGui::End();

    ui_draw_timeline_window();

    bool mouse_activity = io.WantCaptureMouse && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f);
    for (int i = 0; i < 5; i++) {
        mouse_activity = mouse_activity || (io.WantCaptureMouse &&
            (io.MouseClicked[i] || io.MouseReleased[i] || io.MouseDown[i]));
    }
    bool keyboard_activity = io.WantCaptureKeyboard &&
        (ImGui::IsAnyItemActive() || ImGui::IsAnyItemFocused());
    if (mouse_activity || keyboard_activity)
        app_request_scene_render();

    ImGui::Render();
}

void ui_shutdown() {
    ui_release_rt3d_preview_pipeline();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}
