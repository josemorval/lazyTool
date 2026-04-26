#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "types.h"
#include "log.h"
#include "dx11_ctx.h"
#include "resources.h"
#include "commands.h"
#include "project.h"
#include "user_cb.h"
#include "ui.h"
#include "app_settings.h"
#include "resource.h"

// ── math impl ────────────────────────────────────────────────────────────

Mat4 mat4_identity() {
    Mat4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
}

Mat4 mat4_transpose(const Mat4& m) {
    Mat4 r = {};
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            r.m[row * 4 + col] = m.m[col * 4 + row];
    return r;
}

Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
    Mat4 r = {};
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            for (int k   = 0; k   < 4; k++)
                r.m[row*4+col] += a.m[row*4+k] * b.m[k*4+col];
    return r;
}

Mat4 mat4_inverse(const Mat4& m) {
    float a[4][8] = {};
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++)
            a[row][col] = m.m[row * 4 + col];
        a[row][4 + row] = 1.0f;
    }

    for (int col = 0; col < 4; col++) {
        int pivot_row = col;
        float pivot_abs = fabsf(a[pivot_row][col]);
        for (int row = col + 1; row < 4; row++) {
            float v = fabsf(a[row][col]);
            if (v > pivot_abs) {
                pivot_abs = v;
                pivot_row = row;
            }
        }

        if (pivot_abs < 1e-8f)
            return mat4_identity();

        if (pivot_row != col) {
            for (int k = 0; k < 8; k++) {
                float tmp = a[col][k];
                a[col][k] = a[pivot_row][k];
                a[pivot_row][k] = tmp;
            }
        }

        float pivot = a[col][col];
        for (int k = 0; k < 8; k++)
            a[col][k] /= pivot;

        for (int row = 0; row < 4; row++) {
            if (row == col)
                continue;
            float factor = a[row][col];
            if (fabsf(factor) < 1e-8f)
                continue;
            for (int k = 0; k < 8; k++)
                a[row][k] -= factor * a[col][k];
        }
    }

    Mat4 inv = {};
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            inv.m[row * 4 + col] = a[row][4 + col];
    return inv;
}

Mat4 mat4_lookat(Vec3 eye, Vec3 at, Vec3 up) {
    Vec3 z = v3_norm(v3_sub(eye, at));
    Vec3 x = v3_norm(v3_cross(z, up));
    Vec3 y = v3_cross(x, z);
    Mat4 r = mat4_identity();
    r.m[0]  = x.x; r.m[1]  = y.x; r.m[2]  = z.x; r.m[3]  = 0.f;
    r.m[4]  = x.y; r.m[5]  = y.y; r.m[6]  = z.y; r.m[7]  = 0.f;
    r.m[8]  = x.z; r.m[9]  = y.z; r.m[10] = z.z; r.m[11] = 0.f;
    r.m[12] = -v3_dot(x, eye);
    r.m[13] = -v3_dot(y, eye);
    r.m[14] = -v3_dot(z, eye);
    r.m[15] = 1.f;
    return r;
}

Mat4 mat4_perspective(float fov_y, float aspect, float near_z, float far_z) {
    float f = 1.f / tanf(fov_y * 0.5f);
    Mat4 r  = {};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = far_z / (near_z - far_z);
    r.m[11] = -1.f;
    r.m[14] = (near_z * far_z) / (near_z - far_z);
    return r;
}

Mat4 mat4_orthographic(float width, float height, float near_z, float far_z) {
    Mat4 r = {};
    r.m[0]  = 2.0f / width;
    r.m[5]  = 2.0f / height;
    r.m[10] = 1.0f / (near_z - far_z);
    r.m[14] = near_z / (near_z - far_z);
    r.m[15] = 1.0f;
    return r;
}

