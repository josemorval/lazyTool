#include "ui.h"
#include "build_config.h"
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
#include "stb_image.h"
#include <d3dcompiler.h>
#include <psapi.h>
#include <float.h>
#include <stdarg.h>
#include <stdlib.h>
#include <direct.h>
#include <vector>
#include <string>
#pragma comment(lib, "psapi.lib")


// Lightweight UI build profiler. Values are shown one frame late on purpose:
// the profiler panel is drawn while the current UI frame is still being built.
// Timing every top-level panel makes it obvious whether a slow frame is ImGui
// itself, a specific panel, or Present/driver throttling.
enum UiProfileSection {
    UI_PROFILE_FRAME_SETUP = 0,
    UI_PROFILE_TOP_BAR,
    UI_PROFILE_PROJECT_FILE_BAR,
    UI_PROFILE_COMMANDS,
    UI_PROFILE_RESOURCES,
    UI_PROFILE_VIEWPORT,
    UI_PROFILE_LOG,
    UI_PROFILE_INSPECTOR_GENERAL,
    UI_PROFILE_FLOATING_WINDOWS,
    UI_PROFILE_IMGUI_RENDER_FINALIZE,
    UI_PROFILE_COUNT
};

struct UiProfileEntry {
    const char* name;
    float accum_ms;
    float display_ms;
};

static UiProfileEntry s_ui_profile[UI_PROFILE_COUNT] = {
    { "Frame setup",       0.0f, 0.0f },
    { "Top bar",           0.0f, 0.0f },
    { "Project file bar",  0.0f, 0.0f },
    { "Commands",          0.0f, 0.0f },
    { "Resources",         0.0f, 0.0f },
    { "Viewport",          0.0f, 0.0f },
    { "Log",               0.0f, 0.0f },
    { "Inspector/General", 0.0f, 0.0f },
    { "Floating windows",  0.0f, 0.0f },
    { "ImGui::Render",     0.0f, 0.0f },
};

static LARGE_INTEGER s_ui_profile_freq = {};
static bool s_ui_profile_display_valid = false;

static ID3D11Texture2D*          s_app_icon_tex = nullptr;
static ID3D11ShaderResourceView* s_app_icon_srv = nullptr;

static ID3D11Texture2D*          s_app_logo_text_tex = nullptr;
static ID3D11ShaderResourceView* s_app_logo_text_srv = nullptr;
static int                       s_app_logo_text_w = 0;
static int                       s_app_logo_text_h = 0;

static void ui_profile_ensure_freq() {
    if (s_ui_profile_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_ui_profile_freq);
}

static float ui_profile_elapsed_ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    ui_profile_ensure_freq();
    return (float)(((double)(b.QuadPart - a.QuadPart) * 1000.0) / (double)s_ui_profile_freq.QuadPart);
}

static void ui_profile_begin_frame() {
#if !LAZYTOOL_ENABLE_PROFILER
    return;
#else
    ui_profile_ensure_freq();
    if (!g_profiler_enabled) {
        s_ui_profile_display_valid = false;
        for (int i = 0; i < UI_PROFILE_COUNT; i++) {
            s_ui_profile[i].display_ms = 0.0f;
            s_ui_profile[i].accum_ms = 0.0f;
        }
        return;
    }

    const float a = 0.04f;
    for (int i = 0; i < UI_PROFILE_COUNT; i++) {
        if (s_ui_profile_display_valid)
            s_ui_profile[i].display_ms += (s_ui_profile[i].accum_ms - s_ui_profile[i].display_ms) * a;
        else
            s_ui_profile[i].display_ms = s_ui_profile[i].accum_ms;
        s_ui_profile[i].accum_ms = 0.0f;
    }
    s_ui_profile_display_valid = true;
#endif
}

struct UiProfileScope {
    UiProfileSection section;
    LARGE_INTEGER begin;
    bool active;

    explicit UiProfileScope(UiProfileSection section_) : section(section_), begin({}), active(g_profiler_enabled) {
        if (active)
            QueryPerformanceCounter(&begin);
    }

    ~UiProfileScope() {
        if (!active)
            return;
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        s_ui_profile[(int)section].accum_ms += ui_profile_elapsed_ms(begin, end);
    }
};

#define UI_PROFILE_CONCAT_INNER(a, b) a##b
#define UI_PROFILE_CONCAT(a, b) UI_PROFILE_CONCAT_INNER(a, b)
#if LAZYTOOL_ENABLE_PROFILER
#define UI_PROFILE_SCOPE(section) UiProfileScope UI_PROFILE_CONCAT(_ui_profile_scope_, __LINE__)(section)
#else
#define UI_PROFILE_SCOPE(section) ((void)0)
#endif

static void ui_release_app_icon_texture() {
    if (s_app_icon_srv) { s_app_icon_srv->Release(); s_app_icon_srv = nullptr; }
    if (s_app_icon_tex) { s_app_icon_tex->Release(); s_app_icon_tex = nullptr; }
}

static void ui_release_app_logo_text_texture() {
    if (s_app_logo_text_srv) { s_app_logo_text_srv->Release(); s_app_logo_text_srv = nullptr; }
    if (s_app_logo_text_tex) { s_app_logo_text_tex->Release(); s_app_logo_text_tex = nullptr; }
    s_app_logo_text_w = 0;
    s_app_logo_text_h = 0;
}

static ID3D11ShaderResourceView* ui_app_icon_srv() {
    if (s_app_icon_srv)
        return s_app_icon_srv;

    void* bytes = nullptr;
    size_t byte_count = 0;
    if (!lt_read_file("assets/brand/lazytool_icon.png", &bytes, &byte_count))
        return nullptr;
    if (byte_count > (size_t)INT_MAX) {
        lt_free_file(bytes);
        return nullptr;
    }

    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory((const stbi_uc*)bytes, (int)byte_count, &w, &h, &ch, 4);
    lt_free_file(bytes);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels;
    sd.SysMemPitch = (UINT)(w * 4);

    HRESULT hr = g_dx.dev->CreateTexture2D(&td, &sd, &s_app_icon_tex);
    stbi_image_free(pixels);
    if (FAILED(hr) || !s_app_icon_tex) {
        ui_release_app_icon_texture();
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = td.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    hr = g_dx.dev->CreateShaderResourceView(s_app_icon_tex, &sv, &s_app_icon_srv);
    if (FAILED(hr) || !s_app_icon_srv)
        ui_release_app_icon_texture();
    return s_app_icon_srv;
}

static ID3D11ShaderResourceView* ui_app_logo_text_srv(int* out_w = nullptr, int* out_h = nullptr) {
    if (s_app_logo_text_srv) {
        if (out_w) *out_w = s_app_logo_text_w;
        if (out_h) *out_h = s_app_logo_text_h;
        return s_app_logo_text_srv;
    }

    void* bytes = nullptr;
    size_t byte_count = 0;
    if (!lt_read_file("assets/brand/lazytool_onlytext.png", &bytes, &byte_count))
        return nullptr;
    if (byte_count > (size_t)INT_MAX) {
        lt_free_file(bytes);
        return nullptr;
    }

    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory((const stbi_uc*)bytes, (int)byte_count, &w, &h, &ch, 4);
    lt_free_file(bytes);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    HRESULT hr = g_dx.dev->CreateTexture2D(&td, nullptr, &s_app_logo_text_tex);
    if (FAILED(hr) || !s_app_logo_text_tex) {
        stbi_image_free(pixels);
        ui_release_app_logo_text_texture();
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = td.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels = (UINT)-1;
    hr = g_dx.dev->CreateShaderResourceView(s_app_logo_text_tex, &sv, &s_app_logo_text_srv);
    if (FAILED(hr) || !s_app_logo_text_srv) {
        stbi_image_free(pixels);
        ui_release_app_logo_text_texture();
        return nullptr;
    }

    g_dx.ctx->UpdateSubresource(s_app_logo_text_tex, 0, nullptr, pixels, (UINT)(w * 4), 0);
    g_dx.ctx->GenerateMips(s_app_logo_text_srv);
    stbi_image_free(pixels);

    s_app_logo_text_w = w;
    s_app_logo_text_h = h;
    if (out_w) *out_w = s_app_logo_text_w;
    if (out_h) *out_h = s_app_logo_text_h;
    return s_app_logo_text_srv;
}

// The UI module is the editor shell. It presents resources, commands,
// inspectors, logs, and the live scene view on top of the runtime state.

ResHandle g_sel_res = INVALID_HANDLE;
CmdHandle g_sel_cmd = INVALID_HANDLE;
bool g_scene_view_hovered = false;
bool g_scene_view_focused = false;
bool g_scene_view_pointer_over = false;
bool g_viewport_manual_camera_active = false;
bool g_viewport_manual_light_active = false;
bool g_editor_mouse_capture = false;
static RECT s_scene_view_screen_rect = {};
static bool s_scene_view_screen_rect_valid = false;
static RECT s_scene_view_overlay_screen_rect = {};
static bool s_scene_view_overlay_screen_rect_valid = false;

static bool s_rename_active = false;
static char s_rename_buf[MAX_NAME] = {};
static bool s_rename_is_cmd = false;
static char s_project_path[MAX_PATH_LEN] = "projects/";
static ResHandle s_res_nav = INVALID_HANDLE;
static CmdHandle s_cmd_nav = INVALID_HANDLE;
static bool s_res_scroll_to_nav = false;
static bool s_cmd_scroll_to_nav = false;
static void ui_scroll_resources_to_nav_if_requested(int nav_index, float row_h);

enum ProjectFileMode {
    PROJECT_FILE_NONE = 0,
    PROJECT_FILE_SAVE,
    PROJECT_FILE_LOAD
};

static ProjectFileMode s_project_file_mode = PROJECT_FILE_NONE;
static bool s_project_path_focus = false;
static bool s_project_path_open_popup = false;
static char s_project_load_path[MAX_PATH_LEN] = {};
static bool s_project_load_pending = false;
static int  s_project_load_defer_frames = 0;
static bool s_project_load_active = false;
static bool s_project_load_failed = false;
static char s_project_load_status[160] = {};
static bool s_viewport_fullscreen = false;
static bool s_right_panel_general_open = false;
static ResHandle s_right_panel_general_base_res = INVALID_HANDLE;
static CmdHandle s_right_panel_general_base_cmd = INVALID_HANDLE;
static bool s_focus_inspector_panel_next = false;
static bool s_focus_general_panel_next = false;
static bool s_help_popup_open = false;
static bool s_timeline_window_open = false;
static bool s_render_graph_window_open = false;
static bool s_render_graph_center_next = true;
static bool s_render_graph_mouse_dragging = false;
static float s_render_graph_zoom = 1.0f;
static ImVec2 s_render_graph_pan = ImVec2(0.0f, 0.0f);
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
static const float k_ui_scale_default = 1.10f;
static const float k_ui_scale_min = 0.75f;
static const float k_ui_scale_max = 1.25f;
static float s_ui_global_scale = k_ui_scale_default;
static bool s_ui_scale_dirty = false;
static ImGuiStyle s_ui_base_style = {};
static bool s_ui_base_style_valid = false;
static ImFont* s_code_font = nullptr;
static float s_code_font_size = 14.0f;
static bool s_shader_source_editor_focused = false;
static bool s_show_inspector_notes = false;
static bool s_show_interface_hints = true;
static bool s_inspector_resource_note_open[MAX_RESOURCES] = {};
static bool s_inspector_command_note_open[MAX_COMMANDS] = {};
static bool s_inspector_resource_note_editing[MAX_RESOURCES] = {};
static bool s_inspector_command_note_editing[MAX_COMMANDS] = {};
static int s_render_target_preview_columns = 4;

static ResHandle ui_resource_handle_from_ptr(const Resource* r);

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

enum UiIconKind : int {
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
    UI_ICON_RENDER_GRAPH,
    UI_ICON_SHADER_EDITOR,
    UI_ICON_NEW_PROJECT,
    UI_ICON_LOAD_PROJECT,
    UI_ICON_SAVE_PROJECT,
    UI_ICON_COMPILE,
    UI_ICON_EXPORT_EXE,
    UI_ICON_BOUNDS
};

static bool ui_begin_shortcut_section(const char* id, const char* title, ImGuiTableFlags table_flags);
static void ui_draw_shortcut_row(const char* key, const char* desc);
static void ui_help_marker(const char* desc);
static Mat4 ui_mat4_from_raw(const float raw[16]);
static bool ui_project_world_to_screen(const Mat4& view_proj, ImVec2 rect_min, ImVec2 rect_max,
                                       Vec3 world, ImVec2* out_screen);
static void ui_draw_camera_orientation_gizmo(ImVec2 rect_min, ImVec2 rect_max);
static void ui_draw_viewport_bounds_debug(ImVec2 rect_min, ImVec2 rect_max);
static void ui_draw_viewport_light_debug(ImVec2 rect_min, ImVec2 rect_max);
static void ui_draw_viewport_camera_overlay(ImVec2 rect_min, ImVec2 rect_max);
static bool ui_viewport_toolbar_hit_test(ImVec2 rect_min, ImVec2 rect_max);
static void ui_draw_icon_shape(UiIconKind icon, ImVec2 min, ImVec2 max, ImU32 col);
static const char* ui_camera_mode_name(int mode);
static void ui_draw_bounds_values(const char* label, const float bmin[3], const float bmax[3]);
static void ui_draw_command_bounds_inspector(Command* c, CmdHandle h);
static void ui_align_frame_row(float row_y);
static void ui_align_text_row(float row_y);
static float ui_px(float v);
static void ui_draw_profiler_gpu_command_table();
static void ui_fit_text_ellipsis(const char* text, float max_w, char* out, int out_sz);
static bool ui_current_panel_focused();
static void ui_focus_current_panel_window();

static void ui_open_general_panel() {
    s_right_panel_general_open = true;
    s_right_panel_general_base_res = g_sel_res;
    s_right_panel_general_base_cmd = g_sel_cmd;
    s_focus_general_panel_next = true;
}

static void ui_close_general_panel_to_inspector() {
    s_right_panel_general_open = false;
    s_focus_inspector_panel_next = true;
}

static void ui_update_general_panel_selection_autoclose() {
    if (!s_right_panel_general_open)
        return;
    if (g_sel_res == s_right_panel_general_base_res && g_sel_cmd == s_right_panel_general_base_cmd)
        return;
    ui_close_general_panel_to_inspector();
}

struct UiProfilerReadoutCache {
    double next_update_time = -1.0;
    uint64_t app_memory_bytes = 0;
    uint64_t gpu_memory_bytes = 0;
    uint64_t project_gpu_memory_bytes = 0;
    char app_memory[32] = "0 B";
    char gpu_memory[32] = "0 B";
    char project_gpu_memory[32] = "0 B";
};

static UiProfilerReadoutCache s_profiler_readout_cache;
static void ui_draw_basic_monitoring_readout();
static void ui_refresh_profiler_readout_cache(bool force = false);

struct PathCandidate {
    char display[MAX_PATH_LEN];
    char value[MAX_PATH_LEN];
    bool is_dir;
};

struct PathInputResult {
    bool changed;
    bool file_selected;
    bool dir_selected;
    bool submitted;
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


static const char k_ui_path_sep = '/';

// The editor uses forward slashes in every visible and serialized path.
// Win32/DX file APIs still receive normal strings and accept both separators,
// but keeping the UI canonical prevents mixed \\ and / segments while browsing.
static bool ui_path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static void ui_canonicalize_path_separators(char* path, int path_sz) {
    if (!path || path_sz <= 0)
        return;
    for (int i = 0; i < path_sz && path[i]; i++) {
        if (path[i] == '\\')
            path[i] = k_ui_path_sep;
    }
}

static void ui_path_to_win32_pattern(const char* in, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!in)
        return;

    int oi = 0;
    for (int i = 0; in[i] && oi < out_sz - 1; i++)
        out[oi++] = in[i] == '/' ? '\\' : in[i];
    out[oi] = '\0';
}

static void ui_normalize_path_text(const char* in, char* out, int out_sz) {
    if (!out || out_sz <= 0) return;
    out[0] = '\0';
    if (!in || !in[0]) return;

    char sep = k_ui_path_sep;
    int len = (int)strlen(in);
    bool trailing_sep = len > 0 && ui_path_is_sep(in[len - 1]);

    char prefix[MAX_PATH_LEN] = {};
    int pos = 0;
    bool rooted = false;

    if (len >= 2 && in[1] == ':') {
        prefix[0] = in[0];
        prefix[1] = ':';
        prefix[2] = '\0';
        pos = 2;
        if (ui_path_is_sep(in[pos])) {
            prefix[2] = sep;
            prefix[3] = '\0';
            rooted = true;
            while (ui_path_is_sep(in[pos])) pos++;
        }
    } else if (len >= 2 && ui_path_is_sep(in[0]) && ui_path_is_sep(in[1])) {
        // Keep UNC-style paths rooted while still collapsing later . and .. segments.
        prefix[0] = sep;
        prefix[1] = sep;
        prefix[2] = '\0';
        pos = 2;
        rooted = true;
        while (ui_path_is_sep(in[pos])) pos++;
    } else if (ui_path_is_sep(in[0])) {
        prefix[0] = sep;
        prefix[1] = '\0';
        pos = 1;
        rooted = true;
        while (ui_path_is_sep(in[pos])) pos++;
    }

    std::vector<std::string> parts;
    while (pos < len) {
        while (pos < len && ui_path_is_sep(in[pos])) pos++;
        int start = pos;
        while (pos < len && !ui_path_is_sep(in[pos])) pos++;
        int part_len = pos - start;
        if (part_len <= 0) continue;

        std::string part(in + start, in + start + part_len);
        if (part == ".") {
            continue;
        } else if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!rooted) {
                parts.push_back(part);
            }
        } else {
            parts.push_back(part);
        }
    }

    char tmp[MAX_PATH_LEN] = {};
    int written = 0;
    auto append_char = [&](char c) {
        if (written < MAX_PATH_LEN - 1)
            tmp[written++] = c;
    };
    auto append_str = [&](const char* text) {
        if (!text) return;
        while (*text && written < MAX_PATH_LEN - 1)
            tmp[written++] = *text++;
    };

    append_str(prefix);
    bool need_sep = prefix[0] && !ui_path_is_sep(prefix[(int)strlen(prefix) - 1]) && !parts.empty();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (need_sep || (i > 0)) append_char(sep);
        append_str(parts[i].c_str());
        need_sep = false;
    }

    if (trailing_sep && (!parts.empty() || prefix[0])) {
        if (written > 0 && !ui_path_is_sep(tmp[written - 1]))
            append_char(sep);
    }

    tmp[written] = '\0';

    // For relative paths that collapse to the working directory (e.g. "foo/../"),
    // keep the input field clean and empty instead of showing "." or "./".
    strncpy(out, tmp, out_sz - 1);
    out[out_sz - 1] = '\0';
}

static void ui_normalize_path_text_inplace(char* path, int path_sz) {
    if (!path || path_sz <= 0) return;
    char normalized[MAX_PATH_LEN] = {};
    ui_normalize_path_text(path, normalized, MAX_PATH_LEN);
    strncpy(path, normalized, path_sz - 1);
    path[path_sz - 1] = '\0';
}

static bool ui_path_has_extension_ci(const char* path, const char* ext) {
    if (!path || !ext || !path[0] || !ext[0])
        return false;

    const char* dot = strrchr(path, '.');
    return dot && _stricmp(dot, ext) == 0;
}

static void ui_path_seed_if_empty(char* path, int path_sz, const char* default_dir) {
    if (!path || path_sz <= 0 || !default_dir || !default_dir[0] || path[0])
        return;

    strncpy(path, default_dir, path_sz - 1);
    path[path_sz - 1] = '\0';
    ui_normalize_path_text_inplace(path, path_sz);
    int len = (int)strlen(path);
    if (len > 0 && !ui_path_is_sep(path[len - 1]) && len < path_sz - 1) {
        path[len++] = k_ui_path_sep;
        path[len] = '\0';
    }
}

static bool ui_file_exists(const char* path) {
    if (!path || !path[0])
        return false;

    char win32_path[MAX_PATH_LEN] = {};
    ui_path_to_win32_pattern(path, win32_path, MAX_PATH_LEN);
    DWORD attrs = GetFileAttributesA(win32_path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool ui_dir_exists(const char* path) {
    if (!path || !path[0])
        return false;

    char win32_path[MAX_PATH_LEN] = {};
    ui_path_to_win32_pattern(path, win32_path, MAX_PATH_LEN);
    DWORD attrs = GetFileAttributesA(win32_path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void ui_finish_project_load_camera_sync() {
    if (g_camera_controls.mode != CAMERA_MODE_HORIZON_LOCKED)
        return;
    camera_sync_euler_from_quat(&g_camera);
    camera_set_euler(&g_camera, g_camera.yaw, clampf(g_camera.pitch, -1.55334f, 1.55334f), g_camera.roll);
}

static void ui_queue_project_load(const char* path) {
    if (!path || !path[0])
        return;

    strncpy(s_project_load_path, path, MAX_PATH_LEN - 1);
    s_project_load_path[MAX_PATH_LEN - 1] = '\0';
    s_project_file_mode = PROJECT_FILE_NONE;
    s_project_load_pending = true;
    s_project_load_defer_frames = 0;
    s_project_load_active = false;
    s_project_load_failed = false;
    snprintf(s_project_load_status, sizeof(s_project_load_status), "Opening project...");
}

static void ui_execute_pending_project_load_if_ready() {
    if (!s_project_load_pending)
        return;

    // Defer by one fully drawn ImGui frame so the user sees the editor-native
    // loading overlay before the synchronous project parse/reset starts. Large
    // Gaussian splat and NanoVDB payloads still continue through the existing
    // async resource loader after the project file itself has been parsed.
    if (s_project_load_defer_frames <= 0) {
        s_project_load_defer_frames++;
        return;
    }

    char load_path[MAX_PATH_LEN] = {};
    strncpy(load_path, s_project_load_path, MAX_PATH_LEN - 1);
    load_path[MAX_PATH_LEN - 1] = '\0';

    s_project_load_active = true;
    snprintf(s_project_load_status, sizeof(s_project_load_status), "Opening %s", load_path);
    bool ok = project_load_text(load_path);
    s_project_load_pending = false;
    s_project_load_active = false;
    s_project_load_failed = !ok;
    if (ok) {
        snprintf(s_project_load_status, sizeof(s_project_load_status), "Project opened");
        ui_finish_project_load_camera_sync();
        // Loading a project replaces commands/resources but should behave like
        // opening it fresh: transient render targets are cleared and
        // compute_on_reset passes run on the next scene frame. Without this,
        // generated resources such as cloud noise textures could stay black
        // until the user pressed Reset manually.
        app_request_scene_restart();
    } else {
        snprintf(s_project_load_status, sizeof(s_project_load_status), "Project load failed");
    }
}

static void ui_open_project_file_bar(ProjectFileMode mode) {
    s_project_file_mode = mode;
    s_project_path_focus = true;
    s_project_path_open_popup = true;

    const char* current_path = project_current_path();
    if (mode == PROJECT_FILE_SAVE && current_path && current_path[0]) {
        strncpy(s_project_path, current_path, MAX_PATH_LEN - 1);
        s_project_path[MAX_PATH_LEN - 1] = '\0';
        ui_normalize_path_text_inplace(s_project_path, MAX_PATH_LEN);
    } else if (!s_project_path[0] || strcmp(s_project_path, "project.lt") == 0 ||
               (mode == PROJECT_FILE_SAVE && ui_path_is_sep(s_project_path[strlen(s_project_path) - 1]))) {
        strncpy(s_project_path, mode == PROJECT_FILE_SAVE ? "projects/project.lt" : "projects/",
                MAX_PATH_LEN - 1);
        s_project_path[MAX_PATH_LEN - 1] = '\0';
    }
}

static void ui_path_parent_dir(const char* path, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!path || !path[0])
        return;

    const char* slash1 = strrchr(path, '/');
    const char* slash2 = strrchr(path, '\\');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    if (!slash)
        return;

    int len = (int)(slash - path);
    if (len <= 0)
        return;
    if (len >= out_sz)
        len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
    ui_normalize_path_text_inplace(out, out_sz);
}

static bool ui_ensure_directory_tree(const char* dir) {
    if (!dir || !dir[0] || strcmp(dir, ".") == 0)
        return true;

    char clean[MAX_PATH_LEN] = {};
    ui_normalize_path_text(dir, clean, MAX_PATH_LEN);
    if (!clean[0])
        return true;
    if (ui_dir_exists(clean))
        return true;

    // Create every missing segment in order. The editor stores paths with '/',
    // but _mkdir receives the platform form so nested shader folders can be
    // created from a relative path such as shaders/tests/example.hlsl.
    char walk[MAX_PATH_LEN] = {};
    int wi = 0;
    int start = 0;
    int len = (int)strlen(clean);

    if (len >= 2 && clean[1] == ':') {
        walk[0] = clean[0];
        walk[1] = ':';
        walk[2] = '\0';
        wi = 2;
        start = 2;
        if (clean[start] == '/') {
            walk[wi++] = '/';
            walk[wi] = '\0';
            start++;
        }
    } else if (clean[0] == '/') {
        walk[wi++] = '/';
        walk[wi] = '\0';
        start = 1;
    }

    for (int i = start; i <= len; i++) {
        if (clean[i] != '/' && clean[i] != '\0')
            continue;

        int prev_wi = wi;
        for (int j = start; j < i && wi < MAX_PATH_LEN - 1; j++)
            walk[wi++] = clean[j];
        walk[wi] = '\0';
        start = i + 1;

        if (wi == prev_wi || strcmp(walk, ".") == 0 || strcmp(walk, "..") == 0)
            continue;

        if (!ui_dir_exists(walk)) {
            char win32_dir[MAX_PATH_LEN] = {};
            ui_path_to_win32_pattern(walk, win32_dir, MAX_PATH_LEN);
            if (_mkdir(win32_dir) != 0 && !ui_dir_exists(walk))
                return false;
        }

        if (i < len && wi < MAX_PATH_LEN - 1 && walk[wi - 1] != '/') {
            walk[wi++] = '/';
            walk[wi] = '\0';
        }
    }

    return ui_dir_exists(clean);
}

static bool ui_ensure_parent_directory(const char* path) {
    char dir[MAX_PATH_LEN] = {};
    ui_path_parent_dir(path, dir, MAX_PATH_LEN);
    return ui_ensure_directory_tree(dir);
}

static void ui_split_path_for_completion(const char* path, char* dir, int dir_sz,
                                         char* base, int base_sz, char* prefix, int prefix_sz,
                                         char* sep)
{
    dir[0] = base[0] = prefix[0] = '\0';
    *sep = k_ui_path_sep;
    if (!path || !path[0]) {
        strncpy(dir, ".", dir_sz - 1);
        dir[dir_sz - 1] = '\0';
        return;
    }

    int len = (int)strlen(path);
    if (len == 2 && path[1] == ':') {
        snprintf(dir, dir_sz, "%s/", path);
        snprintf(base, base_sz, "%s/", path);
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

    *sep = k_ui_path_sep;
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
    char sep = k_ui_path_sep;
    ui_split_path_for_completion(path, dir, MAX_PATH_LEN, base, MAX_PATH_LEN,
                                 prefix, MAX_PATH_LEN, &sep);

    char pattern[MAX_PATH_LEN] = {};
    int dir_len = (int)strlen(dir);
    if (strcmp(dir, ".") == 0) {
        strncpy(pattern, "*", MAX_PATH_LEN - 1);
    } else if (dir_len > 0 && ui_path_is_sep(dir[dir_len - 1])) {
        snprintf(pattern, MAX_PATH_LEN, "%s*", dir);
    } else {
        snprintf(pattern, MAX_PATH_LEN, "%s/*", dir);
    }
    pattern[MAX_PATH_LEN - 1] = '\0';

    int count = 0;
    WIN32_FIND_DATAA fd = {};
    char win32_pattern[MAX_PATH_LEN] = {};
    ui_path_to_win32_pattern(pattern, win32_pattern, MAX_PATH_LEN);
    HANDLE find = FindFirstFileA(win32_pattern, &fd);
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
        snprintf(c.value, MAX_PATH_LEN, "%s%s%s", base, name, is_dir ? "/" : "");
        c.display[MAX_PATH_LEN - 1] = '\0';
        c.value[MAX_PATH_LEN - 1] = '\0';
        ui_normalize_path_text_inplace(c.value, MAX_PATH_LEN);
    } while (FindNextFileA(find, &fd));

    FindClose(find);
    return count;
}

static void ui_apply_path_candidate(const PathCandidate& c, char* buf, int buf_sz,
                                    PathInputResult* result)
{
    ui_normalize_path_text(c.value, buf, buf_sz);
    result->changed = true;
    result->dir_selected = c.is_dir;
    result->file_selected = !c.is_dir;
}

static PathInputResult ui_path_input_ex(const char* label, char* buf, int buf_sz, const char* ext_filter,
                                        ImGuiInputTextFlags extra_flags = 0,
                                        const char* default_dir = nullptr,
                                        bool force_open_popup = false) {
    static ImGuiID s_refocus_id = 0;
    static ImGuiID s_open_id = 0;
    static ImGuiID s_default_seed_blocked_id = 0;
    static int s_nav_index = 0;

    ImGuiID path_id = ImGui::GetID(label);

    // Seed an empty browser with the preferred folder only as a convenience.
    // Once the user navigates up to the working directory (for example by
    // selecting ../ from projects/), keep the empty/root value instead of
    // forcing the field back to the default folder every frame.
    if (buf && buf_sz > 0 && !buf[0] && default_dir && default_dir[0] &&
        s_default_seed_blocked_id != path_id) {
        ui_path_seed_if_empty(buf, buf_sz, default_dir);
    }
    ui_canonicalize_path_separators(buf, buf_sz);

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
    result.submitted = enter_requested;
    if (result.changed) {
        ui_canonicalize_path_separators(buf, buf_sz);
        if (!buf[0])
            s_default_seed_blocked_id = path_id;
        else if (s_default_seed_blocked_id == path_id)
            s_default_seed_blocked_id = 0;
    }
    bool activated = ImGui::IsItemActivated();
    bool active = ImGui::IsItemActive();
    bool input_focused = ImGui::IsItemFocused();
    ImVec2 input_min = ImGui::GetItemRectMin();
    ImVec2 input_max = ImGui::GetItemRectMax();

    ImGui::PushID(label);
    if (activated || (active && result.changed)) {
        if (s_open_id != path_id)
            s_nav_index = 0;
    }
    if (cb_state.completion_requested || cb_state.nav_delta != 0 || enter_requested ||
        force_open_popup || activated || (active && result.changed)) {
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
                    result.submitted = true;
                    ui_apply_path_candidate(candidates[s_nav_index], buf, buf_sz, &result);
                    if (!buf[0])
                        s_default_seed_blocked_id = path_id;
                    else if (s_default_seed_blocked_id == path_id)
                        s_default_seed_blocked_id = 0;
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
                        if (!buf[0])
                            s_default_seed_blocked_id = path_id;
                        else if (s_default_seed_blocked_id == path_id)
                            s_default_seed_blocked_id = 0;
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

    if (s_default_seed_blocked_id == path_id && !active && s_open_id != path_id &&
        !force_open_popup && !input_focused) {
        s_default_seed_blocked_id = 0;
    }

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

static bool ui_shader_resource_is_compute(const Resource* r) {
    if (!r || r->type != RES_SHADER)
        return false;
    return r->shader_kind == SHADER_PROGRAM_CS;
}

static bool ui_recompile_shader_resource(ResHandle h, Resource* r, const char* path) {
    if (!r || r->type != RES_SHADER) return false;
    bool is_compute = ui_shader_resource_is_compute(r);
    char local_path[MAX_PATH_LEN] = {};
    strncpy(local_path, path ? path : r->path, MAX_PATH_LEN - 1);
    local_path[MAX_PATH_LEN - 1] = '\0';

    bool ok = is_compute
        ? shader_compile_cs(r, local_path, "CSMain")
        : shader_compile_vs_ps(r, local_path, "VSMain", "PSMain");
    ui_sync_commands_for_shader(h);
    // A shader compile should be visible immediately while the scene is paused.
    // Request a redraw and allow matching compute/indirect dispatch commands
    // marked "Only On Reset" to run once without advancing scene time/frame.
    cmd_request_shader_recompute(h);
    app_request_scene_render();
    return ok;
}

static bool ui_reload_mesh_resource(Resource* r, const char* path) {
    if (!r || r->type != RES_MESH || !path || !path[0])
        return false;

    ResHandle owner_h = ui_resource_handle_from_ptr(r);
    if (owner_h == INVALID_HANDLE)
        return false;

    char local_path[MAX_PATH_LEN] = {};
    strncpy(local_path, path, MAX_PATH_LEN - 1);
    local_path[MAX_PATH_LEN - 1] = '\0';

    // Load into a temporary mesh first. The glTF importer creates embedded
    // texture resources as generated children of that temporary mesh. Once the
    // import succeeds, transfer those children to the mesh being edited before
    // deleting the temporary mesh; otherwise the material slots would point at
    // freed textures and show up as "(deleted)" in the inspector.
    ResHandle tmp = res_load_mesh(r->name, local_path);
    if (tmp == INVALID_HANDLE)
        return false;

    Resource* src = res_get(tmp);
    if (src) {
        res_free_generated_children(owner_h);
        res_reassign_generated_children(tmp, owner_h);

        strncpy(r->path, src->path, MAX_PATH_LEN - 1);
        r->path[MAX_PATH_LEN - 1] = '\0';
        res_release_gpu(r);

        r->vb = src->vb; r->ib = src->ib;
        r->vert_count = src->vert_count;
        r->idx_count  = src->idx_count;
        r->vert_stride= src->vert_stride;
        r->mesh_part_count = src->mesh_part_count;
        r->mesh_material_count = src->mesh_material_count;
        memcpy(r->mesh_parts, src->mesh_parts, sizeof(r->mesh_parts));
        memcpy(r->mesh_materials, src->mesh_materials, sizeof(r->mesh_materials));
        r->mesh_bounds_valid = src->mesh_bounds_valid;
        memcpy(r->mesh_bounds_min, src->mesh_bounds_min, sizeof(r->mesh_bounds_min));
        memcpy(r->mesh_bounds_max, src->mesh_bounds_max, sizeof(r->mesh_bounds_max));
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

static float ui_labeled_item_compact_width(const char* label, float max_w = 360.0f, float min_w = 96.0f);

static float ui_current_vertical_scroll_margin(float extra = 8.0f) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window || !window->ScrollbarY)
        return 0.0f;
    return ImGui::GetStyle().ScrollbarSize + ui_margin_px(extra);
}

static bool ui_draw_source_combo(const char* label, int* source) {
    if (!source)
        return false;

    bool changed = false;
    ImGui::SetNextItemWidth(ui_labeled_item_compact_width(label));
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
    ImGui::SetNextItemWidth(ui_labeled_item_compact_width(label));
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

    char* base = out;
    for (char* p = out; *p; ++p) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    char* dot = strrchr(base, '.');
    if (dot)
        *dot = '\0';

    int len = (int)strlen(out);
    if (len <= 0 || len >= out_sz - 1) {
        out[0] = '\0';
        return;
    }
    snprintf(out + len, out_sz - len, "_standalone.exe");
}

static void ui_export_current_project_single_exe() {
    const char* current_project_path = project_current_path();
    if (!current_project_path || !current_project_path[0]) {
        log_warn("Export EXE needs a saved project path first.");
        return;
    }

    // Keep a local copy before saving. project_save_text() updates the current
    // project path, so using the returned static buffer directly here can make
    // later output-path generation depend on a mutable global.
    char project_path[MAX_PATH_LEN] = {};
    strncpy(project_path, current_project_path, MAX_PATH_LEN - 1);
    project_path[MAX_PATH_LEN - 1] = '\0';

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
        focus_path ? ImGuiInputTextFlags_AutoSelectAll : 0,
        "projects", s_project_path_open_popup || focus_path);
    s_project_path_open_popup = false;
    bool path_commit = (path_result.file_selected || path_result.submitted) && !path_result.dir_selected;
    if (path_commit) {
        if (save) {
            project_save_text(s_project_path);
            s_project_file_mode = PROJECT_FILE_NONE;
        } else if (ui_file_exists(s_project_path)) {
            ui_queue_project_load(s_project_path);
        }
    }
    ImGui::SameLine();

    if (ImGui::Button(save ? "Save" : "Load")) {
        if (save)
            project_save_text(s_project_path);
        else
            ui_queue_project_load(s_project_path);
        if (save)
            s_project_file_mode = PROJECT_FILE_NONE;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        s_project_file_mode = PROJECT_FILE_NONE;

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// -- resource combo helper -------------------------------------------------

static bool ui_resource_has_implicit_size_source(const Resource& r) {
    switch (r.type) {
    case RES_TEXTURE2D:
    case RES_RENDER_TEXTURE2D:
    case RES_RENDER_TEXTURE3D:
    case RES_STRUCTURED_BUFFER:
    case RES_GAUSSIAN_SPLAT:
    case RES_NANOVDB:
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
    case RES_BUILTIN_LIGHT:    return "Light";
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
        if (owner->type == RES_STRUCTURED_BUFFER || owner->type == RES_GAUSSIAN_SPLAT || owner->type == RES_NANOVDB)
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
    case RES_GAUSSIAN_SPLAT:      return "GaussianSplat";
    case RES_NANOVDB:             return "NanoVDB";
    case RES_BUILTIN_SHADOW_MAP:  return "DepthTexture2D";
    case RES_BUILTIN_LIGHT:    return "Light";
    case RES_SHADER:
        return r.shader_kind == SHADER_PROGRAM_CS ? "Compute Shader" : "Vertex/Pixel Shader";
    default:                      return res_type_str(r.type);
    }
}

static bool ui_resource_size_source_matches_type(const Resource& owner, ResType type) {
    if (!ui_resource_has_implicit_size_source(owner) || owner.size_handle == INVALID_HANDLE)
        return false;
    Resource* size_res = res_get(owner.size_handle);
    return size_res && size_res->type == type;
}

static bool ui_resource_is_size_source_resource(const Resource& r) {
    return ui_resource_is_implicit_size_resource(r);
}

static void ui_resource_size_source_label(const Resource& owner, const Resource& size_res,
                                          char* out, int out_sz) {
    (void)owner;
    if (!out || out_sz <= 0)
        return;
    snprintf(out, out_sz, "%s (%s)", ui_resource_display_name(size_res), size_res.name);
    out[out_sz - 1] = '\0';
}

static float ui_labeled_item_compact_width(const char* label, float max_w, float min_w) {
    ImGuiStyle& style = ImGui::GetStyle();
    const char* visible_label_end = label ? strstr(label, "##") : nullptr;
    ImVec2 label_size = label && label[0] ? ImGui::CalcTextSize(label, visible_label_end) : ImVec2(0.0f, 0.0f);
    float right_margin = ui_current_vertical_scroll_margin(10.0f);
    float label_reserve = label_size.x > 0.0f ? label_size.x + style.ItemInnerSpacing.x : 0.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    float width = avail - label_reserve - right_margin;
    if (width > ui_px(max_w))
        width = ui_px(max_w);
    if (width < ui_px(min_w))
        width = ui_px(min_w);

    float hard_max = avail - label_reserve - right_margin;
    if (width > hard_max && hard_max > ui_px(64.0f))
        width = hard_max;
    if (width < ui_px(64.0f))
        width = ui_px(64.0f);
    return width;
}

static void res_combo(const char* label, ResHandle* h, ResType filter, bool allow_invalid = true,
                      ResType filter2 = RES_NONE, ResType filter3 = RES_NONE) {
    Resource* cur     = res_get(*h);
    const char* prev  = cur ? ui_resource_display_name(*cur) : "(none)";
    ImGui::SetNextItemWidth(ui_labeled_item_compact_width(label));
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

// -- resources panel -------------------------------------------------------

static void res_combo_render_target(const char* label, ResHandle* h) {
    res_combo(label, h, RES_RENDER_TEXTURE2D, true, RES_BUILTIN_SCENE_COLOR, RES_RENDER_TEXTURE3D);
}

static void res_combo_depth_target(const char* label, ResHandle* h) {
    res_combo(label, h, RES_RENDER_TEXTURE2D, true, RES_BUILTIN_SCENE_DEPTH);
}

static bool ui_shader_matches_program_kind(const Resource& r, ShaderProgramKind kind) {
    if (r.type != RES_SHADER)
        return false;
    return r.shader_kind == kind;
}

static void res_combo_shader_kind(const char* label, ResHandle* h, ShaderProgramKind kind,
                                  bool allow_invalid = true) {
    Resource* cur = res_get(*h);
    if (!cur || !ui_shader_matches_program_kind(*cur, kind))
        *h = INVALID_HANDLE;

    cur = res_get(*h);
    const char* prev = cur ? ui_resource_display_name(*cur) : "(none)";
    ImGui::SetNextItemWidth(ui_labeled_item_compact_width(label));
    if (ImGui::BeginCombo(label, prev)) {
        if (allow_invalid && ImGui::Selectable("(none)", *h == INVALID_HANDLE))
            *h = INVALID_HANDLE;
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active || !ui_shader_matches_program_kind(r, kind))
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

struct UiShadowPreviewCBData {
    UINT layer;
    UINT mode;
    float near_z;
    float far_z;
    UINT width;
    UINT height;
    UINT pad0;
    UINT pad1;
};

static ID3D11Texture2D*           s_shadow_depth_preview_tex = nullptr;
static ID3D11RenderTargetView*    s_shadow_depth_preview_rtv = nullptr;
static ID3D11ShaderResourceView*  s_shadow_depth_preview_srv = nullptr;
static ID3D11VertexShader*        s_shadow_depth_preview_vs = nullptr;
static ID3D11PixelShader*         s_shadow_depth_preview_ps = nullptr;
static ID3D11Buffer*              s_shadow_depth_preview_cb = nullptr;
static int                        s_shadow_depth_preview_w = 0;
static int                        s_shadow_depth_preview_h = 0;

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

static bool ui_resource_is_instance_source_candidate(const Resource& r) {
    if (r.type != RES_INT)
        return false;
    if (!r.is_generated)
        return true;
    return ui_resource_is_implicit_size_resource(r);
}

static ResHandle ui_instance_source_normalize(ResHandle h) {
    Resource* r = res_get(h);
    if (!r)
        return INVALID_HANDLE;
    if (ui_resource_is_instance_source_candidate(*r))
        return h;
    if (r->size_handle == INVALID_HANDLE)
        return INVALID_HANDLE;
    Resource* implicit = res_get(r->size_handle);
    return implicit && ui_resource_is_instance_source_candidate(*implicit) ? r->size_handle : INVALID_HANDLE;
}

static void res_combo_instance_source(const char* label, ResHandle* h) {
    ResHandle normalized = ui_instance_source_normalize(*h);
    if (normalized != *h)
        *h = normalized;
    Resource* cur = res_get(*h);
    const char* prev = cur ? ui_resource_display_name(*cur) : "(hardcoded)";
    ImGui::SetNextItemWidth(ui_labeled_item_compact_width(label));
    if (ImGui::BeginCombo(label, prev)) {
        if (ImGui::Selectable("(hardcoded)", *h == INVALID_HANDLE))
            *h = INVALID_HANDLE;
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active || !ui_resource_is_instance_source_candidate(r))
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

static const char* s_shadow_depth_preview_ps_src = R"HLSL(
cbuffer PreviewCB : register(b0)
{
    uint Layer;
    uint Mode;
    float NearZ;
    float FarZ;
    uint Width;
    uint Height;
    uint Pad0;
    uint Pad1;
};

Texture2DArray<float> ShadowTex : register(t0);

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    uint shadow_w = 0;
    uint shadow_h = 0;
    uint shadow_layers = 0;
    ShadowTex.GetDimensions(shadow_w, shadow_h, shadow_layers);
    if (shadow_w == 0 || shadow_h == 0 || shadow_layers == 0)
        return float4(0.04, 0.04, 0.04, 1.0);

    uint x = min((uint)pos.x, shadow_w - 1);
    uint y = min((uint)pos.y, shadow_h - 1);
    uint layer = min(Layer, shadow_layers - 1);
    float depth01 = ShadowTex.Load(int4((int)x, (int)y, (int)layer, 0));
    if (depth01 >= 0.999999)
        return float4(0.03, 0.03, 0.03, 1.0);

    float normalized = depth01;
    if (Mode != 0) {
        float n = max(NearZ, 1e-5);
        float f = max(FarZ, n + 1e-4);
        float view_depth = (n * f) / max(f - depth01 * (f - n), 1e-6);
        normalized = saturate((view_depth - n) / max(f - n, 1e-5));
    }

    float gray = 1.0 - saturate(normalized);
    return float4(gray, gray, gray, 1.0);
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

static void ui_release_shadow_depth_preview_surface() {
    if (s_shadow_depth_preview_srv) { s_shadow_depth_preview_srv->Release(); s_shadow_depth_preview_srv = nullptr; }
    if (s_shadow_depth_preview_rtv) { s_shadow_depth_preview_rtv->Release(); s_shadow_depth_preview_rtv = nullptr; }
    if (s_shadow_depth_preview_tex) { s_shadow_depth_preview_tex->Release(); s_shadow_depth_preview_tex = nullptr; }
    s_shadow_depth_preview_w = 0;
    s_shadow_depth_preview_h = 0;
}

static void ui_release_shadow_depth_preview_pipeline() {
    ui_release_shadow_depth_preview_surface();
    if (s_shadow_depth_preview_cb) { s_shadow_depth_preview_cb->Release(); s_shadow_depth_preview_cb = nullptr; }
    if (s_shadow_depth_preview_ps) { s_shadow_depth_preview_ps->Release(); s_shadow_depth_preview_ps = nullptr; }
    if (s_shadow_depth_preview_vs) { s_shadow_depth_preview_vs->Release(); s_shadow_depth_preview_vs = nullptr; }
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

static bool ui_init_shadow_depth_preview_pipeline() {
    if (!g_dx.dev || !g_dx.ctx)
        return false;
    if (s_shadow_depth_preview_vs && s_shadow_depth_preview_ps && s_shadow_depth_preview_cb)
        return true;

    ui_release_shadow_depth_preview_pipeline();

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    if (!ui_compile_preview_shader_blob(s_rt3d_preview_vs_src, "VSMain", "vs_5_0", &vs_blob))
        return false;
    if (!ui_compile_preview_shader_blob(s_shadow_depth_preview_ps_src, "PSMain", "ps_5_0", &ps_blob)) {
        vs_blob->Release();
        return false;
    }

    HRESULT hr = g_dx.dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                              nullptr, &s_shadow_depth_preview_vs);
    if (SUCCEEDED(hr))
        hr = g_dx.dev->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                         nullptr, &s_shadow_depth_preview_ps);

    vs_blob->Release();
    ps_blob->Release();

    if (FAILED(hr)) {
        log_error("Shadow depth preview shader create failed: 0x%08X", hr);
        ui_release_shadow_depth_preview_pipeline();
        return false;
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = (UINT)((sizeof(UiShadowPreviewCBData) + 15) & ~15);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_dx.dev->CreateBuffer(&cbd, nullptr, &s_shadow_depth_preview_cb);
    if (FAILED(hr) || !s_shadow_depth_preview_cb) {
        log_error("Shadow depth preview cbuffer create failed: 0x%08X", hr);
        ui_release_shadow_depth_preview_pipeline();
        return false;
    }

    return true;
}

static bool ui_ensure_shadow_depth_preview_surface(int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (s_shadow_depth_preview_tex && s_shadow_depth_preview_w == width && s_shadow_depth_preview_h == height)
        return true;

    ui_release_shadow_depth_preview_surface();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = g_dx.dev->CreateTexture2D(&td, nullptr, &s_shadow_depth_preview_tex);
    if (FAILED(hr) || !s_shadow_depth_preview_tex) {
        log_error("Shadow depth preview texture create failed: 0x%08X", hr);
        ui_release_shadow_depth_preview_surface();
        return false;
    }

    hr = g_dx.dev->CreateRenderTargetView(s_shadow_depth_preview_tex, nullptr, &s_shadow_depth_preview_rtv);
    if (FAILED(hr) || !s_shadow_depth_preview_rtv) {
        log_error("Shadow depth preview RTV create failed: 0x%08X", hr);
        ui_release_shadow_depth_preview_surface();
        return false;
    }

    hr = g_dx.dev->CreateShaderResourceView(s_shadow_depth_preview_tex, nullptr, &s_shadow_depth_preview_srv);
    if (FAILED(hr) || !s_shadow_depth_preview_srv) {
        log_error("Shadow depth preview SRV create failed: 0x%08X", hr);
        ui_release_shadow_depth_preview_surface();
        return false;
    }

    s_shadow_depth_preview_w = width;
    s_shadow_depth_preview_h = height;
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

static ID3D11ShaderResourceView* ui_render_shadow_depth_preview(int width, int height, int layer) {
    if (!g_dx.shadow_srv || width <= 0 || height <= 0)
        return nullptr;
    if (!ui_init_shadow_depth_preview_pipeline())
        return nullptr;
    if (!ui_ensure_shadow_depth_preview_surface(width, height))
        return nullptr;

    if (layer < 0) layer = 0;
    if (g_dx.shadow_layers > 0 && layer >= g_dx.shadow_layers)
        layer = g_dx.shadow_layers - 1;
    if (layer < 0) layer = 0;

    D3D11_MAPPED_SUBRESOURCE ms = {};
    HRESULT hr = g_dx.ctx->Map(s_shadow_depth_preview_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (FAILED(hr)) {
        log_error("Shadow depth preview cbuffer map failed: 0x%08X", hr);
        return nullptr;
    }
    UiShadowPreviewCBData* cb = (UiShadowPreviewCBData*)ms.pData;
    cb->layer = (UINT)layer;
    cb->mode = g_dx.scene_cb_data.light_params[0] >= 0.5f ? 1u : 0u;
    cb->near_z = g_dx.scene_cb_data.shadow_params[1];
    cb->far_z = g_dx.scene_cb_data.shadow_params[2];
    if (cb->mode) {
        if (Resource* dl = res_get(g_builtin_light)) {
            cb->near_z = dl->shadow_near;
            cb->far_z = dl->shadow_far;
        } else {
            cb->near_z = 0.0001f;
            cb->far_z = g_dx.scene_cb_data.light_params[3];
        }
    }
    cb->width = (UINT)width;
    cb->height = (UINT)height;
    cb->pad0 = 0;
    cb->pad1 = 0;
    g_dx.ctx->Unmap(s_shadow_depth_preview_cb, 0);

    ID3D11ShaderResourceView* null_srvs[MAX_SRV_SLOTS] = {};
    ID3D11UnorderedAccessView* null_uavs[MAX_UAV_SLOTS] = {};
    UINT null_counts[MAX_UAV_SLOTS] = {};
    ID3D11RenderTargetView* null_rtv = nullptr;
    g_dx.ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    g_dx.ctx->PSSetShaderResources(0, MAX_SRV_SLOTS, null_srvs);
    g_dx.ctx->CSSetShaderResources(0, MAX_SRV_SLOTS, null_srvs);
    g_dx.ctx->CSSetUnorderedAccessViews(0, MAX_UAV_SLOTS, null_uavs, null_counts);

    float clear[4] = {};
    g_dx.ctx->OMSetRenderTargets(1, &s_shadow_depth_preview_rtv, nullptr);
    g_dx.ctx->ClearRenderTargetView(s_shadow_depth_preview_rtv, clear);

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
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
    g_dx.ctx->VSSetShader(s_shadow_depth_preview_vs, nullptr, 0);
    g_dx.ctx->GSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->HSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->DSSetShader(nullptr, nullptr, 0);
    g_dx.ctx->PSSetShader(s_shadow_depth_preview_ps, nullptr, 0);
    g_dx.ctx->PSSetConstantBuffers(0, 1, &s_shadow_depth_preview_cb);
    ID3D11ShaderResourceView* src_srv = g_dx.shadow_srv;
    g_dx.ctx->PSSetShaderResources(0, 1, &src_srv);
    g_dx.ctx->Draw(3, 0);

    ID3D11ShaderResourceView* null_srv = nullptr;
    g_dx.ctx->PSSetShaderResources(0, 1, &null_srv);
    g_dx.ctx->OMSetRenderTargets(1, &g_dx.back_rtv, nullptr);
    return s_shadow_depth_preview_srv;
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

static bool ui_float_value_editor(const char* label, ResType type, float* fval, float width) {
    ImGui::SetNextItemWidth(width);
    switch (type) {
    case RES_FLOAT:
        return ImGui::SliderFloat(label, &fval[0], 0.0f, 1.0f, "%.3f");
    case RES_FLOAT2:
        return ImGui::SliderFloat2(label, fval, 0.0f, 1.0f, "%.3f");
    case RES_FLOAT3:
        // Use ImGui's native color editor again. The default display mode is set
        // once in ui_init() to Float, so right-click keeps the built-in
        // ColorEdit options menu instead of the custom RGB/float toggle.
        return ImGui::ColorEdit3(label, fval);
    case RES_FLOAT4:
        return ImGui::ColorEdit4(label, fval);
    default:
        return false;
    }
}

static bool ui_user_cb_value_editor(ResType type, int* ival, float* fval, float reserve = 78.0f) {
    float width = reserve > 0.0f ? -reserve : -1.0f;
    switch (type) {
    case RES_FLOAT:
    case RES_FLOAT2:
    case RES_FLOAT3:
    case RES_FLOAT4:
        return ui_float_value_editor("##v", type, fval, width);
    case RES_INT:    ImGui::SetNextItemWidth(width); return ImGui::InputInt("##v",   &ival[0]);
    case RES_INT2:   ImGui::SetNextItemWidth(width); return ImGui::InputInt2("##v",   ival);
    case RES_INT3:   ImGui::SetNextItemWidth(width); return ImGui::InputInt3("##v",   ival);
    default:         ImGui::TextDisabled("(unsupported)"); return false;
    }
}

static void ui_command_param_copy_from_resource(CommandParam* p, const Resource* r) {
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
    } else if (source_kind == USER_CB_SOURCE_LIGHT_POSITION) {
        snprintf(label, sizeof(label), "Light Position");
    } else if (source_kind == USER_CB_SOURCE_LIGHT_TARGET) {
        snprintf(label, sizeof(label), "Light Target");
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
            if (!r.active || r.is_builtin || ui_resource_is_size_source_resource(r) || r.type != p->type) continue;
            ResHandle h = (ResHandle)(i + 1);
            bool sel = source_kind == USER_CB_SOURCE_RESOURCE && p->source == h;
            ImGui::PushID(i);
            if (ImGui::Selectable(ui_resource_display_name(r), sel)) {
                p->source = h;
                p->source_kind = USER_CB_SOURCE_RESOURCE;
                p->source_target[0] = '\0';
                ui_command_param_copy_from_resource(p, &r);
            }
            ImGui::PopID();
        }
        if (p->type == RES_INT || p->type == RES_INT2 || p->type == RES_INT3) {
            ImGui::Separator();
            ImGui::TextDisabled("Resource sizes");
            for (int i = 0; i < MAX_RESOURCES; i++) {
                Resource& owner = g_resources[i];
                if (!owner.active || owner.is_generated || !ui_resource_size_source_matches_type(owner, p->type))
                    continue;
                Resource* size_res = res_get(owner.size_handle);
                if (!size_res)
                    continue;
                ResHandle h = owner.size_handle;
                bool sel = source_kind == USER_CB_SOURCE_RESOURCE && p->source == h;
                char item[192] = {};
                ui_resource_size_source_label(owner, *size_res, item, sizeof(item));
                ImGui::PushID(20000 + i);
                if (ImGui::Selectable(item, sel)) {
                    p->source = h;
                    p->source_kind = USER_CB_SOURCE_RESOURCE;
                    p->source_target[0] = '\0';
                    ui_command_param_copy_from_resource(p, size_res);
                }
                ImGui::PopID();
            }
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
            if (ImGui::Selectable("Light Position", source_kind == USER_CB_SOURCE_LIGHT_POSITION)) {
                p->source = INVALID_HANDLE;
                p->source_kind = USER_CB_SOURCE_LIGHT_POSITION;
                snprintf(p->source_target, MAX_NAME, "light");
            }
            if (ImGui::Selectable("Light Target", source_kind == USER_CB_SOURCE_LIGHT_TARGET)) {
                p->source = INVALID_HANDLE;
                p->source_kind = USER_CB_SOURCE_LIGHT_TARGET;
                snprintf(p->source_target, MAX_NAME, "light");
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

static bool ui_title_case_keep_upper(const char* word, int len) {
    if (!word || len <= 0)
        return false;
    static const char* acronyms[] = {
        "SRV", "UAV", "RTV", "DSV", "GPU", "CPU", "HLSL", "DX11", "MRT", "CB", "CBUFFER"
    };
    for (int i = 0; i < (int)(sizeof(acronyms) / sizeof(acronyms[0])); i++) {
        if ((int)strlen(acronyms[i]) == len && _strnicmp(word, acronyms[i], len) == 0)
            return true;
    }
    return false;
}

static void ui_title_case_label(const char* in, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!in)
        return;

    int oi = 0;
    bool word_start = true;
    for (int i = 0; in[i] && oi < out_sz - 1;) {
        if ((in[i] >= 'A' && in[i] <= 'Z') || (in[i] >= 'a' && in[i] <= 'z') || (in[i] >= '0' && in[i] <= '9')) {
            int start = i;
            while ((in[i] >= 'A' && in[i] <= 'Z') || (in[i] >= 'a' && in[i] <= 'z') || (in[i] >= '0' && in[i] <= '9'))
                i++;
            int len = i - start;
            bool keep_upper = ui_title_case_keep_upper(in + start, len);
            for (int j = 0; j < len && oi < out_sz - 1; j++) {
                char ch = in[start + j];
                if (keep_upper) {
                    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                } else if (word_start && j == 0) {
                    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                } else {
                    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                }
                out[oi++] = ch;
            }
            word_start = false;
        } else {
            out[oi++] = in[i++];
            word_start = true;
        }
    }
    out[oi] = '\0';
}

static bool ui_inspector_section(const char* title, bool default_open = true) {
    char label[128] = {};
    ui_title_case_label(title, label, sizeof(label));

    ImGui::Spacing();
    ImGuiID id = ImGui::GetID(title);
    bool open = ImGui::GetStateStorage()->GetBool(id, default_open);
    float header_h = ImGui::GetTextLineHeight() + ui_margin_px(10.0f);
    float header_w = ImGui::GetContentRegionAvail().x - ui_current_vertical_scroll_margin(8.0f);
    if (header_w < ui_px(48.0f))
        header_w = ui_px(48.0f);
    ImGui::PushID(title);
    ImGui::InvisibleButton("##section_header", ImVec2(header_w, header_h));
    bool hovered = ImGui::IsItemHovered();
    bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        open = !open;
        ImGui::GetStateStorage()->SetBool(id, open);
    }
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = held ? ImVec4(0.205f, 0.138f, 0.124f, 1.00f) :
        (hovered ? ImVec4(0.160f, 0.132f, 0.126f, 1.00f) : ImVec4(0.120f, 0.116f, 0.120f, 1.00f));
    dl->AddRectFilled(min, max, ImGui::GetColorU32(bg), ui_margin_px(4.0f));
    dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.245f, 0.225f, 0.220f, 0.72f)), ui_margin_px(4.0f));
    dl->AddRectFilled(min, ImVec2(min.x + ui_px(2.0f), max.y),
        ImGui::GetColorU32(ImVec4(0.78f, 0.42f, 0.32f, 1.0f)), ui_margin_px(1.5f));
    dl->AddLine(
        ImVec2(min.x, max.y), ImVec2(max.x, max.y),
        ImGui::GetColorU32(ImVec4(0.245f, 0.225f, 0.220f, 0.70f)), 1.0f);

    ImU32 text_col = ImGui::GetColorU32(ImVec4(0.88f, 0.86f, 0.84f, 1.0f));
    ImU32 arrow_col = ImGui::GetColorU32(ImVec4(0.86f, 0.84f, 0.82f, 1.0f));
    float cy = (min.y + max.y) * 0.5f;
    float ax = min.x + ui_px(10.0f);
    float arrow = ui_px(4.2f);
    if (open) {
        dl->AddTriangleFilled(ImVec2(ax - arrow, cy - arrow * 0.55f),
                              ImVec2(ax + arrow, cy - arrow * 0.55f),
                              ImVec2(ax, cy + arrow * 0.75f), arrow_col);
    } else {
        dl->AddTriangleFilled(ImVec2(ax - arrow * 0.55f, cy - arrow),
                              ImVec2(ax - arrow * 0.55f, cy + arrow),
                              ImVec2(ax + arrow * 0.75f, cy), arrow_col);
    }
    const char* display = label[0] ? label : title;
    ImVec2 ts = ImGui::CalcTextSize(display);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.06f,
                ImVec2(min.x + ui_px(20.0f), floorf(cy - ts.y * 0.5f)), text_col, display);

    if (open)
        ImGui::Spacing();
    return open;
}

static void ui_inspector_text_disabled_wrapped(const char* fmt, ...) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    float wrap_pos = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui_current_vertical_scroll_margin(10.0f);
    if (wrap_pos < ImGui::GetCursorPosX() + ui_px(48.0f))
        wrap_pos = ImGui::GetCursorPosX() + ui_px(48.0f);
    ImGui::PushTextWrapPos(wrap_pos);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void ui_hint_text_disabled_wrapped(const char* fmt, ...) {
    if (!s_show_interface_hints)
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    float wrap_pos = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui_current_vertical_scroll_margin(10.0f);
    if (wrap_pos < ImGui::GetCursorPosX() + ui_px(48.0f))
        wrap_pos = ImGui::GetCursorPosX() + ui_px(48.0f);
    ImGui::PushTextWrapPos(wrap_pos);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void ui_hint_text_disabled(const char* fmt, ...) {
    if (!s_show_interface_hints)
        return;
    va_list args;
    va_start(args, fmt);
    ImGui::TextDisabledV(fmt, args);
    va_end(args);
}

static void ui_inspector_note_preview(const char* note, ImVec2 size) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg_col = ImGui::GetColorU32(ImVec4(0.185f, 0.116f, 0.070f, 0.92f));
    ImU32 border_col = ImGui::GetColorU32(ImVec4(0.82f, 0.43f, 0.18f, 0.78f));
    ImU32 text_col = ImGui::GetColorU32(note && note[0]
        ? ImVec4(1.00f, 0.77f, 0.48f, 1.00f)
        : ImVec4(0.92f, 0.58f, 0.34f, 0.72f));

    dl->AddRectFilled(min, max, bg_col, style.FrameRounding);
    dl->AddRect(min, max, border_col, style.FrameRounding);

    const char* display = (note && note[0]) ? note : "Click to add a note.";
    float pad_x = style.FramePadding.x + ui_px(2.0f);
    float pad_y = style.FramePadding.y + ui_px(2.0f);
    ImVec2 text_pos(min.x + pad_x, min.y + pad_y);
    float wrap_width = size.x - pad_x * 2.0f;
    dl->PushClipRect(min, max, true);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.12f,
                text_pos, text_col, display, nullptr, wrap_width);
    dl->PopClipRect();
}

static bool ui_inspector_note_editor(char* note, int note_size, bool* note_open, bool* note_editing) {
    if (!note || note_size <= 0 || !note_open || !note_editing)
        return false;
    if (!s_show_inspector_notes)
        return false;

    if (!ui_inspector_section("NOTES", false)) {
        *note_editing = false;
        return false;
    }
    *note_open = true;

    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    float note_w = ImGui::GetContentRegionAvail().x - ui_current_vertical_scroll_margin(10.0f) - ui_margin_px(6.0f);
    if (note_w < ui_px(24.0f))
        note_w = ui_px(24.0f);
    ImVec2 note_size_px(note_w, ui_px(84.0f));
    if (*note_editing) {
        if (ImGui::GetIO().MouseWheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
            float wheel = ImGui::GetIO().MouseWheel;
            ImGui::SetScrollY(ImGui::GetScrollY() - wheel * ImGui::GetTextLineHeight() * 5.0f);
            *note_editing = false;
            ImGui::ClearActiveID();
        }
    }
    if (*note_editing) {
        ImGui::SetKeyboardFocusHere();
        changed = ImGui::InputTextMultiline("##inspector_note", note, (size_t)note_size,
                                            note_size_px,
                                            ImGuiInputTextFlags_AllowTabInput |
                                            ImGuiInputTextFlags_NoHorizontalScroll |
                                            ImGuiInputTextFlags_WordWrap);
        if (ImGui::IsItemDeactivated())
            *note_editing = false;
    } else {
        ImGui::InvisibleButton("##inspector_note_preview", note_size_px);
        bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        ImVec2 actual_size = ImGui::GetItemRectSize();
        ui_inspector_note_preview(note, actual_size);
        if (clicked)
            *note_editing = true;
    }
    return changed;
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

struct UiShaderUndoState {
    std::string text;
    int         cursor;
    int         select_anchor;
};

struct UiCodeLine {
    const char* begin;
    const char* end;
    int         offset;
};

struct UiShaderSourceEditor {
    ResHandle h;
    char      root_path[MAX_PATH_LEN];
    char      path[MAX_PATH_LEN];
    char*     text;
    size_t    cap;
    int       cursor;
    int       select_anchor;
    int       preferred_col;
    bool      editor_focused;
    bool      dragging_selection;
    bool      cursor_follow;
    bool      ok;
    bool      dirty;
    double    last_edit_time;
    std::vector<UiShaderUndoState> undo_stack;
    std::vector<UiShaderUndoState> redo_stack;
    bool      autocomplete_open;
    int       autocomplete_index;
    int       autocomplete_start;
    std::vector<UiCodeLine> line_cache;
    int       line_cache_max_cols;
    bool      line_cache_dirty;
    bool      viewing_include;
    std::vector<std::string> include_back_stack;
};

static UiShaderSourceEditor s_shader_source_ed = {};
static bool s_shader_editor_floating = false;
static ResHandle s_shader_editor_floating_h = INVALID_HANDLE;

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

static const std::vector<UiCodeLine>& ui_shader_editor_line_cache(UiShaderSourceEditor* ed, int* max_cols) {
    static std::vector<UiCodeLine> s_empty_lines;
    if (max_cols) *max_cols = 0;
    if (!ed || !ed->text) {
        if (s_empty_lines.empty())
            s_empty_lines.push_back({ "", "", 0 });
        return s_empty_lines;
    }

    if (ed->line_cache_dirty || ed->line_cache.empty()) {
        ed->line_cache.clear();
        ed->line_cache_max_cols = 0;
        ui_code_build_lines(ed->text, ed->line_cache, &ed->line_cache_max_cols);
        if (ed->line_cache.empty()) {
            UiCodeLine line = { ed->text, ed->text, 0 };
            ed->line_cache.push_back(line);
        }
        ed->line_cache_dirty = false;
    }
    if (max_cols) *max_cols = ed->line_cache_max_cols;
    return ed->line_cache;
}

static bool ui_shader_path_is_absolute(const char* path) {
    if (!path || !path[0])
        return false;
    if (path[0] == '/' || path[0] == '\\')
        return true;
    return path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static void ui_shader_resolve_include_path(const UiShaderSourceEditor* ed, const char* include_name,
                                           char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';
    if (!include_name || !include_name[0])
        return;

    if (ui_shader_path_is_absolute(include_name)) {
        ui_normalize_path_text(include_name, out, out_sz);
        return;
    }

    char current_dir[MAX_PATH_LEN] = {};
    ui_path_parent_dir(ed ? ed->path : nullptr, current_dir, MAX_PATH_LEN);
    if (current_dir[0]) {
        char candidate[MAX_PATH_LEN] = {};
        snprintf(candidate, sizeof(candidate), "%s/%s", current_dir, include_name);
        ui_normalize_path_text(candidate, out, out_sz);
        if (ui_file_exists(out))
            return;
    }

    char root_dir[MAX_PATH_LEN] = {};
    ui_path_parent_dir(ed ? ed->root_path : nullptr, root_dir, MAX_PATH_LEN);
    if (root_dir[0]) {
        char candidate[MAX_PATH_LEN] = {};
        snprintf(candidate, sizeof(candidate), "%s/%s", root_dir, include_name);
        ui_normalize_path_text(candidate, out, out_sz);
        return;
    }

    ui_normalize_path_text(include_name, out, out_sz);
}

static bool ui_shader_parse_include_line(const UiCodeLine& line, char* out_path, int out_sz,
                                         int* out_start = nullptr, int* out_end = nullptr) {
    if (out_path && out_sz > 0)
        out_path[0] = '\0';
    const char* p = line.begin;
    while (p < line.end && (*p == ' ' || *p == '\t'))
        p++;
    if (p >= line.end || *p != '#')
        return false;
    p++;
    while (p < line.end && (*p == ' ' || *p == '\t'))
        p++;
    const char kw[] = "include";
    for (int i = 0; kw[i]; i++) {
        if (p >= line.end || *p != kw[i])
            return false;
        p++;
    }
    if (p < line.end && ui_ascii_ident_char(*p))
        return false;
    while (p < line.end && (*p == ' ' || *p == '\t'))
        p++;
    if (p >= line.end || (*p != '"' && *p != '<'))
        return false;

    char close = *p == '"' ? '"' : '>';
    p++;
    const char* path_begin = p;
    while (p < line.end && *p != close)
        p++;
    if (p <= path_begin || p >= line.end)
        return false;

    int len = (int)(p - path_begin);
    if (out_path && out_sz > 0) {
        int copy_len = len;
        if (copy_len >= out_sz)
            copy_len = out_sz - 1;
        memcpy(out_path, path_begin, copy_len);
        out_path[copy_len] = '\0';
    }
    if (out_start) *out_start = line.offset + (int)(path_begin - line.begin);
    if (out_end) *out_end = line.offset + (int)(p - line.begin);
    return true;
}

static bool ui_shader_editor_save_current_file(UiShaderSourceEditor* ed);
static bool ui_shader_editor_go_back(UiShaderSourceEditor* ed, ResHandle h, Resource* r);
static bool ui_shader_editor_open_include(UiShaderSourceEditor* ed, const char* include_name);

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
    char path[MAX_PATH_LEN];
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
                const char* path_begin = p;
                while (path_begin > r->compile_err && path_begin[-1] != '\n' && path_begin[-1] != '\r')
                    path_begin--;
                while (*path_begin == ' ' || *path_begin == '\t')
                    path_begin++;
                int path_len = (int)(p - path_begin);
                while (path_len > 0 && (path_begin[path_len - 1] == ' ' || path_begin[path_len - 1] == '\t'))
                    path_len--;
                if (path_len >= MAX_PATH_LEN)
                    path_len = MAX_PATH_LEN - 1;
                memcpy(out->path, path_begin, path_len);
                out->path[path_len] = '\0';
                ui_normalize_path_text_inplace(out->path, MAX_PATH_LEN);
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

static bool ui_shader_error_marker_matches_file(const UiShaderSourceEditor* ed, const Resource* r,
                                                const UiShaderErrorMarker* marker) {
    if (!ed || !r || !marker)
        return false;

    char error_path[MAX_PATH_LEN] = {};
    if (marker->path[0])
        ui_shader_resolve_include_path(ed, marker->path, error_path, MAX_PATH_LEN);
    else
        ui_normalize_path_text(r->path, error_path, MAX_PATH_LEN);

    char edit_path[MAX_PATH_LEN] = {};
    ui_normalize_path_text(ed->path, edit_path, MAX_PATH_LEN);
    return error_path[0] && edit_path[0] && strcmp(error_path, edit_path) == 0;
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
    ed->line_cache_dirty = true;
    ed->last_edit_time = ImGui::GetTime();
}

static bool ui_shader_editor_reserve(UiShaderSourceEditor* ed, size_t need);

static UiShaderUndoState ui_shader_editor_make_state(const UiShaderSourceEditor* ed) {
    UiShaderUndoState st;
    st.text = (ed && ed->text) ? ed->text : "";
    st.cursor = ed ? ed->cursor : 0;
    st.select_anchor = ed ? ed->select_anchor : 0;
    return st;
}

static void ui_shader_editor_trim_undo_stack(std::vector<UiShaderUndoState>& stack) {
    const size_t max_states = 128;
    if (stack.size() > max_states)
        stack.erase(stack.begin(), stack.begin() + (stack.size() - max_states));
}

static void ui_shader_editor_push_undo(UiShaderSourceEditor* ed) {
    if (!ed || !ed->text)
        return;

    UiShaderUndoState st = ui_shader_editor_make_state(ed);
    if (!ed->undo_stack.empty()) {
        const UiShaderUndoState& last = ed->undo_stack.back();
        if (last.text == st.text && last.cursor == st.cursor && last.select_anchor == st.select_anchor)
            return;
    }

    ed->undo_stack.push_back(st);
    ui_shader_editor_trim_undo_stack(ed->undo_stack);
    ed->redo_stack.clear();
}

static bool ui_shader_editor_restore_state(UiShaderSourceEditor* ed, const UiShaderUndoState& st) {
    if (!ed)
        return false;
    if (!ui_shader_editor_reserve(ed, st.text.size() + 1))
        return false;
    memcpy(ed->text, st.text.c_str(), st.text.size() + 1);
    ed->cursor = ui_code_clamp_offset(ed, st.cursor);
    ed->select_anchor = ui_code_clamp_offset(ed, st.select_anchor);
    ed->preferred_col = -1;
    ed->cursor_follow = true;
    ed->autocomplete_open = false;
    ui_shader_editor_mark_dirty(ed);
    return true;
}

static bool ui_shader_editor_undo(UiShaderSourceEditor* ed) {
    if (!ed || ed->undo_stack.empty())
        return false;
    UiShaderUndoState current = ui_shader_editor_make_state(ed);
    UiShaderUndoState target = ed->undo_stack.back();
    ed->undo_stack.pop_back();
    ed->redo_stack.push_back(current);
    ui_shader_editor_trim_undo_stack(ed->redo_stack);
    return ui_shader_editor_restore_state(ed, target);
}

static bool ui_shader_editor_redo(UiShaderSourceEditor* ed) {
    if (!ed || ed->redo_stack.empty())
        return false;
    UiShaderUndoState current = ui_shader_editor_make_state(ed);
    UiShaderUndoState target = ed->redo_stack.back();
    ed->redo_stack.pop_back();
    ed->undo_stack.push_back(current);
    ui_shader_editor_trim_undo_stack(ed->undo_stack);
    return ui_shader_editor_restore_state(ed, target);
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

static bool ui_shader_editor_delete_range(UiShaderSourceEditor* ed, int a, int b, bool record_undo = true) {
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
    if (record_undo)
        ui_shader_editor_push_undo(ed);
    ed->autocomplete_open = false;
    ed->preferred_col = -1;
    memmove(ed->text + a, ed->text + b, (size_t)(len - b + 1));
    ed->cursor = a;
    ed->select_anchor = a;
    ed->cursor_follow = true;
    ui_shader_editor_mark_dirty(ed);
    return true;
}

static bool ui_shader_editor_delete_selection(UiShaderSourceEditor* ed, bool record_undo = true) {
    if (!ui_shader_editor_has_selection(ed))
        return false;
    int a = 0, b = 0;
    ui_shader_editor_selection(ed, &a, &b);
    return ui_shader_editor_delete_range(ed, a, b, record_undo);
}

static bool ui_shader_editor_insert_bytes(UiShaderSourceEditor* ed, const char* text, int insert_len, bool record_undo = true) {
    if (!ed || !text || insert_len <= 0)
        return false;
    if (!ed->text) {
        if (!ui_shader_editor_reserve(ed, 8192))
            return false;
        ed->text[0] = '\0';
    }
    if (record_undo)
        ui_shader_editor_push_undo(ed);
    if (ui_shader_editor_has_selection(ed))
        ui_shader_editor_delete_selection(ed, false);

    int len = ui_code_text_len(ed);
    ed->cursor = ui_code_clamp_offset(ed, ed->cursor);
    if (!ui_shader_editor_reserve(ed, (size_t)len + (size_t)insert_len + 1))
        return false;

    memmove(ed->text + ed->cursor + insert_len, ed->text + ed->cursor,
            (size_t)(len - ed->cursor + 1));
    memcpy(ed->text + ed->cursor, text, (size_t)insert_len);
    ed->cursor += insert_len;
    ed->select_anchor = ed->cursor;
    ed->preferred_col = -1;
    ed->cursor_follow = true;
    ed->autocomplete_open = false;
    ui_shader_editor_mark_dirty(ed);
    return true;
}

static bool ui_shader_editor_replace_range(UiShaderSourceEditor* ed, int a, int b, const char* text) {
    if (!ed || !text)
        return false;
    int len = ui_code_text_len(ed);
    if (a < 0) a = 0;
    if (b > len) b = len;
    if (a > b) {
        int tmp = a;
        a = b;
        b = tmp;
    }
    ui_shader_editor_push_undo(ed);
    ed->cursor = b;
    ed->select_anchor = a;
    if (!ui_shader_editor_delete_selection(ed, false) && a != b)
        return false;
    return ui_shader_editor_insert_bytes(ed, text, (int)strlen(text), false);
}

static void ui_shader_editor_set_cursor(UiShaderSourceEditor* ed, int cursor, bool extend_selection,
                                        bool keep_preferred_col = false) {
    if (!ed)
        return;
    ed->cursor = ui_code_clamp_offset(ed, cursor);
    if (!extend_selection)
        ed->select_anchor = ed->cursor;
    if (!keep_preferred_col)
        ed->preferred_col = -1;
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

static char ui_ascii_lower_char(char c) {
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');
    return c;
}

static bool ui_ascii_starts_with_ci(const char* word, const char* prefix, int prefix_len) {
    if (!word || !prefix || prefix_len <= 0)
        return false;
    for (int i = 0; i < prefix_len; i++) {
        if (!word[i])
            return false;
        if (ui_ascii_lower_char(word[i]) != ui_ascii_lower_char(prefix[i]))
            return false;
    }
    return true;
}

static const char* const k_hlsl_completion_words[] = {
    "AppendStructuredBuffer", "BlendState", "Buffer", "ByteAddressBuffer", "ComputeShader",
    "ConsumeStructuredBuffer", "DepthStencilState", "DomainShader", "GeometryShader",
    "HullShader", "InputPatch", "LineStream", "OutputPatch", "PixelShader", "PointStream",
    "RasterizerState", "RenderTargetView", "RWBuffer", "RWByteAddressBuffer",
    "RWStructuredBuffer", "RWTexture1D", "RWTexture1DArray", "RWTexture2D",
    "RWTexture2DArray", "RWTexture3D", "SamplerComparisonState", "SamplerState",
    "StructuredBuffer", "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray",
    "Texture3D", "TextureCube", "TextureCubeArray", "TriangleStream", "VertexShader",
    "SV_ClipDistance", "SV_CullDistance", "SV_Coverage", "SV_Depth", "SV_DepthGreaterEqual",
    "SV_DepthLessEqual", "SV_DispatchThreadID", "SV_DomainLocation", "SV_GroupID",
    "SV_GroupIndex", "SV_GroupThreadID", "SV_GSInstanceID", "SV_InnerCoverage",
    "SV_InstanceID", "SV_IsFrontFace", "SV_OutputControlPointID", "SV_Position",
    "SV_PrimitiveID", "SV_RenderTargetArrayIndex", "SV_SampleIndex", "SV_StencilRef",
    "SV_Target", "SV_Target0", "SV_Target1", "SV_Target2", "SV_Target3", "SV_VertexID",
    "SV_ViewportArrayIndex", "POSITION", "NORMAL", "TANGENT", "BINORMAL", "TEXCOORD0",
    "TEXCOORD1", "TEXCOORD2", "TEXCOORD3", "COLOR", "COLOR0", "COLOR1",
    "bool", "bool2", "bool3", "bool4", "break", "case", "cbuffer", "centroid",
    "class", "column_major", "const", "continue", "default", "discard", "do", "double",
    "else", "false", "float", "float2", "float2x2", "float2x3", "float2x4",
    "float3", "float3x2", "float3x3", "float3x4", "float4", "float4x2",
    "float4x3", "float4x4", "for", "groupshared", "half", "half2", "half3",
    "half4", "if", "in", "inline", "inout", "int", "int2", "int3", "int4",
    "linear", "matrix", "namespace", "nointerpolation", "numthreads", "out", "packoffset",
    "precise", "register", "return", "row_major", "sampler", "shared", "static", "struct",
    "switch", "true", "tbuffer", "uint", "uint2", "uint3", "uint4", "uniform",
    "void", "volatile", "while",
    "abort", "abs", "acos", "all", "AllMemoryBarrier", "AllMemoryBarrierWithGroupSync",
    "any", "asdouble", "asfloat", "asin", "asint", "asuint", "atan", "atan2", "ceil",
    "CheckAccessFullyMapped", "clamp", "clip", "cos", "cosh", "countbits", "cross",
    "D3DCOLORtoUBYTE4", "ddx", "ddx_coarse", "ddx_fine", "ddy", "ddy_coarse",
    "ddy_fine", "degrees", "determinant", "DeviceMemoryBarrier",
    "DeviceMemoryBarrierWithGroupSync", "distance", "dot", "dst", "EvaluateAttributeAtCentroid",
    "EvaluateAttributeAtSample", "EvaluateAttributeSnapped", "exp", "exp2", "f16tof32",
    "f32tof16", "faceforward", "firstbithigh", "firstbitlow", "floor", "fma", "fmod",
    "frac", "frexp", "fwidth", "GetRenderTargetSampleCount", "GetRenderTargetSamplePosition",
    "GroupMemoryBarrier", "GroupMemoryBarrierWithGroupSync", "InterlockedAdd", "InterlockedAnd",
    "InterlockedCompareExchange", "InterlockedCompareStore", "InterlockedExchange", "InterlockedMax",
    "InterlockedMin", "InterlockedOr", "InterlockedXor", "isfinite", "isinf", "isnan",
    "ldexp", "length", "lerp", "lit", "log", "log10", "log2", "mad", "max", "min",
    "modf", "mul", "noise", "normalize", "pow", "printf", "Process2DQuadTessFactorsAvg",
    "Process2DQuadTessFactorsMax", "Process2DQuadTessFactorsMin", "ProcessIsolineTessFactors",
    "ProcessQuadTessFactorsAvg", "ProcessQuadTessFactorsMax", "ProcessQuadTessFactorsMin",
    "ProcessTriTessFactorsAvg", "ProcessTriTessFactorsMax", "ProcessTriTessFactorsMin",
    "radians", "rcp", "reflect", "refract", "reversebits", "round", "rsqrt", "saturate",
    "sign", "sin", "sincos", "sinh", "smoothstep", "sqrt", "step", "tan", "tanh",
    "tex1D", "tex1Dbias", "tex1Dgrad", "tex1Dlod", "tex1Dproj", "tex2D", "tex2Dbias",
    "tex2Dgrad", "tex2Dlod", "tex2Dproj", "tex3D", "tex3Dbias", "tex3Dgrad",
    "tex3Dlod", "tex3Dproj", "texCUBE", "texCUBEbias", "texCUBEgrad", "texCUBElod",
    "texCUBEproj", "transpose", "trunc",
    "LTPBRMaterial", "LTPBRLighting", "LTAtmosphereParams", "LTRay", "LTRaymarchParams", "LTRaymarchHit",
    "lt_pbr_material", "lt_pbr_brdf_direct", "lt_pbr_ibl", "lt_pbr_shade", "lt_pbr_default_directional",
    "lt_pbr_default_light_dir_ws", "lt_pbr_default_light_radiance", "lt_pbr_unpack_normal",
    "lt_raymarch_camera_ray", "lt_raymarch_camera_ray_near_plane", "lt_raymarch_default_params",
    "lt_raymarch_trace", "lt_raymarch_estimate_normal", "lt_raymarch_soft_shadow", "lt_raymarch_ambient_occlusion",
    "lt_sdf_sphere", "lt_sdf_box", "lt_sdf_round_box", "lt_sdf_plane", "lt_sdf_torus", "lt_sdf_capsule",
    "lt_sdf_union", "lt_sdf_smooth_union", "lt_sdf_subtraction", "lt_sdf_intersection", "lt_rotate2d",
    "lt_atmosphere_default_params", "lt_atmosphere_sky", "lt_atmosphere_ambient_diffuse",
    "lt_atmosphere_ambient_specular", "lt_atmosphere_apply_fog", "lt_atmosphere_tonemap",
    "common_pbr.hlsl", "common_raymarch.hlsl", "common_atmosphere.hlsl"
};

static int ui_shader_autocomplete_prefix(const UiShaderSourceEditor* ed, int* out_start, char* out, int out_sz) {
    if (out_start) *out_start = 0;
    if (out && out_sz > 0) out[0] = '\0';
    if (!ed || !ed->text || out_sz <= 0)
        return 0;

    int cursor = ui_code_clamp_offset(ed, ed->cursor);
    int start = cursor;
    while (start > 0 && ui_ascii_ident_char(ed->text[start - 1]))
        start--;
    int len = cursor - start;
    if (len <= 0 || !ui_ascii_ident_start(ed->text[start]))
        return 0;
    if (len >= out_sz)
        len = out_sz - 1;
    memcpy(out, ed->text + start, (size_t)len);
    out[len] = '\0';
    if (out_start) *out_start = start;
    return len;
}

static int ui_shader_autocomplete_count(const char* prefix, int prefix_len) {
    if (!prefix || prefix_len <= 0)
        return 0;
    int count = 0;
    for (int i = 0; i < (int)(sizeof(k_hlsl_completion_words) / sizeof(k_hlsl_completion_words[0])); i++) {
        const char* w = k_hlsl_completion_words[i];
        if (ui_ascii_starts_with_ci(w, prefix, prefix_len) && !(strlen(w) == (size_t)prefix_len && strncmp(w, prefix, (size_t)prefix_len) == 0))
            count++;
    }
    return count;
}

static const char* ui_shader_autocomplete_nth(const char* prefix, int prefix_len, int nth) {
    if (!prefix || prefix_len <= 0)
        return nullptr;
    int count = 0;
    for (int i = 0; i < (int)(sizeof(k_hlsl_completion_words) / sizeof(k_hlsl_completion_words[0])); i++) {
        const char* w = k_hlsl_completion_words[i];
        if (ui_ascii_starts_with_ci(w, prefix, prefix_len) && !(strlen(w) == (size_t)prefix_len && strncmp(w, prefix, (size_t)prefix_len) == 0)) {
            if (count == nth)
                return w;
            count++;
        }
    }
    return nullptr;
}

static bool ui_shader_autocomplete_refresh(UiShaderSourceEditor* ed, bool open_when_possible) {
    if (!ed)
        return false;
    char prefix[64] = {};
    int start = 0;
    bool was_open = ed->autocomplete_open;
    int prefix_len = ui_shader_autocomplete_prefix(ed, &start, prefix, sizeof(prefix));
    int count = ui_shader_autocomplete_count(prefix, prefix_len);
    int min_prefix = was_open ? 1 : 2;
    if (prefix_len < min_prefix || count <= 0 || (!open_when_possible && !ed->autocomplete_open)) {
        ed->autocomplete_open = false;
        ed->autocomplete_index = 0;
        ed->autocomplete_start = start;
        return false;
    }
    ed->autocomplete_open = true;
    ed->autocomplete_start = start;
    if (ed->autocomplete_index < 0) ed->autocomplete_index = 0;
    if (ed->autocomplete_index >= count) ed->autocomplete_index = count - 1;
    return true;
}

static bool ui_shader_autocomplete_accept(UiShaderSourceEditor* ed) {
    if (!ed || !ed->autocomplete_open)
        return false;
    char prefix[64] = {};
    int start = 0;
    int prefix_len = ui_shader_autocomplete_prefix(ed, &start, prefix, sizeof(prefix));
    int count = ui_shader_autocomplete_count(prefix, prefix_len);
    if (count <= 0) {
        ed->autocomplete_open = false;
        return false;
    }
    if (ed->autocomplete_index < 0) ed->autocomplete_index = 0;
    if (ed->autocomplete_index >= count) ed->autocomplete_index = count - 1;
    const char* word = ui_shader_autocomplete_nth(prefix, prefix_len, ed->autocomplete_index);
    if (!word) {
        ed->autocomplete_open = false;
        return false;
    }
    ed->autocomplete_open = false;
    return ui_shader_editor_replace_range(ed, start, start + prefix_len, word);
}

static void ui_shader_autocomplete_draw(UiShaderSourceEditor* ed, ImDrawList* dl,
                                        ImVec2 pos, float line_h, float char_w, const ImRect& clip) {
    if (!ed || !ed->autocomplete_open || !dl)
        return;
    char prefix[64] = {};
    int start = 0;
    int prefix_len = ui_shader_autocomplete_prefix(ed, &start, prefix, sizeof(prefix));
    int count = ui_shader_autocomplete_count(prefix, prefix_len);
    if (count <= 0) {
        ed->autocomplete_open = false;
        return;
    }
    if (ed->autocomplete_index < 0) ed->autocomplete_index = 0;
    if (ed->autocomplete_index >= count) ed->autocomplete_index = count - 1;

    int visible = count < 8 ? count : 8;
    int first = 0;
    if (ed->autocomplete_index >= visible)
        first = ed->autocomplete_index - visible + 1;
    float w = ui_px(260.0f);
    for (int i = 0; i < visible; i++) {
        const char* word = ui_shader_autocomplete_nth(prefix, prefix_len, first + i);
        if (word) {
            float tw = ImGui::CalcTextSize(word).x + ui_px(22.0f);
            if (tw > w) w = tw;
        }
    }
    float h = (float)visible * line_h + ui_px(8.0f);
    if (pos.x + w > clip.Max.x - ui_px(4.0f))
        pos.x = clip.Max.x - w - ui_px(4.0f);
    if (pos.y + h > clip.Max.y - ui_px(4.0f))
        pos.y -= h + line_h;
    if (pos.x < clip.Min.x + ui_px(4.0f))
        pos.x = clip.Min.x + ui_px(4.0f);
    if (pos.y < clip.Min.y + ui_px(4.0f))
        pos.y = clip.Min.y + ui_px(4.0f);

    ImVec2 min = pos;
    ImVec2 max = ImVec2(pos.x + w, pos.y + h);
    dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.075f, 0.070f, 0.075f, 0.98f)), ui_px(5.0f));
    dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.38f, 0.32f, 0.28f, 1.0f)), ui_px(5.0f));
    for (int i = 0; i < visible; i++) {
        const char* word = ui_shader_autocomplete_nth(prefix, prefix_len, first + i);
        if (!word)
            continue;
        float y = pos.y + ui_px(4.0f) + (float)i * line_h;
        if (first + i == ed->autocomplete_index) {
            dl->AddRectFilled(ImVec2(pos.x + ui_px(3.0f), y),
                              ImVec2(pos.x + w - ui_px(3.0f), y + line_h),
                              ImGui::GetColorU32(ImVec4(0.34f, 0.18f, 0.10f, 0.95f)), ui_px(3.0f));
        }
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(pos.x + ui_px(10.0f), y),
                    ImGui::GetColorU32(ImVec4(0.88f, 0.84f, 0.78f, 1.0f)), word);
    }
    if (count > visible) {
        char more[32] = {};
        snprintf(more, sizeof(more), "+%d", count - visible);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(max.x - ImGui::CalcTextSize(more).x - ui_px(8.0f), max.y - line_h - ui_px(1.0f)),
                    ImGui::GetColorU32(ImVec4(0.62f, 0.58f, 0.54f, 1.0f)), more);
    }
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
    const UiCodeLine& cur_line = lines[line_i];
    const char* cursor_ptr = cur_line.begin + (ed->cursor - cur_line.offset);
    if (cursor_ptr < cur_line.begin) cursor_ptr = cur_line.begin;
    if (cursor_ptr > cur_line.end) cursor_ptr = cur_line.end;

    if (ed->preferred_col < 0)
        ed->preferred_col = ui_code_visual_cols_to(cur_line.begin, cursor_ptr);

    ui_shader_editor_set_cursor(ed, ui_code_offset_from_col(lines[next_i], ed->preferred_col),
                                extend_selection, true);
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

static int ui_utf8_char_len(const char* p, const char* end) {
    if (!p || p >= end)
        return 0;
    unsigned char c = (unsigned char)*p;
    int len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (p + len > end)
        len = 1;
    return len;
}

static float ui_code_glyph_advance(ImFont* font, float font_size,
                                   const char* begin, const char* end,
                                   float fallback_w) {
    if (!font || !begin || begin >= end)
        return fallback_w;
    float adv = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, begin, end).x;
    if (adv <= 0.0f)
        adv = fallback_w;
    return adv;
}

static float ui_code_char_advance(ImFont* font, float font_size,
                                  float x, float line_x,
                                  const char* p, const char* end,
                                  float char_w) {
    if (!p || p >= end)
        return 0.0f;
    if (*p == '\t') {
        int col = (int)((x - line_x) / char_w + 0.01f);
        return line_x + (float)ui_code_tab_next_col(col) * char_w - x;
    }
    if (*p == ' ')
        return char_w;

    int glyph_len = ui_utf8_char_len(p, end);
    const char* next = p + (glyph_len > 0 ? glyph_len : 1);
    return ui_code_glyph_advance(font, font_size, p, next, char_w);
}

static float ui_code_x_from_ptr(const char* begin, const char* at, float char_w) {
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();
    float x = 0.0f;
    for (const char* p = begin; p && p < at; ) {
        int glyph_len = (*p == '\t' || *p == ' ') ? 1 : ui_utf8_char_len(p, at);
        const char* next = p + (glyph_len > 0 ? glyph_len : 1);
        x += ui_code_char_advance(font, font_size, x, 0.0f, p, next, char_w);
        p = next;
    }
    return x;
}

static float ui_code_line_width_px(const UiCodeLine& line, float char_w) {
    return ui_code_x_from_ptr(line.begin, line.end, char_w);
}

static int ui_code_offset_from_x(const UiCodeLine& line, float local_x, float char_w) {
    if (local_x <= 0.0f)
        return line.offset;

    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();
    float x = 0.0f;
    const char* p = line.begin;
    while (p < line.end) {
        int glyph_len = (*p == '\t' || *p == ' ') ? 1 : ui_utf8_char_len(p, line.end);
        const char* next = p + (glyph_len > 0 ? glyph_len : 1);
        float adv = ui_code_char_advance(font, font_size, x, 0.0f, p, next, char_w);
        if (local_x < x + adv * 0.5f)
            break;
        x += adv;
        p = next;
    }
    return line.offset + (int)(p - line.begin);
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

        int glyph_len = ui_utf8_char_len(p, end);
        const char* next = p + (glyph_len > 0 ? glyph_len : 1);
        dl->AddText(font, font_size, ImVec2(x, y), color, p, next);
        x += ui_code_glyph_advance(font, font_size, p, next, char_w);
        p = next;
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
    return ui_code_offset_from_x(lines[line_i], local_x, char_w);
}

static bool ui_shader_code_editor_handle_input(UiShaderSourceEditor* ed,
                                               const std::vector<UiCodeLine>& lines) {
    if (!ed || !ed->editor_focused)
        return false;

    ImGuiIO& io = ImGui::GetIO();
    bool changed = false;
    bool shift = io.KeyShift;
    bool ctrl = io.KeyCtrl;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
        ed->autocomplete_open = false;
        return ui_shader_editor_redo(ed);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
        ed->autocomplete_open = false;
        return ui_shader_editor_undo(ed);
    }
    ed->autocomplete_open = false;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        ed->select_anchor = 0;
        ed->cursor = ui_code_text_len(ed);
        ed->preferred_col = -1;
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
    if (!shift && ImGui::IsKeyPressed(ImGuiKey_Tab, true))
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

    int max_cols = 0;
    const std::vector<UiCodeLine>* lines_ptr = &ui_shader_editor_line_cache(ed, &max_cols);
    const std::vector<UiCodeLine>& lines = *lines_ptr;

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
        ImGui::SetKeyOwner(ImGuiKey_Escape, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_Space, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_Z, editor_id);
        ImGui::SetKeyOwner(ImGuiKey_Y, editor_id);
        changed |= ui_shader_code_editor_handle_input(ed, lines);
        if (changed) {
            lines_ptr = &ui_shader_editor_line_cache(ed, &max_cols);
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

    float max_line_w = 0.0f;
    for (int i = 0; i < (int)lines.size(); i++) {
        float w = ui_code_line_width_px(lines[i], char_w);
        if (w > max_line_w)
            max_line_w = w;
    }
    float content_w = gutter_w + max_line_w + char_w * 8.0f;
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
        float cursor_x = gutter_w;
        if (!lines.empty()) {
            const UiCodeLine& line = lines[cursor_line];
            const char* cursor_ptr = line.begin + (ed->cursor - line.offset);
            if (cursor_ptr < line.begin) cursor_ptr = line.begin;
            if (cursor_ptr > line.end) cursor_ptr = line.end;
            cursor_x += ui_code_x_from_ptr(line.begin, cursor_ptr, char_w);
        }
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
    has_error_marker = has_error_marker && ui_shader_error_marker_matches_file(ed, shader_resource, &error_marker);
    char include_to_open[MAX_PATH_LEN] = {};

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
            float line_w = ui_code_line_width_px(line, char_w);
            if (line_w < char_w)
                line_w = char_w;
            float underline_x1 = text_x + line_w;
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
                    float x0 = origin.x + gutter_w + ui_code_x_from_ptr(line.begin, pa, char_w);
                    float x1 = origin.x + gutter_w + ui_code_x_from_ptr(line.begin, pb, char_w);
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

        char include_name[MAX_PATH_LEN] = {};
        int include_start = 0;
        int include_end = 0;
        if (ui_shader_parse_include_line(line, include_name, MAX_PATH_LEN, &include_start, &include_end)) {
            char include_path[MAX_PATH_LEN] = {};
            ui_shader_resolve_include_path(ed, include_name, include_path, MAX_PATH_LEN);
            bool include_exists = ui_file_exists(include_path);
            const char* path_begin = ed->text + include_start;
            const char* path_end = ed->text + include_end;
            float x0 = origin.x + gutter_w + ui_code_x_from_ptr(line.begin, path_begin, char_w);
            float x1 = origin.x + gutter_w + ui_code_x_from_ptr(line.begin, path_end, char_w);
            if (x1 <= x0)
                x1 = x0 + char_w;

            ImVec4 include_col_v = include_exists
                ? ImVec4(0.96f, 0.54f, 0.24f, 1.0f)
                : ImVec4(0.34f, 0.86f, 0.46f, 1.0f);
            ImU32 include_col = ImGui::GetColorU32(include_col_v);
            char include_label[MAX_PATH_LEN] = {};
            int include_label_len = include_end - include_start;
            if (include_label_len >= MAX_PATH_LEN)
                include_label_len = MAX_PATH_LEN - 1;
            memcpy(include_label, path_begin, include_label_len);
            include_label[include_label_len] = '\0';
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x0, y), include_col, include_label);

            ImVec2 link_min(x0, y);
            ImVec2 link_max(x1, y + line_h);
            bool include_hovered = content_hovered && ImGui::IsMouseHoveringRect(link_min, link_max, false);
            dl->AddLine(ImVec2(x0, y + line_h - ui_px(1.0f)),
                        ImVec2(x1, y + line_h - ui_px(1.0f)), include_col,
                        include_hovered ? ui_px(1.6f) : ui_px(1.0f));
            if (include_hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s include: %s\n%s", include_exists ? "Open" : "Create",
                                  include_name, include_path[0] ? include_path : include_name);
            }
            if (include_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                strncpy(include_to_open, include_name, MAX_PATH_LEN - 1);
                include_to_open[MAX_PATH_LEN - 1] = '\0';
                ed->dragging_selection = false;
            }
        }
    }

    if (ed->editor_focused) {
        double t = ImGui::GetTime();
        if (fmod(t, 1.2) < 0.8) {
            float x = origin.x + gutter_w;
            if (!lines.empty()) {
                const UiCodeLine& line = lines[cursor_line];
                const char* cursor_ptr = line.begin + (ed->cursor - line.offset);
                if (cursor_ptr < line.begin) cursor_ptr = line.begin;
                if (cursor_ptr > line.end) cursor_ptr = line.end;
                x += ui_code_x_from_ptr(line.begin, cursor_ptr, char_w);
            }
            float y = origin.y + (float)cursor_line * line_h;
            dl->AddLine(ImVec2(x, y + 1.0f),
                        ImVec2(x, y + line_h - 1.0f),
                        ImGui::GetColorU32(ImGuiCol_InputTextCursor), 1.0f);
        }
    }

    dl->PopClipRect();

    if (include_to_open[0])
        ui_shader_editor_open_include(ed, include_to_open);

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(content_w, content_h));
    ImGui::EndChild();
    if (s_code_font)
        ImGui::PopFont();
    ImGui::PopStyleColor();
    return changed;
}

static bool ui_shader_editor_load_file(UiShaderSourceEditor* ed, ResHandle h, const char* root_path,
                                       const char* path, bool viewing_include, bool clear_back_stack) {
    if (!ed)
        return false;
    free(ed->text);
    ed->text = nullptr;
    ed->cap = 0;
    ed->ok = false;
    ed->dirty = false;
    ed->cursor = 0;
    ed->select_anchor = 0;
    ed->preferred_col = -1;
    ed->editor_focused = false;
    ed->dragging_selection = false;
    ed->cursor_follow = false;
    ed->autocomplete_open = false;
    ed->autocomplete_index = 0;
    ed->autocomplete_start = 0;
    ed->line_cache.clear();
    ed->line_cache_max_cols = 0;
    ed->line_cache_dirty = true;
    ed->undo_stack.clear();
    ed->redo_stack.clear();
    ed->h = h;
    ed->viewing_include = viewing_include;
    if (clear_back_stack)
        ed->include_back_stack.clear();
    strncpy(ed->root_path, root_path ? root_path : "", MAX_PATH_LEN - 1);
    ed->root_path[MAX_PATH_LEN - 1] = '\0';
    ui_normalize_path_text_inplace(ed->root_path, MAX_PATH_LEN);
    strncpy(ed->path, path ? path : "", MAX_PATH_LEN - 1);
    ed->path[MAX_PATH_LEN - 1] = '\0';
    ui_normalize_path_text_inplace(ed->path, MAX_PATH_LEN);

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

static bool ui_shader_editor_load(UiShaderSourceEditor* ed, ResHandle h, const char* path) {
    return ui_shader_editor_load_file(ed, h, path, path, false, true);
}

static bool ui_write_text_file_atomic(const char* path, const char* text) {
    if (!path || !path[0] || !text)
        return false;
    if (!ui_ensure_parent_directory(path))
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

static bool ui_shader_editor_save_current_file(UiShaderSourceEditor* ed) {
    if (!ed || !ed->ok || !ed->text || !ed->path[0])
        return false;
    if (!ui_write_text_file_atomic(ed->path, ed->text)) {
        log_error("Shader source save failed: %s", ed->path);
        return false;
    }
    ed->dirty = false;
    log_info("Shader source saved: %s", ed->path);
    return true;
}

static bool ui_shader_editor_open_file_preserving_root(UiShaderSourceEditor* ed, const char* path,
                                                       bool include, bool push_back) {
    if (!ed || !path || !path[0])
        return false;
    if (ed->dirty && !ui_shader_editor_save_current_file(ed))
        return false;
    if (push_back && ed->path[0])
        ed->include_back_stack.push_back(ed->path);
    return ui_shader_editor_load_file(ed, ed->h, ed->root_path, path, include, false);
}

static bool ui_shader_editor_open_include(UiShaderSourceEditor* ed, const char* include_name) {
    if (!ed || !include_name || !include_name[0])
        return false;

    char include_path[MAX_PATH_LEN] = {};
    ui_shader_resolve_include_path(ed, include_name, include_path, MAX_PATH_LEN);
    if (!include_path[0])
        return false;

    if (!ui_file_exists(include_path)) {
        char starter[512] = {};
        snprintf(starter, sizeof(starter),
                 "// Include: %s\n\n", include_name);
        if (!ui_write_text_file_atomic(include_path, starter)) {
            log_error("Shader include create failed: %s", include_path);
            return false;
        }
        log_info("Shader include created: %s", include_path);
    }

    if (strcmp(ed->path, include_path) == 0)
        return true;
    return ui_shader_editor_open_file_preserving_root(ed, include_path, true, true);
}

static bool ui_shader_editor_go_back(UiShaderSourceEditor* ed, ResHandle h, Resource* r) {
    (void)h;
    (void)r;
    if (!ed || ed->include_back_stack.empty())
        return false;

    std::string path = ed->include_back_stack.back();
    ed->include_back_stack.pop_back();
    bool include = strcmp(path.c_str(), ed->root_path) != 0;
    if (ed->dirty && !ui_shader_editor_save_current_file(ed))
        return false;
    return ui_shader_editor_load_file(ed, ed->h, ed->root_path, path.c_str(), include, false);
}

static bool ui_shader_editor_open_compile_error_file(UiShaderSourceEditor* ed, const Resource* r) {
    if (!ed || !r || !r->compile_err[0])
        return false;

    UiShaderErrorMarker marker = {};
    if (!ui_shader_compile_error_marker(r, &marker))
        return false;

    char error_path[MAX_PATH_LEN] = {};
    if (marker.path[0] && ui_file_exists(marker.path)) {
        ui_normalize_path_text(marker.path, error_path, MAX_PATH_LEN);
    } else if (marker.path[0]) {
        ui_shader_resolve_include_path(ed, marker.path, error_path, MAX_PATH_LEN);
    } else {
        ui_normalize_path_text(r->path, error_path, MAX_PATH_LEN);
    }

    if (!error_path[0])
        return false;

    bool opened = true;
    if (strcmp(ed->path, error_path) != 0 && ui_file_exists(error_path)) {
        bool include = strcmp(error_path, ed->root_path) != 0;
        opened = ui_shader_editor_open_file_preserving_root(ed, error_path, include, true);
    }
    if (opened && marker.line > 0) {
        int max_cols = 0;
        const std::vector<UiCodeLine>& lines = ui_shader_editor_line_cache(ed, &max_cols);
        int line_i = marker.line - 1;
        if (line_i < 0) line_i = 0;
        if (line_i >= (int)lines.size()) line_i = (int)lines.size() - 1;
        if (!lines.empty())
            ui_shader_editor_set_cursor(ed, lines[line_i].offset, false);
        (void)max_cols;
    }
    return opened;
}

static const char* k_shader_template_common_scene_cb = R"HLSL(// Built-in scene constants supplied by the editor in register(b0).
// These names and order match the CPU SceneCB layout. You can delete unused
// fields from your shader; D3D reflection only keeps constants that are read.
cbuffer SceneCB : register(b0)
{
    float4x4 WorldToView;
    float4x4 ViewToWorld;
    float4x4 ViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4 TimeVec;              // x=time seconds, y=delta seconds, z=frame index, w=reserved.
    float4 CameraParams;         // x=0 perspective/1 orthographic, y=ortho height, z/w=near/far.
    float4 LightDir;             // xyz=main light direction, w=intensity.
    float4 LightColor;           // rgb=color, a=reserved.
    float4 LightPos;             // xyz=main light position.
    float4 LightParams;          // x=0 directional/1 spot, y/z=spot inner/outer cos, w=range.
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4x4 ShadowViewProj;
    float4x4 PrevShadowViewProj;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

)HLSL";

static const char* k_shader_template_object_cb = R"HLSL(// Per-object transform supplied by draw commands in register(b1).
// Mesh shaders normally multiply POSITION by LocalToWorld and then ViewProj.
cbuffer ObjectCB : register(b1)
{
    float4x4 LocalToWorld;
};

)HLSL";

static const char* k_shader_template_vsps_color_cb = R"HLSL(// User parameters supplied by the command in register(b2).
// Color tints the UV visualizer. Leave it at 0,0,0,0 for neutral white,
// or set it to a non-zero value from Shader Parameters to choose a color.
cbuffer UserCB : register(b2)
{
    float4 Color;
};

float4 ResolveUserColor()
{
    return dot(abs(Color), float4(1.0, 1.0, 1.0, 1.0)) > 0.0 ? Color : float4(1.0, 1.0, 1.0, 1.0);
}

)HLSL";

static const char* k_shader_template_compute_uv = R"HLSL(
// Compute template: write normalized UV coordinates into a RWTexture2D.
//
// Editor setup:
//   1. Create or select a RenderTexture2D with UAV enabled.
//   2. Bind that texture to UAV slot u0 on a Dispatch command.
//   3. Set Dispatch From to the same texture and Divisor XYZ to 8,8,1.
//   4. In Shader Parameters, bind TargetSize to the texture size
//      (for example scene_color.size or your_render_target.size).
//
// The shader is intentionally simple: each 8x8 group writes one pixel per
// thread. TargetSize keeps the border safe when the texture size is not a
// multiple of 8.
cbuffer UserCB : register(b2)
{
    int2 TargetSize;             // Usually sourced from a RenderTexture2D size.
};

RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)TargetSize.x || id.y >= (uint)TargetSize.y)
        return;

    float2 target_size = float2((float)TargetSize.x, (float)TargetSize.y);
    float2 safe_size = max(target_size, float2(1.0, 1.0));
    float2 uv = (float2(id.xy) + 0.5) / safe_size;
    Output[id.xy] = float4(uv, 0.0, 1.0);
}
)HLSL";

static const char* k_shader_template_compute_indirect_args = R"HLSL(
// Compute template: fill an indirect argument buffer.
//
// Editor setup:
//   1. Create a StructuredBuffer with UAV enabled and Indirect Args enabled.
//      A stride of 4 bytes is enough because the buffer stores uint DWORDs.
//   2. Bind that buffer to UAV slot u0 on a Dispatch command.
//   3. Dispatch this shader with 1,1,1 groups.
//   4. Use the same buffer as the Indirect Buffer of an Indirect Draw or
//      Indirect Dispatch command.
//
// Mode selects how the first DWORDs are written:
//   Mode = 0: DispatchIndirect      -> x, y, z group counts.
//   Mode = 1: DrawInstancedIndirect -> vertex count, instance count,
//                                      start vertex, start instance.
//   Mode = 2: DrawIndexedInstancedIndirect -> index count, instance count,
//                                             start index, base vertex,
//                                             start instance.
//
// The editor reads the argument layout according to the command that consumes
// the buffer, so unused DWORDs are harmless. Keep Byte Offset on both commands
// aligned to 4 bytes.
//
// Instancing notes:
//   - For DrawInstancedIndirect, InstanceCount is the number of instances the
//     draw shader will receive through SV_InstanceID.
//   - InstanceCount can be hardcoded here, driven from a StructuredBuffer count,
//     or driven from a Gaussian Splat count through Shader Parameters.
//   - For procedural quad instances, set DrawVertexOrIndexCount to 6 and use
//     InstanceCount to choose how many quads to draw.
cbuffer UserCB : register(b2)
{
    int  Mode;
    int3 DispatchGroups;

    int  DrawVertexOrIndexCount;
    int  InstanceCount;
    int  StartVertexOrIndex;
    int  BaseVertex;

    int  StartInstance;
};

RWStructuredBuffer<uint> IndirectArgs : register(u0);

uint positive_or_one(int v)
{
    return (v > 0) ? (uint)v : 1u;
}

uint positive_or_zero(int v)
{
    return (v > 0) ? (uint)v : 0u;
}

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x != 0 || id.y != 0 || id.z != 0)
        return;

    if (Mode == 0)
    {
        IndirectArgs[0] = positive_or_one(DispatchGroups.x);
        IndirectArgs[1] = positive_or_one(DispatchGroups.y);
        IndirectArgs[2] = positive_or_one(DispatchGroups.z);
        return;
    }

    IndirectArgs[0] = positive_or_one(DrawVertexOrIndexCount);
    IndirectArgs[1] = positive_or_one(InstanceCount);
    IndirectArgs[2] = positive_or_zero(StartVertexOrIndex);

    if (Mode == 2)
    {
        IndirectArgs[3] = (uint)BaseVertex;
        IndirectArgs[4] = positive_or_zero(StartInstance);
    }
    else
    {
        IndirectArgs[3] = positive_or_zero(StartInstance);
    }
}
)HLSL";

static const char* k_shader_template_vsps_mesh_uv = R"HLSL(// VS/PS template: display mesh UVs as a tinted color.
//
// Editor setup:
//   1. Use a Draw Mesh command with any mesh that has TEXCOORD0 data.
//   2. Assign this shader to the command.
//   3. In Shader Parameters, edit Color to tint the result. A zero color is
//      treated as neutral white so the first compile is immediately visible.
//
// Instancing notes:
//   - The shader accepts SV_InstanceID, so it can be used in Draw Instanced
//     and Indirect Draw commands without changing the entry point.
//   - For per-instance transforms or colors, create a StructuredBuffer with
//     SRV enabled, bind it to an SRV slot, and index it with instance_id.
//   - For indirect instancing, pair this shader with an indirect-args buffer
//     whose InstanceCount DWORD is filled by a compute shader.
//
// This is useful for checking imported glTF UVs, seams, mirroring, and tiling.
// Values are wrapped with frac() so UVs outside 0..1 repeat visibly.
struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(VSIn v, uint instance_id : SV_InstanceID)
{
    VSOut o;
    float4 world_pos = mul(LocalToWorld, float4(v.pos, 1.0));
    o.pos = mul(ViewProj, world_pos);
    o.uv = v.uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = frac(i.uv);
    float4 uv_color = float4(uv, 0.0, 1.0);
    return uv_color * ResolveUserColor();
}
)HLSL";

static const char* k_shader_template_vsps_procedural_quad = R"HLSL(// VS/PS template: procedural fullscreen quad with tinted UV color.
//
// Editor setup:
//   1. Use a Draw Mesh, Draw Instanced, or Indirect Draw command.
//   2. Change Source to Procedural.
//   3. Set Topology to Triangle List and Vertex Count to 6.
//   4. Assign this shader to the command.
//   5. In Shader Parameters, edit Color to tint the result. A zero color is
//      treated as neutral white so the first compile is immediately visible.
//
// Instancing notes:
//   - SV_InstanceID is available in VSMain. The sample does not move each
//     instance, but you can bind a StructuredBuffer as SRV and use instance_id
//     to read per-instance transforms, colors, or rectangles.
//   - For indirect procedural draws, generate DrawInstancedIndirect arguments
//     with the indirect-args compute template and set VertexCount to 6.
//
// The vertex shader only uses SV_VertexID and SV_InstanceID, so no mesh or
// input layout is required. The quad fills clip space and paints red=U, green=V.
struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID)
{
    uint i = min(vertex_id, 5u);
    float2 pos = float2(-1.0, -1.0);
    float2 uv = float2(0.0, 1.0);

    if (i == 1u) { pos = float2(-1.0,  1.0); uv = float2(0.0, 0.0); }
    if (i == 2u) { pos = float2( 1.0,  1.0); uv = float2(1.0, 0.0); }
    if (i == 3u) { pos = float2(-1.0, -1.0); uv = float2(0.0, 1.0); }
    if (i == 4u) { pos = float2( 1.0,  1.0); uv = float2(1.0, 0.0); }
    if (i == 5u) { pos = float2( 1.0, -1.0); uv = float2(1.0, 1.0); }

    VSOut o;
    o.pos = float4(pos, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(i.uv, 0.0, 1.0) * ResolveUserColor();
}
)HLSL";

static void ui_build_shader_template(bool compute_shader, int template_kind, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    out[0] = '\0';

    // Templates are generated as complete, readable files. Built-in cbuffers
    // are emitted only when the sample actually uses them, which keeps shader
    // reflection focused on the resources the command needs to bind.
    if (compute_shader) {
        snprintf(out, out_sz, "%s%s",
            k_shader_template_common_scene_cb,
            template_kind == 1 ? k_shader_template_compute_indirect_args
                               : k_shader_template_compute_uv);
    } else if (template_kind == 1) {
        snprintf(out, out_sz, "%s%s",
            k_shader_template_vsps_color_cb,
            k_shader_template_vsps_procedural_quad);
    } else {
        snprintf(out, out_sz, "%s%s%s%s",
            k_shader_template_common_scene_cb,
            k_shader_template_object_cb,
            k_shader_template_vsps_color_cb,
            k_shader_template_vsps_mesh_uv);
    }
    out[out_sz - 1] = '\0';
}

static bool ui_create_shader_template_file(ResHandle h, Resource* r, const char* path,
                                           bool compute_shader, int template_kind) {
    if (!r || r->type != RES_SHADER || !path || !path[0])
        return false;
    if (!ui_path_has_extension_ci(path, ".hlsl")) {
        log_warn("Shader template path must end in .hlsl: %s", path);
        return false;
    }
    if (ui_file_exists(path)) {
        log_warn("Shader template target already exists: %s", path);
        return false;
    }

    char source[32768] = {};
    ui_build_shader_template(compute_shader, template_kind, source, sizeof(source));
    if (!source[0])
        return false;

    if (!ui_write_text_file_atomic(path, source)) {
        log_error("Shader template create failed: %s", path);
        return false;
    }

    strncpy(r->path, path, MAX_PATH_LEN - 1);
    r->path[MAX_PATH_LEN - 1] = '\0';
    ui_normalize_path_text_inplace(r->path, MAX_PATH_LEN);
    ui_recompile_shader_resource(h, r, r->path);
    log_info("Shader template created: %s", r->path);
    return true;
}

static void ui_shader_template_buttons(ResHandle h, Resource* r, const char* path) {
    if (!r || r->type != RES_SHADER || !path || !path[0])
        return;

    char clean_path[MAX_PATH_LEN] = {};
    ui_normalize_path_text(path, clean_path, MAX_PATH_LEN);
    if (ui_file_exists(clean_path))
        return;
    if (!ui_path_has_extension_ci(clean_path, ".hlsl")) {
        ImGui::TextDisabled("Template creation is available for missing .hlsl paths.");
        return;
    }

    bool compute_shader = ui_shader_resource_is_compute(r);
    ImGui::Separator();
    ImGui::TextWrapped("File does not exist: %s", clean_path);
    ImGui::TextDisabled("Create a documented starter shader at this path.");

    if (compute_shader) {
        if (ImGui::Button("Create Compute: UVs to UAV", ImVec2(-1.0f, 0.0f)))
            ui_create_shader_template_file(h, r, clean_path, true, 0);
        if (ImGui::Button("Create Compute: Indirect Args", ImVec2(-1.0f, 0.0f)))
            ui_create_shader_template_file(h, r, clean_path, true, 1);
    } else {
        if (ImGui::Button("Create VS/PS: Mesh UV Color", ImVec2(-1.0f, 0.0f)))
            ui_create_shader_template_file(h, r, clean_path, false, 0);
        if (ImGui::Button("Create VS/PS: Procedural Quad UV", ImVec2(-1.0f, 0.0f)))
            ui_create_shader_template_file(h, r, clean_path, false, 1);
    }
}

static bool ui_shader_editor_save(UiShaderSourceEditor* ed, ResHandle h, Resource* r, bool compile_after) {
    if (!ed || !ed->ok || !ed->text || !r)
        return false;
    if (!ui_shader_editor_save_current_file(ed))
        return false;
    if (compile_after) {
        ui_recompile_shader_resource(h, r, r->path);
        if (!r->compiled_ok || r->using_fallback)
            ui_shader_editor_open_compile_error_file(ed, r);
    }
    return true;
}


static bool ui_shader_editor_is_shader_handle(ResHandle h) {
    Resource* r = res_get(h);
    return r && r->type == RES_SHADER;
}

static ResHandle ui_shader_editor_first_shader_handle() {
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (r.active && r.type == RES_SHADER)
            return (ResHandle)(i + 1);
    }
    return INVALID_HANDLE;
}

static int ui_shader_editor_shader_count() {
    int count = 0;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (r.active && r.type == RES_SHADER)
            count++;
    }
    return count;
}

static void ui_shader_editor_close_floating(bool select_active_shader) {
    ResHandle h = s_shader_editor_floating_h;
    s_shader_editor_floating = false;
    if (select_active_shader && ui_shader_editor_is_shader_handle(h)) {
        g_sel_res = h;
        g_sel_cmd = INVALID_HANDLE;
        s_res_nav = h;
    }
}


static void ui_shader_editor_open_floating(ResHandle preferred_h = INVALID_HANDLE) {
    ResHandle h = preferred_h;
    if (!ui_shader_editor_is_shader_handle(h))
        h = s_shader_editor_floating_h;
    if (!ui_shader_editor_is_shader_handle(h))
        h = g_sel_res;
    if (!ui_shader_editor_is_shader_handle(h))
        h = ui_shader_editor_first_shader_handle();

    s_shader_editor_floating_h = h;
    s_shader_editor_floating = true;
}

static void ui_shader_editor_toggle_floating() {
    if (s_shader_editor_floating) {
        ui_shader_editor_close_floating(false);
    } else {
        ui_shader_editor_open_floating(g_sel_res);
    }
}

static void ui_shader_editor_cursor_line_col(UiShaderSourceEditor* ed, int* out_line, int* out_col) {
    int line_no = 1;
    int col_no = 1;
    if (ed && ed->text) {
        int max_cols = 0;
        const std::vector<UiCodeLine>& lines = ui_shader_editor_line_cache(ed, &max_cols);
        if (!lines.empty()) {
            int cursor = ui_code_clamp_offset(ed, ed->cursor);
            int line_i = ui_code_line_from_offset(lines, cursor);
            if (line_i < 0) line_i = 0;
            if (line_i >= (int)lines.size()) line_i = (int)lines.size() - 1;
            const UiCodeLine& line = lines[line_i];
            const char* cursor_ptr = line.begin + (cursor - line.offset);
            if (cursor_ptr < line.begin) cursor_ptr = line.begin;
            if (cursor_ptr > line.end) cursor_ptr = line.end;
            line_no = line_i + 1;
            col_no = ui_code_visual_cols_to(line.begin, cursor_ptr) + 1;
        }
        (void)max_cols;
    }
    if (out_line) *out_line = line_no;
    if (out_col) *out_col = col_no;
}

static bool ui_shader_editor_selector(ResHandle* h, float width = 0.0f) {
    if (!h)
        return false;

    if (!ui_shader_editor_is_shader_handle(*h))
        *h = ui_shader_editor_first_shader_handle();

    Resource* current = res_get(*h);
    const char* preview = current ? current->name : "No shaders";
    bool changed = false;

    ImGui::SetNextItemWidth(width > 0.0f ? width : ui_px(320.0f));
    if (ImGui::BeginCombo("##shader_editor_selector", preview)) {
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active || r.type != RES_SHADER)
                continue;

            ResHandle item_h = (ResHandle)(i + 1);
            bool selected = item_h == *h;
            char label[MAX_NAME + 48] = {};
            snprintf(label, sizeof(label), "%s%s%s", r.name,
                     (!r.compiled_ok || r.using_fallback) ? "  [fallback]" : "",
                     (s_shader_source_ed.h == item_h && s_shader_source_ed.dirty) ? "  *" : "");
            if (ImGui::Selectable(label, selected)) {
                *h = item_h;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static ResHandle ui_shader_editor_next_shader_handle(ResHandle current) {
    ResHandle first = INVALID_HANDLE;
    ResHandle next = INVALID_HANDLE;
    bool return_next = current == INVALID_HANDLE;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        Resource& r = g_resources[i];
        if (!r.active || r.type != RES_SHADER)
            continue;
        ResHandle h = (ResHandle)(i + 1);
        if (first == INVALID_HANDLE)
            first = h;
        if (return_next) {
            next = h;
            break;
        }
        if (h == current)
            return_next = true;
    }
    return next != INVALID_HANDLE ? next : first;
}

static void ui_shader_editor_cycle_shader(UiShaderSourceEditor* ed, bool floating) {
    if (!ed)
        return;
    ResHandle current = floating ? s_shader_editor_floating_h : ed->h;
    ResHandle next = ui_shader_editor_next_shader_handle(current);
    if (next == INVALID_HANDLE || next == current)
        return;
    if (ed->dirty)
        ui_shader_editor_save_current_file(ed);
    if (floating)
        s_shader_editor_floating_h = next;
    else
        g_sel_res = next;
    g_sel_cmd = INVALID_HANDLE;
}

static void ui_shader_source_editor_toolbar(UiShaderSourceEditor* ed, ResHandle h, Resource* r, bool floating) {
    if (!ed || !r)
        return;

    if (ed->viewing_include && ImGui::Button("Back")) {
        ui_shader_editor_go_back(ed, h, r);
    }
    if (ed->viewing_include)
        ImGui::SameLine();

    if (ImGui::Button("Reload")) {
        if (ed->viewing_include)
            ui_shader_editor_load_file(ed, h, r->path, ed->path, true, false);
        else
            ui_shader_editor_load(ed, h, r->path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        ui_shader_editor_save(ed, h, r, false);
    ImGui::SameLine();
    if (ImGui::Button("Save + Compile"))
        ui_shader_editor_save(ed, h, r, true);

    int cursor_line = 1, cursor_col = 1;
    ui_shader_editor_cursor_line_col(ed, &cursor_line, &cursor_col);

    ImGui::SameLine();
    if (floating) {
        if (ImGui::Button("Return to Inspector"))
            ui_shader_editor_close_floating(false);
    } else {
        if (ImGui::Button("Open Floating"))
            ui_shader_editor_open_floating(h);
    }

    float selector_w = 0.0f;
    if (floating) {
        selector_w = ui_px(320.0f);
        ImGui::SameLine();
        float right_x = ImGui::GetWindowContentRegionMax().x - selector_w;
        if (right_x > ImGui::GetCursorPosX())
            ImGui::SetCursorPosX(right_x);
        ResHandle selected_h = s_shader_editor_floating_h;
        if (ui_shader_editor_selector(&selected_h, selector_w)) {
            if (ed->dirty)
                ui_shader_editor_save_current_file(ed);
            s_shader_editor_floating_h = selected_h;
        }

        ui_inspector_text_disabled_wrapped("%s%s  |  %zu bytes%s  |  Ln %d, Col %d  |  Ctrl+S save, Ctrl+D compile root, Shift+Tab cycle shader, Ctrl+Z/Y undo/redo",
                            ed->viewing_include ? "include " : "root ",
                            ed->path[0] ? ed->path : "(no file)",
                            ed->text ? strlen(ed->text) : 0, ed->dirty ? "  modified" : "",
                            cursor_line, cursor_col);
    } else {
        ui_inspector_text_disabled_wrapped("%s%s  |  %zu bytes%s  |  Ln %d, Col %d  |  Ctrl+S save, Ctrl+D compile root, Shift+Tab cycle shader, Ctrl+Z/Y undo/redo",
                            ed->viewing_include ? "include " : "root ",
                            ed->path[0] ? ed->path : "(no file)",
                            ed->text ? strlen(ed->text) : 0, ed->dirty ? "  modified" : "",
                            cursor_line, cursor_col);
    }
}

static void ui_shader_source_editor_body(UiShaderSourceEditor* ed, ResHandle h, Resource* r, bool floating) {
    if (!ed || !r)
        return;
    bool panel_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ui_shader_source_editor_toolbar(ed, h, r, floating);
    if (ed->editor_focused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        ui_shader_editor_save(ed, h, r, false);
    if (panel_focused && ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_Tab))
        ui_shader_editor_cycle_shader(ed, floating);
    ui_shader_code_editor(ed, r);
    s_shader_source_editor_focused = ed->editor_focused;
}

static void ui_shader_source_viewer(ResHandle h, Resource* r) {
    if (s_shader_editor_floating) {
        ui_inspector_text_disabled_wrapped("Shader source is open in the floating Shader Editor panel.");
        if (ImGui::Button("Return to Inspector", ImVec2(-1.0f, 0.0f)))
            ui_shader_editor_close_floating(false);
        return;
    }

    UiShaderSourceEditor& ed = s_shader_source_ed;
    const char* path = r ? r->path : "";
    char root_path[MAX_PATH_LEN] = {};
    ui_normalize_path_text(path ? path : "", root_path, MAX_PATH_LEN);
    bool reload = ed.h != h || strcmp(ed.root_path, root_path) != 0;

    if (reload) {
        if (ed.dirty)
            ui_shader_editor_save_current_file(&ed);
        ui_shader_editor_load(&ed, h, path);
    }

    if (!ed.path[0]) {
        ui_inspector_text_disabled_wrapped("No shader path.");
        return;
    }
    if (!ed.ok || !ed.text) {
        if (ImGui::Button("Reload Source", ImVec2(-1.0f, 0.0f)))
            ui_shader_editor_load(&ed, h, path);
        ui_inspector_text_disabled_wrapped("Could not read source: %s", ed.path);
        return;
    }

    ui_shader_source_editor_body(&ed, h, r, false);
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
    ui_inspector_text_disabled_wrapped("%s", key);
    ImGui::SameLine(120.0f);
    ImGui::TextWrapped("%s", value ? value : "-");
}

static void ui_key_value_handle(const char* key, ResHandle h) {
    Resource* r = res_get(h);
    ui_key_value(key, r ? r->name : "(none)");
}

static void ui_command_shader_params(Command* c, Resource* shader) {
    if (!c || !shader)
        return;

    if (!shader->shader_cb.active) {
        ui_inspector_text_disabled_wrapped("No UserCB cbuffer reflected. Recommended: register(b2).");
        return;
    }

    user_cb_sync_command_params(c, shader);
    ui_inspector_text_disabled_wrapped("%s: register(b%u), %u bytes",
        shader->shader_cb.name, shader->shader_cb.bind_slot, shader->shader_cb.size);

    if (shader->shader_cb.var_count == 0) {
        ui_inspector_text_disabled_wrapped("No supported scalar/vector variables found.");
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
        ui_inspector_text_disabled_wrapped("%s", res_type_str(p.type));

        if (!p.enabled)
            ImGui::BeginDisabled();

        ui_inspector_text_disabled_wrapped("Source");
        ImGui::SameLine(96.0f);
        ImGui::SetNextItemWidth(-1.0f);
        ui_command_param_source_combo(&p);

        ui_inspector_text_disabled_wrapped("Value");
        ImGui::SameLine(96.0f);
        UserCBSourceKind source_kind = p.source_kind;
        if (source_kind == USER_CB_SOURCE_NONE && p.source != INVALID_HANDLE)
            source_kind = USER_CB_SOURCE_RESOURCE;
        if (source_kind != USER_CB_SOURCE_NONE) {
            ui_inspector_text_disabled_wrapped("(source driven)");
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
    case RES_GAUSSIAN_SPLAT:
    case RES_NANOVDB:
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
           (r.type == RES_MESH && r.using_fallback) ||
           (r.type == RES_GAUSSIAN_SPLAT && r.path[0] && !r.compiled_ok) ||
           (r.type == RES_NANOVDB && r.path[0] && !r.compiled_ok);
}

static bool ui_resource_filter_match(const Resource& r, int filter) {
    switch (filter) {
    case 1: return r.type == RES_MESH;
    case 2: return r.type == RES_SHADER;
    case 3: return r.type == RES_TEXTURE2D || r.type == RES_RENDER_TEXTURE2D ||
                   r.type == RES_RENDER_TEXTURE3D ||
                   r.type == RES_BUILTIN_SCENE_COLOR || r.type == RES_BUILTIN_SHADOW_MAP;
    case 4: return r.type == RES_STRUCTURED_BUFFER || r.type == RES_GAUSSIAN_SPLAT ||
                   r.type == RES_NANOVDB || r.type == RES_BUILTIN_SCENE_DEPTH;
    case 5: return r.type == RES_INT || r.type == RES_INT2 || r.type == RES_INT3 || r.type == RES_FLOAT ||
                   r.type == RES_FLOAT2 || r.type == RES_FLOAT3 || r.type == RES_FLOAT4 ||
                   r.type == RES_BUILTIN_TIME || r.type == RES_BUILTIN_LIGHT;
    default: return true;
    }
}

static bool ui_resource_is_variable_inspector_candidate(const Resource& r) {
    if (!r.active || r.is_generated)
        return false;
    switch (r.type) {
    case RES_INT:
    case RES_INT2:
    case RES_INT3:
    case RES_FLOAT:
    case RES_FLOAT2:
    case RES_FLOAT3:
    case RES_FLOAT4:
    case RES_BUILTIN_TIME:
    case RES_BUILTIN_LIGHT:
        return true;
    default:
        return false;
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

static ImVec4 ui_command_type_color(CmdType type) {
    switch (type) {
    case CMD_CLEAR:             return ImVec4(0.45f, 0.72f, 0.94f, 1.0f);
    case CMD_DRAW_MESH:         return ImVec4(0.48f, 0.78f, 0.54f, 1.0f);
    case CMD_DRAW_INSTANCED:    return ImVec4(0.42f, 0.78f, 0.72f, 1.0f);
    case CMD_DISPATCH:          return ImVec4(0.76f, 0.62f, 0.94f, 1.0f);
    case CMD_INDIRECT_DRAW:     return ImVec4(0.92f, 0.70f, 0.38f, 1.0f);
    case CMD_INDIRECT_DISPATCH: return ImVec4(0.84f, 0.58f, 0.86f, 1.0f);
    case CMD_REPEAT:            return ImVec4(0.93f, 0.62f, 0.36f, 1.0f);
    case CMD_GROUP:             return ImVec4(0.66f, 0.68f, 0.74f, 1.0f);
    default:                    return ImVec4(0.60f, 0.62f, 0.66f, 1.0f);
    }
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

static void ui_inline_small_text(const char* id, const char* text, ImVec4 color, float min_h = 0.0f, float scale = 0.82f) {
    if (!text || !text[0])
        return;
    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize() * scale;
    ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
    ImVec2 size(text_size.x, text_size.y > min_h ? text_size.y : min_h);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    float y = pos.y + floorf((size.y - text_size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(font, font_size, ImVec2(pos.x, y), ImGui::GetColorU32(color), text);
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
    float width = ImGui::GetContentRegionAvail().x - ui_current_vertical_scroll_margin(8.0f);
    if (width < ui_px(48.0f))
        width = ui_px(48.0f);
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

static int ui_command_direct_child_count(CmdHandle parent) {
    int count = 0;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (g_commands[i].active && g_commands[i].parent == parent)
            count++;
    }
    return count;
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
        cmd_mark_dirty(new_h);
        mapped[i] = new_h;
        if (i == 0)
            new_root = new_h;
    }

    if (target && ui_command_is_container(target)) {
        target->repeat_expanded = true;
        cmd_mark_dirty(target_h);
    }
    if (anchor != INVALID_HANDLE)
        new_root = cmd_move(new_root, anchor, true);
    app_request_scene_render();
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

    bool container = c.type == CMD_REPEAT || c.type == CMD_GROUP;
    const float row_h = container ? (ImGui::GetTextLineHeight() + 8.0f) : (ImGui::GetTextLineHeight() + 10.0f);
    float width = ImGui::GetContentRegionAvail().x - ui_current_vertical_scroll_margin(8.0f);
    if (width < ui_px(48.0f))
        width = ui_px(48.0f);
    ImGui::InvisibleButton("##cmd_row", ImVec2(width, row_h));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        ImVec2 item_min = ImGui::GetItemRectMin();
        ImVec2 mp = ImGui::GetIO().MousePos;
        if ((c.type == CMD_REPEAT || c.type == CMD_GROUP) &&
            mp.x >= item_min.x + indent && mp.x <= item_min.x + indent + 24.0f) {
            c.repeat_expanded = !c.repeat_expanded;
            cmd_mark_dirty(h);
        }
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
    if (container && !selected) {
        ImVec4 fill = hovered ? ImVec4(0.168f, 0.154f, 0.150f, 0.92f) : ImVec4(0.135f, 0.128f, 0.130f, 0.92f);
        dl->AddRectFilled(min, max, ImGui::GetColorU32(fill), 4.0f);
        dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.245f, 0.225f, 0.220f, 0.64f)), 4.0f);
    }

    float cy = (min.y + max.y) * 0.5f;
    float row_x = min.x + indent;
    if (container) {
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
    bool show_type_badge = !container;
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

    char count_buf[16] = {};
    if (container) {
        snprintf(count_buf, sizeof(count_buf), "%d", ui_command_direct_child_count(h));
        ImVec2 count_ts = ImGui::CalcTextSize(count_buf);
        float count_w = count_ts.x + 12.0f;
        float count_h = count_ts.y + 5.0f;
        ImVec2 count_min = ImVec2(right_x - count_w, floorf(cy - count_h * 0.5f));
        ImVec2 count_max = ImVec2(count_min.x + count_w, count_min.y + count_h);
        ImVec4 count_tint = c.type == CMD_REPEAT ? ImVec4(0.92f, 0.66f, 0.38f, 1.0f) : ImVec4(0.70f, 0.64f, 0.56f, 1.0f);
        dl->AddRectFilled(count_min, count_max, ImGui::GetColorU32(ImVec4(0.120f, 0.108f, 0.102f, 1.0f)), 3.0f);
        dl->AddRect(count_min, count_max, ImGui::GetColorU32(ImVec4(count_tint.x * 0.55f, count_tint.y * 0.55f, count_tint.z * 0.55f, 0.95f)), 3.0f);
        dl->AddText(ImVec2(count_min.x + 6.0f, count_min.y + 2.0f), ImGui::GetColorU32(count_tint), count_buf);
        right_x -= count_w + 8.0f;
    }
    ImVec2 badge_min = ImVec2(right_x - badge_w, floorf(cy - badge_h * 0.5f));
    if (show_type_badge && badge_min.x > row_x + 96.0f)
        ui_draw_badge(dl, badge_min, type, ui_command_type_color(c.type));

    ImU32 name_col = ImGui::GetColorU32(c.enabled ? ImVec4(0.92f, 0.93f, 0.94f, 1.0f) :
        ImVec4(0.48f, 0.49f, 0.51f, 1.0f));
    char display_name[MAX_NAME + 48] = {};
    if (c.type == CMD_GROUP)
        snprintf(display_name, sizeof(display_name), "Group ? %s", c.name);
    else if (c.type == CMD_REPEAT)
        snprintf(display_name, sizeof(display_name), "Repeat x%d ? %s", c.repeat_count, c.name);
    else
        snprintf(display_name, sizeof(display_name), "%s", c.name);
    ImVec2 name_pos = ImVec2(row_x + 28.0f, text_y);
    float clip_right = show_type_badge ? (badge_min.x - 6.0f) : (right_x - 6.0f);
    dl->PushClipRect(name_pos, ImVec2(clip_right, max.y), true);
    dl->AddText(name_pos, name_col, display_name);
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
                        cmd_mark_dirty(src_h);
                        cmd_mark_dirty(h);
                        app_request_scene_render();
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
                        app_request_scene_render();
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
            if (ImGui::MenuItem("Detach from Container")) {
                c.parent = INVALID_HANDLE;
                cmd_mark_dirty(h);
                app_request_scene_render();
            }
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
            cmd_mark_dirty(h);
            app_request_scene_render();
        }
        if (c.type == CMD_REPEAT) {
            if (ImGui::MenuItem("Add Dispatch Child")) {
                char uname[MAX_NAME];
                cmd_make_unique_name("dispatch_0", uname, MAX_NAME);
                CmdHandle child_h = cmd_alloc(uname, CMD_DISPATCH);
                if (Command* child = cmd_get(child_h)) {
                    child->parent = h;
                    cmd_mark_dirty(child_h);
                }
                c.repeat_expanded = true;
                cmd_mark_dirty(h);
                app_request_scene_render();
                g_sel_cmd = child_h;
                g_sel_res = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("Add IndirectDispatch Child")) {
                char uname[MAX_NAME];
                cmd_make_unique_name("idisp_0", uname, MAX_NAME);
                CmdHandle child_h = cmd_alloc(uname, CMD_INDIRECT_DISPATCH);
                if (Command* child = cmd_get(child_h)) {
                    child->parent = h;
                    cmd_mark_dirty(child_h);
                }
                c.repeat_expanded = true;
                cmd_mark_dirty(h);
                app_request_scene_render();
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

struct UiVisibleResourceCache {
    bool builtins;
    int filter;
    uint64_t revision;
    ResHandle items[MAX_RESOURCES];
    int count;
    bool valid;
};

static UiVisibleResourceCache s_visible_resource_cache[2] = {};

static int ui_collect_visible_resources_uncached(bool builtins, int filter, ResHandle* out, int max_count) {
    int count = 0;
    for (int i = 0; i < MAX_RESOURCES && count < max_count; i++) {
        Resource& r = g_resources[i];
        if (!r.active || r.is_generated || r.is_builtin != builtins || !ui_resource_filter_match(r, filter))
            continue;
        out[count++] = (ResHandle)(i + 1);
    }
    return count;
}

static int ui_collect_variable_inspector_resources(ResHandle* out, int max_count) {
    int count = 0;
    for (int i = 0; i < MAX_RESOURCES && count < max_count; i++) {
        Resource& r = g_resources[i];
        if (!ui_resource_is_variable_inspector_candidate(r))
            continue;
        out[count++] = (ResHandle)(i + 1);
    }
    return count;
}

static const UiVisibleResourceCache* ui_visible_resources(bool builtins, int filter) {
    UiVisibleResourceCache& cache = s_visible_resource_cache[builtins ? 1 : 0];
    uint64_t revision = res_revision();
    if (!cache.valid || cache.builtins != builtins || cache.filter != filter || cache.revision != revision) {
        cache.builtins = builtins;
        cache.filter = filter;
        cache.revision = revision;
        cache.count = ui_collect_visible_resources_uncached(builtins, filter, cache.items, MAX_RESOURCES);
        cache.valid = true;
    }
    return &cache;
}

static int ui_find_visible_resource_index(const ResHandle* items, int count, ResHandle h) {
    for (int i = 0; i < count; i++)
        if (items[i] == h)
            return i;
    return -1;
}

struct UiVisibleCommandCache {
    CmdHandle items[MAX_COMMANDS];
    unsigned char depths[MAX_COMMANDS];
    int count;
    uint64_t graph_revision;
    uint64_t expanded_hash;
    bool valid;
};

static UiVisibleCommandCache s_visible_command_cache = {};

static int ui_collect_visible_commands_recursive_uncached(CmdHandle parent, int depth,
                                                          CmdHandle* out, unsigned char* depths,
                                                          int max_count) {
    int count = 0;
    for (int i = 0; i < MAX_COMMANDS && count < max_count; i++) {
        Command& c = g_commands[i];
        if (!c.active || c.parent != parent)
            continue;
        CmdHandle h = (CmdHandle)(i + 1);
        out[count++] = h;
        if (depths)
            depths[count - 1] = (unsigned char)(depth < 255 ? depth : 255);
        if ((c.type == CMD_REPEAT || c.type == CMD_GROUP) && c.repeat_expanded)
            count += ui_collect_visible_commands_recursive_uncached(h, depth + 1,
                                                                    out + count, depths ? depths + count : nullptr,
                                                                    max_count - count);
    }
    return count;
}

static uint64_t ui_command_expanded_hash() {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active || (c.type != CMD_GROUP && c.type != CMD_REPEAT))
            continue;
        h ^= (uint64_t)(i + 1);
        h *= 1099511628211ull;
        h ^= c.repeat_expanded ? 1ull : 0ull;
        h *= 1099511628211ull;
    }
    return h;
}

static const UiVisibleCommandCache* ui_visible_commands() {
    uint64_t graph_revision = cmd_graph_revision();
    uint64_t expanded_hash = ui_command_expanded_hash();
    if (!s_visible_command_cache.valid ||
        s_visible_command_cache.graph_revision != graph_revision ||
        s_visible_command_cache.expanded_hash != expanded_hash) {
        s_visible_command_cache.graph_revision = graph_revision;
        s_visible_command_cache.expanded_hash = expanded_hash;
        s_visible_command_cache.count = ui_collect_visible_commands_recursive_uncached(
            INVALID_HANDLE, 0, s_visible_command_cache.items, s_visible_command_cache.depths, MAX_COMMANDS);
        s_visible_command_cache.valid = true;
    }
    return &s_visible_command_cache;
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
            if (ImGui::MenuItem("Vertex/Pixel Shader")) {
                res_make_unique_name("shader_0", uname, MAX_NAME);
                g_sel_res = res_create_shader(uname, "", "VSMain", "PSMain");
                g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("Compute Shader")) {
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
            if (ImGui::MenuItem("Load Gaussian Splat PLY... (set path in Inspector)")) {
                res_make_unique_name("splat_0", uname, MAX_NAME);
                g_sel_res = res_load_gaussian_splat(uname, "");
                g_sel_cmd = INVALID_HANDLE;
            }
            if (ImGui::MenuItem("Load NanoVDB... (set path in Inspector)")) {
                res_make_unique_name("nvdb_0", uname, MAX_NAME);
                g_sel_res = res_load_nanovdb(uname, "");
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
            const UiVisibleResourceCache* visible_cache = ui_visible_resources(false, s_res_filter);
            const ResHandle* visible_items = visible_cache->items;
            int visible = visible_cache->count;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_None) && !ImGui::IsAnyItemActive()) {
                ImGui::SetNextFrameWantCaptureKeyboard(true);
                if (visible > 0) {
                    int nav_index = ui_find_visible_resource_index(visible_items, visible, s_res_nav);
                    if (nav_index < 0)
                        nav_index = ui_find_visible_resource_index(visible_items, visible, g_sel_res);
                    if (nav_index < 0) nav_index = 0;
                    bool nav_moved = false;
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && nav_index > 0) {
                        nav_index--;
                        nav_moved = true;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && nav_index + 1 < visible) {
                        nav_index++;
                        nav_moved = true;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                        nav_index = 0;
                        nav_moved = true;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
                        nav_index = visible - 1;
                        nav_moved = true;
                    }
                    s_res_nav = visible_items[nav_index];
                    if (nav_moved)
                        s_res_scroll_to_nav = true;
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                        g_sel_res = s_res_nav;
                        g_sel_cmd = INVALID_HANDLE;
                    }
                }
            }
            float row_h = ImGui::GetTextLineHeight() + 10.0f;
            ui_scroll_resources_to_nav_if_requested(ui_find_visible_resource_index(visible_items, visible, s_res_nav), row_h);
            ImGuiListClipper clipper;
            clipper.Begin(visible, row_h);
            while (clipper.Step()) {
                for (int item = clipper.DisplayStart; item < clipper.DisplayEnd; item++) {
                    Resource* r = res_get(visible_items[item]);
                    if (!r) continue;
                    if (ui_resource_row((int)visible_items[item] - 1, *r))
                        continue;
                }
            }
            if (visible == 0)
                ImGui::TextDisabled("No user resources match this filter.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Built-in")) {
            s_user_scope_active = false;
            const UiVisibleResourceCache* visible_cache = ui_visible_resources(true, s_res_filter);
            const ResHandle* visible_items = visible_cache->items;
            int visible = visible_cache->count;
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_None) && !ImGui::IsAnyItemActive()) {
                ImGui::SetNextFrameWantCaptureKeyboard(true);
                if (visible > 0) {
                    int nav_index = ui_find_visible_resource_index(visible_items, visible, s_res_nav);
                    if (nav_index < 0)
                        nav_index = ui_find_visible_resource_index(visible_items, visible, g_sel_res);
                    if (nav_index < 0) nav_index = 0;
                    bool nav_moved = false;
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && nav_index > 0) {
                        nav_index--;
                        nav_moved = true;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && nav_index + 1 < visible) {
                        nav_index++;
                        nav_moved = true;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                        nav_index = 0;
                        nav_moved = true;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
                        nav_index = visible - 1;
                        nav_moved = true;
                    }
                    s_res_nav = visible_items[nav_index];
                    if (nav_moved)
                        s_res_scroll_to_nav = true;
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                        g_sel_res = s_res_nav;
                        g_sel_cmd = INVALID_HANDLE;
                    }
                }
            }
            float row_h = ImGui::GetTextLineHeight() + 10.0f;
            ui_scroll_resources_to_nav_if_requested(ui_find_visible_resource_index(visible_items, visible, s_res_nav), row_h);
            ImGuiListClipper clipper;
            clipper.Begin(visible, row_h);
            while (clipper.Step()) {
                for (int item = clipper.DisplayStart; item < clipper.DisplayEnd; item++) {
                    Resource* r = res_get(visible_items[item]);
                    if (!r) continue;
                    ui_resource_row((int)visible_items[item] - 1, *r);
                }
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

// -- commands panel --------------------------------------------------------

static float ui_command_tree_row_height(const Command& c) {
    return (c.type == CMD_GROUP || c.type == CMD_REPEAT)
        ? (ImGui::GetTextLineHeight() + 8.0f)
        : (ImGui::GetTextLineHeight() + 10.0f);
}

static float ui_command_visible_subtree_height(const UiVisibleCommandCache* cache, int parent_index) {
    if (!cache || parent_index < 0 || parent_index >= cache->count)
        return 0.0f;

    float total = 0.0f;
    bool first = true;
    CmdHandle parent_h = cache->items[parent_index];
    for (int i = parent_index + 1; i < cache->count; i++) {
        if (!ui_command_is_descendant(cache->items[i], parent_h))
            break;
        Command* c = cmd_get(cache->items[i]);
        if (!c)
            continue;
        if (!first)
            total += ImGui::GetStyle().ItemSpacing.y;
        total += ui_command_tree_row_height(*c);
        first = false;
    }
    return total;
}

static void ui_draw_command_tree_cached(const UiVisibleCommandCache* cache) {
    if (!cache)
        return;
    for (int visible_i = 0; visible_i < cache->count; visible_i++) {
        CmdHandle h = cache->items[visible_i];
        Command* c = cmd_get(h);
        if (!c)
            continue;

        int index = (int)h - 1;
        int depth = (int)cache->depths[visible_i];
        if (ui_command_row(index, *c, depth))
            continue;

        if (c->type == CMD_GROUP && c->repeat_expanded) {
            float child_h = ui_command_visible_subtree_height(cache, visible_i);
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
                    ImGui::GetColorU32(ImVec4(0.120f, 0.112f, 0.108f, 0.56f)), 5.0f);
                dl->AddRectFilled(block_min, ImVec2(block_min.x + 2.0f, block_max.y),
                    ImGui::GetColorU32(ImVec4(0.78f, 0.42f, 0.32f, 0.40f)), 2.0f);
            }
        }
    }
}

static float ui_visible_command_y_offset(const UiVisibleCommandCache* cache, int target_index) {
    if (!cache || target_index <= 0)
        return 0.0f;

    float y = 0.0f;
    const float spacing_y = ImGui::GetStyle().ItemSpacing.y;
    int count = target_index < cache->count ? target_index : cache->count;
    for (int i = 0; i < count; i++) {
        Command* c = cmd_get(cache->items[i]);
        if (!c)
            continue;
        if (i > 0)
            y += spacing_y;
        y += ui_command_tree_row_height(*c);
    }
    return y;
}

static void ui_scroll_list_to_row(float list_start_y, float row_y, float row_h) {
    float visible_h = ImGui::GetWindowHeight();
    float target_scroll = list_start_y + row_y - (visible_h - row_h) * 0.5f;
    if (target_scroll < 0.0f)
        target_scroll = 0.0f;
    ImGui::SetScrollY(target_scroll);
}

static void ui_scroll_commands_to_nav_if_requested(const UiVisibleCommandCache* cache, int nav_index) {
    if (!s_cmd_scroll_to_nav)
        return;
    s_cmd_scroll_to_nav = false;
    if (!cache || nav_index < 0 || nav_index >= cache->count)
        return;

    Command* c = cmd_get(cache->items[nav_index]);
    if (!c)
        return;
    ui_scroll_list_to_row(ImGui::GetCursorPosY(), ui_visible_command_y_offset(cache, nav_index),
                          ui_command_tree_row_height(*c));
}

static void ui_scroll_resources_to_nav_if_requested(int nav_index, float row_h) {
    if (!s_res_scroll_to_nav)
        return;
    s_res_scroll_to_nav = false;
    if (nav_index < 0)
        return;
    ui_scroll_list_to_row(ImGui::GetCursorPosY(), (float)nav_index * row_h, row_h);
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
            app_request_scene_render();
        }
        if (ImGui::MenuItem("Repeat")) {
            cmd_make_unique_name("repeat_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_REPEAT); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
            app_request_scene_render();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear")) {
            cmd_make_unique_name("clear_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_CLEAR); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
            app_request_scene_render();
        }
        if (ImGui::MenuItem("DrawMesh")) {
            cmd_make_unique_name("draw_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_DRAW_MESH); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
            app_request_scene_render();
        }
        if (ImGui::MenuItem("DrawInstanced")) {
            cmd_make_unique_name("drawi_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_DRAW_INSTANCED); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
            app_request_scene_render();
        }
        if (ImGui::MenuItem("Dispatch")) {
            cmd_make_unique_name("dispatch_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_DISPATCH); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
            app_request_scene_render();
        }
        if (ImGui::MenuItem("IndirectDraw")) {
            cmd_make_unique_name("idraw_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_INDIRECT_DRAW); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
            app_request_scene_render();
        }
        if (ImGui::MenuItem("IndirectDispatch")) {
            cmd_make_unique_name("idisp_0", uname, MAX_NAME);
            g_sel_cmd = cmd_alloc(uname, CMD_INDIRECT_DISPATCH); g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
            app_request_scene_render();
        }
        ImGui::EndPopup();
    }

    const UiVisibleCommandCache* visible_cache = ui_visible_commands();
    const CmdHandle* visible_items = visible_cache->items;
    int visible = visible_cache->count;
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
            bool nav_moved = false;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) && nav_index > 0) {
                nav_index--;
                nav_moved = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && nav_index + 1 < visible) {
                nav_index++;
                nav_moved = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                nav_index = 0;
                nav_moved = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
                nav_index = visible - 1;
                nav_moved = true;
            }
            s_cmd_nav = visible_items[nav_index];
            if (nav_moved)
                s_cmd_scroll_to_nav = true;
            Command* nav_cmd = cmd_get(s_cmd_nav);
            if (nav_cmd && (nav_cmd->type == CMD_REPEAT || nav_cmd->type == CMD_GROUP)) {
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && nav_cmd->repeat_expanded) {
                    nav_cmd->repeat_expanded = false;
                    cmd_mark_dirty(s_cmd_nav);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && !nav_cmd->repeat_expanded) {
                    nav_cmd->repeat_expanded = true;
                    cmd_mark_dirty(s_cmd_nav);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                g_sel_cmd = s_cmd_nav;
                g_sel_res = INVALID_HANDLE;
            }
        }
    }

    ui_scroll_commands_to_nav_if_requested(visible_cache, ui_find_visible_command_index(visible_items, visible, s_cmd_nav));
    ui_draw_command_tree_cached(visible_cache);

    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_F2)) {
        Command* c = cmd_get(g_sel_cmd);
        if (c) {
            strncpy(s_rename_buf, c->name, MAX_NAME - 1);
            s_rename_active = true; s_rename_is_cmd = true;
        }
    }

    if (!embedded) ImGui::End();
}

// -- inspector -------------------------------------------------------------

static void ui_compute_default_cascade_splits(float near_z, float far_z, int cascade_count,
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

static void ui_seed_light_cascade_range(Resource* r, int from_index, int cascade_count) {
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
    ui_compute_default_cascade_splits(g_camera.near_z, split_far, cascade_count,
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

static void ui_validate_light_cascades(Resource* r) {
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
    if (r->is_builtin) ui_inspector_text_disabled_wrapped("(built-in - read only name)");

    bool value_changed = false;
    switch (r->type) {
    case RES_INT:    value_changed = ImGui::InputInt("value",   &r->ival[0]);       break;
    case RES_INT2:   value_changed = ImGui::InputInt2("value",   r->ival);          break;
    case RES_INT3:   value_changed = ImGui::InputInt3("value",   r->ival);          break;
    case RES_FLOAT:
    case RES_FLOAT2:
    case RES_FLOAT3:
    case RES_FLOAT4:
        value_changed = ui_float_value_editor("value", r->type, r->fval,
                                              ui_labeled_item_compact_width("value"));
        break;

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
            ui_inspector_text_disabled_wrapped("Scene-scaled: %s", ui_rt_scene_scale_name(s_scene_div));
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
            ui_inspector_text_disabled_wrapped("Mode: %s", ui_rt_scene_scale_name(r->scene_scale_divisor));
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
            ui_inspector_text_disabled_wrapped("Depth formats are only supported by RenderTexture2D.");

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
            ui_inspector_text_disabled_wrapped("Preview requires SRV enabled.");
            break;
        }
        if (!ui_rt3d_preview_supported_format(r->tex_fmt)) {
            ui_inspector_text_disabled_wrapped("Preview is not available for this format yet.");
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
            ui_inspector_text_disabled_wrapped("3D texture preview is unavailable.");
        break;
    }

    case RES_TEXTURE2D: {
        ImGui::Text("Size: %dx%d", r->width, r->height);
        ImGui::TextWrapped("Path: %s", r->path);
        static ResHandle tex_edit = INVALID_HANDLE;
        static char tex_path[MAX_PATH_LEN] = {};
        if (tex_edit != h) {
            tex_edit = h;
            strncpy(tex_path, r->path, MAX_PATH_LEN - 1);
            tex_path[MAX_PATH_LEN - 1] = '\0';
        }
        PathInputResult tex_path_result =
            ui_path_input_ex("Path##tex", tex_path, MAX_PATH_LEN,
                             ".png;.jpg;.jpeg;.tga;.bmp;.hdr", 0, "assets/textures");
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
        ui_inspector_text_disabled_wrapped("Total: %d bytes", s_stride * s_count);
        ImGui::Checkbox("SRV", &s_srv);
        ImGui::SameLine();
        ImGui::Checkbox("UAV", &s_uav);
        ImGui::Checkbox("Indirect Args", &s_indirect_args);
        if (s_indirect_args)
            ui_inspector_text_disabled_wrapped("Indirect args use typed uint views for D3D11 compatibility.");
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

    case RES_GAUSSIAN_SPLAT: {
        static ResHandle gs_edit = INVALID_HANDLE;
        static char gs_path[MAX_PATH_LEN] = {};
        if (gs_edit != h) {
            gs_edit = h;
            strncpy(gs_path, r->path, MAX_PATH_LEN - 1);
            gs_path[MAX_PATH_LEN - 1] = '\0';
        }

        ResourceLoadProgress load_progress = {};
        bool loading_this = res_get_load_progress(&load_progress) && load_progress.active &&
                            strcmp(load_progress.path, r->path) == 0 &&
                            strcmp(load_progress.label, r->name) == 0;
        if (r->srv && r->compiled_ok) {
            ImGui::TextColored({0.35f, 1, 0.45f, 1}, "Status: OK");
        } else if (loading_this) {
            ImGui::TextColored({1.0f, 0.55f, 0.22f, 1.0f}, "Status: LOADING %.0f%%", load_progress.fraction * 100.0f);
        } else if (r->path[0]) {
            ImGui::TextColored({1, 0.35f, 0.3f, 1}, "Status: ERROR");
        } else {
            ui_inspector_text_disabled_wrapped("Status: no file loaded");
        }
        if (r->compile_err[0])
            ImGui::TextWrapped("%s", r->compile_err);

        ImGui::TextWrapped("Path: %s", r->path[0] ? r->path : "(none)");
        PathInputResult gs_path_result =
            ui_path_input_ex("Path##gs", gs_path, MAX_PATH_LEN, ".ply", 0, "assets/models");
        if (gs_path_result.file_selected) {
            if (res_reload_gaussian_splat(r, gs_path)) {
                strncpy(gs_path, r->path, MAX_PATH_LEN - 1);
                gs_path[MAX_PATH_LEN - 1] = '\0';
            }
        }
        if (loading_this)
            ImGui::BeginDisabled();
        if (ImGui::Button(loading_this ? "Loading PLY" : (r->srv ? "Reload PLY" : "Load PLY"))) {
            if (res_reload_gaussian_splat(r, gs_path)) {
                strncpy(gs_path, r->path, MAX_PATH_LEN - 1);
                gs_path[MAX_PATH_LEN - 1] = '\0';
            }
        }
        if (loading_this)
            ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::Text("Splats: %d", r->elem_count);
        ImGui::Text("Stride: %d bytes", r->elem_size);
        ImGui::Text("GPU: %.3f MB", (double)res_estimate_gpu_bytes(*r) / (1024.0 * 1024.0));
        ImGui::Text("SRV:%s UAV:%s", r->has_srv ? "Y" : "N", r->has_uav ? "Y" : "N");
        ui_inspector_text_disabled_wrapped("Read-only StructuredBuffer SRV; bind it in SRV slots for draw/compute shaders.");
        ui_inspector_text_disabled_wrapped("GPU layout: float4 pos_opacity, float4 quat_xyzw, float4 scale, float4 color.");

        ImGui::Separator();
        ImGui::Text("Bounds min: %.4g %.4g %.4g",
            r->splat_bounds_min[0], r->splat_bounds_min[1], r->splat_bounds_min[2]);
        ImGui::Text("Bounds max: %.4g %.4g %.4g",
            r->splat_bounds_max[0], r->splat_bounds_max[1], r->splat_bounds_max[2]);
        ImGui::Text("Average max scale: %.6g", r->splat_avg_scale);
        ImGui::Text("f_rest coeffs: %d", r->splat_rest_count);
        if (r->splat_sh_degree > 0)
            ImGui::Text("Detected SH degree: %d", r->splat_sh_degree);
        else
            ui_inspector_text_disabled_wrapped("Detected SH degree: none/DC only");

        if (r->size_handle != INVALID_HANDLE) {
            if (Resource* sr = res_get(r->size_handle)) {
                ImGui::Separator();
                char label[MAX_NAME + 64];
                snprintf(label, sizeof(label), "Size variable: %s = %d##gs_size", sr->name, sr->ival[0]);
                if (ImGui::Selectable(label, false)) {
                    g_sel_res = r->size_handle;
                    g_sel_cmd = INVALID_HANDLE;
                }
                ui_inspector_text_disabled_wrapped("Use this int resource for cbuffer params or dispatch counts.");
            }
        }
        break;
    }

    case RES_NANOVDB: {
        static ResHandle nvdb_edit = INVALID_HANDLE;
        static char nvdb_path[MAX_PATH_LEN] = {};
        if (nvdb_edit != h) {
            nvdb_edit = h;
            strncpy(nvdb_path, r->path, MAX_PATH_LEN - 1);
            nvdb_path[MAX_PATH_LEN - 1] = '\0';
        }

        ResourceLoadProgress load_progress = {};
        bool loading_this = res_get_load_progress(&load_progress) && load_progress.active &&
                            strcmp(load_progress.path, r->path) == 0 &&
                            strcmp(load_progress.label, r->name) == 0;
        if (r->srv && r->compiled_ok) {
            ImGui::TextColored({0.35f, 1, 0.45f, 1}, "Status: OK");
        } else if (loading_this) {
            ImGui::TextColored({1.0f, 0.55f, 0.22f, 1.0f}, "Status: LOADING %.0f%%", load_progress.fraction * 100.0f);
        } else if (r->path[0]) {
            ImGui::TextColored({1, 0.35f, 0.3f, 1}, "Status: ERROR");
        } else {
            ui_inspector_text_disabled_wrapped("Status: no file loaded");
        }
        if (r->compile_err[0])
            ImGui::TextWrapped("%s", r->compile_err);

        ImGui::TextWrapped("Path: %s", r->path[0] ? r->path : "(none)");
        PathInputResult nvdb_path_result =
            ui_path_input_ex("Path##nvdb", nvdb_path, MAX_PATH_LEN, ".nvdb", 0, "assets/models");
        if (nvdb_path_result.file_selected) {
            if (res_reload_nanovdb(r, nvdb_path)) {
                strncpy(nvdb_path, r->path, MAX_PATH_LEN - 1);
                nvdb_path[MAX_PATH_LEN - 1] = '\0';
            }
        }
        if (loading_this)
            ImGui::BeginDisabled();
        if (ImGui::Button(loading_this ? "Loading NVDB" : (r->srv ? "Reload NVDB" : "Load NVDB"))) {
            if (res_reload_nanovdb(r, nvdb_path)) {
                strncpy(nvdb_path, r->path, MAX_PATH_LEN - 1);
                nvdb_path[MAX_PATH_LEN - 1] = '\0';
            }
        }
        if (loading_this)
            ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::Text("File bytes: %.3f MB", (double)r->nanovdb_byte_count / (1024.0 * 1024.0));
        ImGui::Text("GPU uints: %d", r->elem_count);
        ImGui::Text("Grid byte offset: %u", r->nanovdb_grid_byte_offset);
        ImGui::Text("GPU: %.3f MB", (double)res_estimate_gpu_bytes(*r) / (1024.0 * 1024.0));
        ImGui::Text("SRV:%s UAV:%s", r->has_srv ? "Y" : "N", r->has_uav ? "Y" : "N");
        ui_inspector_text_disabled_wrapped("Read-only StructuredBuffer<uint> SRV; bind it in SRV slots for NanoVDB raymarch shaders.");
        ui_inspector_text_disabled_wrapped("The provided shaders expect an uncompressed/raw NanoVDB grid or a correct grid byte offset parameter.");

        if (r->size_handle != INVALID_HANDLE) {
            if (Resource* sr = res_get(r->size_handle)) {
                ImGui::Separator();
                ImGui::Text("Size variable: %s = %d", sr->name, sr->ival[0]);
                ui_inspector_text_disabled_wrapped("This is the uint count in the uploaded NanoVDB buffer.");
            }
        }
        break;
    }

    case RES_MESH: {
        ImGui::Text("Vertices: %d", r->vert_count);
        ImGui::Text("Indices:  %d", r->idx_count);
        ImGui::Text("Stride:   %d bytes", r->vert_stride);
        ImGui::Text("Parts:    %d", r->mesh_part_count);
        ImGui::Text("Materials:%d", r->mesh_material_count);
        ImGui::TextWrapped("Path: %s", r->path);
        if (r->mesh_bounds_valid) {
            ImGui::Separator();
            ui_draw_bounds_values("Mesh bounds", r->mesh_bounds_min, r->mesh_bounds_max);
        }
        static ResHandle mesh_edit = INVALID_HANDLE;
        static char mesh_path[MAX_PATH_LEN] = {};
        if (mesh_edit != h) {
            mesh_edit = h;
            strncpy(mesh_path, r->path, MAX_PATH_LEN - 1);
            mesh_path[MAX_PATH_LEN - 1] = '\0';
        }
        if (r->using_fallback) {
            ImGui::TextColored({1, 0.35f, 0.3f, 1}, "Status: FALLBACK CUBE");
            if (r->compile_err[0]) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.35f, 0.3f, 1));
                ImGui::TextWrapped("%s", r->compile_err);
                ImGui::PopStyleColor();
            }
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
                    ui_inspector_text_disabled_wrapped("mat %s", r->mesh_materials[part.material_index].name);
                }
                if (part.bounds_valid && ImGui::TreeNodeEx("Bounds", ImGuiTreeNodeFlags_None)) {
                    ui_draw_bounds_values(nullptr, part.bounds_min, part.bounds_max);
                    ImGui::TreePop();
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
                ui_inspector_text_disabled_wrapped("Double-sided");
                    else
                ui_inspector_text_disabled_wrapped("Backface culled by default");
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
            ui_path_input_ex("Path##mesh", mesh_path, MAX_PATH_LEN, ".gltf;.glb", 0, "assets/models");
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
            ui_path_input_ex("Path##sh", shader_path, MAX_PATH_LEN, ".hlsl;.hlsli;.fx", 0, "shaders");
        if (!r->compiled_ok) ImGui::PopStyleColor();
        if (shader_path_result.file_selected) {
            strncpy(r->path, shader_path, MAX_PATH_LEN - 1);
            r->path[MAX_PATH_LEN - 1] = '\0';
            ui_normalize_path_text_inplace(r->path, MAX_PATH_LEN);
            strncpy(shader_path, r->path, MAX_PATH_LEN - 1);
            shader_path[MAX_PATH_LEN - 1] = '\0';
            ui_recompile_shader_resource(h, r, r->path);
        }
        ui_shader_template_buttons(h, r, shader_path);

        if (ui_inspector_section("SOURCE"))
            ui_shader_source_viewer(h, r);

        ImGui::Separator();
        if (r->shader_cb.active) {
            ImGui::Text("Reflected cbuffer: %s (b%u, %u bytes)",
                r->shader_cb.name, r->shader_cb.bind_slot, r->shader_cb.size);
            for (int i = 0; i < r->shader_cb.var_count; i++) {
                const ShaderCBVar& v = r->shader_cb.vars[i];
                ui_inspector_text_disabled_wrapped("%s %s @ %u", res_type_str(v.type), v.name, v.offset);
            }
            if (r->object_cb_active)
                ui_inspector_text_disabled_wrapped("ObjectCB slot: b%u", r->object_cb_bind_slot);
        } else {
            ui_inspector_text_disabled_wrapped("No UserCB cbuffer reflected. Recommended: register(b2).");
            if (r->object_cb_active)
                ui_inspector_text_disabled_wrapped("ObjectCB slot: b%u", r->object_cb_bind_slot);
        }
        break;
    }

    case RES_BUILTIN_LIGHT: {
        Vec3 target = v3(r->light_target[0], r->light_target[1], r->light_target[2]);
        Vec3 pos = v3(r->light_pos[0], r->light_pos[1], r->light_pos[2]);
        Vec3 offset = v3_sub(pos, target);
        float distance = sqrtf(v3_dot(offset, offset));
        if (distance < 0.001f) {
            distance = 0.001f;
            offset = v3(0.0f, 1.0f, 0.0f);
        }

        bool light_changed = false;
        const char* light_types[] = { "Directional", "Spot" };
        int light_type = r->light_type == LIGHT_TYPE_SPOT ? 1 : 0;
        if (ImGui::Combo("Light Type", &light_type, light_types, 2)) {
            r->light_type = light_type == 1 ? LIGHT_TYPE_SPOT : LIGHT_TYPE_DIRECTIONAL;
            light_changed = true;
        }
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
        if (ImGui::Checkbox("Debug Draw Light", &r->light_debug_draw))
            app_request_scene_render();
        if (r->light_type == LIGHT_TYPE_SPOT) {
            light_changed |= ImGui::DragFloat("Spot Angle", &r->spot_angle, 0.01f, 0.05f, 3.0f);
            light_changed |= ImGui::DragFloat("Spot Softness", &r->spot_softness, 0.01f, 0.0f, 0.95f);
            r->spot_angle = clampf(r->spot_angle, 0.05f, 3.0f);
            r->spot_softness = clampf(r->spot_softness, 0.0f, 0.95f);
        }
        if (light_changed)
            timeline_capture_if_tracked(TIMELINE_TRACK_LIGHT, "light", RES_NONE);
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
            dx_create_shadow_map(r->shadow_width, r->shadow_height,
                                 r->light_type == LIGHT_TYPE_SPOT ? 1 : r->shadow_cascade_count);
        }
        if (r->light_type == LIGHT_TYPE_DIRECTIONAL) {
            int cascade_count = r->shadow_cascade_count > 0 ? r->shadow_cascade_count : 1;
            int prev_cascade_count = cascade_count;
            if (ImGui::InputInt("Shadow Cascades", &cascade_count)) {
                if (cascade_count < 1) cascade_count = 1;
                if (cascade_count > MAX_SHADOW_CASCADES) cascade_count = MAX_SHADOW_CASCADES;
                r->shadow_cascade_count = cascade_count;
                if (cascade_count > prev_cascade_count)
                    ui_seed_light_cascade_range(r, prev_cascade_count, cascade_count);
                dx_create_shadow_map(r->shadow_width, r->shadow_height, r->shadow_cascade_count);
            }
        }
        ui_validate_light_cascades(r);
        if (r->light_type == LIGHT_TYPE_DIRECTIONAL && r->shadow_cascade_count > 1) {
            for (int cascade = 0; cascade < r->shadow_cascade_count; cascade++) {
                ImGui::PushID(cascade);
                if (cascade > 0)
                    ImGui::Separator();
                ui_inspector_text_disabled_wrapped("Cascade %d", cascade + 1);
                ImGui::DragFloat("Split Far", &r->shadow_cascade_split[cascade], 0.05f, 0.0f, 1000.0f);
                ImGui::DragFloat2("Ortho Size", r->shadow_cascade_extent[cascade], 0.01f, 0.01f, 100.0f);
                ImGui::DragFloat("Near", &r->shadow_cascade_near[cascade], 0.001f, 0.0001f, 100.0f);
                ImGui::DragFloat("Far", &r->shadow_cascade_far[cascade], 0.01f, 0.001f, 1000.0f);
                ImGui::PopID();
            }
            ui_validate_light_cascades(r);
            ui_inspector_text_disabled_wrapped("Each cascade is stored in a separate Texture2DArray slice.");
        } else {
            if (r->light_type == LIGHT_TYPE_DIRECTIONAL)
                ImGui::DragFloat2("Shadow Ortho Size", r->shadow_extent, 0.01f, 0.01f, 100.0f);
            ImGui::DragFloat("Shadow Near", &r->shadow_near, 0.001f, 0.0001f, 100.0f);
            ImGui::DragFloat("Shadow Far", &r->shadow_far, 0.01f, 0.001f, 1000.0f);
            if (r->shadow_far <= r->shadow_near + 0.001f)
                r->shadow_far = r->shadow_near + 0.001f;
            ui_inspector_text_disabled_wrapped(r->light_type == LIGHT_TYPE_SPOT ?
                "Spot shadows use a perspective shadow projection in the first array slice." :
                "Single-cascade mode uses the manual ortho box above.");
        }
        ImGui::Separator();
        if (Resource* shadow_map = res_get(g_builtin_shadow_map)) {
            ImGui::Text("Shadow Array Preview (%dx%d, %d layer%s)",
                        shadow_map->width, shadow_map->height,
                        g_dx.shadow_layers > 0 ? g_dx.shadow_layers : 1,
                        (g_dx.shadow_layers == 1 ? "" : "s"));
            ID3D11ShaderResourceView* preview_srv = ui_render_shadow_depth_preview(shadow_map->width, shadow_map->height, 0);
            if (preview_srv) {
                ui_inspector_text_disabled_wrapped(g_dx.scene_cb_data.light_params[0] >= 0.5f ?
                    "Preview is linearized and inverted for spot depth readability." :
                    "Preview is inverted so nearer shadow casters are brighter.");
                ui_image_fill_panel_width(preview_srv, shadow_map->width, shadow_map->height);
            }
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
        Resource* dl = res_get(g_builtin_light);
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
            dx_create_shadow_map(shadow_size[0], shadow_size[1],
                                 dl && dl->light_type == LIGHT_TYPE_SPOT ? 1 :
                                 (dl && dl->shadow_cascade_count > 0 ? dl->shadow_cascade_count : 1));
        }
        if (dl) {
            ui_inspector_text_disabled_wrapped("%d array layer%s%s",
                                g_dx.shadow_layers > 0 ? g_dx.shadow_layers : 1,
                                g_dx.shadow_layers == 1 ? "" : "s",
                                dl->light_type == LIGHT_TYPE_SPOT ? " (spot)" : "");
        }
        if (ID3D11ShaderResourceView* preview_srv = ui_render_shadow_depth_preview(r->width, r->height, 0))
            ui_image_fill_panel_width(preview_srv, r->width, r->height);
        break;
    }

    default: break;
    }

    if (value_changed) {
        ui_timeline_capture_user_vars_for_resource(h);
        app_request_scene_render();
    }

    if (!r->is_builtin) {
        int note_idx = (int)h - 1;
        if (note_idx >= 0 && note_idx < MAX_RESOURCES)
            ui_inspector_note_editor(r->note, MAX_NOTE,
                                     &s_inspector_resource_note_open[note_idx],
                                     &s_inspector_resource_note_editing[note_idx]);
    }
}

static bool ui_compute_shader_combo(const char* label, ResHandle* h) {
    Resource* cur = res_get(*h);
    if (!cur || !ui_shader_matches_program_kind(*cur, SHADER_PROGRAM_CS))
        *h = INVALID_HANDLE;
    cur = res_get(*h);
    const char* prev = cur ? cur->name : "(none)";
    bool changed = false;
    if (ImGui::BeginCombo(label, prev)) {
        if (ImGui::Selectable("(none)", *h == INVALID_HANDLE)) {
            *h = INVALID_HANDLE;
            changed = true;
        }
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active || !ui_shader_matches_program_kind(r, SHADER_PROGRAM_CS)) continue;
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
        cmd_mark_dirty(child_h);
    }
    if (Command* repeat = cmd_get(parent)) {
        repeat->repeat_expanded = true;
        cmd_mark_dirty(parent);
    }
    app_request_scene_render();
    return child_h;
}

static bool ui_command_has_transform(const Command* c) {
    return c && (c->type == CMD_DRAW_MESH ||
                 c->type == CMD_DRAW_INSTANCED ||
                 c->type == CMD_INDIRECT_DRAW);
}

static bool ui_quat_array_close(const float a[4], const float b[4]) {
    if (!a || !b)
        return false;
    for (int i = 0; i < 4; i++) {
        if (fabsf(a[i] - b[i]) > 0.00001f)
            return false;
    }
    return true;
}

static bool ui_command_rotation_editor(Command* c) {
    static CmdHandle s_edit_cmd = INVALID_HANDLE;
    static float s_edit_euler[3] = {};
    static float s_edit_rotq[4] = {};

    if (!c)
        return false;

    CmdHandle h = cmd_find_by_name(c->name);
    if (h != s_edit_cmd || !ui_quat_array_close(c->rotq, s_edit_rotq)) {
        quat_to_euler_xyz(quat_from_array(c->rotq), h == s_edit_cmd ? s_edit_euler : nullptr, s_edit_euler);
        quat_to_array(quat_from_array(c->rotq), s_edit_rotq);
        s_edit_cmd = h;
    }

    bool changed = ui_tinted_transform_row(
        "Rotation", s_edit_euler, 0.01f,
        ImVec4(0.055f, 0.130f, 0.070f, 0.3f),
        ImVec4(0.080f, 0.200f, 0.105f, 0.5f)
    );
    if (changed) {
        quat_to_array(quat_from_euler_xyz(v3(s_edit_euler[0], s_edit_euler[1], s_edit_euler[2])), c->rotq);
        quat_to_array(quat_from_array(c->rotq), s_edit_rotq);
    }
    return changed;
}

static void ui_command_transform_editor(Command* c) {
    if (!c)
        return;

    bool transform_changed = false;
    if (ui_inspector_section("TRANSFORM")) {
        transform_changed |= ui_tinted_transform_row(
            "Position", c->pos, 0.001f,
            ImVec4(0.150f, 0.055f, 0.050f, 0.3f),
            ImVec4(0.230f, 0.080f, 0.070f, 0.5f)
        );

        transform_changed |= ui_command_rotation_editor(c);

        transform_changed |= ui_tinted_transform_row(
            "Scale", c->scale, 0.001f,
            ImVec4(0.050f, 0.075f, 0.150f, 0.3f),
            ImVec4(0.070f, 0.105f, 0.230f, 0.5f)
        );
    }
    if (transform_changed)
        timeline_capture_if_tracked(TIMELINE_TRACK_COMMAND_TRANSFORM, c->name, RES_NONE);
}

static Resource* ui_clear_source_resource(const char* source_name, ResType type) {
    if (!source_name || !source_name[0])
        return nullptr;
    Resource* r = res_get(res_find_by_name(source_name));
    return (r && r->type == type) ? r : nullptr;
}

static bool ui_clear_resource_source_combo(const char* label, char* source_name, ResType type) {
    if (!source_name)
        return false;

    Resource* current = ui_clear_source_resource(source_name, type);
    char preview[160] = {};
    if (current) {
        snprintf(preview, sizeof(preview), "%s", ui_resource_display_name(*current));
    } else if (source_name[0]) {
        snprintf(preview, sizeof(preview), "(missing) %s", source_name);
    } else {
        snprintf(preview, sizeof(preview), "(hardcoded)");
    }

    bool changed = false;
    ImGui::SetNextItemWidth(ui_labeled_item_compact_width(label));
    if (ImGui::BeginCombo(label, preview)) {
        bool hardcoded = source_name[0] == '\0';
        if (ImGui::Selectable("(hardcoded)", hardcoded)) {
            source_name[0] = '\0';
            changed = true;
        }
        ImGui::Separator();
        ui_inspector_text_disabled_wrapped("Resources");
        bool any_resource = false;
        for (int i = 0; i < MAX_RESOURCES; i++) {
            Resource& r = g_resources[i];
            if (!r.active || r.is_builtin || r.type != type)
                continue;
            any_resource = true;
            bool selected = current == &r;
            ImGui::PushID(i);
            if (ImGui::Selectable(ui_resource_display_name(r), selected)) {
                strncpy(source_name, r.name, MAX_NAME - 1);
                source_name[MAX_NAME - 1] = '\0';
                changed = true;
            }
            ImGui::PopID();
        }
        if (!any_resource)
            ui_inspector_text_disabled_wrapped("No %s resources.", res_type_str(type));
        ImGui::EndCombo();
    }
    return changed;
}

static bool ui_clear_source_valid(const char* source_name, ResType type) {
    return ui_clear_source_resource(source_name, type) != nullptr;
}



// Compute dispatches and indirect compute dispatches share the same shader
// resource model: SRVs are bound to CS t# and UAVs are bound to CS u#.
// The indirect argument buffer is only the source of DispatchIndirect group
// counts; it does not replace the shader inputs/outputs declared by the CS.
static void ui_command_compute_resource_bindings(Command* c, const char* id_suffix, int srv_id_base, int uav_id_base) {
    if (!c)
        return;

    if (!ui_inspector_section("BINDINGS"))
        return;
    ui_hint_text_disabled_wrapped("SRVs are bound to t# in CS. UAVs are bound to u# in CS.");
    ImGui::Text("SRV Slots t# (%d / %d):", c->srv_count, MAX_SRV_SLOTS);
    for (int s = 0; s < c->srv_count; s++) {
        ImGui::PushID(srv_id_base + s);
        char lbl[64]; snprintf(lbl, sizeof(lbl), "t%d srv##%s", (int)c->srv_slots[s], id_suffix);
        res_combo(lbl, &c->srv_handles[s], RES_NONE);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(48.f);
        ImGui::InputScalar("slot##cs_srv_slot", ImGuiDataType_U32, &c->srv_slots[s]);
        ImGui::PopID();
    }
    char add_srv[64]; snprintf(add_srv, sizeof(add_srv), "+##cs_srv_%s", id_suffix);
    char rem_srv[64]; snprintf(rem_srv, sizeof(rem_srv), "-##cs_srv_%s", id_suffix);
    if (c->srv_count < MAX_SRV_SLOTS && ImGui::SmallButton(add_srv)) c->srv_count++;
    if (c->srv_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton(rem_srv)) c->srv_count--; }

    ImGui::Spacing();
    ImGui::Text("UAV Slots u# (%d / %d):", c->uav_count, MAX_UAV_SLOTS);
    for (int u = 0; u < c->uav_count; u++) {
        ImGui::PushID(uav_id_base + u);
        char lbl[64]; snprintf(lbl, sizeof(lbl), "u%d uav##%s", (int)c->uav_slots[u], id_suffix);
        res_combo(lbl, &c->uav_handles[u], RES_NONE);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(48.f);
        ImGui::InputScalar("slot##cs_uav_slot", ImGuiDataType_U32, &c->uav_slots[u]);
        ImGui::PopID();
    }
    char add_uav[64]; snprintf(add_uav, sizeof(add_uav), "+##cs_uav_%s", id_suffix);
    char rem_uav[64]; snprintf(rem_uav, sizeof(rem_uav), "-##cs_uav_%s", id_suffix);
    if (c->uav_count < MAX_UAV_SLOTS && ImGui::SmallButton(add_uav)) c->uav_count++;
    if (c->uav_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton(rem_uav)) c->uav_count--; }
}

static void ui_inspector_command(Command* c) {
    if (!c)
        return;

    CmdHandle inspected_h = INVALID_HANDLE;
    int inspected_idx = (int)(c - g_commands);
    if (inspected_idx >= 0 && inspected_idx < MAX_COMMANDS)
        inspected_h = (CmdHandle)(inspected_idx + 1);

    Command before_edit = *c;

    if (ImGui::Checkbox("Enabled", &c->enabled))
        timeline_capture_if_tracked(TIMELINE_TRACK_COMMAND_ENABLED, c->name, RES_NONE);

    if (ui_command_has_transform(c))
        ui_command_transform_editor(c);

    switch (c->type) {
    case CMD_GROUP: {
        if (ui_inspector_section("GROUP")) {
            if (ImGui::Checkbox("Expanded", &c->repeat_expanded))
                cmd_mark_dirty(inspected_h);
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
                ui_inspector_text_disabled_wrapped("%s", cmd_type_str(child.type));
                ImGui::PopID();
            }
        }
        break;
    }

    case CMD_REPEAT: {
        if (ui_inspector_section("REPEAT")) {
            ImGui::InputInt("Iterations", &c->repeat_count);
            if (c->repeat_count < 1) c->repeat_count = 1;
            if (ImGui::Checkbox("Expanded", &c->repeat_expanded))
                cmd_mark_dirty(inspected_h);
        }

        static CmdHandle s_repeat_parent = INVALID_HANDLE;
        static ResHandle s_repeat_shader = INVALID_HANDLE;
        if (s_repeat_parent != g_sel_cmd) {
            s_repeat_parent = g_sel_cmd;
            s_repeat_shader = INVALID_HANDLE;
        }
        CmdHandle repeat_h = g_sel_cmd;
        if (ui_inspector_section("COMPUTE SHADERS")) {
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
                ui_inspector_text_disabled_wrapped("%s", cmd_type_str(child.type));
                ImGui::PopID();
            }
        }
        break;
    }

    case CMD_CLEAR: {
        bool clear_changed = false;
        if (ui_inspector_section("CLEAR")) {
            clear_changed |= ImGui::Checkbox("Clear Color", &c->clear_color_enabled);
            if (c->clear_color_enabled) {
                clear_changed |= ui_clear_resource_source_combo("Color Source", c->clear_color_source, RES_FLOAT4);
                Resource* color_res = ui_clear_source_resource(c->clear_color_source, RES_FLOAT4);
                if (color_res) {
                    ImGui::TextWrapped("Clear color is driven by resource '%s'.", color_res->name);
                } else if (ui_clear_source_valid(c->clear_color_source, RES_FLOAT4)) {
                    ImGui::TextWrapped("Clear color is driven by resource '%s'.", c->clear_color_source);
                } else {
                    if (c->clear_color_source[0])
                        ImGui::TextWrapped("Selected color source is missing or not float4; using the hardcoded fallback value.");
                    ui_inspector_text_disabled_wrapped("Clear Color Value");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    clear_changed |= ImGui::ColorEdit4("##clear_color_value", c->clear_color);
                }
            }
            clear_changed |= ImGui::Checkbox("Clear Depth", &c->clear_depth);
            if (c->clear_depth) {
                clear_changed |= ui_clear_resource_source_combo("Depth Source", c->clear_depth_source, RES_FLOAT);
                Resource* depth_res = ui_clear_source_resource(c->clear_depth_source, RES_FLOAT);
                if (depth_res) {
                    ImGui::TextWrapped("Depth clear is driven by resource '%s'.", depth_res->name);
                } else if (ui_clear_source_valid(c->clear_depth_source, RES_FLOAT)) {
                    ImGui::TextWrapped("Depth clear is driven by resource '%s'.", c->clear_depth_source);
                } else {
                    if (c->clear_depth_source[0])
                        ImGui::TextWrapped("Selected depth source is missing or not float; using the hardcoded fallback value.");
                    ui_inspector_text_disabled_wrapped("Depth Value");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    clear_changed |= ImGui::DragFloat("##depth_value", &c->depth_clear_val, 0.01f, 0.f, 1.f);
                }
            }
        }
        if (clear_changed)
            app_request_scene_render();

        if (ui_inspector_section("TARGETS")) {
            res_combo_render_target("Render Target##clrt", &c->rt);
            res_combo_depth_target("Depth Buffer##cldp", &c->depth);
        }
        break;
    }

    case CMD_DRAW_MESH:
    case CMD_DRAW_INSTANCED: {
        if (ui_inspector_section("DRAW SOURCE & SHADER")) {
            bool source_changed = ui_draw_source_combo("Source##dm", &c->draw_source);
            if (source_changed && c->draw_source == DRAW_SOURCE_PROCEDURAL && c->vertex_count < 1)
                c->vertex_count = 3;
            if (ui_command_uses_procedural_draw(*c)) {
                ui_draw_topology_combo("Topology##dm", &c->draw_topology);
                ImGui::InputInt("Vertex Count", &c->vertex_count);
                if (c->vertex_count < 0) c->vertex_count = 0;
                ui_hint_text_disabled_wrapped("No mesh is bound. The VS should use SV_VertexID / VS SRVs.");
            } else {
                res_combo("Mesh##dm", &c->mesh, RES_MESH, false);
            }
            res_combo_shader_kind("Shader##dm", &c->shader, SHADER_PROGRAM_VSPS, false);
            if (c->type == CMD_DRAW_INSTANCED) {
                res_combo_instance_source("Instance From##dm", &c->instance_count_source);
                if (c->instance_count_source != INVALID_HANDLE) {
                    Resource* src = res_get(c->instance_count_source);
                    ImGui::Text("Resolved instances: %d", src ? (src->ival[0] > 0 ? src->ival[0] : 1) : 1);
                } else {
                    ImGui::InputInt("Instance Count", &c->instance_count);
                    if (c->instance_count < 1)
                        c->instance_count = 1;
                }
            }
        }
        ui_draw_command_bounds_inspector(c, g_sel_cmd);

        if (ui_inspector_section("SHADER PARAMETERS"))
            ui_command_shader_params(c, res_get(c->shader));

        if (ui_inspector_section("TARGETS")) {
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
        }

        if (ui_inspector_section("RENDER STATE")) {
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
                res_combo_shader_kind("Shadow Shader##dm", &c->shadow_shader, SHADER_PROGRAM_VSPS);
        }

        if (ui_inspector_section("SRV BINDINGS")) {
            ui_hint_text_disabled_wrapped("SRVs are bound to t# in both VS and PS. Mesh material textures still bind PS slots first; manual SRVs override them.");
            ui_hint_text_disabled_wrapped("Reserved PS material slots: t0 base, t1 metal-rough, t2 normal, t3 emissive, t4 occlusion, t5 env, t7 shadow.");
            if (s_show_interface_hints) {
                if (ImGui::TreeNodeEx("Slot Reference##draw_slots", ImGuiTreeNodeFlags_None)) {
                    ui_draw_texture_slot_reference("##draw_slot_reference");
                    ImGui::TreePop();
                }
            }
            ImGui::Text("SRV Slots (%d / %d):", c->srv_count, MAX_SRV_SLOTS);
            for (int s = 0; s < c->srv_count; s++) {
                ImGui::PushID(100 + s);
                char lbl[32]; snprintf(lbl, sizeof(lbl), "t%d srv", (int)c->srv_slots[s]);
                res_combo(lbl, &c->srv_handles[s], RES_NONE);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(48.f);
                ImGui::InputScalar("##sslot", ImGuiDataType_U32, &c->srv_slots[s]);
                ImGui::PopID();
            }
            if (c->srv_count < MAX_SRV_SLOTS && ImGui::SmallButton("+##s")) c->srv_count++;
            if (c->srv_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##s")) c->srv_count--; }
        }

        if (ui_inspector_section("PIXEL UAV OUTPUTS")) {
            UINT draw_rtv_count = ui_draw_command_rtv_count(*c);
            ui_hint_text_disabled_wrapped("DX11 output slots are shared by RTVs and PS UAVs.");
            ui_hint_text_disabled_wrapped("With %u RTV%s active, the first valid UAV slot is u%u.",
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
        }
        break;
    }

    case CMD_DISPATCH: {
        if (ui_inspector_section("COMPUTE")) {
            res_combo_shader_kind("Compute Shader##dp", &c->shader, SHADER_PROGRAM_CS, false);
            ImGui::Checkbox("Only On Reset", &c->compute_on_reset);
            res_combo_dispatch_source("Dispatch From##dp", &c->dispatch_size_source);
            if (c->dispatch_size_source != INVALID_HANDLE) {
                ImGui::InputInt3("Divisor XYZ", &c->thread_x);
                ui_hint_text_disabled_wrapped("Dispatch = ceil(source_size / divisor)");
            } else {
                ImGui::InputInt3("Dispatch XYZ", &c->thread_x);
            }
        }

        if (ui_inspector_section("SHADER PARAMETERS"))
            ui_command_shader_params(c, res_get(c->shader));

        ui_command_compute_resource_bindings(c, "dispatch", 200, 300);
        break;
    }

    case CMD_INDIRECT_DRAW: {
        if (ui_inspector_section("INDIRECT COMMAND")) {
            bool source_changed = ui_draw_source_combo("Source##id", &c->draw_source);
            if (source_changed && c->draw_source == DRAW_SOURCE_PROCEDURAL)
                c->draw_topology = DRAW_TOPOLOGY_TRIANGLE_LIST;
            Resource* mesh = res_get(c->mesh);
            if (ui_command_uses_procedural_draw(*c)) {
                ui_draw_topology_combo("Topology##id", &c->draw_topology);
                ui_hint_text_disabled_wrapped("Uses DrawInstancedIndirect args.");
            } else {
                res_combo("Mesh##id", &c->mesh, RES_MESH, false);
                ui_hint_text_disabled_wrapped("%s",
                    (mesh && mesh->ib) ? "Uses DrawIndexedInstancedIndirect args."
                                       : "Uses DrawInstancedIndirect args.");
            }
            res_combo_shader_kind("Shader##id", &c->shader, SHADER_PROGRAM_VSPS, false);
        }
        ui_draw_command_bounds_inspector(c, g_sel_cmd);
        if (ui_inspector_section("SHADER PARAMETERS"))
            ui_command_shader_params(c, res_get(c->shader));
        if (ui_inspector_section("TARGETS")) {
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
        }

        if (ui_inspector_section("RENDER STATE")) {
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
                res_combo_shader_kind("Shadow Shader##id", &c->shadow_shader, SHADER_PROGRAM_VSPS);
        }

        if (ui_inspector_section("SRV BINDINGS")) {
            ui_hint_text_disabled_wrapped("SRVs are bound to t# in both VS and PS. Mesh material textures still bind PS slots first; manual SRVs override them.");
            ui_hint_text_disabled_wrapped("Reserved PS material slots: t0 base, t1 metal-rough, t2 normal, t3 emissive, t4 occlusion, t5 env, t7 shadow.");
            if (s_show_interface_hints) {
                if (ImGui::TreeNodeEx("Slot Reference##indirect_draw_slots", ImGuiTreeNodeFlags_None)) {
                    ui_draw_texture_slot_reference("##indirect_draw_slot_reference");
                    ImGui::TreePop();
                }
            }
            ImGui::Text("SRV Slots (%d / %d):", c->srv_count, MAX_SRV_SLOTS);
            for (int s = 0; s < c->srv_count; s++) {
                ImGui::PushID(1000 + s);
                char lbl[32]; snprintf(lbl, sizeof(lbl), "t%d srv", (int)c->srv_slots[s]);
                res_combo(lbl, &c->srv_handles[s], RES_NONE);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(48.f);
                ImGui::InputScalar("##idsslot", ImGuiDataType_U32, &c->srv_slots[s]);
                ImGui::PopID();
            }
            if (c->srv_count < MAX_SRV_SLOTS && ImGui::SmallButton("+##id_s")) c->srv_count++;
            if (c->srv_count > 0) { ImGui::SameLine(); if (ImGui::SmallButton("-##id_s")) c->srv_count--; }
        }

        if (ui_inspector_section("PIXEL UAV OUTPUTS")) {
            UINT draw_rtv_count = ui_draw_command_rtv_count(*c);
            ui_hint_text_disabled_wrapped("DX11 output slots are shared by RTVs and PS UAVs.");
            ui_hint_text_disabled_wrapped("With %u RTV%s active, the first valid UAV slot is u%u.",
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
        }

        if (ui_inspector_section("INDIRECT BUFFER")) {
            res_combo("Indirect Buffer##id", &c->indirect_buf, RES_STRUCTURED_BUFFER, false);
            ImGui::InputScalar("Byte Offset", ImGuiDataType_U32, &c->indirect_offset);
        }
        break;
    }

    case CMD_INDIRECT_DISPATCH: {
        if (ui_inspector_section("INDIRECT COMMAND")) {
            res_combo_shader_kind("Shader##id", &c->shader, SHADER_PROGRAM_CS, false);
            ImGui::Checkbox("Only On Reset", &c->compute_on_reset);
        }
        if (ui_inspector_section("SHADER PARAMETERS"))
            ui_command_shader_params(c, res_get(c->shader));
        ui_command_compute_resource_bindings(c, "indirect_dispatch", 1200, 1300);
        if (ui_inspector_section("INDIRECT BUFFER")) {
            res_combo("Indirect Buffer##id", &c->indirect_buf, RES_STRUCTURED_BUFFER, false);
            ImGui::InputScalar("Byte Offset", ImGuiDataType_U32, &c->indirect_offset);
        }
        break;
    }

    default: break;
    }

    if (inspected_idx >= 0 && inspected_idx < MAX_COMMANDS)
        ui_inspector_note_editor(c->note, MAX_NOTE,
                                 &s_inspector_command_note_open[inspected_idx],
                                 &s_inspector_command_note_editing[inspected_idx]);

    if (inspected_h != INVALID_HANDLE && memcmp(&before_edit, c, sizeof(Command)) != 0) {
        bool graph_changed = before_edit.parent != c->parent ||
                             before_edit.type != c->type ||
                             before_edit.active != c->active ||
                             before_edit.repeat_count != c->repeat_count;
        (void)graph_changed;
        cmd_mark_dirty(inspected_h);
        app_request_scene_render();
    }
}

static bool ui_inspector_variable_value_editor(Resource* r, ResHandle h) {
    if (!r)
        return false;

    bool changed = false;
    float value_w = ImGui::GetContentRegionAvail().x;
    if (value_w < ui_px(80.0f))
        value_w = ui_px(80.0f);
    switch (r->type) {
    case RES_INT:
        ImGui::SetNextItemWidth(value_w);
        changed = ImGui::InputInt("##var_value", &r->ival[0]);
        break;
    case RES_INT2:
        ImGui::SetNextItemWidth(value_w);
        changed = ImGui::InputInt2("##var_value", r->ival);
        break;
    case RES_INT3:
        ImGui::SetNextItemWidth(value_w);
        changed = ImGui::InputInt3("##var_value", r->ival);
        break;
    case RES_FLOAT:
    case RES_FLOAT2:
    case RES_FLOAT3:
    case RES_FLOAT4:
        changed = ui_float_value_editor("##var_value", r->type, r->fval, value_w);
        break;
    case RES_BUILTIN_TIME: {
        float t = app_scene_time();
        ImGui::SetNextItemWidth(value_w);
        if (ImGui::InputFloat("##scene_time_value", &t, 0.0f, 0.0f, "%.3f")) {
            app_set_scene_time(t);
            changed = true;
        }
        break;
    }
    default:
        ui_inspector_text_disabled_wrapped("%s", ui_resource_display_type(*r));
        break;
    }

    if (changed) {
        if (!r->is_builtin)
            ui_timeline_capture_user_vars_for_resource(h);
        app_request_scene_render();
    }
    return changed;
}

static void ui_inspector_variable_resource_row(ResHandle h) {
    Resource* r = res_get(h);
    if (!r)
        return;

    ImGui::PushID((int)h);
    float row_h = ImGui::GetFrameHeight();
    float row_w = ImGui::GetContentRegionAvail().x - ui_current_vertical_scroll_margin(6.0f);
    if (row_w < ui_px(96.0f)) row_w = ui_px(96.0f);
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    bool show_type_badge = r->type == RES_BUILTIN_TIME || r->type == RES_BUILTIN_LIGHT;
    float name_w = ImMin(ui_px(show_type_badge ? 118.0f : 185.0f), row_w * (show_type_badge ? 0.34f : 0.42f));
    float type_w = show_type_badge ? ImMin(ui_px(74.0f), row_w * 0.22f) : 0.0f;
    if (name_w < ui_px(54.0f)) name_w = ui_px(54.0f);
    if (show_type_badge && type_w < ui_px(42.0f)) type_w = ui_px(42.0f);
    float min_value_w = ui_px(72.0f);
    float used_w = name_w + type_w + spacing * (show_type_badge ? 2.0f : 1.0f);
    if (row_w - used_w < min_value_w) {
        float deficit = min_value_w - (row_w - used_w);
        float name_room = ImMax(0.0f, name_w - ui_px(54.0f));
        float take = ImMin(name_room, deficit);
        name_w -= take;
        deficit -= take;
        if (show_type_badge) {
            float type_room = ImMax(0.0f, type_w - ui_px(42.0f));
            take = ImMin(type_room, deficit);
            type_w -= take;
        }
    }
    char name_fit[MAX_NAME] = {};
    ui_fit_text_ellipsis(ui_resource_display_name(*r), name_w, name_fit, sizeof(name_fit));
    ImVec2 name_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##var_name", ImVec2(name_w, row_h));
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(name_pos.x, name_pos.y + floorf((row_h - ImGui::GetTextLineHeight()) * 0.5f)),
        ImGui::GetColorU32(ImGuiCol_Text), name_fit);
    if (ImGui::IsItemHovered() && strcmp(name_fit, ui_resource_display_name(*r)) != 0)
        ImGui::SetTooltip("%s", ui_resource_display_name(*r));
    ImGui::SameLine();
    if (show_type_badge) {
        ImVec2 badge_pos = ImGui::GetCursorScreenPos();
        ui_draw_badge(ImGui::GetWindowDrawList(), ImVec2(badge_pos.x, badge_pos.y + ui_px(2.0f)),
                      ui_resource_display_type(*r), ui_type_color(r->type));
        ImGui::Dummy(ImVec2(type_w, row_h));
        ImGui::SameLine();
    }
    ui_inspector_variable_value_editor(r, h);
    ImGui::PopID();
}

static void ui_inspector_builtin_light_summary(Resource* r) {
    if (!r)
        return;

    bool changed = false;
    const char* light_types[] = { "Directional", "Spot" };
    int light_type = r->light_type == LIGHT_TYPE_SPOT ? 1 : 0;
    if (ImGui::Combo("Light Type", &light_type, light_types, 2)) {
        r->light_type = light_type == 1 ? LIGHT_TYPE_SPOT : LIGHT_TYPE_DIRECTIONAL;
        changed = true;
    }
    changed |= ImGui::ColorEdit3("Light Color", r->light_color);
    changed |= ImGui::DragFloat("Light Intensity", &r->light_intensity, 0.01f, 0.0f, 10.0f);
    changed |= ImGui::DragFloat3("Light Target", r->light_target, 0.01f);
    changed |= ImGui::DragFloat3("Light Position", r->light_pos, 0.01f);
    if (ImGui::Checkbox("Debug Draw Light", &r->light_debug_draw))
        app_request_scene_render();
    if (r->light_type == LIGHT_TYPE_SPOT) {
        changed |= ImGui::DragFloat("Spot Angle", &r->spot_angle, 0.01f, 0.05f, 3.0f);
        changed |= ImGui::DragFloat("Spot Softness", &r->spot_softness, 0.01f, 0.0f, 0.95f);
        r->spot_angle = clampf(r->spot_angle, 0.05f, 3.0f);
        r->spot_softness = clampf(r->spot_softness, 0.0f, 0.95f);
    }
    if (changed) {
        timeline_capture_if_tracked(TIMELINE_TRACK_LIGHT, "light", RES_NONE);
        app_request_scene_render();
    }
}

static ID3D11ShaderResourceView* ui_runtime_preview_srv(const Resource* r) {
    if (!r)
        return nullptr;
    switch (r->type) {
    case RES_BUILTIN_SCENE_COLOR: return g_dx.scene_srv;
    case RES_BUILTIN_SCENE_DEPTH: return g_dx.depth_srv;
    case RES_BUILTIN_SHADOW_MAP:  return ui_render_shadow_depth_preview(g_dx.shadow_width, g_dx.shadow_height, 0);
    case RES_RENDER_TEXTURE2D:    return r->srv;
    default:                      return nullptr;
    }
}

static bool ui_runtime_preview_resource_dims(const Resource* r, int* out_w, int* out_h) {
    if (!r)
        return false;
    int w = r->width;
    int h = r->height;
    if (r->type == RES_BUILTIN_SCENE_COLOR || r->type == RES_BUILTIN_SCENE_DEPTH) {
        w = g_dx.scene_width;
        h = g_dx.scene_height;
    } else if (r->type == RES_BUILTIN_SHADOW_MAP) {
        w = g_dx.shadow_width;
        h = g_dx.shadow_height;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

static bool ui_is_runtime_preview_render_target(const Resource& r) {
    if (!r.active || r.is_generated)
        return false;
    return r.type == RES_RENDER_TEXTURE2D ||
           r.type == RES_BUILTIN_SCENE_COLOR ||
           r.type == RES_BUILTIN_SCENE_DEPTH ||
           r.type == RES_BUILTIN_SHADOW_MAP;
}

static void ui_inspector_render_target_grid() {
    if (!ui_inspector_section("RENDER TARGETS"))
        return;

    ResHandle handles[MAX_RESOURCES] = {};
    int count = 0;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (ui_is_runtime_preview_render_target(g_resources[i]))
            handles[count++] = (ResHandle)(i + 1);
    }

    if (count == 0) {
        ui_inspector_text_disabled_wrapped("No render targets.");
        return;
    }

    float table_w = ImGui::GetContentRegionAvail().x - ui_current_vertical_scroll_margin(6.0f);
    if (table_w < ui_px(48.0f)) table_w = ui_px(48.0f);
    int cols = s_render_target_preview_columns;
    if (cols < 1) cols = 1;
    if (cols > 8) cols = 8;
    int cols_that_fit = (int)floorf(table_w / ui_px(54.0f));
    if (cols_that_fit < 1) cols_that_fit = 1;
    if (cols > cols_that_fit) cols = cols_that_fit;
    int row_count = (count + cols - 1) / cols;
    float approx_cell_w = table_w / (float)cols;
    if (approx_cell_w < ui_px(48.0f)) approx_cell_w = ui_px(48.0f);
    float approx_thumb_h = approx_cell_w * 0.62f;
    if (approx_thumb_h > ui_px(78.0f)) approx_thumb_h = ui_px(78.0f);
    float row_h = approx_thumb_h + ImGui::GetTextLineHeightWithSpacing() * 2.0f + ui_margin_px(8.0f);

    if (ImGui::BeginTable("##rt_preview_grid", cols, ImGuiTableFlags_SizingStretchSame)) {
        ImGuiListClipper clipper;
        clipper.Begin(row_count, row_h);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                ImGui::TableNextRow(0, row_h);
                for (int col = 0; col < cols; col++) {
                    int item = row * cols + col;
                    ImGui::TableSetColumnIndex(col);
                    if (item >= count)
                        continue;

                    Resource* r = res_get(handles[item]);
                    if (!r)
                        continue;

                    ImGui::PushID((int)handles[item]);
                    float cell_w = ImGui::GetContentRegionAvail().x;
                    if (cell_w < ui_px(48.0f)) cell_w = ui_px(48.0f);
                    float thumb_h = cell_w * 0.62f;
                    if (thumb_h > ui_px(78.0f)) thumb_h = ui_px(78.0f);

                    ImVec2 thumb_size(cell_w, thumb_h);
                    ImGui::InvisibleButton("##rt_thumb", thumb_size);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 min = ImGui::GetItemRectMin();
                    ImVec2 max = ImGui::GetItemRectMax();
                    dl->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4(0.055f, 0.052f, 0.055f, 1.0f)), ui_px(3.0f));
                    dl->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.24f, 0.22f, 0.21f, 0.85f)), ui_px(3.0f));

                    int w = 1, h = 1;
                    ui_runtime_preview_resource_dims(r, &w, &h);
                    ID3D11ShaderResourceView* srv = ui_runtime_preview_srv(r);
                    if (srv) {
                        float src_w = (float)w;
                        float src_h = (float)h;
                        float scale = (thumb_size.x - ui_px(6.0f)) / src_w;
                        float sy = (thumb_size.y - ui_px(6.0f)) / src_h;
                        if (sy < scale) scale = sy;
                        if (scale <= 0.0f) scale = 1.0f;
                        ImVec2 img_size(src_w * scale, src_h * scale);
                        ImVec2 img_min(min.x + (thumb_size.x - img_size.x) * 0.5f,
                                       min.y + (thumb_size.y - img_size.y) * 0.5f);
                        dl->AddImage((ImTextureID)srv, img_min, ImVec2(img_min.x + img_size.x, img_min.y + img_size.y));
                    }
                    char name_fit[MAX_NAME] = {};
                    ui_fit_text_ellipsis(ui_resource_display_name(*r), cell_w, name_fit, sizeof(name_fit));
                    ImGui::TextUnformatted(name_fit);
                    ImGui::TextDisabled("%dx%d", w, h);
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndTable();
    }
}

static void ui_inspector_variables_tab() {
    ResHandle vars[MAX_RESOURCES] = {};
    int var_count = ui_collect_variable_inspector_resources(vars, MAX_RESOURCES);
    int user_count = 0;
    for (int i = 0; i < var_count; i++) {
        Resource* r = res_get(vars[i]);
        if (r && !r->is_builtin)
            user_count++;
    }

    if (ui_inspector_section("USER VARIABLES")) {
        if (user_count > 0) {
            for (int i = 0; i < var_count; i++) {
                Resource* r = res_get(vars[i]);
                if (!r || r->is_builtin)
                    continue;
                ui_inspector_variable_resource_row(vars[i]);
            }
        } else {
            ui_inspector_text_disabled_wrapped("No user scalar/vector variables.");
        }
    }

    if (ui_inspector_section("BUILT-IN VARIABLES")) {
        if (Resource* t = res_get(g_builtin_time))
            ui_inspector_variable_resource_row(g_builtin_time);
        if (Resource* dl = res_get(g_builtin_light)) {
            ImGui::PushID((int)g_builtin_light);
            ImGui::TextUnformatted(ui_resource_display_name(*dl));
            ui_inspector_builtin_light_summary(dl);
            ImGui::PopID();
        }
    }

    ui_inspector_render_target_grid();
}

static void ui_inspector_selection_tab() {
    if (g_sel_res != INVALID_HANDLE) {
        Resource* r = res_get(g_sel_res);
        if (r) {
            ImGui::PushID((int)g_sel_res);
            ui_inspector_resource(r, g_sel_res);
            ImGui::PopID();
        } else {
            ui_inspector_text_disabled_wrapped("(stale selection)");
        }
    } else if (g_sel_cmd != INVALID_HANDLE) {
        Command* c = cmd_get(g_sel_cmd);
        if (c) ui_inspector_command(c);
        else   ui_inspector_text_disabled_wrapped("(stale selection)");
    } else {
        ui_inspector_text_disabled_wrapped("Nothing selected.");
        ui_inspector_text_disabled_wrapped("Right-click Resources or Commands panels to create items.");
    }
}

static void ui_panel_inspector(bool embedded = false) {
    if (!embedded) ImGui::Begin("Inspector");
    ui_inspector_selection_tab();
    if (!embedded) ImGui::End();
}

static void ui_panel_resources_viewer(bool embedded = false) {
    if (!embedded) ImGui::Begin("Resources Viewer");
    ui_inspector_variables_tab();
    if (!embedded) ImGui::End();
}

// -- user cb panel ---------------------------------------------------------

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
    ui_inspector_text_disabled_wrapped("%s", stage ? stage : "-");
    if (slot >= 0) {
        ImGui::SameLine();
        ui_inspector_text_disabled_wrapped("slot %d", slot);
    }

    if (r) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_type_color(r->type));
        ImGui::TextUnformatted(ui_resource_display_name(*r));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ui_inspector_text_disabled_wrapped("%s", ui_resource_display_type(*r));
    } else {
        ui_inspector_text_disabled_wrapped("(none)");
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
            ui_inspector_text_disabled_wrapped("(stale command)");
        } else {
            switch (c->type) {
            case CMD_CLEAR:
                if (ui_inspector_section("OUTPUTS")) {
                    ui_binding_row("Render Target", "OM", -1, c->rt);
                    ui_binding_row("Depth Buffer", "OM", -1, c->depth);
                }
                break;
            case CMD_DRAW_MESH:
            case CMD_DRAW_INSTANCED:
                if (ui_inspector_section("PIPELINE")) {
                    if (ui_command_uses_procedural_draw(*c))
                        ui_inspector_text_disabled_wrapped("Source: Procedural (%s)", ui_draw_topology_name(c->draw_topology));
                    else
                        ui_binding_row("Mesh", "IA", -1, c->mesh);
                    ui_binding_row("Shader", "VS/PS", -1, c->shader);
                    if (c->shadow_cast)
                        ui_binding_row("Shadow Shader", "VS", -1, c->shadow_shader);
                }
                if (ui_inspector_section("OUTPUTS")) {
                    ui_binding_row("Render Target 0", "OM", 0, c->rt);
                    for (int i = 0; i < c->mrt_count; i++) {
                        char role[32];
                        snprintf(role, sizeof(role), "Render Target %d", i + 1);
                        ui_binding_row(role, "OM", i + 1, c->mrt_handles[i]);
                    }
                    ui_binding_row("Depth Buffer", "OM", -1, c->depth);
                    for (int i = 0; i < c->uav_count; i++)
                        ui_binding_row("UAV", "OM/PS", (int)c->uav_slots[i], c->uav_handles[i]);
                }
                if (c->srv_count > 0 && ui_inspector_section("SHADER RESOURCES")) {
                    for (int i = 0; i < c->srv_count; i++)
                        ui_binding_row("SRV", "VS/PS", (int)c->srv_slots[i], c->srv_handles[i]);
                }
                break;
            case CMD_DISPATCH:
                if (ui_inspector_section("COMPUTE"))
                    ui_binding_row("Compute Shader", "CS", -1, c->shader);
                if (c->srv_count > 0 && ui_inspector_section("INPUTS")) {
                    for (int i = 0; i < c->srv_count; i++)
                        ui_binding_row("SRV", "CS", (int)c->srv_slots[i], c->srv_handles[i]);
                }
                if (c->uav_count > 0 && ui_inspector_section("OUTPUTS")) {
                    for (int i = 0; i < c->uav_count; i++)
                        ui_binding_row("UAV", "CS", (int)c->uav_slots[i], c->uav_handles[i]);
                }
                break;
            case CMD_INDIRECT_DRAW:
                if (ui_inspector_section("PIPELINE")) {
                    if (ui_command_uses_procedural_draw(*c))
                        ui_inspector_text_disabled_wrapped("Source: Procedural (%s)", ui_draw_topology_name(c->draw_topology));
                    else
                        ui_binding_row("Mesh", "IA", -1, c->mesh);
                    ui_binding_row("Shader", "VS/PS", -1, c->shader);
                    if (c->shadow_cast)
                        ui_binding_row("Shadow Shader", "VS", -1, c->shadow_shader);
                }
                if (ui_inspector_section("OUTPUTS")) {
                    ui_binding_row("Render Target 0", "OM", 0, c->rt);
                    for (int i = 0; i < c->mrt_count; i++) {
                        char role[32];
                        snprintf(role, sizeof(role), "Render Target %d", i + 1);
                        ui_binding_row(role, "OM", i + 1, c->mrt_handles[i]);
                    }
                    ui_binding_row("Depth Buffer", "OM", -1, c->depth);
                    for (int i = 0; i < c->uav_count; i++)
                        ui_binding_row("UAV", "OM/PS", (int)c->uav_slots[i], c->uav_handles[i]);
                }
                if (c->srv_count > 0 && ui_inspector_section("SHADER RESOURCES")) {
                    for (int i = 0; i < c->srv_count; i++)
                        ui_binding_row("SRV", "VS/PS", (int)c->srv_slots[i], c->srv_handles[i]);
                }
                if (ui_inspector_section("ARGUMENTS"))
                    ui_binding_row("Indirect Buffer", "ARG", -1, c->indirect_buf);
                break;
            case CMD_INDIRECT_DISPATCH:
                if (ui_inspector_section("COMPUTE"))
                    ui_binding_row("Compute Shader", "CS", -1, c->shader);
                if (c->srv_count > 0 && ui_inspector_section("INPUTS")) {
                    for (int i = 0; i < c->srv_count; i++)
                        ui_binding_row("SRV", "CS", (int)c->srv_slots[i], c->srv_handles[i]);
                }
                if (c->uav_count > 0 && ui_inspector_section("OUTPUTS")) {
                    for (int i = 0; i < c->uav_count; i++)
                        ui_binding_row("UAV", "CS", (int)c->uav_slots[i], c->uav_handles[i]);
                }
                if (ui_inspector_section("ARGUMENTS"))
                    ui_binding_row("Indirect Buffer", "ARG", -1, c->indirect_buf);
                break;
            default:
                break;
            }
        }
    } else if (g_sel_res != INVALID_HANDLE) {
        Resource* r = res_get(g_sel_res);
        if (!r) {
            ui_inspector_text_disabled_wrapped("(stale resource)");
        } else if (r->type == RES_SHADER && r->shader_cb.active && ui_inspector_section("REFLECTED CBUFFER")) {
            ui_inspector_text_disabled_wrapped("%s: register(b%u), %u bytes",
                r->shader_cb.name, r->shader_cb.bind_slot, r->shader_cb.size);
            for (int i = 0; i < r->shader_cb.var_count; i++) {
                const ShaderCBVar& v = r->shader_cb.vars[i];
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.080f, 0.077f, 0.081f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
                ImGui::BeginChild("##shader_var_card", ImVec2(0.0f, 58.0f), true);
                ImGui::TextUnformatted(v.name);
                ImGui::SameLine();
                ui_inspector_text_disabled_wrapped("%s", res_type_str(v.type));
                ui_inspector_text_disabled_wrapped("offset %u  size %u", v.offset, v.size);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        } else if (r->type == RES_MESH && (r->mesh_part_count > 0 || r->mesh_material_count > 0)) {
            if (r->mesh_part_count > 0) {
                if (ui_inspector_section("MESH PARTS")) {
                    for (int i = 0; i < r->mesh_part_count; i++) {
                        MeshPart& part = r->mesh_parts[i];
                        ImGui::Text("%s [%s]", part.name[0] ? part.name : "(part)", part.enabled ? "on" : "off");
                    }
                }
            }
            if (r->mesh_material_count > 0) {
                if (ui_inspector_section("MESH MATERIALS")) {
                    for (int mi = 0; mi < r->mesh_material_count; mi++) {
                        MeshMaterial& mat = r->mesh_materials[mi];
                        ui_inspector_text_disabled_wrapped("%s", mat.name[0] ? mat.name : "(material)");
                        for (int slot = 0; slot < MAX_MESH_MATERIAL_TEXTURES; slot++) {
                            if (mat.textures[slot] != INVALID_HANDLE)
                                ui_key_value_handle(ui_mesh_material_slot_name(slot), mat.textures[slot]);
                        }
                    }
                }
            }
        } else {
            if (ui_inspector_section("GPU BIND FLAGS")) {
                ImGui::Text("RTV: %s", r->has_rtv ? "yes" : "no");
                ImGui::Text("SRV: %s", r->has_srv ? "yes" : "no");
                ImGui::Text("UAV: %s", r->has_uav ? "yes" : "no");
                ImGui::Text("DSV: %s", r->has_dsv ? "yes" : "no");
            }
        }
    } else {
        ui_inspector_text_disabled_wrapped("Nothing selected.");
    }

    if (!embedded) ImGui::End();
}

static void ui_panel_selection_state(bool embedded = false) {
    if (!embedded) ImGui::Begin("State");

    if (g_sel_cmd != INVALID_HANDLE) {
        Command* c = cmd_get(g_sel_cmd);
        if (!c) {
            ui_inspector_text_disabled_wrapped("(stale command)");
        } else {
            if (ui_inspector_section("COMMAND STATE")) {
                ImGui::Text("Enabled: %s", c->enabled ? "yes" : "no");
                ImGui::Text("Warning: %s", ui_command_has_warning(*c) ? "yes" : "no");
                ImGui::Text("Type: %s", cmd_type_str(c->type));
            }
            if (c->type == CMD_DRAW_MESH || c->type == CMD_DRAW_INSTANCED || c->type == CMD_INDIRECT_DRAW) {
                if (ui_inspector_section("DRAW STATE")) {
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
            }
            if (c->type == CMD_DISPATCH) {
                if (ui_inspector_section("DISPATCH"))
                    ImGui::Text("Threads: %d, %d, %d", c->thread_x, c->thread_y, c->thread_z);
            }
        }
    } else if (g_sel_res != INVALID_HANDLE) {
        Resource* r = res_get(g_sel_res);
        if (!r) {
            ui_inspector_text_disabled_wrapped("(stale resource)");
        } else {
            if (ui_inspector_section("RESOURCE STATE")) {
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
        }
    } else {
        ui_inspector_text_disabled_wrapped("Nothing selected.");
    }

    if (!embedded) ImGui::End();
}

static void ui_panel_user_cb() {
    ImGui::Begin("User CB (b2)");
    user_cb_enforce_unique_names();

    ui_inspector_text_disabled_wrapped("Slot = 16 bytes (float4). Recommended: cbuffer UserCB : register(b2).");
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
            } else if (e.source_kind == USER_CB_SOURCE_LIGHT_POSITION) {
                snprintf(source_label, sizeof(source_label), "Light Position");
            } else if (e.source_kind == USER_CB_SOURCE_LIGHT_TARGET) {
                snprintf(source_label, sizeof(source_label), "Light Target");
            } else {
                snprintf(source_label, sizeof(source_label), "(hardcoded)");
            }
            if (ImGui::BeginCombo("##source", source_label)) {
                if (ImGui::Selectable("(hardcoded)", e.source_kind == USER_CB_SOURCE_NONE)) {
                    user_cb_set_source(i, INVALID_HANDLE);
                    user_changed = true;
                }
                ImGui::Separator();
                ui_inspector_text_disabled_wrapped("Resources");
                for (int r_i = 0; r_i < MAX_RESOURCES; r_i++) {
                    Resource& r = g_resources[r_i];
                    if (!r.active || r.is_builtin || ui_resource_is_size_source_resource(r) || r.type != e.type) continue;
                    ResHandle h = (ResHandle)(r_i + 1);
                    bool sel = (e.source_kind == USER_CB_SOURCE_RESOURCE && e.source == h);
                    ImGui::PushID(r_i);
                    if (ImGui::Selectable(ui_resource_display_name(r), sel)) {
                        user_cb_set_source(i, h);
                        user_changed = true;
                    }
                    ImGui::PopID();
                }
                if (e.type == RES_INT || e.type == RES_INT2 || e.type == RES_INT3) {
                    // Integral variables can be driven by resource dimensions:
                    // int = count, int2 = width/height, int3 = width/height/depth.
                    ImGui::Separator();
                    ui_inspector_text_disabled_wrapped("Resource sizes");
                    for (int r_i = 0; r_i < MAX_RESOURCES; r_i++) {
                        Resource& owner = g_resources[r_i];
                        if (!owner.active || owner.is_generated || !ui_resource_size_source_matches_type(owner, e.type))
                            continue;
                        Resource* size_res = res_get(owner.size_handle);
                        if (!size_res)
                            continue;
                        ResHandle h = owner.size_handle;
                        bool sel = e.source_kind == USER_CB_SOURCE_RESOURCE && e.source == h;
                        char item[192] = {};
                        ui_resource_size_source_label(owner, *size_res, item, sizeof(item));
                        ImGui::PushID(20000 + r_i);
                        if (ImGui::Selectable(item, sel)) {
                            user_cb_set_source(i, h);
                            user_changed = true;
                        }
                        ImGui::PopID();
                    }
                }
                if (e.type == RES_FLOAT3 || e.type == RES_FLOAT4) {
                    ImGui::Separator();
                    ui_inspector_text_disabled_wrapped("Scene transforms");
                    if (ImGui::Selectable("Camera Position", e.source_kind == USER_CB_SOURCE_CAMERA_POSITION)) {
                        user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_CAMERA_POSITION, "camera");
                    }
                    if (ImGui::Selectable("Camera Rotation", e.source_kind == USER_CB_SOURCE_CAMERA_ROTATION)) {
                        user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_CAMERA_ROTATION, "camera");
                    }
                    if (ImGui::Selectable("Light Position", e.source_kind == USER_CB_SOURCE_LIGHT_POSITION)) {
                        user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_LIGHT_POSITION, "light");
                    }
                    if (ImGui::Selectable("Light Target", e.source_kind == USER_CB_SOURCE_LIGHT_TARGET)) {
                        user_changed |= user_cb_set_scene_source(i, USER_CB_SOURCE_LIGHT_TARGET, "light");
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
                ui_inspector_text_disabled_wrapped("(source driven)");
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

// -- log panel -------------------------------------------------------------

static void ui_format_bytes(uint64_t bytes, char* out, int out_sz);
static uint64_t ui_process_memory_bytes();
static uint64_t ui_estimated_gpu_memory_bytes();
static void ui_reset_camera_view();

static void ui_panel_general(bool embedded = false) {
    if (!embedded) ImGui::Begin("General");
    bool settings_dirty = false;

    if (ui_inspector_section("INTERFACE")) {
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
        ui_hint_text_disabled("Scales fonts and layout globally without changing project data.");

        bool show_interface_hints = ui_show_interface_hints();
        if (ImGui::Checkbox("Show Interface Hints", &show_interface_hints)) {
            ui_set_show_interface_hints(show_interface_hints);
            settings_dirty = true;
        }

        bool show_inspector_notes = ui_show_inspector_notes();
        if (ImGui::Checkbox("Show Inspector Notes", &show_inspector_notes)) {
            ui_set_show_inspector_notes(show_inspector_notes);
            settings_dirty = true;
        }
        ui_hint_text_disabled("Controls whether per-resource and per-command notes appear in the Inspector.");

        int rt_preview_columns = ui_render_target_preview_columns();
        if (ImGui::SliderInt("RT Preview Columns", &rt_preview_columns, 1, 8)) {
            ui_set_render_target_preview_columns(rt_preview_columns);
            settings_dirty = true;
        }
    }

    if (ui_inspector_section("SHADER EDITOR")) {
        float font_size = ui_code_font_size();
        if (ImGui::SliderFloat("Code Font Size", &font_size, 10.0f, 28.0f, "%.0f px")) {
            ui_set_code_font_size(font_size);
            settings_dirty = true;
        }
        ui_hint_text_disabled("Shader source editor defaults. Saved outside project files.");
    }

    if (ui_inspector_section("APPLICATION")) {
        settings_dirty |= ImGui::Checkbox("VSync", &g_dx.vsync);
        ImGui::SameLine();
        ui_hint_text_disabled("%s", g_dx.vsync ? "Present interval 1" :
            (g_dx.present_allow_tearing ? "Present immediate + tearing" : "Present immediate"));

        float frame_cap = app_editor_frame_cap_fps();
        if (ImGui::SliderFloat("Editor Frame Cap", &frame_cap, 0.0f, 1000.0f, frame_cap <= 0.0f ? "uncapped" : "%.0f fps")) {
            app_set_editor_frame_cap_fps(frame_cap);
            settings_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Uncap##editor_frame_cap")) {
            app_set_editor_frame_cap_fps(0.0f);
            settings_dirty = true;
        }
        ui_hint_text_disabled("Editor-only throttle. It is ignored when VSync is enabled and does not affect exported players.");
    }

    if (ui_inspector_section("EXPORTER / STANDALONE")) {
        ui_hint_text_disabled("Project settings used by Export EXE and build64k.");
        // Keep every item in this section explicitly namespaced. The General panel
        // also has editor/runtime diagnostics with similarly named controls, and
        // Dear ImGui IDs are scoped by window/ID stack, not by visual collapsing
        // headers. Hidden ## suffixes avoid accidental ID collisions while keeping
        // the visible labels readable.
        ImGui::Checkbox("Camera/Light Controls##export_camera_light_controls", &g_export_settings.camera_light_controls_enabled);
        ImGui::SameLine();
        ui_hint_text_disabled("off by default for demo playback");
        ImGui::Checkbox("Autoplay Timeline##export_timeline_autoplay", &g_export_settings.timeline_autoplay);
        ImGui::SameLine();
        ui_hint_text_disabled("starts the sequence from frame 0 in the player");
        ImGui::Checkbox("Exit After Timeline##export_exit_after_timeline", &g_export_settings.exit_after_timeline);
        ImGui::SameLine();
        ui_hint_text_disabled("overrides timeline loop in player/export");
        ImGui::Checkbox("Esc Closes Player##export_escape_closes_player", &g_export_settings.escape_closes_player);
        ImGui::Checkbox("VSync In Export##export_vsync", &g_export_settings.vsync);
        ImGui::Checkbox("Show FPS In Title##export_show_fps_title", &g_export_settings.show_fps_title);

        if (ImGui::Button("Demo Preset")) {
            g_export_settings.camera_light_controls_enabled = false;
            g_export_settings.timeline_autoplay = true;
            g_export_settings.exit_after_timeline = true;
            g_export_settings.escape_closes_player = true;
            g_export_settings.vsync = false;
            g_export_settings.show_fps_title = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Interactive Preset")) {
            g_export_settings.camera_light_controls_enabled = true;
            g_export_settings.timeline_autoplay = true;
            g_export_settings.exit_after_timeline = false;
            g_export_settings.escape_closes_player = true;
            g_export_settings.vsync = false;
            g_export_settings.show_fps_title = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##export_settings")) {
            g_export_settings = project_default_export_settings();
        }
        ui_hint_text_disabled("Exported players ignore editor wireframe, grid, profiler and shader diagnostics.");
    }

    if (ui_inspector_section("VIEWPORT")) {
        if (ImGui::Checkbox("Show Grid", &g_dx.scene_grid_enabled)) {
            settings_dirty = true;
            app_request_scene_render();
        }
        if (ImGui::ColorEdit4("Grid Color", g_dx.scene_grid_color,
            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)) {
            settings_dirty = true;
            app_request_scene_render();
        }
        ui_hint_text_disabled("Grid level opacity");
        if (ImGui::SliderFloat("Fine##grid_level_alpha", &g_dx.scene_grid_level_alpha[0], 0.0f, 1.0f, "%.2f")) {
            settings_dirty = true;
            app_request_scene_render();
        }
        if (ImGui::SliderFloat("Medium##grid_level_alpha", &g_dx.scene_grid_level_alpha[1], 0.0f, 1.0f, "%.2f")) {
            settings_dirty = true;
            app_request_scene_render();
        }
        if (ImGui::SliderFloat("Coarse##grid_level_alpha", &g_dx.scene_grid_level_alpha[2], 0.0f, 1.0f, "%.2f")) {
            settings_dirty = true;
            app_request_scene_render();
        }
        if (ImGui::Checkbox("Grid Distance Fade", &g_dx.scene_grid_distance_fade)) {
            settings_dirty = true;
            app_request_scene_render();
        }
        if (g_dx.scene_grid_distance_fade) {
            if (ImGui::DragFloat("Grid Fade Start", &g_dx.scene_grid_fade_start, 1.0f, 0.0f, 10000.0f, "%.0f")) {
                settings_dirty = true;
                app_request_scene_render();
            }
            if (ImGui::DragFloat("Grid Fade End", &g_dx.scene_grid_fade_end, 1.0f, 1.0f, 10000.0f, "%.0f")) {
                settings_dirty = true;
                app_request_scene_render();
            }
            if (g_dx.scene_grid_fade_end < g_dx.scene_grid_fade_start + 1.0f)
                g_dx.scene_grid_fade_end = g_dx.scene_grid_fade_start + 1.0f;
        }
        settings_dirty |= ImGui::Checkbox("Show Camera Orientation Gizmo", &g_dx.scene_orientation_gizmo_enabled);
        if (g_dx.scene_orientation_gizmo_enabled) {
            ImGui::Indent(ui_margin_px(8.0f));
            if (ImGui::SliderFloat("Gizmo Size##scene_orientation_gizmo_size", &g_dx.scene_orientation_gizmo_size_px, 72.0f, 180.0f, "%.0f px")) {
                settings_dirty = true;
                g_dx.scene_orientation_gizmo_size_px = clampf(g_dx.scene_orientation_gizmo_size_px, 72.0f, 180.0f);
            }
            ImGui::Unindent(ui_margin_px(8.0f));
        }
        settings_dirty |= ImGui::Checkbox("Show Manual Control Overlay", &g_dx.scene_manual_control_overlay_enabled);
#if LAZYTOOL_ENABLE_DEBUG_OVERLAYS
        if (ImGui::Checkbox("Debug Draw Bounds", &g_dx.scene_bounds_debug_enabled)) {
            settings_dirty = true;
            app_request_scene_render();
        }
#else
        g_dx.scene_bounds_debug_enabled = false;
#endif
        ui_hint_text_disabled("Infinite grid overlay on y=0. Color alpha controls overall intensity; level opacity balances fine/medium/coarse lines.");
    }

    if (ui_inspector_section("DIAGNOSTICS")) {
        ImGui::Text("DX11 Adapter: %s", g_dx.adapter_name[0] ? g_dx.adapter_name : "unknown");
        ImGui::TextDisabled("vendor=0x%04X device=0x%04X dedicated=%llu MB",
            g_dx.adapter_vendor_id,
            g_dx.adapter_device_id,
            g_dx.adapter_dedicated_vram_mb);
        ImGui::TextDisabled("High-performance GPU hints are exported; on hybrid laptops the final desktop composition may still show iGPU activity.");
        ImGui::Separator();

#if LAZYTOOL_ENABLE_D3D11_VALIDATION
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
#else
        g_dx.d3d11_validation = false;
        g_dx.d3d11_validation_active = false;
        ImGui::TextDisabled("D3D11 validation is compiled out for this build profile.");
#endif

#if LAZYTOOL_ENABLE_SHADER_BINDING_WARNINGS
        settings_dirty |= ImGui::Checkbox("Shader Binding Warnings##runtime_shader_binding_warnings", &g_dx.shader_validation_warnings);
        ImGui::TextDisabled("Editor/runtime diagnostic only. Export defaults are controlled in Exporter / Standalone.");
#else
        g_dx.shader_validation_warnings = false;
        ImGui::TextDisabled("Shader binding warnings are compiled out for this build profile.");
#endif

#if LAZYTOOL_ENABLE_D3D11_VALIDATION
        bool can_flush_d3d11 = g_dx.d3d11_validation_active && g_dx.info_queue;
        if (!can_flush_d3d11)
            ImGui::BeginDisabled();
        if (ImGui::Button("Flush D3D11 Messages"))
            dx_debug_log_messages();
        if (!can_flush_d3d11)
            ImGui::EndDisabled();
#endif
    }

    if (ui_inspector_section(LAZYTOOL_ENABLE_PROFILER ? "PROFILER" : "MONITORING")) {
#if LAZYTOOL_ENABLE_PROFILER
        settings_dirty |= ImGui::Checkbox("Enable profiling", &g_profiler_enabled);
        if (g_profiler_enabled) {
            ui_refresh_profiler_readout_cache();
            ImGui::SeparatorText("CPU");
            ImGui::Text("CPU frame: %.3f ms", app_cpu_frame_ms());
            ui_help_marker("Total measured CPU time for one editor frame, from the start of the main-loop iteration until Present returns. It includes scene work, ImGui build, ImGui backend draw submission, Present blocking, and small miscellaneous work.");
            ImGui::Text("Scene CPU: %.3f ms", app_cpu_scene_ms());
            ui_help_marker("CPU time spent updating scene constants and executing lazyTool scene commands. When the scene is paused and no redraw is requested this should be near zero.");
            ImGui::Text("ImGui build CPU: %.3f ms", app_cpu_ui_build_ms());
            ui_help_marker("CPU time spent constructing the editor UI for this frame: ImGui::NewFrame, all lazyTool panel/window/widget code, layout, text formatting, visible lists, and ImGui::Render. This is the main immediate-mode UI cost.");
            ImGui::Text("ImGui render CPU: %.3f ms", app_cpu_ui_render_ms());
            ui_help_marker("CPU time spent in the Dear ImGui DirectX 11 backend submitting already-built draw lists to D3D11. This is command submission, not the GPU time needed to rasterize the UI.");
            ImGui::Text("Present CPU: %.3f ms", app_cpu_present_ms());
            ui_help_marker("CPU time spent inside IDXGISwapChain::Present. Even with VSync off, this can block on DWM, the driver, or the swapchain queue, so it is not necessarily CPU work done by lazyTool.");
            ImGui::Text("Other CPU: %.3f ms", app_cpu_other_ms());
            ui_help_marker("Everything in the measured frame not attributed to scene, ImGui build, ImGui render, or Present. Ideally small; includes message handling and loop overhead.");
            ImGuiIO& profiler_io = ImGui::GetIO();
            ImGui::Text("ImGui output: %d verts, %d indices, %d draw cmds, %d lists, %d windows",
                profiler_io.MetricsRenderVertices,
                profiler_io.MetricsRenderIndices,
                app_imgui_draw_command_count(),
                app_imgui_draw_list_count(),
                profiler_io.MetricsRenderWindows);
            ui_help_marker("Final draw-list size generated by Dear ImGui. Draw cmds map closely to D3D11 UI draw submissions, while vertices/indices affect UI GPU cost.");
            if (ImGui::TreeNodeEx("ImGui build breakdown", ImGuiTreeNodeFlags_DefaultOpen)) {
                ui_help_marker("Breakdown of the ImGui build CPU time by lazyTool UI section. Percentages are relative to ImGui build CPU, not total frame time.");
                float total = app_cpu_ui_build_ms();
                if (total <= 0.0001f) total = 1.0f;
                for (int i = 0; i < UI_PROFILE_COUNT; i++) {
                    float ms = s_ui_profile[i].display_ms;
                    ImGui::Text("%s: %.3f ms  %.1f%%", s_ui_profile[i].name, ms, (ms / total) * 100.0f);
                }
                ImGui::TreePop();
            }

            ImGui::SeparatorText("GPU");
            if (app_imgui_gpu_ready()) {
                ImGui::Text("ImGui render GPU: %.3f ms", app_imgui_gpu_ms());
                ui_help_marker("GPU timestamp duration for rasterizing the Dear ImGui draw lists into the backbuffer. This is separate from ImGui render CPU, which only measures D3D11 backend submission on the CPU.");
            } else {
                ImGui::TextDisabled("ImGui render GPU: warming up...");
            }
            if (cmd_profile_total_ready()) {
                ImGui::Text("Frame GPU time: %.3f ms", cmd_profile_total_frame_ms());
                ui_help_marker("GPU timestamp duration for the whole rendered scene frame captured by lazyTool. This is GPU execution time, not CPU frame time.");
            } else
                ImGui::TextDisabled("Frame GPU time: warming up...");
            if (cmd_profile_ready()) {
                ImGui::Text("Command GPU time: %.3f ms", cmd_profile_frame_ms());
                ui_help_marker("GPU timestamp duration of lazyTool command execution. Groups and repeats include their children.");
            } else
                ImGui::TextDisabled("Command GPU time: warming up...");
            if (cmd_profile_ready()) {
                ImGui::Text("Shadow prepass GPU: %.3f ms", cmd_profile_shadow_ms());
                ui_help_marker("GPU time spent rendering the shadow map prepass for commands that cast shadows.");
            } else
                ImGui::TextDisabled("Shadow prepass GPU: warming up...");
            ui_draw_profiler_gpu_command_table();

            ImGui::SeparatorText("Memory");
            ImGui::Text("Application memory: %s", s_profiler_readout_cache.app_memory);
            ImGui::Text("Estimated GPU memory: %s", s_profiler_readout_cache.gpu_memory);
            ImGui::TextDisabled("Project GPU resources: %s", s_profiler_readout_cache.project_gpu_memory);
        } else {
#if LAZYTOOL_ENABLE_BASIC_MONITORING
            ui_refresh_profiler_readout_cache();
            ui_draw_basic_monitoring_readout();
            ImGui::SeparatorText("Memory");
            ImGui::Text("Application memory: %s", s_profiler_readout_cache.app_memory);
            ImGui::Text("Estimated GPU memory: %s", s_profiler_readout_cache.gpu_memory);
            ImGui::TextDisabled("Project GPU resources: %s", s_profiler_readout_cache.project_gpu_memory);
            ImGui::TextDisabled("Enable profiling for detailed CPU/GPU timings.");
#endif
        }
#else
        g_profiler_enabled = false;
#if LAZYTOOL_ENABLE_BASIC_MONITORING
        ui_refresh_profiler_readout_cache();
        ui_draw_basic_monitoring_readout();
        ImGui::SeparatorText("Memory");
        ImGui::Text("Application memory: %s", s_profiler_readout_cache.app_memory);
        ImGui::Text("Estimated GPU memory: %s", s_profiler_readout_cache.gpu_memory);
        ImGui::TextDisabled("Project GPU resources: %s", s_profiler_readout_cache.project_gpu_memory);
        ImGui::TextDisabled("Detailed CPU/GPU profiling is compiled out for this build profile.");
#endif
#endif
    }

    if (ui_inspector_section("CAMERA")) {
        settings_dirty |= ImGui::Checkbox("Enabled", &g_camera_controls.enabled);
        const char* camera_modes[] = { "Horizon Locked", "Free Camera" };
        int camera_mode = g_camera_controls.mode == CAMERA_MODE_FREE ? 1 : 0;
        if (ImGui::Combo("Mode", &camera_mode, camera_modes, 2)) {
            g_camera_controls.mode = camera_mode == 1 ? CAMERA_MODE_FREE : CAMERA_MODE_HORIZON_LOCKED;
            if (g_camera_controls.mode == CAMERA_MODE_HORIZON_LOCKED) {
                camera_sync_euler_from_quat(&g_camera);
                camera_set_euler(&g_camera, g_camera.yaw, clampf(g_camera.pitch, -1.55334f, 1.55334f), g_camera.roll);
            }
            settings_dirty = true;
            app_request_scene_render();
        }
        settings_dirty |= ImGui::Checkbox("Mouse Look", &g_camera_controls.mouse_look);
        ImGui::SameLine();
        settings_dirty |= ImGui::Checkbox("Invert Y", &g_camera_controls.invert_y);

        settings_dirty |= ImGui::DragFloat("Move Speed", &g_camera_controls.move_speed, 0.01f, 0.001f, 100.0f);
        settings_dirty |= ImGui::DragFloat("Fast Mult", &g_camera_controls.fast_mult, 0.05f, 1.0f, 20.0f);
        settings_dirty |= ImGui::DragFloat("Slow Mult", &g_camera_controls.slow_mult, 0.01f, 0.01f, 1.0f);
        settings_dirty |= ImGui::DragFloat("Mouse Sensitivity", &g_camera_controls.mouse_sensitivity, 0.0001f, 0.0001f, 0.05f, "%.4f");

        ImGui::Separator();
        bool camera_changed = false;
        bool camera_rotation_changed = false;
        camera_changed |= ImGui::DragFloat3("Position", g_camera.position, 0.01f);
        camera_rotation_changed |= ImGui::DragFloat("Yaw", &g_camera.yaw, 0.01f);
        camera_rotation_changed |= ImGui::DragFloat("Pitch", &g_camera.pitch, 0.01f);
        camera_rotation_changed |= ImGui::DragFloat("Roll", &g_camera.roll, 0.01f);
        camera_changed |= camera_rotation_changed;
        const char* projection_types[] = { "Perspective", "Orthographic" };
        int projection_type = g_camera.projection_type == CAMERA_PROJECTION_ORTHOGRAPHIC ? 1 : 0;
        if (ImGui::Combo("Projection", &projection_type, projection_types, 2)) {
            g_camera.projection_type = projection_type == 1 ? CAMERA_PROJECTION_ORTHOGRAPHIC : CAMERA_PROJECTION_PERSPECTIVE;
            camera_changed = true;
        }
        if (g_camera.projection_type == CAMERA_PROJECTION_ORTHOGRAPHIC)
            camera_changed |= ImGui::DragFloat("Ortho Height", &g_camera.ortho_height, 0.01f, 0.001f, 10000.0f);
        else
            camera_changed |= ImGui::DragFloat("FOV", &g_camera.fov_y, 0.01f, 0.10f, 2.80f);
        camera_changed |= ImGui::DragFloat("Near Plane", &g_camera.near_z, 0.001f, 0.0001f, 100.0f);
        camera_changed |= ImGui::DragFloat("Far Plane", &g_camera.far_z, 0.05f, 0.001f, 10000.0f);

        if (camera_rotation_changed)
            camera_set_euler(&g_camera, g_camera.yaw, g_camera.pitch, g_camera.roll);
        if (g_camera.fov_y < 0.10f) g_camera.fov_y = 0.10f;
        if (g_camera.fov_y > 2.80f) g_camera.fov_y = 2.80f;
        if (g_camera.ortho_height < 0.001f) g_camera.ortho_height = 0.001f;
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

    ImGuiListClipper clipper;
    clipper.Begin(total, ImGui::GetTextLineHeightWithSpacing());
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const LogEntry& e = g_log.entries[(start + i) % LOG_MAX_ENTRIES];
            ImVec4 col = {0.85f, 0.85f, 0.85f, 1.f};
            const char* prefix = "   ";
            if (e.level == LOG_WARN)  { col = {1.0f, 0.85f, 0.2f, 1.f};  prefix = "[W]"; }
            if (e.level == LOG_ERROR) { col = {1.0f, 0.35f, 0.3f, 1.f};  prefix = "[E]"; }
            ImGui::TextColored(col, "[%s] %s %s", e.time[0] ? e.time : "--:--:--", prefix, e.msg);
        }
    }

    if (g_log.scroll_to_bottom) {
        ImGui::SetScrollHereY(1.0f);
        g_log.scroll_to_bottom = false;
    }
    ImGui::EndChild();
    if (!embedded) ImGui::End();
}

// -- scene viewport --------------------------------------------------------

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
        quat_to_array(quat_from_mat4(final_rot), c->rotq);
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
    rot = mat4_rotation_quat(quat_from_array(c->rotq));
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

static Vec3 ui_camera_forward_basis() {
    return camera_forward(g_camera);
}

static Vec3 ui_camera_right_basis() {
    return camera_right(g_camera);
}

static Vec3 ui_camera_up_basis() {
    return camera_up(g_camera);
}

struct UiOrientationAxisPoint {
    ImVec2 pos;
    float depth;
    int axis;
    bool positive;
};

static ImVec2 ui_orientation_gizmo_project(Vec3 p, Vec3 cam_right, Vec3 cam_up,
                                           ImVec2 center, float scale) {
    float sx = v3_dot(p, cam_right);
    float sy = -v3_dot(p, cam_up);
    return ImVec2(center.x + sx * scale, center.y + sy * scale);
}

static ImU32 ui_axis_color_u32(int axis, float alpha = 1.0f) {
    switch (axis) {
    case 0: return ImGui::GetColorU32(ImVec4(1.00f, 0.18f, 0.30f, alpha)); // X
    case 1: return ImGui::GetColorU32(ImVec4(0.48f, 0.78f, 0.08f, alpha)); // Y
    default: return ImGui::GetColorU32(ImVec4(0.18f, 0.52f, 0.88f, alpha)); // Z
    }
}

static void ui_draw_camera_orientation_gizmo(ImVec2 rect_min, ImVec2 rect_max) {
    float w = rect_max.x - rect_min.x;
    float h = rect_max.y - rect_min.y;

    // Classic axis tripod: positive axes are filled labelled handles, opposite
    // axes are outline handles. Size is user-configurable from Settings >
    // Viewport, and the internal details scale with the same factor.
    const float base_size = 104.0f;
    float gizmo_size = clampf(g_dx.scene_orientation_gizmo_size_px, 72.0f, 180.0f);
    float scale = gizmo_size / base_size;
    float size = ui_px(gizmo_size);
    if (w < size || h < size)
        return;

    float pad = ui_margin_px(18.0f * scale);
    float min_pad = ui_margin_px(8.0f * scale);
    ImVec2 box_min(rect_max.x - pad - size, rect_min.y + pad);
    ImVec2 box_max(box_min.x + size, box_min.y + size);
    if (box_min.x < rect_min.x + min_pad) {
        box_min.x = rect_max.x - min_pad - size;
        box_max.x = box_min.x + size;
    }
    if (box_max.y > rect_max.y - min_pad) {
        box_min.y = rect_min.y + min_pad;
        box_max.y = box_min.y + size;
    }

    ImVec2 center((box_min.x + box_max.x) * 0.5f, (box_min.y + box_max.y) * 0.5f);
    float axis_len = size * 0.34f;
    float ball_r = ui_px(13.5f * scale);
    float ring_r = ui_px(12.0f * scale);
    float line_thick = ui_px(4.0f * scale);
    float halo_thick = line_thick + ui_px(2.5f * scale);

    Vec3 cam_right = ui_camera_right_basis();
    Vec3 cam_up = ui_camera_up_basis();
    Vec3 cam_forward = ui_camera_forward_basis();
    Vec3 dirs[3] = {
        v3(1.0f, 0.0f, 0.0f),
        v3(0.0f, 1.0f, 0.0f),
        v3(0.0f, 0.0f, 1.0f)
    };
    const char* labels[3] = { "X", "Y", "Z" };

    UiOrientationAxisPoint points[6] = {};
    int point_count = 0;
    for (int axis = 0; axis < 3; axis++) {
        points[point_count++] = {
            ui_orientation_gizmo_project(dirs[axis], cam_right, cam_up, center, axis_len),
            v3_dot(dirs[axis], cam_forward), axis, true
        };
        points[point_count++] = {
            ui_orientation_gizmo_project(v3_scale(dirs[axis], -1.0f), cam_right, cam_up, center, axis_len),
            v3_dot(v3_scale(dirs[axis], -1.0f), cam_forward), axis, false
        };
    }

    // Farther endpoints first, closer endpoints last. Negative endpoints are
    // still drawn as rings so the labelled positive axes remain easy to read.
    for (int a = 0; a < point_count - 1; a++) {
        for (int b = a + 1; b < point_count; b++) {
            if (points[a].depth < points[b].depth) {
                UiOrientationAxisPoint tmp = points[a];
                points[a] = points[b];
                points[b] = tmp;
            }
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 shadow = ImGui::GetColorU32(ImVec4(0.03f, 0.035f, 0.04f, 0.68f));
    ImU32 ring_fill = ImGui::GetColorU32(ImVec4(0.05f, 0.06f, 0.07f, 0.34f));
    ImU32 text_col = ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.025f, 0.96f));
    ImU32 border = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.14f));

    for (int i = 0; i < point_count; i++) {
        if (points[i].positive)
            continue;
        dl->AddCircleFilled(points[i].pos, ring_r + ui_px(1.5f * scale), shadow, 32);
        dl->AddCircleFilled(points[i].pos, ring_r, ring_fill, 32);
        dl->AddCircle(points[i].pos, ring_r, ui_axis_color_u32(points[i].axis, 0.70f), 32, ui_px(2.8f * scale));
    }

    for (int axis = 0; axis < 3; axis++) {
        ImVec2 end = ui_orientation_gizmo_project(dirs[axis], cam_right, cam_up, center, axis_len - ball_r * 0.72f);
        dl->AddLine(center, end, shadow, halo_thick);
        dl->AddLine(center, end, ui_axis_color_u32(axis, 0.95f), line_thick);
    }
    dl->AddCircleFilled(center, ui_px(3.0f * scale), shadow, 18);

    for (int i = 0; i < point_count; i++) {
        if (!points[i].positive)
            continue;
        int axis = points[i].axis;
        dl->AddCircleFilled(points[i].pos, ball_r + ui_px(2.0f * scale), shadow, 32);
        dl->AddCircleFilled(points[i].pos, ball_r, ui_axis_color_u32(axis, 1.0f), 32);
        dl->AddCircle(points[i].pos, ball_r, border, 32, ui_px(1.0f * scale));

        ImVec2 text_sz = ImGui::CalcTextSize(labels[axis]);
        ImVec2 text_pos(points[i].pos.x - text_sz.x * 0.5f,
                        points[i].pos.y - text_sz.y * 0.5f - ui_px(0.5f * scale));
        dl->AddText(text_pos, text_col, labels[axis]);
    }
}


static const char* ui_camera_mode_name(int mode) {
    return mode == CAMERA_MODE_FREE ? "Free Camera" : "Horizon Locked";
}

static void ui_bounds_include_point(float out_min[3], float out_max[3], Vec3 p) {
    if (p.x < out_min[0]) out_min[0] = p.x;
    if (p.y < out_min[1]) out_min[1] = p.y;
    if (p.z < out_min[2]) out_min[2] = p.z;
    if (p.x > out_max[0]) out_max[0] = p.x;
    if (p.y > out_max[1]) out_max[1] = p.y;
    if (p.z > out_max[2]) out_max[2] = p.z;
}

static bool ui_selected_or_any_bounds(float out_min[3], float out_max[3]) {
    if (g_sel_cmd != INVALID_HANDLE) {
        Command* sel = cmd_get(g_sel_cmd);
        if (sel && (sel->type == CMD_DRAW_MESH || sel->type == CMD_DRAW_INSTANCED || sel->type == CMD_INDIRECT_DRAW) &&
            cmd_compute_world_bounds(g_sel_cmd, out_min, out_max))
            return true;
    }
    for (int i = 0; i < MAX_COMMANDS; i++) {
        CmdHandle h = (CmdHandle)(i + 1);
        Command* c = cmd_get(h);
        if (!c || !c->enabled)
            continue;
        if (c->type != CMD_DRAW_MESH && c->type != CMD_DRAW_INSTANCED && c->type != CMD_INDIRECT_DRAW)
            continue;
        if (cmd_compute_world_bounds(h, out_min, out_max))
            return true;
    }
    out_min[0] = out_min[1] = out_min[2] = -0.5f;
    out_max[0] = out_max[1] = out_max[2] =  0.5f;
    return true;
}


static void ui_draw_bounds_values(const char* label, const float bmin[3], const float bmax[3]) {
    if (label && label[0])
        ImGui::TextDisabled("%s", label);
    Vec3 center = v3((bmin[0] + bmax[0]) * 0.5f,
                     (bmin[1] + bmax[1]) * 0.5f,
                     (bmin[2] + bmax[2]) * 0.5f);
    Vec3 size = v3(bmax[0] - bmin[0], bmax[1] - bmin[1], bmax[2] - bmin[2]);
    float rows[4][3] = {
        { bmin[0], bmin[1], bmin[2] },
        { bmax[0], bmax[1], bmax[2] },
        { center.x, center.y, center.z },
        { size.x, size.y, size.z },
    };
    const char* names[4] = { "Min", "Max", "Center", "Size" };
    ImVec4 name_cols[4] = {
        ImVec4(0.68f, 0.73f, 0.78f, 1.0f),
        ImVec4(0.82f, 0.72f, 0.62f, 1.0f),
        ImVec4(0.74f, 0.58f, 0.44f, 1.0f),
        ImVec4(0.55f, 0.72f, 0.58f, 1.0f),
    };

    if (ImGui::BeginTable("##bounds_values", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ui_px(58.0f));
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Z", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int r = 0; r < 4; r++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, name_cols[r]);
            ImGui::TextUnformatted(names[r]);
            ImGui::PopStyleColor();
            for (int axis = 0; axis < 3; axis++) {
                ImGui::TableSetColumnIndex(axis + 1);
                ImGui::Text("%.4g", rows[r][axis]);
            }
        }
        ImGui::EndTable();
    }
}

static void ui_draw_command_bounds_inspector(Command* c, CmdHandle h) {
    if (!c)
        return;
    if (!s_show_interface_hints)
        return;
    if (!ui_inspector_section("BOUNDING BOX"))
        return;
    cmd_refresh_draw_bounds(h);
    if (c->bbox_identity)
        ui_inspector_text_disabled_wrapped("Identity/unit bounds are being used because this draw has no mesh geometry available.");
    else
        ui_inspector_text_disabled_wrapped("Auto bounds from the draw mesh geometry. Used by Alt+LMB orbit, F frame, and debug draw.");
    float wmin[3] = {};
    float wmax[3] = {};
    if (cmd_compute_world_bounds(h, wmin, wmax))
        ui_draw_bounds_values("World bounds", wmin, wmax);
}

static void ui_draw_projected_bounds_box(ImDrawList* dl, const Mat4& view_proj,
                                         ImVec2 rect_min, ImVec2 rect_max,
                                         const float bmin[3], const float bmax[3],
                                         ImU32 line_col, ImU32 fill_col)
{
    if (!dl || !bmin || !bmax)
        return;

    Vec3 corners[8] = {
        v3(bmin[0], bmin[1], bmin[2]), v3(bmax[0], bmin[1], bmin[2]),
        v3(bmax[0], bmax[1], bmin[2]), v3(bmin[0], bmax[1], bmin[2]),
        v3(bmin[0], bmin[1], bmax[2]), v3(bmax[0], bmin[1], bmax[2]),
        v3(bmax[0], bmax[1], bmax[2]), v3(bmin[0], bmax[1], bmax[2]),
    };
    ImVec2 pts[8] = {};
    bool ok[8] = {};
    for (int i = 0; i < 8; i++)
        ok[i] = ui_project_world_to_screen(view_proj, rect_min, rect_max, corners[i], &pts[i]);

    const int faces[6][4] = {
        {0,1,2,3}, {4,5,6,7}, {0,1,5,4}, {2,3,7,6}, {1,2,6,5}, {0,3,7,4}
    };
    for (int f = 0; f < 6; f++) {
        int a = faces[f][0], b = faces[f][1], c = faces[f][2], d = faces[f][3];
        if (ok[a] && ok[b] && ok[c]) dl->AddTriangleFilled(pts[a], pts[b], pts[c], fill_col);
        if (ok[a] && ok[c] && ok[d]) dl->AddTriangleFilled(pts[a], pts[c], pts[d], fill_col);
    }

    const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7}
    };
    for (int e = 0; e < 12; e++) {
        int a = edges[e][0], b = edges[e][1];
        if (ok[a] && ok[b])
            dl->AddLine(pts[a], pts[b], line_col, ui_px(1.5f));
    }
}

static void ui_draw_viewport_bounds_debug(ImVec2 rect_min, ImVec2 rect_max) {
#if !LAZYTOOL_ENABLE_DEBUG_OVERLAYS
    (void)rect_min;
    (void)rect_max;
    return;
#else
    if (!g_dx.scene_bounds_debug_enabled)
        return;

    Mat4 view_proj = ui_mat4_from_raw(g_dx.scene_cb_data.view_proj);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 selected_line = ImGui::GetColorU32(ImVec4(1.0f, 0.68f, 0.24f, 0.95f));
    ImU32 selected_fill = ImGui::GetColorU32(ImVec4(1.0f, 0.55f, 0.18f, 0.055f));
    ImU32 normal_line = ImGui::GetColorU32(ImVec4(0.55f, 0.75f, 1.0f, 0.72f));
    ImU32 normal_fill = ImGui::GetColorU32(ImVec4(0.30f, 0.55f, 1.0f, 0.035f));

    for (int i = 0; i < MAX_COMMANDS; i++) {
        CmdHandle h = (CmdHandle)(i + 1);
        Command* c = cmd_get(h);
        if (!c || !c->enabled)
            continue;
        if (c->type != CMD_DRAW_MESH && c->type != CMD_DRAW_INSTANCED && c->type != CMD_INDIRECT_DRAW)
            continue;
        float bmin[3] = {};
        float bmax[3] = {};
        if (!cmd_compute_world_bounds(h, bmin, bmax))
            continue;
        bool selected = h == g_sel_cmd;
        ui_draw_projected_bounds_box(dl, view_proj, rect_min, rect_max, bmin, bmax,
                                     selected ? selected_line : normal_line,
                                     selected ? selected_fill : normal_fill);
    }
#endif
}

static void ui_draw_viewport_light_debug(ImVec2 rect_min, ImVec2 rect_max) {
    Resource* light = res_get(g_builtin_light);
    if (!light || !light->light_debug_draw)
        return;

    Mat4 view_proj = ui_mat4_from_raw(g_dx.scene_cb_data.view_proj);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 line_col = ImGui::GetColorU32(ImVec4(1.0f, 0.78f, 0.22f, 0.95f));
    ImU32 soft_col = ImGui::GetColorU32(ImVec4(1.0f, 0.58f, 0.18f, 0.58f));
    ImU32 fill_col = ImGui::GetColorU32(ImVec4(1.0f, 0.62f, 0.16f, 0.08f));

    Vec3 pos = v3(light->light_pos[0], light->light_pos[1], light->light_pos[2]);
    Vec3 target = v3(light->light_target[0], light->light_target[1], light->light_target[2]);
    Vec3 delta = v3_sub(target, pos);
    Vec3 dir = v3_norm(delta);
    if (v3_dot(dir, dir) < 0.0001f)
        dir = v3(0.0f, -1.0f, 0.0f);

    float range = light->light_type == LIGHT_TYPE_SPOT ? light->shadow_far : sqrtf(v3_dot(delta, delta));
    if (range < 0.05f)
        range = 0.05f;
    Vec3 tip = v3_add(pos, v3_scale(dir, range));

    ImVec2 pos_s = {};
    ImVec2 tip_s = {};
    bool pos_ok = ui_project_world_to_screen(view_proj, rect_min, rect_max, pos, &pos_s);
    bool tip_ok = ui_project_world_to_screen(view_proj, rect_min, rect_max, tip, &tip_s);
    if (pos_ok) {
        dl->AddCircleFilled(pos_s, ui_px(4.0f), line_col, 18);
        dl->AddCircle(pos_s, ui_px(7.0f), line_col, 24, ui_px(1.4f));
    }
    if (pos_ok && tip_ok)
        ui_draw_translate_axis(dl, pos_s, tip_s, line_col, ui_px(1.8f));

    if (light->light_type != LIGHT_TYPE_SPOT)
        return;

    Vec3 up_seed = fabsf(v3_dot(dir, v3(0.0f, 1.0f, 0.0f))) > 0.92f ? v3(0.0f, 0.0f, 1.0f) : v3(0.0f, 1.0f, 0.0f);
    Vec3 right = v3_norm(v3_cross(up_seed, dir));
    Vec3 up = v3_norm(v3_cross(dir, right));
    float angle = clampf(light->spot_angle, 0.05f, 3.0f);
    float radius = tanf(angle * 0.5f) * range;
    Vec3 center = tip;
    const int segments = 36;
    ImVec2 ring[segments] = {};
    bool ring_ok[segments] = {};
    for (int i = 0; i < segments; i++) {
        float a = 6.28318530718f * (float)i / (float)segments;
        Vec3 p = v3_add(center,
                        v3_add(v3_scale(right, cosf(a) * radius),
                               v3_scale(up, sinf(a) * radius)));
        ring_ok[i] = ui_project_world_to_screen(view_proj, rect_min, rect_max, p, &ring[i]);
    }
    for (int i = 0; i < segments; i++) {
        int j = (i + 1) % segments;
        if (ring_ok[i] && ring_ok[j])
            dl->AddLine(ring[i], ring[j], soft_col, ui_px(1.3f));
    }
    for (int i = 0; i < segments; i += segments / 4) {
        if (pos_ok && ring_ok[i])
            dl->AddLine(pos_s, ring[i], soft_col, ui_px(1.2f));
    }
    if (ring_ok[0] && ring_ok[9] && ring_ok[18])
        dl->AddTriangleFilled(ring[0], ring[9], ring[18], fill_col);
    if (ring_ok[0] && ring_ok[18] && ring_ok[27])
        dl->AddTriangleFilled(ring[0], ring[18], ring[27], fill_col);
}

static float ui_viewport_overlay_text_button_width(const char* label) {
    float pad_x = ui_px(8.0f);
    float min_h = ui_px(22.0f);
    ImVec2 text_sz = ImGui::CalcTextSize(label ? label : "", nullptr, true);
    float w = text_sz.x + pad_x * 2.0f;
    if (w < min_h)
        w = min_h;
    return w;
}

static float ui_viewport_overlay_icon_button_size() {
    return ui_px(22.0f);
}

static void ui_viewport_toolbar_rect(ImVec2 rect_min, ImVec2 rect_max, ImVec2* out_min, ImVec2* out_max) {
    (void)rect_max;
    const float spacing = ui_px(5.0f);
    const float sep = ui_px(5.0f);
    const float h = ui_viewport_overlay_icon_button_size();
    ImVec2 min(rect_min.x + ui_px(8.0f), rect_min.y + ui_px(8.0f));
    float x = min.x;

    x += ui_viewport_overlay_text_button_width(ui_camera_mode_name(g_camera_controls.mode));
    x += spacing + sep;
#if LAZYTOOL_ENABLE_DEBUG_OVERLAYS
    // Move / rotate / scale, wireframe / grid, bounds debug.
    x += ui_viewport_overlay_icon_button_size() * 6.0f + spacing * 5.0f;
#else
    // Move / rotate / scale, wireframe / grid. Bounds debug is compiled out.
    x += ui_viewport_overlay_icon_button_size() * 5.0f + spacing * 4.0f;
#endif

    if (out_min) *out_min = min;
    if (out_max) *out_max = ImVec2(x, min.y + h);
}

static bool ui_viewport_toolbar_hit_test(ImVec2 rect_min, ImVec2 rect_max) {
    ImVec2 min = {}, max = {};
    ui_viewport_toolbar_rect(rect_min, rect_max, &min, &max);
    ImVec2 mouse = ImGui::GetMousePos();
    return mouse.x >= min.x && mouse.x <= max.x && mouse.y >= min.y && mouse.y <= max.y;
}

static void ui_viewport_overlay_draw_button_bg(ImDrawList* dl, ImVec2 min, ImVec2 max,
                                               bool hovered, bool held, bool active) {
    ImU32 bg = ImGui::GetColorU32(
        held ? ImVec4(0.58f, 0.30f, 0.14f, 1.0f) :
        active ? (hovered ? ImVec4(0.45f, 0.24f, 0.12f, 0.98f)
                          : ImVec4(0.33f, 0.18f, 0.10f, 0.96f)) :
        hovered ? ImVec4(0.20f, 0.14f, 0.10f, 0.92f)
                : ImVec4(0.07f, 0.065f, 0.07f, 0.78f));
    ImU32 border = ImGui::GetColorU32(
        active ? ImVec4(0.95f, 0.55f, 0.24f, hovered ? 0.90f : 0.72f) :
        hovered ? ImVec4(0.95f, 0.55f, 0.24f, 0.75f)
                : ImVec4(0.30f, 0.28f, 0.27f, 0.78f));
    dl->AddRectFilled(min, max, bg, ui_px(4.0f));
    dl->AddRect(min, max, border, ui_px(4.0f), 0, ui_px(1.0f));
}

static bool ui_viewport_overlay_text_button(ImDrawList* dl, ImVec2 pos, const char* label,
                                            const char* tooltip, bool active, ImVec2* out_next_pos) {
    if (!dl || !label) {
        if (out_next_pos) *out_next_pos = pos;
        return false;
    }

    float pad_x = ui_px(8.0f);
    float pad_y = ui_px(3.0f);
    float min_h = ui_viewport_overlay_icon_button_size();
    ImVec2 text_sz = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 size(ui_viewport_overlay_text_button_width(label), text_sz.y + pad_y * 2.0f);
    if (size.y < min_h)
        size.y = min_h;

    ImVec2 max(pos.x + size.x, pos.y + size.y);
    ImVec2 mouse = ImGui::GetMousePos();
    bool hovered = mouse.x >= pos.x && mouse.x <= max.x && mouse.y >= pos.y && mouse.y <= max.y;
    bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    ui_viewport_overlay_draw_button_bg(dl, pos, max, hovered, held, active);
    ImU32 text_col = ImGui::GetColorU32(active ? ImVec4(1.0f, 0.88f, 0.74f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    dl->AddText(ImVec2(pos.x + pad_x, pos.y + floorf((size.y - text_sz.y) * 0.5f)), text_col, label);

    if (hovered && tooltip && tooltip[0])
        ImGui::SetTooltip("%s", tooltip);

    if (out_next_pos)
        *out_next_pos = ImVec2(max.x + ui_px(5.0f), pos.y);
    return clicked;
}

static void ui_draw_bounds_overlay_icon(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col) {
    float w = max.x - min.x;
    float h = max.y - min.y;
    float s = (w < h ? w : h) * 0.54f;
    ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    ImVec2 off(s * 0.22f, -s * 0.18f);
    ImVec2 a(c.x - s * 0.42f, c.y - s * 0.26f);
    ImVec2 b(c.x + s * 0.20f, c.y - s * 0.26f);
    ImVec2 cc(c.x + s * 0.20f, c.y + s * 0.36f);
    ImVec2 d(c.x - s * 0.42f, c.y + s * 0.36f);
    ImVec2 e(a.x + off.x, a.y + off.y);
    ImVec2 f(b.x + off.x, b.y + off.y);
    ImVec2 g(cc.x + off.x, cc.y + off.y);
    ImVec2 hh(d.x + off.x, d.y + off.y);
    float th = ui_px(1.5f);
    // Front face
    dl->AddLine(a, b, col, th);
    dl->AddLine(b, cc, col, th);
    dl->AddLine(cc, d, col, th);
    dl->AddLine(d, a, col, th);
    // Back face
    dl->AddLine(e, f, col, th);
    dl->AddLine(f, g, col, th);
    dl->AddLine(g, hh, col, th);
    dl->AddLine(hh, e, col, th);
    // Connecting edges
    dl->AddLine(a, e, col, th);
    dl->AddLine(b, f, col, th);
    dl->AddLine(cc, g, col, th);
    dl->AddLine(d, hh, col, th);
}

static bool ui_viewport_overlay_icon_button(ImDrawList* dl, ImVec2 pos, UiIconKind icon,
                                            const char* tooltip, bool active, ImVec2* out_next_pos) {
    if (!dl) {
        if (out_next_pos) *out_next_pos = pos;
        return false;
    }

    float size = ui_viewport_overlay_icon_button_size();
    ImVec2 max(pos.x + size, pos.y + size);
    ImVec2 mouse = ImGui::GetMousePos();
    bool hovered = mouse.x >= pos.x && mouse.x <= max.x && mouse.y >= pos.y && mouse.y <= max.y;
    bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    ui_viewport_overlay_draw_button_bg(dl, pos, max, hovered, held, active);
    ImU32 icon_col = ImGui::GetColorU32(
        held ? ImVec4(1.0f, 0.98f, 0.92f, 1.0f) :
        active ? ImVec4(1.0f, 0.78f, 0.46f, 1.0f) :
        hovered ? ImVec4(0.92f, 0.93f, 0.95f, 1.0f)
                : ImVec4(0.70f, 0.72f, 0.75f, 1.0f));
    if (icon == UI_ICON_BOUNDS)
        ui_draw_bounds_overlay_icon(dl, pos, max, icon_col);
    else
        ui_draw_icon_shape(icon, pos, max, icon_col);

    if (hovered && tooltip && tooltip[0])
        ImGui::SetTooltip("%s", tooltip);

    if (out_next_pos)
        *out_next_pos = ImVec2(max.x + ui_px(5.0f), pos.y);
    return clicked;
}

static void ui_draw_viewport_camera_overlay(ImVec2 rect_min, ImVec2 rect_max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl)
        return;

    ImVec2 toolbar_min = {}, toolbar_max = {};
    ui_viewport_toolbar_rect(rect_min, rect_max, &toolbar_min, &toolbar_max);
    s_scene_view_overlay_screen_rect.left = (LONG)floorf(toolbar_min.x);
    s_scene_view_overlay_screen_rect.top = (LONG)floorf(toolbar_min.y);
    s_scene_view_overlay_screen_rect.right = (LONG)ceilf(toolbar_max.x);
    s_scene_view_overlay_screen_rect.bottom = (LONG)ceilf(toolbar_max.y);
    s_scene_view_overlay_screen_rect_valid = true;

    ImVec2 pos = toolbar_min;
    if (ui_viewport_overlay_text_button(dl, pos, ui_camera_mode_name(g_camera_controls.mode),
            g_camera_controls.mode == CAMERA_MODE_FREE ? "Switch to horizon locked camera" : "Switch to free camera",
            true, &pos)) {
        g_camera_controls.mode = g_camera_controls.mode == CAMERA_MODE_FREE ? CAMERA_MODE_HORIZON_LOCKED : CAMERA_MODE_FREE;
        if (g_camera_controls.mode == CAMERA_MODE_HORIZON_LOCKED) {
            camera_sync_euler_from_quat(&g_camera);
            camera_set_euler(&g_camera, g_camera.yaw, clampf(g_camera.pitch, -1.55334f, 1.55334f), g_camera.roll);
        }
        app_settings_save();
        app_request_scene_render();
    }
    pos.x += ui_px(5.0f);
    if (ui_viewport_overlay_icon_button(dl, pos, UI_ICON_GIZMO_MOVE, "Move gizmo (1)",
            s_viewport_gizmo_mode == UI_GIZMO_TRANSLATE, &pos)) {
        ui_set_viewport_gizmo_mode(UI_GIZMO_TRANSLATE);
    }
    if (ui_viewport_overlay_icon_button(dl, pos, UI_ICON_GIZMO_ROTATE, "Rotate gizmo (2)",
            s_viewport_gizmo_mode == UI_GIZMO_ROTATE, &pos)) {
        ui_set_viewport_gizmo_mode(UI_GIZMO_ROTATE);
    }
    if (ui_viewport_overlay_icon_button(dl, pos, UI_ICON_GIZMO_SCALE, "Scale gizmo (3)",
            s_viewport_gizmo_mode == UI_GIZMO_SCALE, &pos)) {
        ui_set_viewport_gizmo_mode(UI_GIZMO_SCALE);
    }
    if (ui_viewport_overlay_icon_button(dl, pos, UI_ICON_WIREFRAME,
            g_dx.scene_wireframe ? "Disable wireframe" : "Enable wireframe", g_dx.scene_wireframe, &pos)) {
        g_dx.scene_wireframe = !g_dx.scene_wireframe;
        app_request_scene_render();
    }
    if (ui_viewport_overlay_icon_button(dl, pos, UI_ICON_GRID,
            g_dx.scene_grid_enabled ? "Hide grid" : "Show grid", g_dx.scene_grid_enabled, &pos)) {
        g_dx.scene_grid_enabled = !g_dx.scene_grid_enabled;
        app_settings_save();
        app_request_scene_render();
    }
#if LAZYTOOL_ENABLE_DEBUG_OVERLAYS
    if (ui_viewport_overlay_icon_button(dl, pos, UI_ICON_BOUNDS,
            g_dx.scene_bounds_debug_enabled ? "Hide bounds" : "Show bounds", g_dx.scene_bounds_debug_enabled, &pos)) {
        g_dx.scene_bounds_debug_enabled = !g_dx.scene_bounds_debug_enabled;
        app_settings_save();
        app_request_scene_render();
    }
#else
    g_dx.scene_bounds_debug_enabled = false;
#endif
}

static void ui_draw_rotation_feedback_quad(ImDrawList* dl, ImVec2 min, const char* label,
                                           float yaw, float pitch, ImVec4 tint, float alpha) {
    float size = ui_px(70.0f);
    float label_h = ui_px(15.0f);
    ImVec2 max(min.x + size, min.y + size + label_h);
    ImU32 bg = ImGui::GetColorU32(ImVec4(0.045f, 0.043f, 0.047f, 0.78f * alpha));
    ImU32 border = ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.82f * alpha));
    ImU32 line = ImGui::GetColorU32(ImVec4(0.78f, 0.76f, 0.72f, 0.20f * alpha));
    ImU32 text = ImGui::GetColorU32(ImVec4(0.92f, 0.90f, 0.86f, 0.86f * alpha));
    ImU32 dot = ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, alpha));

    dl->AddRectFilled(min, max, bg, ui_px(4.0f));
    dl->AddRect(min, max, border, ui_px(4.0f), 0, ui_px(1.0f));
    ImVec2 q0(min.x + ui_px(8.0f), min.y + ui_px(8.0f));
    ImVec2 q1(min.x + size - ui_px(8.0f), min.y + size - ui_px(8.0f));
    dl->AddRect(q0, q1, line, ui_px(2.0f));
    dl->AddLine(ImVec2((q0.x + q1.x) * 0.5f, q0.y), ImVec2((q0.x + q1.x) * 0.5f, q1.y), line);
    dl->AddLine(ImVec2(q0.x, (q0.y + q1.y) * 0.5f), ImVec2(q1.x, (q0.y + q1.y) * 0.5f), line);

    const float pi = 3.14159265358979323846f;
    float x = fmodf(yaw + pi, pi * 2.0f);
    if (x < 0.0f) x += pi * 2.0f;
    x /= pi * 2.0f;
    float y = 0.5f - clampf(pitch / (pi * 0.5f), -1.0f, 1.0f) * 0.5f;
    ImVec2 p(q0.x + x * (q1.x - q0.x), q0.y + y * (q1.y - q0.y));
    dl->AddCircleFilled(p, ui_px(4.4f), dot, 18);
    dl->AddCircle(p, ui_px(6.2f), ImGui::GetColorU32(ImVec4(0.02f, 0.018f, 0.016f, 0.70f * alpha)), 18, ui_px(1.0f));

    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(min.x + (size - ts.x) * 0.5f, min.y + size - ui_px(2.0f)), text, label);
}

static void ui_draw_manual_control_feedback(ImVec2 rect_min, ImVec2 rect_max) {
    if (!g_dx.scene_manual_control_overlay_enabled)
        return;

    static float s_camera_feedback = 0.0f;
    static float s_light_feedback = 0.0f;
    float dt = ImGui::GetIO().DeltaTime;
    float fade_speed = dt > 0.0f ? dt * 4.5f : 0.08f;
    if (g_viewport_manual_camera_active) s_camera_feedback = 1.0f;
    else s_camera_feedback = s_camera_feedback > fade_speed ? s_camera_feedback - fade_speed : 0.0f;
    if (g_viewport_manual_light_active) s_light_feedback = 1.0f;
    else s_light_feedback = s_light_feedback > fade_speed ? s_light_feedback - fade_speed : 0.0f;
    if (s_camera_feedback <= 0.001f && s_light_feedback <= 0.001f)
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float size = ui_px(70.0f);
    float gap = ui_px(8.0f);
    float total_w = 0.0f;
    if (s_camera_feedback > 0.001f) total_w += size;
    if (s_light_feedback > 0.001f) total_w += (total_w > 0.0f ? gap : 0.0f) + size;
    ImVec2 pos(rect_max.x - total_w - ui_px(12.0f), rect_max.y - size - ui_px(30.0f));
    if (pos.x < rect_min.x + ui_px(8.0f)) pos.x = rect_min.x + ui_px(8.0f);
    if (pos.y < rect_min.y + ui_px(48.0f)) pos.y = rect_min.y + ui_px(48.0f);

    if (s_camera_feedback > 0.001f) {
        ui_draw_rotation_feedback_quad(dl, pos, "Camera", g_camera.yaw, g_camera.pitch,
                                       ImVec4(0.58f, 0.74f, 0.96f, 1.0f), s_camera_feedback);
        pos.x += size + gap;
    }
    if (s_light_feedback > 0.001f) {
        Resource* dl_res = res_get(g_builtin_light);
        float yaw = 0.0f;
        float pitch = 0.0f;
        if (dl_res) {
            Vec3 light_dir = v3_norm(v3_sub(v3(dl_res->light_target[0], dl_res->light_target[1], dl_res->light_target[2]),
                                           v3(dl_res->light_pos[0], dl_res->light_pos[1], dl_res->light_pos[2])));
            yaw = atan2f(light_dir.x, light_dir.z);
            pitch = asinf(clampf(light_dir.y, -1.0f, 1.0f));
        }
        ui_draw_rotation_feedback_quad(dl, pos, "Light", yaw, pitch,
                                       ImVec4(1.00f, 0.70f, 0.36f, 1.0f), s_light_feedback);
    }
}

static void ui_panel_scene(bool embedded = false) {
    if (!embedded) ImGui::Begin("Scene");
    bool hovered = false;
    ImVec2 image_min = {};
    ImVec2 image_max = {};
    s_scene_view_screen_rect_valid = false;
    s_scene_view_overlay_screen_rect_valid = false;
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
            hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            image_min = ImGui::GetItemRectMin();
            image_max = ImGui::GetItemRectMax();
            s_scene_view_screen_rect.left = (LONG)floorf(image_min.x);
            s_scene_view_screen_rect.top = (LONG)floorf(image_min.y);
            s_scene_view_screen_rect.right = (LONG)ceilf(image_max.x);
            s_scene_view_screen_rect.bottom = (LONG)ceilf(image_max.y);
            s_scene_view_screen_rect_valid = true;
        }
    }
    bool overlay_hovered = false;
    if (g_dx.scene_srv && image_max.x > image_min.x && image_max.y > image_min.y)
        overlay_hovered = ui_viewport_toolbar_hit_test(image_min, image_max);
    bool panel_focused = ui_current_panel_focused();
    bool pointer_over_viewport = hovered && !overlay_hovered;
    bool viewport_hovered = panel_focused && pointer_over_viewport;
    ui_handle_viewport_gizmo_hotkeys(viewport_hovered);
    g_scene_view_hovered = viewport_hovered;
    g_scene_view_focused = panel_focused;
    g_scene_view_pointer_over = pointer_over_viewport;
    if (g_dx.scene_srv && image_max.x > image_min.x && image_max.y > image_min.y) {
        ui_draw_viewport_bounds_debug(image_min, image_max);
        ui_draw_viewport_light_debug(image_min, image_max);
        ui_draw_viewport_gizmo(image_min, image_max, viewport_hovered);
        if (g_dx.scene_orientation_gizmo_enabled)
            ui_draw_camera_orientation_gizmo(image_min, image_max);
        ui_draw_manual_control_feedback(image_min, image_max);
        ui_draw_viewport_camera_overlay(image_min, image_max);
    }
    if (!embedded) ImGui::End();
}

// -- public ----------------------------------------------------------------

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
static bool s_panel_focus_stack[16] = {};
static int s_panel_focus_count = 0;
static ImGuiID s_focused_panel_id = 0;

static ImVec4 ui_panel_bg(UiPanelTone tone) {
    (void)tone;
    return ImVec4(0.090f, 0.087f, 0.092f, 1.0f);
}

static ImVec4 ui_panel_focus_bg(UiPanelTone tone) {
    ImVec4 bg = ui_panel_bg(tone);
    bg.x += 0.014f;
    bg.y += 0.012f;
    bg.z += 0.010f;
    return bg;
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

static bool ui_current_panel_focused() {
    if (s_panel_focus_count > 0)
        return s_panel_focus_stack[s_panel_focus_count - 1];
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
}

static void ui_push_panel_focus(bool focused) {
    if (s_panel_focus_count < (int)(sizeof(s_panel_focus_stack) / sizeof(s_panel_focus_stack[0])))
        s_panel_focus_stack[s_panel_focus_count++] = focused;
}

static void ui_pop_panel_focus() {
    if (s_panel_focus_count > 0)
        s_panel_focus_count--;
}

static void ui_focus_current_panel_window() {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window)
        return;

    s_focused_panel_id = window->ID;
    ImGui::SetWindowFocus();
    ImGui::ClearActiveID();
    if (s_panel_focus_count > 0)
        s_panel_focus_stack[s_panel_focus_count - 1] = true;
}

static bool ui_update_panel_focus_from_current_window(bool accept_imgui_focus = true,
                                                      bool accept_mouse_focus = true) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window)
        return false;

    ImGuiID panel_id = window->ID;
    bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    bool mouse_focus_click = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                             ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                             ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    if ((accept_imgui_focus && focused) ||
        (accept_mouse_focus && hovered && mouse_focus_click))
        s_focused_panel_id = panel_id;
    return s_focused_panel_id == panel_id;
}

static void ui_draw_panel_focus_bg(UiPanelTone tone) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetWindowPos();
    ImVec2 max = ImVec2(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight());
    dl->AddRectFilled(min, max, ImGui::GetColorU32(ui_with_alpha(ui_panel_focus_bg(tone), 0.28f)), 4.0f);
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
    s.WindowPadding = ImVec2(7.0f, 7.0f);
    s.FramePadding = ImVec2(6.0f, 3.0f);
    s.CellPadding = ImVec2(6.0f, 3.0f);
    s.ItemSpacing = ImVec2(6.0f, 5.0f);
    s.ItemInnerSpacing = ImVec2(5.0f, 3.0f);
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
    case UI_ICON_RENDER_GRAPH:     return "assets/icons/lucide-workflow.svg";
    case UI_ICON_SHADER_EDITOR:    return "assets/icons/lucide-file-code-2.svg";
    case UI_ICON_NEW_PROJECT:      return "assets/icons/lucide-plus.svg";
    case UI_ICON_LOAD_PROJECT:     return "assets/icons/lucide-folder-open.svg";
    case UI_ICON_SAVE_PROJECT:     return "assets/icons/lucide-save.svg";
    case UI_ICON_COMPILE:          return "assets/icons/lucide-hammer.svg";
    case UI_ICON_EXPORT_EXE:       return "assets/icons/lucide-file-down.svg";
    case UI_ICON_BOUNDS:           return "assets/icons/lucide-cuboid.svg";
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
        { UI_ICON_RENDER_GRAPH, nullptr, false },
        { UI_ICON_SHADER_EDITOR, nullptr, false },
        { UI_ICON_NEW_PROJECT, nullptr, false },
        { UI_ICON_LOAD_PROJECT, nullptr, false },
        { UI_ICON_SAVE_PROJECT, nullptr, false },
        { UI_ICON_COMPILE, nullptr, false },
        { UI_ICON_EXPORT_EXE, nullptr, false },
        { UI_ICON_BOUNDS, nullptr, false },
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


static bool ui_icon_text_button(const char* id, UiIconKind icon, const char* label, const char* tooltip = nullptr) {
    const char* safe_label = label ? label : "";
    ImGuiStyle& style = ImGui::GetStyle();
    float h = ImGui::GetFrameHeight();
    float icon_size = ui_px(16.0f);
    ImVec2 text_sz = ImGui::CalcTextSize(safe_label);
    float w = style.FramePadding.x * 3.0f + icon_size + text_sz.x;
    if (w < h)
        w = h;

    ImGui::PushID(id);
    bool clicked = ImGui::Button("##icon_text", ImVec2(w, h));
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();

    ImU32 icon_col = ImGui::GetColorU32(ImGui::IsItemActive() ? ImVec4(0.98f, 0.98f, 1.0f, 1.0f) :
        (ImGui::IsItemHovered() ? ImVec4(0.92f, 0.93f, 0.95f, 1.0f) : ImVec4(0.78f, 0.79f, 0.82f, 1.0f)));
    ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);

    float icon_y = min.y + floorf((h - icon_size) * 0.5f);
    ImVec2 icon_min(min.x + style.FramePadding.x, icon_y);
    ImVec2 icon_max(icon_min.x + icon_size, icon_min.y + icon_size);
    ui_draw_icon_shape(icon, icon_min, icon_max, icon_col);

    float text_x = icon_max.x + style.FramePadding.x;
    float text_y = min.y + floorf((h - text_sz.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(text_x, text_y), text_col, safe_label);

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
    if (strncmp(action, "Close", 5) == 0) {
        if (tooltip) *tooltip = "Close";
        return UI_ICON_CLOSE;
    }
    if (strncmp(action, "ShaderEditor", 12) == 0) {
        if (tooltip) *tooltip = "Shader editor";
        return UI_ICON_SHADER_EDITOR;
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
    bool focused = ui_current_panel_focused();
    ImVec2 header_pos = ImGui::GetCursorScreenPos();
    float header_h = ImGui::GetTextLineHeight() + ui_margin_px(10.0f);
    ImDrawList* header_dl = ImGui::GetWindowDrawList();
    float header_right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    ImVec2 header_min(header_pos.x - ui_margin_px(3.0f), header_pos.y - ui_margin_px(2.0f));
    ImVec2 header_max(header_right, header_pos.y + header_h + ui_margin_px(2.0f));
    ImVec2 mouse = ImGui::GetMousePos();
    bool header_window_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    bool was_focused = focused;
    bool header_clicked = header_window_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        mouse.x >= header_min.x && mouse.x <= header_max.x &&
        mouse.y >= header_min.y && mouse.y <= header_max.y;
    if (header_clicked) {
        ui_focus_current_panel_window();
        focused = true;
    }

    header_dl->AddRectFilled(header_min, header_max,
        ImGui::GetColorU32(ui_with_alpha(accent, focused ? 0.075f : 0.035f)), 3.0f);
    header_dl->AddRectFilled(header_min, ImVec2(header_pos.x, header_max.y),
        ImGui::GetColorU32(ui_with_alpha(accent, focused ? 0.86f : 0.50f)), 1.5f);

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
        ImGui::GetColorU32(ui_with_alpha(accent, focused ? 1.0f : 0.74f)), title);

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
            if (ui_header_action_button(actions[i]) && clicks[i] && was_focused)
                *clicks[i] = true;
        }
        buttons_x += action_w + button_spacing;
    }

    ImGui::SetCursorScreenPos(ImVec2(header_pos.x, header_pos.y));
    ImGui::Dummy(ImVec2(0.0f, header_h));
    ImGui::Separator();
}

static void ui_draw_shader_editor_window() {
    static bool s_shader_editor_was_open = false;
    if (!s_shader_editor_floating) {
        s_shader_editor_was_open = false;
        return;
    }

    if (!ui_shader_editor_is_shader_handle(s_shader_editor_floating_h))
        s_shader_editor_floating_h = ui_shader_editor_first_shader_handle();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowSize(ImVec2(ui_px(900.0f), ui_px(640.0f)), ImGuiCond_FirstUseEver);
    bool focus_on_open = !s_shader_editor_was_open;
    if (focus_on_open)
        ImGui::SetNextWindowFocus();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoTitleBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_panel_bg(UI_PANEL_DEFAULT));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    bool open = true;
    bool began = ImGui::Begin("Shader Editor###ShaderSourceFloating", &open, window_flags);
    s_shader_editor_was_open = true;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (!began) {
        ImGui::End();
        if (!open)
            ui_shader_editor_close_floating(false);
        return;
    }

    Resource* r = res_get(s_shader_editor_floating_h);
    int shader_count = ui_shader_editor_shader_count();
    char detail[128] = {};
    if (r) {
        const char* shader_name = (r->name && r->name[0]) ? r->name : "(unnamed)";
        snprintf(detail, sizeof(detail), "%s  ?  %s  ?  %d shader%s",
                 shader_name,
                 r->compiled_ok && !r->using_fallback ? "compiled" : "fallback",
                 shader_count, shader_count == 1 ? "" : "s");
    } else {
        snprintf(detail, sizeof(detail), "%d shader%s", shader_count, shader_count == 1 ? "" : "s");
    }

    if (s_panel_tone_count < (int)(sizeof(s_panel_tone_stack) / sizeof(s_panel_tone_stack[0])))
        s_panel_tone_stack[s_panel_tone_count++] = UI_PANEL_DEFAULT;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_panel_bg(UI_PANEL_DEFAULT));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.205f, 0.200f, 1.0f));

    if (!ImGui::BeginChild("##shader_editor_panel", ImGui::GetContentRegionAvail(), true)) {
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (s_panel_tone_count > 0)
            s_panel_tone_count--;
        ImGui::End();
        if (!open)
            ui_shader_editor_close_floating(false);
        return;
    }

    bool close_clicked = false;
    bool focused = ui_update_panel_focus_from_current_window(false, true);
    ui_push_panel_focus(focused);
    if (focus_on_open) {
        ui_focus_current_panel_window();
        focused = true;
    }
    if (focused)
        ui_draw_panel_focus_bg(UI_PANEL_DEFAULT);
    ui_panel_header("SHADER EDITOR", detail, "Close##shader_editor_close", &close_clicked);
    ui_pop_panel_focus();
    if (close_clicked)
        open = false;
    ImGui::Spacing();

    if (!r) {
        ui_inspector_text_disabled_wrapped("No shader resources in the current project.");
    } else {
        UiShaderSourceEditor& ed = s_shader_source_ed;
        const char* path = r->path;
        char root_path[MAX_PATH_LEN] = {};
        ui_normalize_path_text(path ? path : "", root_path, MAX_PATH_LEN);
        bool reload = ed.h != s_shader_editor_floating_h || strcmp(ed.root_path, root_path) != 0;
        if (reload) {
            if (ed.dirty)
                ui_shader_editor_save_current_file(&ed);
            ui_shader_editor_load(&ed, s_shader_editor_floating_h, path);
        }

        if (!ed.path[0]) {
            ui_inspector_text_disabled_wrapped("No shader path.");
        } else if (!ed.ok || !ed.text) {
            if (ImGui::Button("Reload Source", ImVec2(-1.0f, 0.0f)))
                ui_shader_editor_load(&ed, s_shader_editor_floating_h, path);
            ui_inspector_text_disabled_wrapped("Could not read source: %s", ed.path);
            ui_shader_template_buttons(s_shader_editor_floating_h, r, path);
        } else {
            ui_shader_source_editor_body(&ed, s_shader_editor_floating_h, r, true);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (s_panel_tone_count > 0)
        s_panel_tone_count--;
    ImGui::End();

    if (!open)
        ui_shader_editor_close_floating(false);
}


static bool ui_compile_open_shader_editor() {
    UiShaderSourceEditor& ed = s_shader_source_ed;
    if (!ed.ok || !ed.text || !ui_shader_editor_is_shader_handle(ed.h))
        return false;

    Resource* r = res_get(ed.h);
    if (!r || r->type != RES_SHADER)
        return false;

    if (ed.dirty)
        return ui_shader_editor_save(&ed, ed.h, r, true);

    ui_recompile_shader_resource(ed.h, r, r->path);
    if (!r->compiled_ok || r->using_fallback)
        ui_shader_editor_open_compile_error_file(&ed, r);
    return true;
}

static void ui_recompile_active_or_selected_shader() {
    // When the shader source editor is open, Ctrl+D should target the source
    // currently being edited, not an unrelated resource selected in the
    // inspector. This is especially important for the floating editor, whose
    // selector is intentionally independent from the main selection.
    if ((s_shader_editor_floating || s_shader_source_ed.editor_focused || s_shader_source_editor_focused) &&
        ui_compile_open_shader_editor()) {
        return;
    }

    ui_recompile_selected_shader();
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
    bool open = ImGui::BeginChild(id, size, true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ui_lock_current_window_scroll_x();
    bool force_focus = (strcmp(id, "##inspector_panel") == 0 && s_focus_inspector_panel_next) ||
                       (strcmp(id, "##general_panel") == 0 && s_focus_general_panel_next);
    if (force_focus) {
        if (strcmp(id, "##inspector_panel") == 0)
            s_focus_inspector_panel_next = false;
        if (strcmp(id, "##general_panel") == 0)
            s_focus_general_panel_next = false;
    }
    bool focused = ui_update_panel_focus_from_current_window(false);
    ui_push_panel_focus(focused);
    if (force_focus) {
        ui_focus_current_panel_window();
        focused = true;
    }
    if (focused)
        ui_draw_panel_focus_bg(tone);
    ui_panel_header(title, detail,
                    action_a, clicked_a, action_b, clicked_b, action_c, clicked_c,
                    action_d, clicked_d, action_e, clicked_e, action_f, clicked_f,
                    action_g, clicked_g, action_h, clicked_h);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, ui_margin_px(6.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::SetNextWindowContentSize(ImVec2(content_w, 0.0f));
    bool content_open = ImGui::BeginChild("##tool_panel_content", ImVec2(0.0f, 0.0f), false);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return open && content_open;
}

static void ui_end_tool_panel() {
    ui_lock_current_window_scroll_x();
    ImGui::EndChild();
    ui_pop_panel_focus();
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
        bool focused = ui_update_panel_focus_from_current_window();
        ui_push_panel_focus(focused);
        if (focused)
            ui_draw_panel_focus_bg(tone);
        ui_panel_header(title, detail);
        clicked = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0);
        ui_pop_panel_focus();
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

static void ui_draw_profiler_gpu_command_table() {
    if (!cmd_profile_ready()) {
        ImGui::TextDisabled("Command GPU ranges: warming up...");
        return;
    }

    int active_count = 0;
    for (int i = 0; i < MAX_COMMANDS; i++)
        if (g_commands[i].active)
            active_count++;

    if (active_count == 0) {
        ImGui::TextDisabled("Command GPU ranges: no active commands");
        return;
    }

    ImGui::TextDisabled("Command GPU ranges. Groups and repeats include their children.");

    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;
    const float table_h = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
    if (!ImGui::BeginTable("##profiler_gpu_command_table", 5, flags, ImVec2(0.0f, table_h)))
        return;

    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ui_px(34.0f));
    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, ui_px(72.0f));
    ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, ui_px(74.0f));
    ImGui::TableHeadersRow();

    if (cmd_profile_shadow_ms() > 0.00001f) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("--");
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.70f, 0.38f, 1.0f));
        ImGui::TextUnformatted("shadow_prepass");
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted("Shadow");
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted("internal");
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%.3f", cmd_profile_shadow_ms());
    }

    for (int i = 0; i < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active)
            continue;

        CmdHandle h = (CmdHandle)(i + 1);
        bool dim = !c.enabled || cmd_profile_ms(h) <= 0.00001f;
        if (dim)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%03d", i + 1);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(c.name[0] ? c.name : "<unnamed>");
        ImGui::TableSetColumnIndex(2);
        if (!dim)
            ImGui::PushStyleColor(ImGuiCol_Text, ui_command_type_color(c.type));
        ImGui::TextUnformatted(cmd_type_str(c.type));
        if (!dim)
            ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(c.enabled ? "enabled" : "disabled");
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%.3f", cmd_profile_ms(h));

        if (dim)
            ImGui::PopStyleColor();
    }

    ImGui::EndTable();
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


static int ui_active_resource_count(bool include_builtins) {
    int count = 0;
    for (int i = 0; i < MAX_RESOURCES; i++) {
        if (!g_resources[i].active)
            continue;
        if (!include_builtins && g_resources[i].is_builtin)
            continue;
        count++;
    }
    return count;
}

static int ui_enabled_command_count() {
    int count = 0;
    for (int i = 0; i < MAX_COMMANDS; i++) {
        if (g_commands[i].active && g_commands[i].enabled)
            count++;
    }
    return count;
}

static void ui_draw_basic_monitoring_readout() {
    ImGui::Text("FPS: %.1f  Frame: %.2f ms", app_frame_fps(), app_frame_delta_ms());
    ImGui::Text("Scene: %d x %d", g_dx.scene_width, g_dx.scene_height);
    ImGui::Text("Commands: %d enabled / %d total", ui_enabled_command_count(), g_command_count);
    ImGui::Text("Resources: %d project / %d total", ui_active_resource_count(false), ui_active_resource_count(true));
}

static void ui_refresh_profiler_readout_cache(bool force) {
    // These readouts are useful diagnostics, not per-frame simulation data.
    // Keep them slightly decimated so an open profiler panel does not add
    // process-memory queries and resource-memory walks to every ImGui frame.
    double now = ImGui::GetTime();
    if (!force && s_profiler_readout_cache.next_update_time >= 0.0 &&
        now < s_profiler_readout_cache.next_update_time) {
        return;
    }

    s_profiler_readout_cache.app_memory_bytes = ui_process_memory_bytes();
    s_profiler_readout_cache.gpu_memory_bytes = ui_estimated_gpu_memory_bytes();
    s_profiler_readout_cache.project_gpu_memory_bytes = res_estimate_gpu_total(false);
    ui_format_bytes(s_profiler_readout_cache.app_memory_bytes,
                    s_profiler_readout_cache.app_memory,
                    sizeof(s_profiler_readout_cache.app_memory));
    ui_format_bytes(s_profiler_readout_cache.gpu_memory_bytes,
                    s_profiler_readout_cache.gpu_memory,
                    sizeof(s_profiler_readout_cache.gpu_memory));
    ui_format_bytes(s_profiler_readout_cache.project_gpu_memory_bytes,
                    s_profiler_readout_cache.project_gpu_memory,
                    sizeof(s_profiler_readout_cache.project_gpu_memory));
    s_profiler_readout_cache.next_update_time = now + 0.25;
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

bool ui_scene_view_contains_screen_point(int x, int y) {
    if (!s_scene_view_screen_rect_valid)
        return false;
    if (s_scene_view_overlay_screen_rect_valid &&
        x >= s_scene_view_overlay_screen_rect.left && x < s_scene_view_overlay_screen_rect.right &&
        y >= s_scene_view_overlay_screen_rect.top && y < s_scene_view_overlay_screen_rect.bottom) {
        return false;
    }
    return x >= s_scene_view_screen_rect.left && x < s_scene_view_screen_rect.right &&
           y >= s_scene_view_screen_rect.top && y < s_scene_view_screen_rect.bottom;
}

bool ui_scene_view_screen_rect(RECT* out_rect) {
    if (!out_rect || !s_scene_view_screen_rect_valid)
        return false;
    *out_rect = s_scene_view_screen_rect;
    return true;
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

static void ui_draw_help_shortcuts_tab() {
    ImGuiTableFlags table_flags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_PadOuterX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 5.0f));

    if (ui_begin_shortcut_section("##help_shortcuts_execution", "EXECUTION", table_flags)) {
        ui_draw_shortcut_row("Space", "Pause / resume scene execution");
        ui_draw_shortcut_row("F6", "Restart scene from frame 0");
        ui_draw_shortcut_row("F10", "Toggle player preview with no editor UI");
        ui_draw_shortcut_row("F11", "Toggle viewport fullscreen");
        ImGui::EndTable();
    }

    if (ui_begin_shortcut_section("##help_shortcuts_project", "PROJECT", table_flags)) {
        ui_draw_shortcut_row("F5", "Compile all shaders");
        ui_draw_shortcut_row("Ctrl+D", "Compile edited/selected shader");
        ui_draw_shortcut_row("Ctrl+S", "Save shader source or project");
        ui_draw_shortcut_row("Shift+Tab", "Cycle shader editor file");
        ui_draw_shortcut_row("Ctrl+L", "Load project");
        ui_draw_shortcut_row("F1", "Toggle this help panel");
        ImGui::EndTable();
    }

    if (ui_begin_shortcut_section("##help_shortcuts_timeline", "TIMELINE", table_flags)) {
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

    if (ui_begin_shortcut_section("##help_shortcuts_selection", "SELECTION", table_flags)) {
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

    if (ui_begin_shortcut_section("##help_shortcuts_camera", "CAMERA", table_flags)) {
        ui_draw_shortcut_row("RMB", "Mouse look");
        ui_draw_shortcut_row("MMB / Wheel", "Zoom around selected/scene bounding box");
        ui_draw_shortcut_row("WASD", "Move on camera forward/right axes");
        ui_draw_shortcut_row("R / T", "Move up / down on camera up axis");
        ui_draw_shortcut_row("Q / E", "Roll left / right");
        ui_draw_shortcut_row("Alt + LMB", "Orbit selected/scene bounding box");
        ui_draw_shortcut_row("F", "Frame selected/scene bounding box");
        ui_draw_shortcut_row("Shift", "Faster movement");
        ui_draw_shortcut_row("Ctrl", "Slower movement");
        ui_draw_shortcut_row("L", "Orbit light");
        ImGui::EndTable();
    }

    if (ui_inspector_section("DRAW SRV SLOTS")) {
        ui_inspector_text_disabled_wrapped("Common shader t# convention used by mesh materials and PBR shaders.");
        ui_draw_texture_slot_reference("##help_draw_slots");
        ui_inspector_text_disabled_wrapped("Manual SRV bindings in a draw command override mesh material textures on the same slot.");
    }

    ImGui::PopStyleVar();
}

static void ui_draw_common_function_row(const char* signature, const char* args, const char* desc) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextWrapped("%s", signature);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", args);
    ImGui::TableSetColumnIndex(2);
    ImGui::TextWrapped("%s", desc);
}

static bool ui_draw_help_common_section(const char* title) {
    return ui_inspector_section(title);
}

static void ui_draw_help_common_tab() {
    ui_inspector_text_disabled_wrapped("Reference for shaders/common.hlsl. Include it with #include \"common.hlsl\" from shaders in the root folder, or adjust the relative path from subfolders.");
    ui_inspector_text_disabled_wrapped("Define LT_NO_DEFAULT_SHADOWMAP before including it if you want to bind your own shadow map and comparison sampler.");
    ImGui::Spacing();

    ImGuiTableFlags flags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_PadOuterX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(7.0f, 5.0f));

    if (ui_draw_help_common_section("CAMERA / TRANSFORM") &&
        ImGui::BeginTable("##help_common_camera", 3, flags)) {
        ImGui::TableSetupColumn("Signature", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableSetupColumn("Arguments", ImGuiTableColumnFlags_WidthStretch, 1.00f);
        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableHeadersRow();
        ui_draw_common_function_row("float3 lt_camera_position_ws()", "-", "Camera position in world space.");
        ui_draw_common_function_row("float3 lt_camera_forward_ws()", "-", "Normalized camera forward vector.");
        ui_draw_common_function_row("float3 lt_vector_to_camera_ws(float3 world_pos)", "world_pos: position in world space.", "Direction from a point toward the camera.");
        ui_draw_common_function_row("float3 lt_ray_from_camera_ws(float3 world_pos)", "world_pos: position in world space.", "Direction from the camera through a world point.");
        ui_draw_common_function_row("float4 lt_object_to_world(float3 object_pos)", "object_pos: local/object position.", "Transforms local position with ObjectCB.LocalToWorld.");
        ui_draw_common_function_row("float3 lt_object_normal_to_world(float3 object_normal)", "object_normal: local/object normal.", "Transforms and normalizes a local normal.");
        ui_draw_common_function_row("float4 lt_world_to_clip(float3 world_pos)", "world_pos: position in world space.", "Projects a world point with SceneCB.ViewProj.");
        ui_draw_common_function_row("float4 lt_world_to_view(float3 world_pos)", "world_pos: position in world space.", "Transforms a world point with SceneCB.WorldToView.");
        ui_draw_common_function_row("float4 lt_view_to_world(float3 view_pos)", "view_pos: camera/view-space position.", "Transforms a view-space point with SceneCB.ViewToWorld.");
        ui_draw_common_function_row("float3 lt_clip_to_ndc(float4 clip_pos)", "clip_pos: homogeneous clip position.", "Divides xyz by w.");
        ui_draw_common_function_row("float2 lt_ndc_to_uv(float2 ndc)", "ndc: xy in -1..1 clip space.", "Converts NDC to texture UV with D3D y flip.");
        ui_draw_common_function_row("float2 lt_clip_to_uv(float4 clip_pos)", "clip_pos: homogeneous clip position.", "Projects clip space directly to UV.");
        ImGui::EndTable();
    }

    if (ui_draw_help_common_section("DEPTH / SCREEN") &&
        ImGui::BeginTable("##help_common_depth", 3, flags)) {
        ImGui::TableSetupColumn("Signature", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableSetupColumn("Arguments", ImGuiTableColumnFlags_WidthStretch, 1.00f);
        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableHeadersRow();
        ui_draw_common_function_row("float4 lt_uv_depth_to_clip(float2 uv, float depth01)", "uv: texture coordinates. depth01: hardware depth.", "Builds D3D clip position for reconstruction.");
        ui_draw_common_function_row("float3 lt_scene_depth_to_world(float2 uv, float depth01)", "uv: texture coordinates. depth01: sampled scene depth.", "Reconstructs world position using InvViewProj.");
        ui_draw_common_function_row("float lt_view_depth_from_world(float3 world_pos)", "world_pos: position in world space.", "Signed camera-forward distance.");
        ui_draw_common_function_row("float lt_scene_depth_to_view_depth(float2 uv, float depth01)", "uv/depth01: sampled depth pixel.", "Reconstructs world and returns view depth.");
        ui_draw_common_function_row("float lt_depth01_to_view_depth(float depth01)", "depth01: D3D hardware depth.", "Linear depth using camera near/far; supports perspective and orthographic.");
        ui_draw_common_function_row("float lt_view_depth_to_depth01(float view_depth)", "view_depth: camera-forward distance.", "Converts linear view depth back to hardware depth.");
        ui_draw_common_function_row("float2 lt_sv_position_to_uv(float4 sv_position, float2 render_size)", "sv_position: pixel position. render_size: target size.", "Pixel shader SV_POSITION to UV.");
        ui_draw_common_function_row("float2 lt_uv_to_pixel(float2 uv, float2 render_size)", "uv, render_size.", "UV to pixel coordinates.");
        ui_draw_common_function_row("float2 lt_pixel_to_uv(float2 pixel, float2 render_size)", "pixel, render_size.", "Pixel coordinates to UV.");
        ui_draw_common_function_row("float2 lt_viewport_uv_to_ndc(float2 uv)", "uv: texture coordinates.", "UV to NDC with D3D y direction.");
        ui_draw_common_function_row("float2 lt_motion_vector_uv(float3 world_pos)", "world_pos: current world position.", "Current UV minus previous-frame UV.");
        ImGui::EndTable();
    }

    if (ui_draw_help_common_section("SHADOW") &&
        ImGui::BeginTable("##help_common_shadow", 3, flags)) {
        ImGui::TableSetupColumn("Signature", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableSetupColumn("Arguments", ImGuiTableColumnFlags_WidthStretch, 1.00f);
        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableHeadersRow();
        ui_draw_common_function_row("int lt_shadow_cascade_count()", "-", "Cascade count clamped to 1..4.");
        ui_draw_common_function_row("int lt_select_shadow_cascade(float3 world_pos)", "world_pos: position in world space.", "Selects cascade from camera-forward depth.");
        ui_draw_common_function_row("float4 lt_shadow_clip(int cascade_index, float3 world_pos)", "cascade_index, world_pos.", "Projects to cascade clip space.");
        ui_draw_common_function_row("float3 lt_shadow_ndc(int cascade_index, float3 world_pos)", "cascade_index, world_pos.", "Cascade projection divided by w.");
        ui_draw_common_function_row("float2 lt_shadow_local_uv_from_ndc(float3 shadow_ndc)", "shadow_ndc: cascade NDC.", "Cascade-local shadow UV.");
        ui_draw_common_function_row("float3 lt_shadow_array_uv(int cascade_index, float2 local_uv)", "cascade_index, local_uv.", "Maps cascade-local UV into Texture2DArray UV.");
        ui_draw_common_function_row("bool lt_shadow_inside(float3 shadow_ndc, float2 local_uv)", "shadow_ndc, local_uv.", "Checks cascade UV and depth bounds.");
        ui_draw_common_function_row("float lt_shadow_bias(float ndl)", "ndl: normal dot light.", "Default slope-ish bias used by PCF helpers.");
        ui_draw_common_function_row("float lt_sample_shadow_cascade_pcf3x3(int cascade_index, float3 world_pos, float ndl)", "cascade_index, world_pos, ndl.", "Samples default ShadowMap/ShadowSampler at one cascade.");
        ui_draw_common_function_row("float lt_sample_shadow_pcf3x3(float3 world_pos, float3 normal_ws, float3 light_dir_ws)", "world_pos, normal_ws, light_dir_ws.", "Selects cascade and samples default 3x3 PCF.");
        ui_draw_common_function_row("float lt_sample_shadow_cascade_pcf3x3(Texture2DArray shadow_map, SamplerComparisonState shadow_sampler, int cascade_index, float3 world_pos, float ndl)", "custom shadow map/sampler plus cascade data.", "Expert overload for custom bindings.");
        ui_draw_common_function_row("float lt_sample_shadow_pcf3x3(Texture2DArray shadow_map, SamplerComparisonState shadow_sampler, float3 world_pos, float3 normal_ws, float3 light_dir_ws)", "custom shadow map/sampler plus surface data.", "Expert overload with custom bindings.");
        ImGui::EndTable();
    }

    if (ui_draw_help_common_section("UTILITY") &&
        ImGui::BeginTable("##help_common_utility", 3, flags)) {
        ImGui::TableSetupColumn("Signature", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableSetupColumn("Arguments", ImGuiTableColumnFlags_WidthStretch, 1.00f);
        ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableHeadersRow();
        ui_draw_common_function_row("float/float2/float3 lt_square(x)", "x: scalar/vector.", "Returns x*x.");
        ui_draw_common_function_row("float2/float3 lt_safe_normalize(v)", "v: vector.", "Normalize with a small length guard.");
        ui_draw_common_function_row("float lt_luminance(float3 c)", "c: linear RGB color.", "Rec.709 luminance.");
        ui_draw_common_function_row("float3 lt_aces_fitted(float3 color)", "color: HDR linear RGB.", "Simple fitted ACES tone map.");
        ui_draw_common_function_row("float3 lt_decode_normal_rgb(float4 enc)", "enc: normal encoded in 0..1 RGB.", "Decodes to normalized -1..1 normal.");
        ImGui::EndTable();
    }

    ImGui::PopStyleVar();
}

static void ui_draw_help_popup() {
    static bool s_help_was_open = false;
    if (!s_help_popup_open) {
        s_help_was_open = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_Appearing);
    bool focus_on_open = !s_help_was_open;
    if (focus_on_open)
        ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_panel_bg(UI_PANEL_GENERAL));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.205f, 0.200f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    if (ImGui::Begin("Help", &s_help_popup_open,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar))
    {
        s_help_was_open = true;
        bool close_clicked = false;
        bool focused = ui_update_panel_focus_from_current_window(false, true);
        ui_push_panel_focus(focused);
        if (focus_on_open) {
            ui_focus_current_panel_window();
            focused = true;
        }
        if (focused)
            ui_draw_panel_focus_bg(UI_PANEL_GENERAL);
        ui_panel_header("HELP", "F1", "Close##help_close", &close_clicked);
        bool panel_interactive = ui_current_panel_focused();
        ui_pop_panel_focus();
        if (close_clicked)
            s_help_popup_open = false;

        if (!panel_interactive)
            ImGui::BeginDisabled();
        if (ImGui::BeginTabBar("##help_tabs")) {
            if (ImGui::BeginTabItem("Shortcuts")) {
                ImGui::BeginChild("##help_shortcuts_scroll", ImVec2(0.0f, 0.0f), false,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
                ui_draw_help_shortcuts_tab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Common HLSL")) {
                ImGui::BeginChild("##help_common_scroll", ImVec2(0.0f, 0.0f), false,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
                ui_draw_help_common_tab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!panel_interactive)
            ImGui::EndDisabled();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

static bool ui_begin_shortcut_section(const char* id, const char* title, ImGuiTableFlags table_flags) {
    if (!ui_inspector_section(title))
        return false;
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

static void ui_help_marker(const char* desc) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
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
static int s_timeline_slot_selection_timeline = -1;
static int s_timeline_frame_context_frame = 0;

static bool ui_timeline_slot_selection_valid() {
    if (!s_timeline_slot_selection.valid)
        return false;
    if (s_timeline_slot_selection_timeline != timeline_current_index())
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
    s_timeline_slot_selection_timeline = timeline_current_index();
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

static const char* ui_timeline_interpolation_label(int mode) {
    switch (mode) {
    case TIMELINE_INTERP_STEP:      return "Step / Flat";
    case TIMELINE_INTERP_LINEAR:    return "Linear";
    case TIMELINE_INTERP_QUADRATIC: return "Quadratic";
    case TIMELINE_INTERP_CUBIC:     return "Cubic";
    default:                        return "Cubic";
    }
}

static void ui_timeline_set_key_interpolation(TimelineKey& key, int mode) {
    if (mode < TIMELINE_INTERP_STEP)
        mode = TIMELINE_INTERP_STEP;
    if (mode > TIMELINE_INTERP_CUBIC)
        mode = TIMELINE_INTERP_CUBIC;
    if (key.interpolation_mode == mode)
        return;
    key.interpolation_mode = mode;
    if (key.tangent_scale < 0.0f || key.tangent_scale > 4.0f)
        key.tangent_scale = 1.0f;
    app_request_scene_render();
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
    if (!ui_inspector_section("ADD TRACK"))
        return;

    ui_timeline_add_track_button("Camera", TIMELINE_TRACK_CAMERA, "camera", RES_NONE);
    ImGui::SameLine();
    ui_timeline_add_track_button("Light", TIMELINE_TRACK_LIGHT, "light", RES_NONE);

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
    case TIMELINE_TRACK_LIGHT:
        snprintf(out, out_sz, "Light");
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
    int key_index = timeline_find_key_index(track, frame);
    bool has_key = key_index >= 0;
    TimelineKey* key = has_key ? &track.keys[key_index] : nullptr;
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
        if (has_key && key) {
            ImGui::Separator();
            if (ImGui::BeginMenu("Interpolation")) {
                if (ImGui::MenuItem("Step / Flat", nullptr, key->interpolation_mode == TIMELINE_INTERP_STEP))
                    ui_timeline_set_key_interpolation(*key, TIMELINE_INTERP_STEP);
                if (ImGui::MenuItem("Linear", nullptr, key->interpolation_mode == TIMELINE_INTERP_LINEAR))
                    ui_timeline_set_key_interpolation(*key, TIMELINE_INTERP_LINEAR);
                if (ImGui::MenuItem("Quadratic", nullptr, key->interpolation_mode == TIMELINE_INTERP_QUADRATIC))
                    ui_timeline_set_key_interpolation(*key, TIMELINE_INTERP_QUADRATIC);
                if (ImGui::MenuItem("Cubic", nullptr, key->interpolation_mode == TIMELINE_INTERP_CUBIC))
                    ui_timeline_set_key_interpolation(*key, TIMELINE_INTERP_CUBIC);
                ImGui::EndMenu();
            }
            if (key->interpolation_mode == TIMELINE_INTERP_CUBIC) {
                float tangent = clampf(key->tangent_scale, 0.0f, 4.0f);
                ImGui::SetNextItemWidth(ui_px(140.0f));
                if (ImGui::SliderFloat("Tangent", &tangent, 0.0f, 3.0f, "%.2f")) {
                    key->tangent_scale = clampf(tangent, 0.0f, 4.0f);
                    app_request_scene_render();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Cubic tangent scale. 0 is flat, 1 is automatic, higher values overshoot more.");
            }
        }
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
        if (hovered && key) {
            if (key->interpolation_mode == TIMELINE_INTERP_CUBIC) {
                ImGui::SetTooltip("%s, tangent %.2f",
                                  ui_timeline_interpolation_label(key->interpolation_mode),
                                  key->tangent_scale);
            } else {
                ImGui::SetTooltip("%s", ui_timeline_interpolation_label(key->interpolation_mode));
            }
        }
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
    static bool s_timeline_was_open = false;
    if (!s_timeline_window_open) {
        s_timeline_was_open = false;
        s_timeline_keyboard_focus = false;
        return;
    }
    timeline_delete_invalid_user_var_tracks();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowSize(ImVec2(ui_px(920.0f), ui_px(420.0f)), ImGuiCond_FirstUseEver);
    bool focus_on_open = !s_timeline_was_open;
    if (focus_on_open)
        ImGui::SetNextWindowFocus();
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoTitleBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_panel_bg(UI_PANEL_DEFAULT));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    bool timeline_window_open = ImGui::Begin("Timeline", nullptr, window_flags);
    s_timeline_was_open = true;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (!timeline_window_open) {
        ImGui::End();
        return;
    }

    char detail[160] = {};
    snprintf(detail, sizeof(detail), "%s  timeline %d / %d  frame %d / %d  %.2fs",
             timeline_enabled() ? "active" : "disabled",
             timeline_current_index() + 1, timeline_count(),
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
    bool focused = ui_update_panel_focus_from_current_window(false, true);
    ui_push_panel_focus(focused);
    if (focus_on_open) {
        ui_focus_current_panel_window();
        focused = true;
    }
    if (focused)
        ui_draw_panel_focus_bg(UI_PANEL_DEFAULT);
    bool close_clicked = false;
    ui_panel_header("TIMELINE", detail, "Close##timeline_close", &close_clicked);
    bool panel_interactive = ui_current_panel_focused();
    ui_pop_panel_focus();
    if (close_clicked) {
        s_timeline_window_open = false;
        s_timeline_keyboard_focus = false;
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (s_panel_tone_count > 0)
            s_panel_tone_count--;
        ImGui::End();
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    bool track_enabled = timeline_enabled();
    bool timeline_focused = panel_interactive && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    bool timeline_hovered = panel_interactive && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
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
    ImGui::SameLine(0.0f, ui_margin_px(12.0f));

    int current_timeline = timeline_current_index();
    const char* timeline_preview = timeline_name(current_timeline);
    ImGui::SetNextItemWidth(ui_px(136.0f));
    if (ImGui::BeginCombo("##timeline_selector", timeline_preview && timeline_preview[0] ? timeline_preview : "Timeline")) {
        for (int i = 0; i < timeline_count(); i++) {
            char item_label[128] = {};
            snprintf(item_label, sizeof(item_label), "%s%s",
                     timeline_timeline_enabled(i) ? "" : "[off] ",
                     timeline_name(i));
            bool selected = i == current_timeline;
            if (ImGui::Selectable(item_label, selected)) {
                if (timeline_set_current_index(i)) {
                    s_timeline_slot_selection.valid = false;
                    s_timeline_slot_selection_timeline = -1;
                    s_timeline_visible_first_frame = 0;
                    s_timeline_ensure_current_visible = true;
                }
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, ui_margin_px(4.0f));
    if (ImGui::Button("+##timeline_add", ImVec2(ui_px(26.0f), 0.0f))) {
        if (timeline_add(nullptr) >= 0) {
            s_timeline_slot_selection.valid = false;
            s_timeline_slot_selection_timeline = -1;
            s_timeline_visible_first_frame = 0;
            s_timeline_ensure_current_visible = true;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create timeline");
    ImGui::SameLine(0.0f, ui_margin_px(4.0f));
    bool can_delete_timeline = timeline_count() > 1;
    if (!can_delete_timeline)
        ImGui::BeginDisabled();
    if (ImGui::Button("-##timeline_delete", ImVec2(ui_px(26.0f), 0.0f))) {
        if (timeline_delete(timeline_current_index())) {
            s_timeline_slot_selection.valid = false;
            s_timeline_slot_selection_timeline = -1;
            s_timeline_visible_first_frame = 0;
            s_timeline_ensure_current_visible = true;
        }
    }
    if (!can_delete_timeline)
        ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(can_delete_timeline ? "Delete selected timeline" : "At least one timeline is always kept");
    ImGui::SameLine(0.0f, ui_margin_px(8.0f));
    bool selected_timeline_enabled = timeline_timeline_enabled(timeline_current_index());
    bool can_toggle_selected_timeline = selected_timeline_enabled ? (timeline_enabled_count() > 1) : true;
    if (!can_toggle_selected_timeline)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Enabled##timeline_clip_enabled", &selected_timeline_enabled))
        timeline_set_timeline_enabled(timeline_current_index(), selected_timeline_enabled);
    if (!can_toggle_selected_timeline)
        ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(can_toggle_selected_timeline ? "Skip this timeline during playback/export when disabled" : "At least one timeline must stay enabled");

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
    ImGui::SameLine(0.0f, ui_margin_px(18.0f));
    ImGui::SetNextItemWidth(ui_px(112.0f));
    ImGui::SliderFloat("zoom", &s_timeline_slot_zoom, 0.55f, 1.60f, "%.2f");
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
        int frame_context_request = -1;
        for (int f = visible_first; f < visible_last; f++) {
            int frame_index = f - visible_first;
            ImVec2 slot_min = ImVec2(grid_min.x + (float)frame_index * col_w, grid_min.y);
            ImVec2 slot_max = ImVec2(slot_min.x + slot_size.x, slot_min.y + ruler_h);
            ImGui::PushID(f);
            ImGui::SetCursorScreenPos(slot_min);
            ImGui::InvisibleButton("##frame_header", ImVec2(slot_size.x, ruler_h));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                timeline_set_current_frame(f);
                s_timeline_slot_selection.valid = false;
                s_timeline_slot_selection_timeline = -1;
                s_timeline_ensure_current_visible = true;
            }
            if (ImGui::IsMouseHoveringRect(slot_min, slot_max) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                frame_context_request = f;
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
        if (frame_context_request >= 0) {
            s_timeline_frame_context_frame = frame_context_request;
            ImGui::OpenPopup("##timeline_frame_context_menu");
        }
        if (ImGui::BeginPopup("##timeline_frame_context_menu")) {
            char length_label[64] = {};
            snprintf(length_label, sizeof(length_label), "Set last frame to %d", s_timeline_frame_context_frame);
            if (ImGui::MenuItem(length_label))
                pending_length_frames = s_timeline_frame_context_frame + 1;
            ImGui::EndPopup();
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


// -- floating render graph ---------------------------------------------------

enum UiRenderGraphUseFlags {
    UI_RENDER_GRAPH_READ  = 1 << 0,
    UI_RENDER_GRAPH_WRITE = 1 << 1
};

static const int UI_RENDER_GRAPH_MAX_LANES = 128;

struct UiRenderGraphBuild {
    CmdHandle cmd_handles[MAX_COMMANDS];
    int       cmd_depths[MAX_COMMANDS];
    bool      cmd_effective_enabled[MAX_COMMANDS];
    int       cmd_count;

    ResHandle resource_handles[UI_RENDER_GRAPH_MAX_LANES];
    unsigned char usage[UI_RENDER_GRAPH_MAX_LANES][MAX_COMMANDS];
    int       resource_count;
};

struct UiRenderGraphCache {
    UiRenderGraphBuild graph;
    uint64_t cmd_revision;
    uint64_t cmd_graph_revision;
    uint64_t res_revision;
    bool valid;
};

static UiRenderGraphCache s_render_graph_build_cache = {};

static bool ui_render_graph_command_effectively_enabled(CmdHandle h) {
    int guard = 0;
    while (h != INVALID_HANDLE && guard++ < MAX_COMMANDS) {
        Command* c = cmd_get(h);
        if (!c || !c->active || !c->enabled)
            return false;
        h = c->parent;
    }
    return true;
}

static void ui_render_graph_collect_commands_recursive(CmdHandle parent, int depth, UiRenderGraphBuild* graph) {
    if (!graph || graph->cmd_count >= MAX_COMMANDS)
        return;

    for (int i = 0; i < MAX_COMMANDS && graph->cmd_count < MAX_COMMANDS; i++) {
        Command& c = g_commands[i];
        if (!c.active || c.parent != parent)
            continue;

        CmdHandle h = (CmdHandle)(i + 1);
        int idx = graph->cmd_count++;
        graph->cmd_handles[idx] = h;
        graph->cmd_depths[idx] = depth;
        graph->cmd_effective_enabled[idx] = ui_render_graph_command_effectively_enabled(h);

        if (c.type == CMD_GROUP || c.type == CMD_REPEAT)
            ui_render_graph_collect_commands_recursive(h, depth + 1, graph);
    }
}

static bool ui_render_graph_resource_is_traceable(const Resource& r) {
    switch (r.type) {
    case RES_TEXTURE2D:
    case RES_RENDER_TEXTURE2D:
    case RES_RENDER_TEXTURE3D:
    case RES_STRUCTURED_BUFFER:
    case RES_GAUSSIAN_SPLAT:
    case RES_MESH:
    case RES_BUILTIN_SCENE_COLOR:
    case RES_BUILTIN_SCENE_DEPTH:
    case RES_BUILTIN_SHADOW_MAP:
        return true;
    default:
        return false;
    }
}

static int ui_render_graph_find_lane(UiRenderGraphBuild* graph, ResHandle h) {
    if (!graph || h == INVALID_HANDLE)
        return -1;
    for (int i = 0; i < graph->resource_count; i++) {
        if (graph->resource_handles[i] == h)
            return i;
    }
    return -1;
}

static int ui_render_graph_add_lane(UiRenderGraphBuild* graph, ResHandle h) {
    if (!graph || h == INVALID_HANDLE)
        return -1;

    Resource* r = res_get(h);
    if (!r || !ui_render_graph_resource_is_traceable(*r))
        return -1;

    int lane = ui_render_graph_find_lane(graph, h);
    if (lane >= 0)
        return lane;
    if (graph->resource_count >= UI_RENDER_GRAPH_MAX_LANES)
        return -1;

    lane = graph->resource_count++;
    graph->resource_handles[lane] = h;
    for (int c = 0; c < MAX_COMMANDS; c++)
        graph->usage[lane][c] = 0;
    return lane;
}

static void ui_render_graph_add_usage(UiRenderGraphBuild* graph, int cmd_index, ResHandle h, unsigned int flags) {
    if (!graph || cmd_index < 0 || cmd_index >= graph->cmd_count || h == INVALID_HANDLE || flags == 0)
        return;

    int lane = ui_render_graph_add_lane(graph, h);
    if (lane < 0)
        return;
    graph->usage[lane][cmd_index] |= (unsigned char)(flags & (UI_RENDER_GRAPH_READ | UI_RENDER_GRAPH_WRITE));
}

static void ui_render_graph_add_source_name_usage(UiRenderGraphBuild* graph, int cmd_index, const char* source_name) {
    if (!source_name || !source_name[0])
        return;
    ResHandle h = res_find_by_name(source_name);
    if (h != INVALID_HANDLE)
        ui_render_graph_add_usage(graph, cmd_index, h, UI_RENDER_GRAPH_READ);
}

static void ui_render_graph_add_command_usages(UiRenderGraphBuild* graph, int cmd_index, Command& c) {
    if (!graph)
        return;

    // Shader resources and explicit command inputs.
    for (int s = 0; s < c.srv_count; s++)
        ui_render_graph_add_usage(graph, cmd_index, c.srv_handles[s], UI_RENDER_GRAPH_READ);
    for (int t = 0; t < c.tex_count; t++)
        ui_render_graph_add_usage(graph, cmd_index, c.tex_handles[t], UI_RENDER_GRAPH_READ);
    for (int p = 0; p < c.param_count; p++) {
        CommandParam& param = c.params[p];
        UserCBSourceKind source_kind = param.source_kind;
        if (source_kind == USER_CB_SOURCE_NONE && param.source != INVALID_HANDLE)
            source_kind = USER_CB_SOURCE_RESOURCE;
        if (param.enabled && source_kind == USER_CB_SOURCE_RESOURCE)
            ui_render_graph_add_usage(graph, cmd_index, param.source, UI_RENDER_GRAPH_READ);
    }

    switch (c.type) {
    case CMD_CLEAR: {
        if (c.clear_color_enabled)
            ui_render_graph_add_usage(graph, cmd_index, c.rt, UI_RENDER_GRAPH_WRITE);
        if (c.clear_depth)
            ui_render_graph_add_usage(graph, cmd_index, c.depth, UI_RENDER_GRAPH_WRITE);
        ui_render_graph_add_source_name_usage(graph, cmd_index, c.clear_color_source);
        ui_render_graph_add_source_name_usage(graph, cmd_index, c.clear_depth_source);
        break;
    }

    case CMD_DRAW_MESH:
    case CMD_DRAW_INSTANCED:
    case CMD_INDIRECT_DRAW: {
        if (c.draw_source == DRAW_SOURCE_MESH)
            ui_render_graph_add_usage(graph, cmd_index, c.mesh, UI_RENDER_GRAPH_READ);

        if (c.color_write) {
            ui_render_graph_add_usage(graph, cmd_index, c.rt, UI_RENDER_GRAPH_WRITE);
            for (int rt_i = 0; rt_i < c.mrt_count; rt_i++)
                ui_render_graph_add_usage(graph, cmd_index, c.mrt_handles[rt_i], UI_RENDER_GRAPH_WRITE);
        }

        unsigned int depth_flags = 0;
        if (c.depth_test)  depth_flags |= UI_RENDER_GRAPH_READ;
        if (c.depth_write) depth_flags |= UI_RENDER_GRAPH_WRITE;
        ui_render_graph_add_usage(graph, cmd_index, c.depth, depth_flags);

        for (int u = 0; u < c.uav_count; u++)
            ui_render_graph_add_usage(graph, cmd_index, c.uav_handles[u], UI_RENDER_GRAPH_READ | UI_RENDER_GRAPH_WRITE);

        if (c.type == CMD_INDIRECT_DRAW)
            ui_render_graph_add_usage(graph, cmd_index, c.indirect_buf, UI_RENDER_GRAPH_READ);
        if (c.shadow_cast)
            ui_render_graph_add_usage(graph, cmd_index, g_builtin_shadow_map, UI_RENDER_GRAPH_WRITE);
        if (c.shadow_receive)
            ui_render_graph_add_usage(graph, cmd_index, g_builtin_shadow_map, UI_RENDER_GRAPH_READ);
        break;
    }

    case CMD_DISPATCH:
    case CMD_INDIRECT_DISPATCH: {
        for (int u = 0; u < c.uav_count; u++)
            ui_render_graph_add_usage(graph, cmd_index, c.uav_handles[u], UI_RENDER_GRAPH_READ | UI_RENDER_GRAPH_WRITE);
        ui_render_graph_add_usage(graph, cmd_index, c.dispatch_size_source, UI_RENDER_GRAPH_READ);
        if (c.type == CMD_INDIRECT_DISPATCH)
            ui_render_graph_add_usage(graph, cmd_index, c.indirect_buf, UI_RENDER_GRAPH_READ);
        break;
    }

    case CMD_GROUP:
    case CMD_REPEAT:
    default:
        break;
    }
}

static void ui_render_graph_build(UiRenderGraphBuild* graph) {
    if (!graph)
        return;
    memset(graph, 0, sizeof(*graph));
    ui_render_graph_collect_commands_recursive(INVALID_HANDLE, 0, graph);

    for (int i = 0; i < graph->cmd_count; i++) {
        Command* c = cmd_get(graph->cmd_handles[i]);
        if (c)
            ui_render_graph_add_command_usages(graph, i, *c);
    }
}

static const UiRenderGraphBuild* ui_render_graph_cached_build() {
    uint64_t cr = cmd_revision();
    uint64_t cgr = cmd_graph_revision();
    uint64_t rr = res_revision();
    if (!s_render_graph_build_cache.valid ||
        s_render_graph_build_cache.cmd_revision != cr ||
        s_render_graph_build_cache.cmd_graph_revision != cgr ||
        s_render_graph_build_cache.res_revision != rr) {
        ui_render_graph_build(&s_render_graph_build_cache.graph);
        s_render_graph_build_cache.cmd_revision = cr;
        s_render_graph_build_cache.cmd_graph_revision = cgr;
        s_render_graph_build_cache.res_revision = rr;
        s_render_graph_build_cache.valid = true;
    }
    return &s_render_graph_build_cache.graph;
}

static const char* ui_render_graph_usage_label(unsigned int flags) {
    if ((flags & UI_RENDER_GRAPH_READ) && (flags & UI_RENDER_GRAPH_WRITE)) return "R/W";
    if (flags & UI_RENDER_GRAPH_WRITE) return "Write";
    if (flags & UI_RENDER_GRAPH_READ) return "Read";
    return "";
}

static ImVec4 ui_render_graph_usage_color(unsigned int flags) {
    if ((flags & UI_RENDER_GRAPH_READ) && (flags & UI_RENDER_GRAPH_WRITE)) return ImVec4(0.30f, 0.62f, 0.36f, 1.0f);
    if (flags & UI_RENDER_GRAPH_WRITE) return ImVec4(0.84f, 0.43f, 0.17f, 1.0f);
    if (flags & UI_RENDER_GRAPH_READ) return ImVec4(0.30f, 0.48f, 0.78f, 1.0f);
    return ImVec4(0.42f, 0.42f, 0.46f, 1.0f);
}

static ImVec4 ui_render_graph_resource_color(const Resource& r) {
    switch (r.type) {
    case RES_BUILTIN_SCENE_COLOR:
    case RES_RENDER_TEXTURE2D:
    case RES_RENDER_TEXTURE3D:
        return ImVec4(0.25f, 0.48f, 0.82f, 1.0f);
    case RES_BUILTIN_SCENE_DEPTH:
    case RES_BUILTIN_SHADOW_MAP:
        return ImVec4(0.30f, 0.63f, 0.32f, 1.0f);
    case RES_STRUCTURED_BUFFER:
        return ImVec4(0.54f, 0.34f, 0.80f, 1.0f);
    case RES_TEXTURE2D:
        return ImVec4(0.76f, 0.58f, 0.24f, 1.0f);
    case RES_MESH:
    case RES_GAUSSIAN_SPLAT:
        return ImVec4(0.74f, 0.50f, 0.32f, 1.0f);
    default:
        return ImVec4(0.50f, 0.50f, 0.54f, 1.0f);
    }
}

static void ui_render_graph_fit_label(const char* text, float max_w, char* out, int out_sz) {
    if (!out || out_sz <= 0)
        return;
    ui_fit_text_ellipsis(text ? text : "", max_w, out, out_sz);
}


static int ui_render_graph_subtree_end(const UiRenderGraphBuild* graph, int cmd_index) {
    if (!graph || cmd_index < 0 || cmd_index >= graph->cmd_count)
        return cmd_index + 1;
    int depth = graph->cmd_depths[cmd_index];
    int end = cmd_index + 1;
    while (end < graph->cmd_count && graph->cmd_depths[end] > depth)
        end++;
    return end;
}

static ImVec2 ui_render_graph_cmd_min(ImVec2 origin, float left_pad, float top, float cmd_w,
                                      float cmd_h, float cmd_gap, float depth_gap,
                                      int cmd_index, int depth) {
    (void)cmd_h;
    return ImVec2(origin.x + left_pad + (float)cmd_index * (cmd_w + cmd_gap),
                  origin.y + top + (float)depth * (cmd_h + depth_gap));
}

static ImVec2 ui_render_graph_res_min(ImVec2 origin, float left_pad, float resource_top,
                                      float cmd_w, float cmd_gap, float res_node_w,
                                      float res_lane_h, float res_node_h,
                                      int cmd_index, int lane) {
    float cx = origin.x + left_pad + (float)cmd_index * (cmd_w + cmd_gap) + cmd_w * 0.5f;
    (void)res_node_h;
    return ImVec2(cx - res_node_w * 0.5f,
                  origin.y + resource_top + (float)lane * res_lane_h);
}

static ImVec2 ui_render_graph_v2_add(ImVec2 a, ImVec2 b) {
    return ImVec2(a.x + b.x, a.y + b.y);
}

static ImVec2 ui_render_graph_v2_sub(ImVec2 a, ImVec2 b) {
    return ImVec2(a.x - b.x, a.y - b.y);
}

static ImVec2 ui_render_graph_v2_scale(ImVec2 a, float s) {
    return ImVec2(a.x * s, a.y * s);
}

static ImVec2 ui_render_graph_v2_norm(ImVec2 v) {
    float len = sqrtf(v.x * v.x + v.y * v.y);
    if (len <= 0.0001f)
        return ImVec2(1.0f, 0.0f);
    return ImVec2(v.x / len, v.y / len);
}

static void ui_render_graph_draw_arrow_head(ImDrawList* dl, ImVec2 tip, ImVec2 dir, ImU32 col, float size) {
    dir = ui_render_graph_v2_norm(dir);
    ImVec2 n(-dir.y, dir.x);
    ImVec2 base = ui_render_graph_v2_sub(tip, ui_render_graph_v2_scale(dir, size));
    dl->AddTriangleFilled(tip,
                          ui_render_graph_v2_add(base, ui_render_graph_v2_scale(n, size * 0.55f)),
                          ui_render_graph_v2_sub(base, ui_render_graph_v2_scale(n, size * 0.55f)),
                          col);
}

static void ui_render_graph_draw_ortho_arrow(ImDrawList* dl, ImVec2 from, ImVec2 to,
                                             ImU32 col, float thickness, float scale,
                                             float lane_bias = 0.0f) {
    if (!dl)
        return;

    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float r = 10.0f * scale;
    if (fabsf(dx) < 2.0f * scale || fabsf(dy) < 2.0f * scale) {
        dl->AddLine(from, to, col, thickness);
        ui_render_graph_draw_arrow_head(dl, to, ui_render_graph_v2_sub(to, from), col, 7.0f * scale);
        return;
    }

    bool horizontal_first = fabsf(dx) >= fabsf(dy);
    ImVec2 mid = horizontal_first ?
        ImVec2(from.x + dx * 0.50f + lane_bias, from.y) :
        ImVec2(from.x, from.y + dy * 0.50f + lane_bias);
    ImVec2 corner_b = horizontal_first ? ImVec2(mid.x, to.y) : ImVec2(to.x, mid.y);
    ImVec2 dir_a = ui_render_graph_v2_norm(ui_render_graph_v2_sub(mid, from));
    ImVec2 dir_b = ui_render_graph_v2_norm(ui_render_graph_v2_sub(corner_b, mid));
    ImVec2 dir_c = ui_render_graph_v2_norm(ui_render_graph_v2_sub(to, corner_b));

    ImVec2 p0 = from;
    ImVec2 p1 = ui_render_graph_v2_sub(mid, ui_render_graph_v2_scale(dir_a, r));
    ImVec2 p2 = ui_render_graph_v2_add(mid, ui_render_graph_v2_scale(dir_b, r));
    ImVec2 p3 = ui_render_graph_v2_sub(corner_b, ui_render_graph_v2_scale(dir_b, r));
    ImVec2 p4 = ui_render_graph_v2_add(corner_b, ui_render_graph_v2_scale(dir_c, r));

    dl->PathClear();
    dl->PathLineTo(p0);
    dl->PathLineTo(p1);
    dl->PathBezierQuadraticCurveTo(mid, p2, 8);
    dl->PathLineTo(p3);
    dl->PathBezierQuadraticCurveTo(corner_b, p4, 8);
    dl->PathLineTo(to);
    dl->PathStroke(col, 0, thickness);
    ui_render_graph_draw_arrow_head(dl, to, dir_c, col, 7.0f * scale);
}

static void ui_render_graph_draw_grid_layer(ImDrawList* dl, ImVec2 min, ImVec2 max,
                                            float scroll_x, float scroll_y,
                                            float step, ImU32 col) {
    if (!dl)
        return;
    if (step < 2.0f)
        return;
    float start_x = min.x - fmodf(scroll_x, step);
    float start_y = min.y - fmodf(scroll_y, step);
    for (float x = start_x; x < max.x; x += step)
        dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), col, 1.0f);
    for (float y = start_y; y < max.y; y += step)
        dl->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), col, 1.0f);
}

static void ui_render_graph_draw_canvas_grid(ImDrawList* dl, ImVec2 min, ImVec2 max,
                                             float scroll_x, float scroll_y, float scale) {
    if (!dl)
        return;

    float fine_step = 32.0f * scale;
    while (fine_step < 18.0f) fine_step *= 3.0f;
    while (fine_step > 54.0f) fine_step /= 3.0f;
    float coarse_step = fine_step * 3.0f;

    ImU32 fine_col = ImGui::GetColorU32(ImVec4(0.20f, 0.19f, 0.20f, 0.30f));
    ImU32 coarse_col = ImGui::GetColorU32(ImVec4(0.34f, 0.30f, 0.27f, 0.44f));
    ui_render_graph_draw_grid_layer(dl, min, max, scroll_x, scroll_y, fine_step, fine_col);
    ui_render_graph_draw_grid_layer(dl, min, max, scroll_x, scroll_y, coarse_step, coarse_col);
}

static void ui_draw_render_graph_window() {
    static bool s_render_graph_was_open = false;
    if (!s_render_graph_window_open) {
        s_render_graph_was_open = false;
        s_render_graph_center_next = true;
        return;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(vp->ID);
    ImVec2 default_size(ui_px(980.0f), ui_px(520.0f));
    ImGui::SetNextWindowSize(default_size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f - default_size.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y - default_size.y - ui_margin_px(24.0f)),
                            ImGuiCond_FirstUseEver);
    bool focus_on_open = !s_render_graph_was_open;
    if (focus_on_open)
        ImGui::SetNextWindowFocus();

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoTitleBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_panel_bg(UI_PANEL_DEFAULT));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    bool render_graph_window_open = ImGui::Begin("Render Graph", nullptr, window_flags);
    s_render_graph_was_open = true;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (!render_graph_window_open) {
        ImGui::End();
        return;
    }

    const UiRenderGraphBuild& graph = *ui_render_graph_cached_build();

    char detail[128] = {};
    snprintf(detail, sizeof(detail), "%d cmds  %d resources", graph.cmd_count, graph.resource_count);

    if (s_panel_tone_count < (int)(sizeof(s_panel_tone_stack) / sizeof(s_panel_tone_stack[0])))
        s_panel_tone_stack[s_panel_tone_count++] = UI_PANEL_DEFAULT;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_panel_bg(UI_PANEL_DEFAULT));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.205f, 0.200f, 1.0f));
    float panel_child_w = ImGui::GetContentRegionAvail().x;
    float panel_content_w = panel_child_w - ui_margin_px(16.0f);
    if (panel_content_w < ui_px(1.0f))
        panel_content_w = ui_px(1.0f);
    ImGui::SetNextWindowContentSize(ImVec2(panel_content_w, 0.0f));
    if (!ImGui::BeginChild("##render_graph_panel", ImGui::GetContentRegionAvail(), true)) {
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (s_panel_tone_count > 0)
            s_panel_tone_count--;
        ImGui::End();
        return;
    }

    bool focused = ui_update_panel_focus_from_current_window(false, true);
    ui_push_panel_focus(focused);
    if (focus_on_open) {
        ui_focus_current_panel_window();
        focused = true;
    }
    if (focused)
        ui_draw_panel_focus_bg(UI_PANEL_DEFAULT);
    bool close_clicked = false;
    ui_panel_header("RENDER GRAPH", detail, "Close##render_graph_close", &close_clicked);
    bool graph_interactive = ui_current_panel_focused();
    ui_pop_panel_focus();
    if (close_clicked) {
        s_render_graph_window_open = false;
        s_render_graph_center_next = true;
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (s_panel_tone_count > 0)
            s_panel_tone_count--;
        ImGui::End();
        return;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Commands are shown in execution order. Mouse wheel zooms, right-drag pans, double middle-click resets.");
    ImGui::SameLine();
    ImGui::TextDisabled("Zoom %.0f%%", s_render_graph_zoom * 100.0f);
    ImGui::Spacing();

    if (graph.cmd_count == 0) {
        ImGui::TextDisabled("No commands in the current project.");
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (s_panel_tone_count > 0)
            s_panel_tone_count--;
        ImGui::End();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.048f, 0.047f, 0.050f, 1.0f));
    ImGui::BeginChild("##render_graph_canvas", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImGuiIO& io = ImGui::GetIO();
    bool canvas_hovered = graph_interactive && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    if (!graph_interactive || !ImGui::IsMouseDown(ImGuiMouseButton_Right))
        s_render_graph_mouse_dragging = false;
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        s_render_graph_mouse_dragging = true;
    if (graph_interactive && (canvas_hovered || s_render_graph_mouse_dragging)) {
        g_editor_mouse_capture = true;
    }
    ImVec2 canvas_pos = ImGui::GetWindowPos();
    ImVec2 canvas_size = ImGui::GetWindowSize();
    ImVec2 mouse_local(io.MousePos.x - canvas_pos.x, io.MousePos.y - canvas_pos.y);
    float zoom_before = s_render_graph_zoom;
    if (canvas_hovered && io.MouseWheel != 0.0f) {
        ImVec2 graph_under_mouse(
            (mouse_local.x - s_render_graph_pan.x) / zoom_before,
            (mouse_local.y - s_render_graph_pan.y) / zoom_before);
        float zoom_next = s_render_graph_zoom * powf(1.12f, io.MouseWheel);
        if (zoom_next < 0.35f) zoom_next = 0.35f;
        if (zoom_next > 2.50f) zoom_next = 2.50f;
        if (fabsf(zoom_next - s_render_graph_zoom) > 0.0001f) {
            s_render_graph_zoom = zoom_next;
            s_render_graph_pan.x = mouse_local.x - graph_under_mouse.x * s_render_graph_zoom;
            s_render_graph_pan.y = mouse_local.y - graph_under_mouse.y * s_render_graph_zoom;
        }
    }
    if (graph_interactive && s_render_graph_mouse_dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        s_render_graph_pan.x += io.MouseDelta.x;
        s_render_graph_pan.y += io.MouseDelta.y;
    }
    if (canvas_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
        s_render_graph_zoom = 1.0f;
        s_render_graph_center_next = true;
    }
    if (s_render_graph_zoom < 0.35f) s_render_graph_zoom = 0.35f;
    if (s_render_graph_zoom > 2.50f) s_render_graph_zoom = 2.50f;
    ImGui::SetWindowFontScale(s_render_graph_zoom);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float scale = ui_global_scale() * s_render_graph_zoom;
    ImVec2 origin = ImVec2(canvas_pos.x + s_render_graph_pan.x,
                           canvas_pos.y + s_render_graph_pan.y);
    float left_pad = 42.0f * scale;
    float top = 28.0f * scale;
    float cmd_w = 166.0f * scale;
    float cmd_h = 58.0f * scale;
    float cmd_gap = 78.0f * scale;
    float depth_gap = 30.0f * scale;
    float res_node_w = 176.0f * scale;
    float res_node_h = 34.0f * scale;
    float res_lane_h = 50.0f * scale;
    float group_pad = 14.0f * scale;
    float group_header_h = 46.0f * scale;

    int subtree_end[MAX_COMMANDS] = {};
    int max_depth = 0;
    for (int ci = 0; ci < graph.cmd_count; ci++) {
        subtree_end[ci] = ui_render_graph_subtree_end(&graph, ci);
        if (graph.cmd_depths[ci] > max_depth)
            max_depth = graph.cmd_depths[ci];
    }

    ImVec2 cmd_node_min[MAX_COMMANDS] = {};
    ImVec2 cmd_node_max[MAX_COMMANDS] = {};
    ImVec2 group_box_min[MAX_COMMANDS] = {};
    ImVec2 group_box_max[MAX_COMMANDS] = {};
    bool has_group_box[MAX_COMMANDS] = {};
    int group_desc_count[MAX_COMMANDS] = {};
    float command_right = left_pad;
    float command_bottom = top;

    for (int ci = 0; ci < graph.cmd_count; ci++) {
        Command* c = cmd_get(graph.cmd_handles[ci]);
        if (!c)
            continue;

        ImVec2 base0 = ui_render_graph_cmd_min(origin, left_pad, top, cmd_w, cmd_h, cmd_gap, depth_gap, ci, graph.cmd_depths[ci]);
        ImVec2 base1(base0.x + cmd_w, base0.y + cmd_h);
        cmd_node_min[ci] = base0;
        cmd_node_max[ci] = base1;
        command_right = ImMax(command_right, base1.x - origin.x);
        command_bottom = ImMax(command_bottom, base1.y - origin.y);

        if (c->type != CMD_GROUP && c->type != CMD_REPEAT)
            continue;

        int last = subtree_end[ci] - 1;
        int deepest = graph.cmd_depths[ci];
        for (int j = ci + 1; j <= last; j++) {
            if (graph.cmd_depths[j] > deepest)
                deepest = graph.cmd_depths[j];
        }

        ImVec2 last0 = base0;
        ImVec2 last1 = base1;
        if (last >= ci) {
            last0 = ui_render_graph_cmd_min(origin, left_pad, top, cmd_w, cmd_h, cmd_gap, depth_gap, last, graph.cmd_depths[last]);
            last1 = ImVec2(last0.x + cmd_w, last0.y + cmd_h);
        }

        ImVec2 outer0(base0.x - group_pad, base0.y - group_pad);
        ImVec2 outer1(last1.x + group_pad, last1.y + group_pad);
        float min_outer_bottom = outer0.y + group_header_h + 10.0f * scale;
        if (outer1.y < min_outer_bottom)
            outer1.y = min_outer_bottom;

        has_group_box[ci] = true;
        group_box_min[ci] = outer0;
        group_box_max[ci] = outer1;
        cmd_node_min[ci] = ImVec2(outer0.x + 8.0f * scale, outer0.y + 8.0f * scale);
        cmd_node_max[ci] = ImVec2(outer1.x - 8.0f * scale, outer0.y + 8.0f * scale + group_header_h);
        group_desc_count[ci] = subtree_end[ci] - ci - 1;

        command_right = ImMax(command_right, outer1.x - origin.x);
        command_bottom = ImMax(command_bottom, outer1.y - origin.y);
    }

    float resource_top = command_bottom + 50.0f * scale;
    float resource_area_h = graph.resource_count > 0 ? (float)graph.resource_count * res_lane_h + 34.0f * scale : 70.0f * scale;
    float graph_w = ImMax(left_pad + (float)graph.cmd_count * (cmd_w + cmd_gap) + 60.0f * scale,
                          command_right + 60.0f * scale);
    float graph_h = resource_top + resource_area_h;

    if (s_render_graph_center_next) {
        ImVec2 old_pan = s_render_graph_pan;
        s_render_graph_pan.x = floorf((canvas_size.x - graph_w) * 0.5f);
        s_render_graph_pan.y = floorf((canvas_size.y - graph_h) * 0.5f);
        ImVec2 delta(s_render_graph_pan.x - old_pan.x, s_render_graph_pan.y - old_pan.y);
        origin.x += delta.x;
        origin.y += delta.y;
        for (int i = 0; i < MAX_COMMANDS; i++) {
            cmd_node_min[i].x += delta.x; cmd_node_min[i].y += delta.y;
            cmd_node_max[i].x += delta.x; cmd_node_max[i].y += delta.y;
            group_box_min[i].x += delta.x; group_box_min[i].y += delta.y;
            group_box_max[i].x += delta.x; group_box_max[i].y += delta.y;
        }
        s_render_graph_center_next = false;
    }

    const ImU32 bg_col = ImGui::GetColorU32(ImVec4(0.058f, 0.056f, 0.060f, 1.0f));
    const ImU32 seq_col = ImGui::GetColorU32(ImVec4(0.72f, 0.47f, 0.31f, 0.42f));
    const ImU32 cmd_bg_col = ImGui::GetColorU32(ImVec4(0.105f, 0.100f, 0.105f, 1.0f));
    const ImU32 cmd_bg_disabled_col = ImGui::GetColorU32(ImVec4(0.075f, 0.073f, 0.077f, 1.0f));
    const ImU32 cmd_border_col = ImGui::GetColorU32(ImVec4(0.38f, 0.32f, 0.29f, 1.0f));
    const ImU32 cmd_selected_col = ImGui::GetColorU32(ImVec4(0.95f, 0.44f, 0.13f, 1.0f));
    const ImU32 group_fill_col = ImGui::GetColorU32(ImVec4(0.90f, 0.42f, 0.13f, 0.075f));
    const ImU32 group_fill_head_col = ImGui::GetColorU32(ImVec4(0.90f, 0.42f, 0.13f, 0.16f));
    const ImU32 group_border_col = ImGui::GetColorU32(ImVec4(0.95f, 0.50f, 0.18f, 0.34f));
    const ImU32 repeat_fill_col = ImGui::GetColorU32(ImVec4(0.50f, 0.34f, 0.82f, 0.095f));
    const ImU32 repeat_fill_head_col = ImGui::GetColorU32(ImVec4(0.50f, 0.34f, 0.82f, 0.18f));
    const ImU32 repeat_border_col = ImGui::GetColorU32(ImVec4(0.64f, 0.46f, 0.95f, 0.42f));
    const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 text_disabled_col = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    ImVec2 canvas_min = ImGui::GetWindowPos();
    ImVec2 canvas_max(canvas_min.x + ImGui::GetWindowWidth(), canvas_min.y + ImGui::GetWindowHeight());
    dl->AddRectFilled(canvas_min, canvas_max, bg_col);
    ui_render_graph_draw_canvas_grid(dl, canvas_min, canvas_max, -s_render_graph_pan.x, -s_render_graph_pan.y, scale);

    if (graph.resource_count > 0) {
        dl->AddText(ImVec2(origin.x + 16.0f * scale, origin.y + resource_top - 30.0f * scale),
                    text_disabled_col, "Resources");
    } else {
        dl->AddText(ImVec2(origin.x + 16.0f * scale, origin.y + resource_top),
                    text_disabled_col, "No traceable render resources are currently referenced by the command list.");
    }

    // Group/repeat containers act as the parent nodes.
    for (int ci = 0; ci < graph.cmd_count; ci++) {
        Command* c = cmd_get(graph.cmd_handles[ci]);
        if (!c || !has_group_box[ci])
            continue;

        bool selected = g_sel_cmd == graph.cmd_handles[ci];
        ImU32 fill = c->type == CMD_REPEAT ? repeat_fill_col : group_fill_col;
        ImU32 head_fill = c->type == CMD_REPEAT ? repeat_fill_head_col : group_fill_head_col;
        ImU32 border = selected ? cmd_selected_col : (c->type == CMD_REPEAT ? repeat_border_col : group_border_col);
        ImVec2 outer0 = group_box_min[ci];
        ImVec2 outer1 = group_box_max[ci];
        ImVec2 head0 = cmd_node_min[ci];
        ImVec2 head1 = cmd_node_max[ci];

        dl->AddRectFilled(outer0, outer1, fill, 10.0f * scale);
        dl->AddRect(outer0, outer1, border, 10.0f * scale, 0, selected ? 2.0f * scale : 1.2f * scale);
        dl->AddRectFilled(head0, head1, head_fill, 7.0f * scale);
        dl->AddRect(head0, head1, border, 7.0f * scale, 0, selected ? 1.8f * scale : 1.1f * scale);
        dl->AddRectFilled(ImVec2(head0.x, head0.y), ImVec2(head0.x + 5.0f * scale, head1.y), border, 5.0f * scale);

        char idx_buf[16] = {};
        snprintf(idx_buf, sizeof(idx_buf), "%02d", ci + 1);
        dl->AddText(ImVec2(head0.x + 12.0f * scale, head0.y + 6.0f * scale), text_disabled_col, idx_buf);

        char badge[64] = {};
        if (c->type == CMD_REPEAT)
            snprintf(badge, sizeof(badge), "REPEAT x%d", c->repeat_count < 1 ? 1 : c->repeat_count);
        else
            snprintf(badge, sizeof(badge), "GROUP");
        float badge_w = ImGui::CalcTextSize(badge).x;
        dl->AddText(ImVec2(head1.x - badge_w - 12.0f * scale, head0.y + 6.0f * scale), border, badge);

        char title[MAX_NAME + 32] = {};
        float title_max_w = (head1.x - head0.x) - badge_w - 48.0f * scale;
        if (title_max_w < 20.0f * scale)
            title_max_w = 20.0f * scale;
        ui_render_graph_fit_label(c->name, title_max_w, title, sizeof(title));
        dl->AddText(ImVec2(head0.x + 12.0f * scale, head0.y + 24.0f * scale), text_col, title);

        char meta[64] = {};
        if (group_desc_count[ci] > 0)
            snprintf(meta, sizeof(meta), "%d nested cmd%s", group_desc_count[ci], group_desc_count[ci] == 1 ? "" : "s");
        else
            snprintf(meta, sizeof(meta), "empty");
        float meta_w = ImGui::CalcTextSize(meta).x;
        dl->AddText(ImVec2(head1.x - meta_w - 12.0f * scale, head0.y + 24.0f * scale), text_disabled_col, meta);
    }

    // Execution-order arrows. Groups behave as one node at their parent level;
    // their children connect only to siblings inside the group.
    for (int ci = 0; ci < graph.cmd_count; ci++) {
        Command* c = cmd_get(graph.cmd_handles[ci]);
        if (!c)
            continue;

        int next = has_group_box[ci] ? subtree_end[ci] : ci + 1;
        if (next >= graph.cmd_count)
            continue;

        Command* n = cmd_get(graph.cmd_handles[next]);
        if (!n || n->parent != c->parent || graph.cmd_depths[next] != graph.cmd_depths[ci])
            continue;

        ImVec2 from(cmd_node_max[ci].x, (cmd_node_min[ci].y + cmd_node_max[ci].y) * 0.5f);
        ImVec2 to(cmd_node_min[next].x, (cmd_node_min[next].y + cmd_node_max[next].y) * 0.5f);
        float bias = ((ci & 1) ? -7.0f : 7.0f) * scale;
        ui_render_graph_draw_ortho_arrow(dl, from, to, seq_col, 1.2f * scale, scale, bias);
    }

    // Resource trace arrows and command/resource dependency arrows.
    for (int lane = 0; lane < graph.resource_count; lane++) {
        Resource* r = res_get(graph.resource_handles[lane]);
        if (!r)
            continue;
        ImVec4 res_col_v = ui_render_graph_resource_color(*r);
        ImU32 trace_col = ImGui::GetColorU32(ui_with_alpha(res_col_v, 0.60f));
        ImVec2 prev_center(0.0f, 0.0f);
        ImVec2 prev_max(0.0f, 0.0f);
        bool has_prev = false;
        for (int ci = 0; ci < graph.cmd_count; ci++) {
            unsigned int flags = graph.usage[lane][ci];
            if (!flags)
                continue;

            ImVec2 r0 = ui_render_graph_res_min(origin, left_pad, resource_top, cmd_w, cmd_gap, res_node_w, res_lane_h, res_node_h, ci, lane);
            ImVec2 r1(r0.x + res_node_w, r0.y + res_node_h);
            ImVec2 rc((r0.x + r1.x) * 0.5f, (r0.y + r1.y) * 0.5f);
            ImVec2 c0 = cmd_node_min[ci];
            ImVec2 c1 = cmd_node_max[ci];
            ImVec4 use_col_v = ui_render_graph_usage_color(flags);
            ImU32 use_col = ImGui::GetColorU32(ui_with_alpha(use_col_v, 0.64f));

            if (has_prev) {
                ImVec2 from(prev_max.x, prev_center.y);
                ImVec2 to(r0.x, rc.y);
                ui_render_graph_draw_ortho_arrow(dl, from, to, trace_col, 1.6f * scale, scale,
                                                 ((lane & 1) ? -10.0f : 10.0f) * scale);
            }
            has_prev = true;
            prev_center = rc;
            prev_max = r1;

            if ((flags & UI_RENDER_GRAPH_READ) && !(flags & UI_RENDER_GRAPH_WRITE)) {
                ImVec2 from(rc.x - res_node_w * 0.24f, r0.y);
                ImVec2 to(c0.x + (c1.x - c0.x) * 0.36f, c1.y);
                ui_render_graph_draw_ortho_arrow(dl, from, to, use_col, 1.2f * scale, scale, -7.0f * scale);
            } else {
                ImVec2 from(c0.x + (c1.x - c0.x) * 0.64f, c1.y);
                ImVec2 to(rc.x + res_node_w * 0.24f, r0.y);
                ui_render_graph_draw_ortho_arrow(dl, from, to, use_col, 1.2f * scale, scale, 7.0f * scale);
            }
        }
    }

    // Leaf / regular command nodes. Group and repeat commands use the enclosing box as their node.
    for (int ci = 0; ci < graph.cmd_count; ci++) {
        Command* c = cmd_get(graph.cmd_handles[ci]);
        if (!c || has_group_box[ci])
            continue;

        ImVec2 n0 = cmd_node_min[ci];
        ImVec2 n1 = cmd_node_max[ci];
        bool selected = g_sel_cmd == graph.cmd_handles[ci];
        bool enabled = graph.cmd_effective_enabled[ci];
        ImU32 node_bg = enabled ? cmd_bg_col : cmd_bg_disabled_col;
        ImU32 border = selected ? cmd_selected_col : cmd_border_col;

        dl->AddRectFilled(n0, n1, node_bg, ui_px(7.0f));
        dl->AddRect(n0, n1, border, ui_px(7.0f), 0, selected ? ui_px(2.0f) : ui_px(1.2f));

        char idx_buf[16] = {};
        snprintf(idx_buf, sizeof(idx_buf), "%02d", ci + 1);
        dl->AddText(ImVec2(n0.x + 10.0f * scale, n0.y + 7.0f * scale), enabled ? text_col : text_disabled_col, idx_buf);

        char title[MAX_NAME + 32] = {};
        ui_render_graph_fit_label(c->name, cmd_w - 22.0f * scale, title, sizeof(title));
        dl->AddText(ImVec2(n0.x + 10.0f * scale, n0.y + 28.0f * scale), enabled ? text_col : text_disabled_col, title);

        char type_buf[64] = {};
        snprintf(type_buf, sizeof(type_buf), "%s", cmd_type_str(c->type));
        float type_w = ImGui::CalcTextSize(type_buf).x;
        ImVec4 type_col_v = enabled ? ui_command_type_color(c->type) : ImVec4(0.46f, 0.47f, 0.50f, 1.0f);
        dl->AddText(ImVec2(n1.x - type_w - 10.0f * scale, n0.y + 7.0f * scale),
                    ImGui::GetColorU32(type_col_v), type_buf);
    }

    // Resource nodes.
    for (int lane = 0; lane < graph.resource_count; lane++) {
        Resource* r = res_get(graph.resource_handles[lane]);
        if (!r)
            continue;
        ImVec4 res_col_v = ui_render_graph_resource_color(*r);
        ImU32 res_col = ImGui::GetColorU32(res_col_v);
        for (int ci = 0; ci < graph.cmd_count; ci++) {
            unsigned int flags = graph.usage[lane][ci];
            if (!flags)
                continue;
            ImVec2 node0 = ui_render_graph_res_min(origin, left_pad, resource_top, cmd_w, cmd_gap, res_node_w, res_lane_h, res_node_h, ci, lane);
            ImVec2 node1(node0.x + res_node_w, node0.y + res_node_h);
            ImVec4 use_col_v = ui_render_graph_usage_color(flags);
            ImU32 use_col = ImGui::GetColorU32(use_col_v);
            ImU32 bg = ImGui::GetColorU32(ImVec4(0.082f, 0.080f, 0.086f, 0.98f));
            ImU32 bg_tint = ImGui::GetColorU32(ui_with_alpha(res_col_v, 0.17f));
            ImU32 border = ImGui::GetColorU32(ui_with_alpha(use_col_v, 0.95f));
            dl->AddRectFilled(node0, node1, bg, 6.0f * scale);
            dl->AddRectFilled(node0, node1, bg_tint, 6.0f * scale);
            dl->AddRect(node0, node1, border, 6.0f * scale, 0, 1.1f * scale);
            dl->AddRectFilled(ImVec2(node0.x, node0.y), ImVec2(node0.x + 5.0f * scale, node1.y), res_col, 6.0f * scale);

            char label[MAX_NAME + 32] = {};
            ui_render_graph_fit_label(ui_resource_display_name(*r), res_node_w - 62.0f * scale, label, sizeof(label));
            dl->AddText(ImVec2(node0.x + 12.0f * scale, node0.y + 8.0f * scale), text_col, label);

            const char* use_label = ui_render_graph_usage_label(flags);
            ImVec2 use_sz = ImGui::CalcTextSize(use_label);
            ImVec2 pill0(node1.x - use_sz.x - 18.0f * scale, node0.y + 7.0f * scale);
            ImVec2 pill1(node1.x - 8.0f * scale, node1.y - 7.0f * scale);
            dl->AddRectFilled(pill0, pill1, ImGui::GetColorU32(ui_with_alpha(use_col_v, 0.20f)), 4.0f * scale);
            dl->AddText(ImVec2(pill0.x + 5.0f * scale, pill0.y + floorf((pill1.y - pill0.y - use_sz.y) * 0.5f)), use_col, use_label);
        }
    }

    // Command interactions.
    for (int ci = 0; ci < graph.cmd_count; ci++) {
        Command* c = cmd_get(graph.cmd_handles[ci]);
        if (!c)
            continue;
        ImVec2 n0 = cmd_node_min[ci];
        ImVec2 n1 = cmd_node_max[ci];
        ImGui::PushID(10000 + ci);
        ImGui::SetCursorScreenPos(n0);
        ImGui::InvisibleButton("cmd_node", ImVec2(n1.x - n0.x, n1.y - n0.y));
        if (graph_interactive && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            g_sel_cmd = graph.cmd_handles[ci];
            g_sel_res = INVALID_HANDLE;
            s_cmd_nav = g_sel_cmd;
        }
        if (graph_interactive && ImGui::IsItemHovered()) {
            if (g_profiler_enabled && cmd_profile_ready())
                ImGui::SetTooltip("%02d %s\n%s\nGPU %.3f ms", ci + 1, c->name, cmd_type_str(c->type), cmd_profile_ms(graph.cmd_handles[ci]));
            else
                ImGui::SetTooltip("%02d %s\n%s", ci + 1, c->name, cmd_type_str(c->type));
        }
        ImGui::PopID();
    }

    for (int lane = 0; lane < graph.resource_count; lane++) {
        Resource* r = res_get(graph.resource_handles[lane]);
        if (!r)
            continue;
        for (int ci = 0; ci < graph.cmd_count; ci++) {
            unsigned int flags = graph.usage[lane][ci];
            if (!flags)
                continue;
            Command* c = cmd_get(graph.cmd_handles[ci]);
            if (!c)
                continue;
            ImVec2 node0 = ui_render_graph_res_min(origin, left_pad, resource_top, cmd_w, cmd_gap, res_node_w, res_lane_h, res_node_h, ci, lane);
            ImGui::PushID(30000 + lane * MAX_COMMANDS + ci);
            ImGui::SetCursorScreenPos(node0);
            ImGui::InvisibleButton("resource_node", ImVec2(res_node_w, res_node_h));
            if (graph_interactive && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                g_sel_res = graph.resource_handles[lane];
                g_sel_cmd = INVALID_HANDLE;
                s_res_nav = g_sel_res;
            }
            if (graph_interactive && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s %s\nCommand: %s\n%s", ui_render_graph_usage_label(flags), ui_resource_display_name(*r), c->name, ui_resource_display_type(*r));
            ImGui::PopID();
        }
    }

    ImGui::SetCursorScreenPos(canvas_pos);
    ImGui::Dummy(canvas_size);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (s_panel_tone_count > 0)
        s_panel_tone_count--;
    ImGui::End();
}

static void ui_top_bar_load_progress(float row_y, float row_h) {
    ResourceLoadProgress progress = {};
    if (!res_get_load_progress(&progress) || !progress.active)
        return;

    ImGui::SameLine(0.0f, ui_margin_px(10.0f));
    ui_align_frame_row(row_y);
    const float w = ui_px(132.0f);
    const float h = ImMax(2.0f, ui_px(3.0f));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##resource_load_progress", ImVec2(w, row_h));
    ImVec2 bar_min(p0.x, p0.y + floorf((row_h - h) * 0.5f));
    ImVec2 bar_max(p0.x + w, bar_min.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bar_min, bar_max, IM_COL32(58, 48, 42, 255), h * 0.5f);
    dl->AddRectFilled(bar_min, ImVec2(bar_min.x + w * progress.fraction, bar_max.y),
                      IM_COL32(230, 116, 47, 255), h * 0.5f);
    if (ImGui::IsItemHovered()) {
        int pct = (int)(progress.fraction * 100.0f + 0.5f);
        ImGui::SetTooltip("Loading %s  %d%%\n%s", progress.label, pct, progress.path);
    }
}

static float ui_top_bar_chip_width(const char* text) {
    return ImGui::CalcTextSize(text ? text : "").x + ui_margin_px(14.0f);
}

static void ui_top_bar_chip(ImDrawList* dl, ImVec2 pos, const char* text, float row_h, ImVec4 tint) {
    const char* safe_text = text ? text : "";
    ImVec2 text_sz = ImGui::CalcTextSize(safe_text);
    ImVec2 size(ui_top_bar_chip_width(safe_text), row_h);
    ImVec2 max(pos.x + size.x, pos.y + size.y);
    ImVec4 bg = ImVec4(tint.x * 0.16f + 0.070f, tint.y * 0.16f + 0.066f, tint.z * 0.16f + 0.068f, 1.0f);
    ImVec4 border = ImVec4(tint.x * 0.42f, tint.y * 0.42f, tint.z * 0.42f, 0.86f);
    dl->AddRectFilled(pos, max, ImGui::GetColorU32(bg), ui_margin_px(3.0f));
    dl->AddRect(pos, max, ImGui::GetColorU32(border), ui_margin_px(3.0f));
    dl->AddText(ImVec2(pos.x + ui_margin_px(8.0f), pos.y + floorf((row_h - text_sz.y) * 0.5f)),
                ImGui::GetColorU32(ImVec4(0.90f, 0.88f, 0.86f, 1.0f)), safe_text);
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
    int logo_w = 0;
    int logo_h = 0;
    if (ID3D11ShaderResourceView* logo_text = ui_app_logo_text_srv(&logo_w, &logo_h)) {
        const float logo_x_offset = ui_margin_px(4.0f);
        const float logo_y_offset = ui_px(1.5f);
        const float logo_max_h = toolbar_h - ui_px(6.0f);
        float logo_h_px = ImMin(logo_max_h, row_h * 0.9f);
        float logo_w_px = logo_h_px * ((float)logo_w / (float)logo_h);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + logo_x_offset);
        ImGui::SetCursorPosY(floorf((toolbar_h - logo_h_px) * 0.5f + logo_y_offset));
        ImGui::Image((ImTextureID)logo_text, ImVec2(logo_w_px, logo_h_px));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.92f, 0.91f, 1.0f));
        ImGui::TextUnformatted("lazyTool");
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (ID3D11ShaderResourceView* app_icon = ui_app_icon_srv()) {
            ImVec2 title_min = ImGui::GetItemRectMin();
            ImVec2 title_max = ImGui::GetItemRectMax();
            ImGui::SetNextWindowPos(ImVec2(title_min.x, title_max.y + ui_margin_px(8.0f)), ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(10.0f), ui_margin_px(10.0f)));
            ImGui::BeginTooltip();
            float icon_size = ui_px(64.0f);
            ImGui::Image((ImTextureID)app_icon, ImVec2(icon_size, icon_size));
            ImGui::EndTooltip();
            ImGui::PopStyleVar();
        }
    }
    ImGui::SameLine(0.0f, ui_margin_px(12.0f));
    ui_align_frame_row(row_y);

    if (ui_icon_text_button("##new_project_button", UI_ICON_NEW_PROJECT, "New", "New project"))
        project_new_default();
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    if (ui_icon_text_button("##save_project_button", UI_ICON_SAVE_PROJECT, "Save", "Save project"))
        ui_open_project_file_bar(PROJECT_FILE_SAVE);
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    if (ui_icon_text_button("##load_project_button", UI_ICON_LOAD_PROJECT, "Load", "Load project"))
        ui_open_project_file_bar(PROJECT_FILE_LOAD);
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
    bool shader_editor_was_open = s_shader_editor_floating;
    if (shader_editor_was_open) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.33f, 0.18f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.24f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.30f, 0.14f, 1.0f));
    }
    if (ui_icon_button("##shader_editor_button", UI_ICON_SHADER_EDITOR, ImVec2(ui_px(28.0f), 0.0f), "Shader editor"))
        ui_shader_editor_toggle_floating();
    if (shader_editor_was_open)
        ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    bool render_graph_was_open = s_render_graph_window_open;
    if (render_graph_was_open) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.33f, 0.18f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.24f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.58f, 0.30f, 0.14f, 1.0f));
    }
    if (ui_icon_button("##render_graph_button", UI_ICON_RENDER_GRAPH, ImVec2(ui_px(28.0f), 0.0f), "Render graph")) {
        s_render_graph_window_open = !s_render_graph_window_open;
        if (s_render_graph_window_open)
            s_render_graph_center_next = true;
    }
    if (render_graph_was_open)
        ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ui_align_frame_row(row_y);
    if (ui_icon_button("##export_exe_button", UI_ICON_EXPORT_EXE, ImVec2(ui_px(28.0f), 0.0f), "Export EXE"))
        ui_export_current_project_single_exe();
    ImGui::SameLine(0.0f, ui_margin_px(6.0f));
    ui_align_frame_row(row_y);
    if (ui_icon_button("##help_button", UI_ICON_HELP, ImVec2(ui_px(28.0f), 0.0f), "Help"))
        s_help_popup_open = !s_help_popup_open;
    ImGui::SameLine(0.0f, ui_margin_px(8.0f));
    ui_align_text_row(row_y);
    ImGui::TextDisabled("workspace");
    ImGui::SameLine(0.0f, ui_margin_px(8.0f));
    ui_align_frame_row(row_y);
    ui_inline_badge("##project_name_badge", project_current_name() ? project_current_name() : "untitled",
                    ImVec4(0.74f, 0.53f, 0.42f, 1.0f), row_h);
    ImGui::SameLine(0.0f, ui_margin_px(7.0f));
    ui_align_text_row(row_y);
    char build_label[64] = {};
    snprintf(build_label, sizeof(build_label), "build %s", LAZYTOOL_BUILD_CODE_STR);
    ui_inline_small_text("##workspace_build_label", build_label, ImVec4(0.48f, 0.46f, 0.45f, 1.0f), row_h, 0.78f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Build %s (#%s)", LAZYTOOL_BUILD_CODE_STR, LAZYTOOL_BUILD_NUMBER_STR);
    ui_top_bar_load_progress(row_y, row_h);

    static bool s_frame_ms_display_valid = false;
    static float s_frame_ms_display = 0.0f;
    float frame_ms_raw = ImGui::GetIO().DeltaTime * 1000.0f;
    if (s_frame_ms_display_valid)
        s_frame_ms_display += (frame_ms_raw - s_frame_ms_display) * 0.04f;
    else {
        s_frame_ms_display = frame_ms_raw;
        s_frame_ms_display_valid = true;
    }
    float frame_ms = s_frame_ms_display;
    const char* present_chip = g_dx.vsync ? "VSync" : (g_dx.present_allow_tearing ? "Tearing" : "No VSync");
    char cfg_chip[32] = {};
    ui_title_case_label(LAZYTOOL_BUILD_CONFIG, cfg_chip, sizeof(cfg_chip));
    char fps_chip[32] = {};
    char ms_chip[32] = {};
    char res_chip[32] = {};
    char frame_chip[40] = {};
    char time_chip[40] = {};
    char mode_chip[48] = {};
    float fps = frame_ms > 0.001f ? 1000.0f / frame_ms : 0.0f;
    snprintf(fps_chip, sizeof(fps_chip), "%.0f FPS", fps);
    snprintf(ms_chip, sizeof(ms_chip), "%.1f ms", frame_ms);
    snprintf(res_chip, sizeof(res_chip), "%dx%d", g_dx.scene_width, g_dx.scene_height);
    snprintf(frame_chip, sizeof(frame_chip), "Frame %llu", (unsigned long long)app_scene_frame());
    snprintf(time_chip, sizeof(time_chip), "Time %.2fs", app_scene_time());
    snprintf(mode_chip, sizeof(mode_chip), "%s / %s", present_chip, cfg_chip);

    float content_max_x = ImGui::GetWindowContentRegionMax().x;
    float controls_w = ui_window_controls_width();
    float controls_x = content_max_x - controls_w;
    float summary_gap = ui_margin_px(12.0f);
    float drag_gap = ui_margin_px(8.0f);
    float left_end_x = ImGui::GetCursorPosX();
    const char* chips[] = { fps_chip, ms_chip, frame_chip, time_chip, res_chip, mode_chip };
    ImVec4 chip_tints[] = {
        ImVec4(0.70f, 0.78f, 0.86f, 1.0f),
        ImVec4(0.72f, 0.66f, 0.88f, 1.0f),
        ImVec4(0.86f, 0.68f, 0.45f, 1.0f),
        ImVec4(0.80f, 0.74f, 0.58f, 1.0f),
        ImVec4(0.60f, 0.78f, 0.68f, 1.0f),
        ImVec4(0.78f, 0.42f, 0.32f, 1.0f)
    };
    const int chip_count = (int)(sizeof(chips) / sizeof(chips[0]));
    float chip_spacing = ui_margin_px(6.0f);
    float chips_w = 0.0f;
    for (int i = 0; i < chip_count; i++) {
        if (i > 0)
            chips_w += chip_spacing;
        chips_w += ui_top_bar_chip_width(chips[i]);
    }
    float chips_x = controls_x - summary_gap - chips_w;

    float drag_w = chips_x - left_end_x - drag_gap;
    if (drag_w > ui_margin_px(24.0f)) {
        ImGui::SameLine();
        ui_align_frame_row(row_y);
        ImGui::InvisibleButton("##title_drag", ImVec2(drag_w, row_h));
        if (ImGui::IsItemActivated()) {
            ReleaseCapture();
            SendMessageW(g_dx.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
    }

    if (chips_x > left_end_x + ui_margin_px(12.0f)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 chip_pos = ImGui::GetWindowPos();
        chip_pos.x += chips_x;
        chip_pos.y += row_y;
        for (int i = 0; i < chip_count; i++) {
            if (i > 0)
                chip_pos.x += chip_spacing;
            ui_top_bar_chip(dl, chip_pos, chips[i], row_h, chip_tints[i]);
            chip_pos.x += ui_top_bar_chip_width(chips[i]);
        }
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
        UI_PROFILE_SCOPE(UI_PROFILE_VIEWPORT);
        ui_panel_scene(true);
        return;
    }

    if (s_viewport_fullscreen) {
        bool restart_clicked = false;
        bool pause_clicked = false;
        bool exit_clicked = false;
        if (ui_begin_tool_panel("##viewport_panel_full", "VIEWPORT", nullptr, avail, UI_PANEL_VIEWPORT,
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
            {
                UI_PROFILE_SCOPE(UI_PROFILE_VIEWPORT);
                ui_panel_scene(true);
            }
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
        UI_PROFILE_SCOPE(UI_PROFILE_COMMANDS);
        ui_panel_commands(true);
    }
    ui_end_tool_panel();

    if (ui_begin_tool_panel("##resources_panel", "RESOURCES", "right click to create", ImVec2(left_w, 0.0f), UI_PANEL_RESOURCES)) {
        UI_PROFILE_SCOPE(UI_PROFILE_RESOURCES);
        ui_panel_resources(true);
    }
    ui_end_tool_panel();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, col_gap);
    ImGui::BeginGroup();
    bool restart_clicked = false;
    bool pause_clicked = false;
    bool fullscreen_clicked = false;
    if (ui_begin_tool_panel("##viewport_panel", "VIEWPORT", nullptr, ImVec2(center_w, viewport_h), UI_PANEL_VIEWPORT,
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
        {
            UI_PROFILE_SCOPE(UI_PROFILE_VIEWPORT);
            ui_panel_scene(true);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ui_end_tool_panel();

    if (ui_begin_tool_panel("##log_panel", "LOG", nullptr, ImVec2(center_w, 0.0f), UI_PANEL_LOG)) {
        UI_PROFILE_SCOPE(UI_PROFILE_LOG);
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

        ui_update_general_panel_selection_autoclose();
        if (s_right_panel_general_open) {
            if (collapsed_h > 0.0f) {
                if (ui_header_only_panel("##inspector_collapsed_panel", "INSPECTOR", ui_inspector_header_detail(),
                                         ImVec2(0.0f, collapsed_h), UI_PANEL_INSPECTOR)) {
                    ui_close_general_panel_to_inspector();
                }
            }

            if (collapsed_h > 0.0f)
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + col_gap);

            if (ui_begin_tool_panel("##general_panel", "GENERAL", nullptr,
                                    ImVec2(0.0f, expanded_h), UI_PANEL_GENERAL)) {
                UI_PROFILE_SCOPE(UI_PROFILE_INSPECTOR_GENERAL);
                ui_panel_general(true);
            }
            ui_end_tool_panel();
        } else {
            if (ui_begin_tool_panel("##inspector_panel", "INSPECTOR", ui_inspector_header_detail(),
                                    ImVec2(0.0f, expanded_h), UI_PANEL_INSPECTOR)) {
                UI_PROFILE_SCOPE(UI_PROFILE_INSPECTOR_GENERAL);
                if (ImGui::BeginTabBar("##inspector_tabs")) {
                    if (ImGui::BeginTabItem("Properties")) {
                        ui_panel_inspector(true);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Resources Viewer")) {
                        ui_panel_resources_viewer(true);
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
                                         ImVec2(0.0f, collapsed_h), UI_PANEL_GENERAL)) {
                    ui_open_general_panel();
                }
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

void ui_set_show_inspector_notes(bool show) {
    s_show_inspector_notes = show;
    if (!show) {
        memset(s_inspector_resource_note_editing, 0, sizeof(s_inspector_resource_note_editing));
        memset(s_inspector_command_note_editing, 0, sizeof(s_inspector_command_note_editing));
    }
}

bool ui_show_inspector_notes() {
    return s_show_inspector_notes;
}

void ui_set_show_interface_hints(bool show) {
    s_show_interface_hints = show;
}

bool ui_show_interface_hints() {
    return s_show_interface_hints;
}

void ui_set_render_target_preview_columns(int columns) {
    if (columns < 1) columns = 1;
    if (columns > 8) columns = 8;
    s_render_target_preview_columns = columns;
}

int ui_render_target_preview_columns() {
    return s_render_target_preview_columns;
}

void ui_request_scene_surface_layout_refresh() {
    s_scene_surface_resize_armed = true;
    s_scene_surface_host_w = 0;
    s_scene_surface_host_h = 0;
}

void ui_init() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui::SetColorEditOptions(ImGuiColorEditFlags_Float);

    ImFont* ui_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f);
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
    ui_apply_global_scale_now();
    s_ui_scale_dirty = false;

    ImGui_ImplWin32_Init(g_dx.hwnd);
    ImGui_ImplDX11_Init(g_dx.dev, g_dx.ctx);
    ui_init_rt3d_preview_pipeline();
}


static void ui_draw_loading_overlay() {
    ResourceLoadProgress progress = {};
    bool resource_loading = res_get_load_progress(&progress) && progress.active;
    bool project_loading = s_project_load_pending || s_project_load_active;
    if (!resource_loading && !project_loading)
        return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList(vp);
    ImVec2 vp_min = vp->Pos;
    ImVec2 vp_max(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    bg->AddRectFilled(vp_min, vp_max, IM_COL32(8, 7, 8, 135));

    const float card_w = ui_px(420.0f);
    const float card_h = ui_px(resource_loading ? 132.0f : 82.0f);
    ImVec2 card_pos(vp->Pos.x + (vp->Size.x - card_w) * 0.5f,
                    vp->Pos.y + (vp->Size.y - card_h) * 0.5f);

    ImGui::SetNextWindowPos(card_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(card_w, card_h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui_margin_px(12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(18.0f), ui_margin_px(16.0f)));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.075f, 0.071f, 0.078f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.32f, 0.26f, 0.22f, 0.92f));
    ImGui::Begin("##loading_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);

    ImGui::TextUnformatted(project_loading ? "Opening project" : "Loading asset");
    ImGui::Spacing();

    if (project_loading) {
        // Project loading is a short synchronous operation after this overlay has
        // had a frame to appear. Do not show fake progress or async wording here:
        // keep the modal clean and avoid implying that the bar can advance.
    } else if (resource_loading) {
        char title[192] = {};
        snprintf(title, sizeof(title), "%s", progress.label[0] ? progress.label : "Resource");
        ImGui::TextColored(ImVec4(0.92f, 0.78f, 0.62f, 1.0f), "%s", title);
        ImGui::TextDisabled("%s", progress.path);
        ImGui::Spacing();
        int pct = (int)(progress.fraction * 100.0f + 0.5f);
        char pct_text[32] = {};
        snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
        ImGui::ProgressBar(progress.fraction, ImVec2(-1.0f, ui_px(9.0f)), pct_text);
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// Draw one full editor frame on top of the already-rendered scene texture.
void ui_draw() {
    ui_profile_begin_frame();
    g_editor_mouse_capture = false;
    g_scene_view_hovered = false;
    g_scene_view_focused = false;
    g_scene_view_pointer_over = false;

    {
        UI_PROFILE_SCOPE(UI_PROFILE_FRAME_SETUP);
        if (s_ui_scale_dirty) {
            ui_apply_global_scale_now();
            s_ui_scale_dirty = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }
    ui_execute_pending_project_load_if_ready();
    ImGuiIO& io = ImGui::GetIO();
    bool shader_editor_was_focused = s_shader_source_editor_focused;
    s_shader_source_editor_focused = false;
    bool hotkeys_ok = !io.WantTextInput && !io.WantCaptureKeyboard &&
                      !ImGui::IsAnyItemActive() && !shader_editor_was_focused;
    bool editor_selection_hotkeys_ok = !io.WantTextInput && !ImGui::IsAnyItemActive() &&
                                       !shader_editor_was_focused && !s_timeline_keyboard_focus;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
        ui_recompile_all_shaders();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
        ui_recompile_active_or_selected_shader();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !shader_editor_was_focused)
        ui_open_project_file_bar(PROJECT_FILE_SAVE);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false) && !shader_editor_was_focused)
        ui_open_project_file_bar(PROJECT_FILE_LOAD);
    if (hotkeys_ok && ImGui::IsKeyPressed(ImGuiKey_F1, false))
        s_help_popup_open = !s_help_popup_open;
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

    {
        UI_PROFILE_SCOPE(UI_PROFILE_TOP_BAR);
        ui_top_bar();
    }
    {
        UI_PROFILE_SCOPE(UI_PROFILE_PROJECT_FILE_BAR);
        ui_project_file_bar();
    }

    ImVec2 workspace_size = ImGui::GetContentRegionAvail();
    if (workspace_size.x < 1.0f) workspace_size.x = 1.0f;
    if (workspace_size.y < 1.0f) workspace_size.y = 1.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_margin_px(8.0f), ui_margin_px(7.0f)));
    ImGui::BeginChild("##workspace_root", workspace_size, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ui_workspace_layout();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    {
        UI_PROFILE_SCOPE(UI_PROFILE_FLOATING_WINDOWS);
        ui_draw_help_popup();
    }
    ImGui::End();

    {
        UI_PROFILE_SCOPE(UI_PROFILE_FLOATING_WINDOWS);
        ui_draw_render_graph_window();
        ui_draw_timeline_window();
        ui_draw_shader_editor_window();
    }

    ui_draw_loading_overlay();

    {
        UI_PROFILE_SCOPE(UI_PROFILE_IMGUI_RENDER_FINALIZE);
        ImGui::Render();
    }
}

void ui_shutdown() {
    ui_release_app_icon_texture();
    ui_release_app_logo_text_texture();
    ui_release_rt3d_preview_pipeline();
    ui_release_shadow_depth_preview_pipeline();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}