Mat4 mat4_translation(Vec3 t) {
    Mat4 r = mat4_identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

Mat4 mat4_scale(Vec3 s) {
    Mat4 r = {};
    r.m[0]  = s.x;
    r.m[5]  = s.y;
    r.m[10] = s.z;
    r.m[15] = 1.0f;
    return r;
}

Mat4 mat4_rotation_xyz(Vec3 a) {
    float cx = cosf(a.x), sx = sinf(a.x);
    float cy = cosf(a.y), sy = sinf(a.y);
    float cz = cosf(a.z), sz = sinf(a.z);

    Mat4 rx = mat4_identity();
    rx.m[5] = cx;  rx.m[6] = sx;
    rx.m[9] = -sx; rx.m[10] = cx;

    Mat4 ry = mat4_identity();
    ry.m[0] = cy;  ry.m[2] = -sy;
    ry.m[8] = sy;  ry.m[10] = cy;

    Mat4 rz = mat4_identity();
    rz.m[0] = cz;  rz.m[1] = sz;
    rz.m[4] = -sz; rz.m[5] = cz;

    return mat4_mul(mat4_mul(rx, ry), rz);
}

Vec3 camera_eye(const Camera& c) {
    return v3(c.position[0], c.position[1], c.position[2]);
}

static Vec3 camera_forward(const Camera& c) {
    float cp = cosf(c.pitch);
    return v3_norm(v3(
        sinf(c.yaw) * cp,
        sinf(c.pitch),
        cosf(c.yaw) * cp));
}

static Vec3 camera_right(const Camera& c) {
    Vec3 right = v3_norm(v3_cross(v3(0.0f, 1.0f, 0.0f), camera_forward(c)));
    if (v3_dot(right, right) < 0.0001f)
        right = v3(1.0f, 0.0f, 0.0f);
    return right;
}

// ── globals ───────────────────────────────────────────────────────────────

static bool     g_running        = true;
static int      g_pending_w      = 0;
static int      g_pending_h      = 0;
static bool     g_pending_resize = false;
static int      g_pending_scene_surface_w = 0;
static int      g_pending_scene_surface_h = 0;
static bool     g_pending_scene_surface_resize = false;
static float    g_time           = 0.f;
static float    g_dt             = 0.f;
static uint64_t g_frame          = 0;
static bool     g_restart_scene_requested = false;
static bool     g_scene_paused = false;
static CmdHandle g_default_pixelize_cmd = INVALID_HANDLE;

void app_request_scene_restart() {
    g_restart_scene_requested = true;
}

void app_request_scene_surface_resize(int w, int h) {
    if (w < 1 || h < 1)
        return;
    g_pending_scene_surface_w = w;
    g_pending_scene_surface_h = h;
    g_pending_scene_surface_resize = true;
}

void app_set_scene_paused(bool paused) {
    g_scene_paused = paused;
}

bool app_scene_paused() {
    return g_scene_paused;
}

float app_scene_time() {
    return g_time;
}

uint64_t app_scene_frame() {
    return g_frame;
}

Camera g_camera = {
    /* position */ { 1.838f, 1.182f, 3.354f },
    /* yaw      */ 3.6415927f,
    /* pitch    */ -0.3f,
    /* fov_y     */ 1.047f,   // ~60 deg
    /* near_z    */ 0.001f,
    /* far_z     */ 100.f
};

// Camera controls

CameraControls g_camera_controls = {
    /* enabled           */ true,
    /* mouse_look        */ true,
    /* invert_y          */ false,
    /* move_speed        */ 6.0f,
    /* fast_mult         */ 4.0f,
    /* slow_mult         */ 0.25f,
    /* mouse_sensitivity */ 0.004f
};

static bool key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void set_cursor_hidden(bool hidden) {
    static bool s_hidden = false;
    if (s_hidden == hidden)
        return;
    s_hidden = hidden;
    if (hidden) {
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        while (ShowCursor(TRUE) < 0) {}
    }
}

static POINT scene_view_center_screen() {
    RECT rc = {};
    GetClientRect(g_dx.hwnd, &rc);
    POINT center = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
    ClientToScreen(g_dx.hwnd, &center);
    return center;
}

struct ViewportMouseGesture {
    bool  active;
    POINT restore_cursor;
};

static ViewportMouseGesture s_camera_mouse_gesture = {};
static ViewportMouseGesture s_light_orbit_gesture = {};

static bool imgui_keyboard_capture_requested() {
    if (!ImGui::GetCurrentContext())
        return false;
    ImGuiIO& io = ImGui::GetIO();
    return io.WantTextInput || ImGui::IsAnyItemActive();
}

static void end_viewport_mouse_gesture(ViewportMouseGesture* gesture) {
    if (!gesture || !gesture->active)
        return;
    ReleaseCapture();
    set_cursor_hidden(false);
    SetCursorPos(gesture->restore_cursor.x, gesture->restore_cursor.y);
    gesture->active = false;
}

// Camera look and light orbit both want the same interaction model:
// keep the cursor hidden, capture the window, read deltas from the viewport
// center, then recenter every frame. Sharing the gesture code keeps both tools
// behaving the same and avoids two subtly different input paths to maintain.
static bool begin_or_update_viewport_mouse_gesture(
    ViewportMouseGesture* gesture, bool trigger_down, bool hovered, bool allow_start,
    float* dx, float* dy)
{
    if (dx) *dx = 0.0f;
    if (dy) *dy = 0.0f;

    if (GetForegroundWindow() != g_dx.hwnd) {
        end_viewport_mouse_gesture(gesture);
        return false;
    }

    bool can_start = trigger_down && hovered && allow_start;
    if (!trigger_down || (!gesture->active && !can_start)) {
        end_viewport_mouse_gesture(gesture);
        return false;
    }

    if (!gesture->active) {
        gesture->active = true;
        GetCursorPos(&gesture->restore_cursor);
        SetCapture(g_dx.hwnd);
        set_cursor_hidden(true);
        POINT center = scene_view_center_screen();
        SetCursorPos(center.x, center.y);
        return true;
    }

    POINT center = scene_view_center_screen();
    POINT p = {};
    GetCursorPos(&p);
    if (dx) *dx = (float)(p.x - center.x);
    if (dy) *dy = (float)(p.y - center.y);
    SetCursorPos(center.x, center.y);
    return true;
}

// Free-look camera: while RMB is held we recenter the cursor every frame and
// integrate deltas from the window center. That removes the screen-edge limit
// without needing raw-input plumbing yet.
static void update_camera_mouse() {
    float dx = 0.0f;
    float dy = 0.0f;
    bool active = begin_or_update_viewport_mouse_gesture(
        &s_camera_mouse_gesture,
        key_down(VK_RBUTTON),
        g_scene_view_hovered,
        g_camera_controls.enabled && g_camera_controls.mouse_look && !imgui_keyboard_capture_requested(),
        &dx, &dy);
    if (!active)
        return;

    if (dx == 0.0f && dy == 0.0f)
        return;

    float sens = g_camera_controls.mouse_sensitivity;
    if (sens < 0.0001f) sens = 0.0001f;
    g_camera.yaw += dx * sens;
    g_camera.pitch += (g_camera_controls.invert_y ? -dy : dy) * sens;
    g_camera.pitch = clampf(g_camera.pitch, -1.50f, 1.50f);
}

// Holding L rotates the built-in directional light around its target. The edit
// keeps distance stable and only changes azimuth/elevation, which matches the
// inspector fields and makes it easy to extend with gizmos later.
static bool update_dirlight_orbit() {
    float dx = 0.0f;
    float dy = 0.0f;
    bool active = begin_or_update_viewport_mouse_gesture(
        &s_light_orbit_gesture,
        key_down('L'),
        g_scene_view_hovered,
        !imgui_keyboard_capture_requested(),
        &dx, &dy);
    if (!active)
        return false;

    end_viewport_mouse_gesture(&s_camera_mouse_gesture);

    Resource* dl = res_get(g_builtin_dirlight);
    if (!dl)
        return true;

    Vec3 target = v3(dl->light_target[0], dl->light_target[1], dl->light_target[2]);
    Vec3 pos = v3(dl->light_pos[0], dl->light_pos[1], dl->light_pos[2]);
    Vec3 offset = v3_sub(pos, target);
    float distance = sqrtf(v3_dot(offset, offset));
    if (distance < 0.001f)
        distance = 0.001f;

    if (dx == 0.0f && dy == 0.0f)
        return true;

    float planar = sqrtf(offset.x * offset.x + offset.z * offset.z);
    float yaw = atan2f(offset.x, offset.z);
    float pitch = atan2f(offset.y, planar);
    float sens = g_camera_controls.mouse_sensitivity * 0.85f;
    if (sens < 0.0001f) sens = 0.0001f;

    yaw += dx * sens;
    pitch = clampf(pitch - dy * sens, -1.45f, 1.45f);

    float cp = cosf(pitch);
    Vec3 orbit = v3(sinf(yaw) * cp, sinf(pitch), cosf(yaw) * cp);
    Vec3 new_pos = v3_add(target, v3_scale(orbit, distance));
    Vec3 new_dir = v3_norm(v3_sub(target, new_pos));

    dl->light_pos[0] = new_pos.x;
    dl->light_pos[1] = new_pos.y;
    dl->light_pos[2] = new_pos.z;
    dl->light_dir[0] = new_dir.x;
    dl->light_dir[1] = new_dir.y;
    dl->light_dir[2] = new_dir.z;
    return true;
}

static void update_camera_keyboard(float dt) {
    if (!g_camera_controls.enabled)
        return;

    if (!g_scene_view_hovered && !key_down(VK_RBUTTON))
        return;

    if (imgui_keyboard_capture_requested())
        return;

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (key_down('W')) z += 1.0f;
    if (key_down('S')) z -= 1.0f;
    if (key_down('D')) x += 1.0f;
    if (key_down('A')) x -= 1.0f;
    if (key_down('E')) y += 1.0f;
    if (key_down('Q')) y -= 1.0f;
    if (x == 0.0f && y == 0.0f && z == 0.0f)
        return;

    float len = sqrtf(x * x + y * y + z * z);
    x /= len;
    y /= len;
    z /= len;

    Vec3 forward = camera_forward(g_camera);
    Vec3 right   = camera_right(g_camera);
    Vec3 up      = v3(0.0f, 1.0f, 0.0f);
    Vec3 move    = v3_add(v3_add(v3_scale(forward, z), v3_scale(right, x)), v3_scale(up, y));

    float speed = g_camera_controls.move_speed;
    if (speed < 0.001f) speed = 0.001f;
    if (key_down(VK_SHIFT))   speed *= g_camera_controls.fast_mult;
    if (key_down(VK_CONTROL)) speed *= g_camera_controls.slow_mult;

    g_camera.position[0] += move.x * speed * dt;
    g_camera.position[1] += move.y * speed * dt;
    g_camera.position[2] += move.z * speed * dt;
}

static void update_camera_controls(float dt) {
    if (GetForegroundWindow() != g_dx.hwnd)
        return;

    update_camera_mouse();
    update_camera_keyboard(dt);
}

// Win32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lp;
            if (IsZoomed(hwnd)) {
                MONITORINFO mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
                    params->rgrc[0] = mi.rcWork;
            }
            return 0;
        }
        return 0;
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit != HTCLIENT)
            return hit;

        RECT rc = {};
        GetWindowRect(hwnd, &rc);
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int border = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        if (border < 8) border = 8;

        bool left = p.x >= rc.left && p.x < rc.left + border;
        bool right = p.x < rc.right && p.x >= rc.right - border;
        bool top = p.y >= rc.top && p.y < rc.top + border;
        bool bottom = p.y < rc.bottom && p.y >= rc.bottom - border;

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }
    }

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_pending_w      = LOWORD(lp);
            g_pending_h      = HIWORD(lp);
            g_pending_resize = true;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── frame update ──────────────────────────────────────────────────────────

static void update_builtins_and_scene_cb() {
    Resource* bt = res_get(g_builtin_time);
    if (bt) bt->fval[0] = g_time;

    Resource* sc = res_get(g_builtin_scene_color);
    if (sc) {
        sc->srv = g_dx.scene_srv;
        sc->uav = g_dx.scene_uav;
        sc->width = g_dx.scene_width;
        sc->height = g_dx.scene_height;
        sc->tex_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc->has_srv = true;
        sc->has_uav = true;
        res_sync_size_resource(g_builtin_scene_color);
    }

    Resource* sd = res_get(g_builtin_scene_depth);
    if (sd) {
        sd->srv = g_dx.depth_srv;
        sd->width = g_dx.scene_width;
        sd->height = g_dx.scene_height;
        sd->tex_fmt = DXGI_FORMAT_R24G8_TYPELESS;
        sd->has_srv = true;
        res_sync_size_resource(g_builtin_scene_depth);
    }

    Resource* sm = res_get(g_builtin_shadow_map);
    if (sm) {
        sm->srv = g_dx.shadow_srv;
        sm->dsv = g_dx.shadow_dsv;
        sm->width = g_dx.shadow_width;
        sm->height = g_dx.shadow_height;
        sm->tex_fmt = DXGI_FORMAT_R24G8_TYPELESS;
        sm->has_srv = true;
        sm->has_dsv = true;
        res_sync_size_resource(g_builtin_shadow_map);
    }

    Vec3  eye    = camera_eye(g_camera);
    Vec3  at     = v3_add(eye, camera_forward(g_camera));
    float aspect = g_dx.scene_height > 0 ? (float)g_dx.scene_width / g_dx.scene_height : 1.f;

    Mat4 view = mat4_lookat(eye, at, {0.f, 1.f, 0.f});
    Mat4 proj = mat4_perspective(g_camera.fov_y, aspect, g_camera.near_z, g_camera.far_z);
    Mat4 vp   = mat4_mul(view, proj);
    Mat4 inv_vp = mat4_inverse(vp);

    // CPU matrices are stored as row-major row-vector transforms.
    // HLSL shaders use default column-major matrices with mul(M, v).
    // Upload the CPU memory as-is: column-major binding interprets it as the
    // transposed matrix needed for column-vector multiplication.
    SceneCBData cb = {};
    memcpy(cb.view_proj, vp.m, sizeof(vp.m));
    memcpy(cb.inv_view_proj, inv_vp.m, sizeof(inv_vp.m));
    cb.time_vec[0] = g_time;
    cb.time_vec[1] = g_dt;
    cb.time_vec[2] = (float)g_frame;
    cb.cam_pos[0]  = eye.x; cb.cam_pos[1] = eye.y; cb.cam_pos[2] = eye.z;

    Resource* dl = res_get(g_builtin_dirlight);
    Vec3 light_dir = v3(-0.577f, -0.577f, -0.577f);
    Vec3 light_pos = v3(-0.8f, 1.2f, -0.8f);
    Vec3 light_target = v3(0.0f, 0.0f, 0.0f);
    float shadow_w = 2.2f;
    float shadow_h = 2.2f;
    float shadow_near = 0.01f;
    float shadow_far = 4.0f;
    if (dl) {
        if (dl->shadow_width > 0 && dl->shadow_height > 0 &&
            (dl->shadow_width != g_dx.shadow_width || dl->shadow_height != g_dx.shadow_height)) {
            dx_create_shadow_map(dl->shadow_width, dl->shadow_height);
        }

        light_pos = v3(dl->light_pos[0], dl->light_pos[1], dl->light_pos[2]);
        light_target = v3(dl->light_target[0], dl->light_target[1], dl->light_target[2]);
        light_dir = v3_norm(v3_sub(light_target, light_pos));
        dl->light_dir[0] = light_dir.x;
        dl->light_dir[1] = light_dir.y;
        dl->light_dir[2] = light_dir.z;

        shadow_w = dl->shadow_extent[0] > 0.01f ? dl->shadow_extent[0] : 0.01f;
        shadow_h = dl->shadow_extent[1] > 0.01f ? dl->shadow_extent[1] : 0.01f;
        shadow_near = dl->shadow_near > 0.0001f ? dl->shadow_near : 0.0001f;
        shadow_far = dl->shadow_far > shadow_near + 0.001f ? dl->shadow_far : shadow_near + 0.001f;

        cb.light_dir[0] = light_dir.x;
        cb.light_dir[1] = light_dir.y;
        cb.light_dir[2] = light_dir.z;
        cb.light_dir[3] = dl->light_intensity;
        cb.light_color[0] = dl->light_color[0];
        cb.light_color[1] = dl->light_color[1];
        cb.light_color[2] = dl->light_color[2];
    }

    light_dir = v3_norm(light_dir);
    Vec3 shadow_up = fabsf(v3_dot(light_dir, v3(0.0f, 1.0f, 0.0f))) > 0.92f ? v3(0.0f, 0.0f, 1.0f) : v3(0.0f, 1.0f, 0.0f);
    Mat4 shadow_view = mat4_lookat(light_pos, light_target, shadow_up);
    Mat4 shadow_proj = mat4_orthographic(shadow_w, shadow_h, shadow_near, shadow_far);
    Mat4 shadow_vp = mat4_mul(shadow_view, shadow_proj);
    memcpy(cb.shadow_view_proj, shadow_vp.m, sizeof(shadow_vp.m));

    dx_update_scene_cb(cb);
}

// ── entry point ───────────────────────────────────────────────────────────

static void update_default_example_commands() {
    Command* c = cmd_get(g_default_pixelize_cmd);
    if (!c || !strstr(c->name, "pixelize_to_scene")) {
        g_default_pixelize_cmd = INVALID_HANDLE;
        c = nullptr;
        for (int i = 0; i < MAX_COMMANDS; i++) {
            if (g_commands[i].active && strstr(g_commands[i].name, "pixelize_to_scene")) {
                g_default_pixelize_cmd = (CmdHandle)(i + 1);
                c = &g_commands[i];
                break;
            }
        }
    }
    if (!c) return;

    c->thread_x = (g_dx.scene_width  + 7) / 8;
    c->thread_y = (g_dx.scene_height + 7) / 8;
    c->thread_z = 1;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_path, L'\\');
    if (!slash) slash = wcsrchr(exe_path, L'/');
    if (slash) {
        *slash = L'\0';
        SetCurrentDirectoryW(exe_path);
    }

    WNDCLASSEXW wc  = {};
    wc.cbSize       = sizeof(wc);
    wc.style        = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hInst;
    wc.hCursor      = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon        = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm      = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"lazyTool_wnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, L"lazyTool_wnd", L"lazyTool",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1600, 900,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    log_init();

    if (!dx_init(hwnd, 1600, 900)) {
        MessageBoxA(nullptr, "DX11 init failed.\nMake sure you have a DX11-capable GPU.", "lazyTool", MB_OK | MB_ICONERROR);
        return 1;
    }

    ui_init();
    res_init();
    cmd_init();
    user_cb_init();

    project_new_default();
    app_settings_load_or_create();

    log_info("lazyTool ready.");
    log_info("Right-click Resources panel  -> create resources.");
    log_info("Right-click Commands panel   -> create commands.");
    log_info("Use User CB panel to create variables; link scalar/vector resources as sources.");

    LARGE_INTEGER freq, prev_t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev_t);

    MSG msg = {};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        if (g_pending_resize) {
            dx_resize(g_pending_w, g_pending_h);
            g_pending_resize = false;
        }
        if (g_pending_scene_surface_resize) {
            if (g_pending_scene_surface_w != g_dx.scene_width || g_pending_scene_surface_h != g_dx.scene_height) {
                dx_create_scene_rt(g_pending_scene_surface_w, g_pending_scene_surface_h);
                res_sync_scene_dependent_render_textures();
                log_info("Scene surface resized to %dx%d", g_pending_scene_surface_w, g_pending_scene_surface_h);
            }
            g_pending_scene_surface_resize = false;
        }

        LARGE_INTEGER now_t;
        QueryPerformanceCounter(&now_t);
        g_dt = (float)(now_t.QuadPart - prev_t.QuadPart) / (float)freq.QuadPart;
        if (g_dt > 0.1f) g_dt = 0.1f;
        prev_t  = now_t;
        if (g_restart_scene_requested) {
            g_time = 0.0f;
            g_dt = 0.0f;
            g_frame = 0;
            g_restart_scene_requested = false;
            dx_create_scene_rt(g_dx.scene_width, g_dx.scene_height);
            Resource* dl = res_get(g_builtin_dirlight);
            if (dl && dl->shadow_width > 0 && dl->shadow_height > 0)
                dx_create_shadow_map(dl->shadow_width, dl->shadow_height);
            else
                dx_create_shadow_map(g_dx.shadow_width, g_dx.shadow_height);
            res_reset_transient_gpu_resources();
            log_info("Scene restarted from frame 0.");
        } else if (!g_scene_paused) {
            g_time += g_dt;
            g_frame++;
        } else {
            g_dt = 0.0f;
        }

        // Pause is intentionally a "freeze the scene texture" state: we do not
        // execute commands and we do not clear/rebuild the scene RT, so the
        // last rendered frame stays visible while the editor remains responsive.
        bool light_orbit_active = false;
        if (!g_scene_paused)
            light_orbit_active = update_dirlight_orbit();
        if (!g_scene_paused && !light_orbit_active)
            update_camera_controls(g_dt);
        update_builtins_and_scene_cb();
        update_default_example_commands();

        if (!g_scene_paused) {
            // User cbuffer: pack + upload + bind before any draw
            user_cb_update();

            cmd_profile_begin_frame_capture();

            // Scene render
            dx_begin_scene();
            user_cb_bind();       // b1 stays bound for all commands
            cmd_execute_all();
            dx_end_scene();
        }

        // ImGui render to backbuffer
        dx_begin_ui();
        ui_draw();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        cmd_profile_end_frame_capture();

        g_dx.sc->Present(g_dx.vsync ? 1 : 0, 0);
    }

    app_settings_save();
    user_cb_shutdown();
    ui_shutdown();
    cmd_shutdown();
    res_shutdown();
    dx_shutdown();

    return 0;
}
