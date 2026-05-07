#include "timeline.h"
#include "commands.h"
#include "resources.h"
#include "user_cb.h"
#include "ui.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

TimelineTrack g_timeline_tracks[MAX_TIMELINE_TRACKS] = {};
int           g_timeline_track_count = 0;

static int   s_timeline_fps = 24;
static int   s_timeline_length_frames = 240;
static int   s_timeline_current_frame = 0;
static float s_timeline_sample_frame = 0.0f;
static bool  s_timeline_setting_scene_time = false;
static bool  s_timeline_enabled = false;
static bool  s_timeline_loop = false;
static bool  s_timeline_interpolate_frames = false;


static const float TIMELINE_PI     = 3.14159265358979323846f;
static const float TIMELINE_TWO_PI = 6.28318530717958647692f;

static float timeline_wrap_angle(float a) {
    while (a > TIMELINE_PI) a -= TIMELINE_TWO_PI;
    while (a < -TIMELINE_PI) a += TIMELINE_TWO_PI;
    return a;
}

static float timeline_wrap_angle_near(float reference, float angle) {
    while (angle - reference > TIMELINE_PI) angle -= TIMELINE_TWO_PI;
    while (angle - reference < -TIMELINE_PI) angle += TIMELINE_TWO_PI;
    return angle;
}

static float timeline_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static float timeline_lerp_angle(float a, float b, float t) {
    return a + timeline_wrap_angle(b - a) * t;
}

struct TimelineQuat {
    float x, y, z, w;
};

static TimelineQuat timeline_quat_normalize(TimelineQuat q) {
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 1e-8f)
        return {0.0f, 0.0f, 0.0f, 1.0f};
    float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

static TimelineQuat timeline_quat_from_mat4(const Mat4& m) {
    float m00 = m.m[0],  m01 = m.m[1],  m02 = m.m[2];
    float m10 = m.m[4],  m11 = m.m[5],  m12 = m.m[6];
    float m20 = m.m[8],  m21 = m.m[9],  m22 = m.m[10];
    TimelineQuat q = {0.0f, 0.0f, 0.0f, 1.0f};
    float trace = m00 + m11 + m22;

    // The engine matrices use the same row-major/sign convention as
    // mat4_rotation_xyz(). These formulas are the matching matrix->quat
    // inverse, so Euler keys such as 0 and 2*pi become the same orientation.
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m12 - m21) / s;
        q.y = (m20 - m02) / s;
        q.z = (m01 - m10) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m12 - m21) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m20 + m02) / s;
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m20 - m02) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m01 - m10) / s;
        q.x = (m20 + m02) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return timeline_quat_normalize(q);
}

static TimelineQuat timeline_quat_from_euler_xyz(const float* r) {
    Mat4 m = mat4_rotation_xyz(v3(r[0], r[1], r[2]));
    return timeline_quat_from_mat4(m);
}

static TimelineQuat timeline_quat_slerp(TimelineQuat a, TimelineQuat b, float t) {
    a = timeline_quat_normalize(a);
    b = timeline_quat_normalize(b);
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        dot = -dot;
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
    }

    if (dot > 0.9995f) {
        TimelineQuat q = {
            timeline_lerp(a.x, b.x, t),
            timeline_lerp(a.y, b.y, t),
            timeline_lerp(a.z, b.z, t),
            timeline_lerp(a.w, b.w, t)
        };
        return timeline_quat_normalize(q);
    }

    dot = clampf(dot, -1.0f, 1.0f);
    float theta0 = acosf(dot);
    float theta = theta0 * t;
    float sin_theta = sinf(theta);
    float sin_theta0 = sinf(theta0);
    if (fabsf(sin_theta0) <= 1e-8f)
        return a;

    float s0 = cosf(theta) - dot * sin_theta / sin_theta0;
    float s1 = sin_theta / sin_theta0;
    TimelineQuat q = {
        a.x * s0 + b.x * s1,
        a.y * s0 + b.y * s1,
        a.z * s0 + b.z * s1,
        a.w * s0 + b.w * s1
    };
    return timeline_quat_normalize(q);
}

static Mat4 timeline_mat4_from_quat(TimelineQuat q) {
    q = timeline_quat_normalize(q);
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float xw = q.x * q.w, yw = q.y * q.w, zw = q.z * q.w;

    Mat4 m = mat4_identity();
    m.m[0]  = 1.0f - 2.0f * (yy + zz);
    m.m[1]  = 2.0f * (xy + zw);
    m.m[2]  = 2.0f * (xz - yw);
    m.m[4]  = 2.0f * (xy - zw);
    m.m[5]  = 1.0f - 2.0f * (xx + zz);
    m.m[6]  = 2.0f * (yz + xw);
    m.m[8]  = 2.0f * (xz + yw);
    m.m[9]  = 2.0f * (yz - xw);
    m.m[10] = 1.0f - 2.0f * (xx + yy);
    return m;
}

static void timeline_euler_xyz_from_mat4(const Mat4& rot, const float reference[3], float out[3]) {
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
        x = timeline_wrap_angle_near(reference[0], x);
        y = timeline_wrap_angle_near(reference[1], y);
        z = timeline_wrap_angle_near(reference[2], z);
    }

    out[0] = x;
    out[1] = y;
    out[2] = z;
}

static void timeline_sample_command_transform(const TimelineKey& a, const TimelineKey& b,
                                              float t, TimelineKey* out) {
    if (!out)
        return;

    for (int i = 0; i < 3; i++)
        out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], t);

    TimelineQuat qa = timeline_quat_from_euler_xyz(&a.fval[3]);
    TimelineQuat qb = timeline_quat_from_euler_xyz(&b.fval[3]);
    TimelineQuat q = timeline_quat_slerp(qa, qb, t);
    Mat4 rot = timeline_mat4_from_quat(q);
    timeline_euler_xyz_from_mat4(rot, &a.fval[3], &out->fval[3]);

    for (int i = 6; i < 9; i++)
        out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], t);
}

static void timeline_sample_camera(const TimelineKey& a, const TimelineKey& b,
                                   float t, TimelineKey* out) {
    if (!out)
        return;

    for (int i = 0; i < 3; i++)
        out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], t);
    out->fval[3] = timeline_lerp_angle(a.fval[3], b.fval[3], t); // yaw wraps.
    out->fval[4] = timeline_lerp(a.fval[4], b.fval[4], t);       // pitch is clamped, not wrapped.
    out->fval[5] = timeline_lerp(a.fval[5], b.fval[5], t);
    out->fval[6] = timeline_lerp(a.fval[6], b.fval[6], t);
    out->fval[7] = timeline_lerp(a.fval[7], b.fval[7], t);
}

static int timeline_clamp_frame(int frame) {
    if (frame < 0) frame = 0;
    if (frame >= s_timeline_length_frames)
        frame = s_timeline_length_frames - 1;
    return frame;
}

static int timeline_user_var_index(const char* name) {
    if (!name || !name[0])
        return -1;
    for (int i = 0; i < g_user_cb_count; i++) {
        if (strcmp(g_user_cb_entries[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int timeline_res_type_components(ResType type) {
    switch (type) {
    case RES_INT:
    case RES_FLOAT:
        return 1;
    case RES_INT2:
    case RES_FLOAT2:
        return 2;
    case RES_INT3:
    case RES_FLOAT3:
        return 3;
    case RES_FLOAT4:
        return 4;
    default:
        return 0;
    }
}

static bool timeline_res_type_is_integral(ResType type) {
    return type == RES_INT || type == RES_INT2 || type == RES_INT3;
}

int timeline_track_value_count(const TimelineTrack& track) {
    switch (track.kind) {
    case TIMELINE_TRACK_USER_VAR:
        return timeline_res_type_components(track.value_type);
    case TIMELINE_TRACK_COMMAND_TRANSFORM:
        return 9;
    case TIMELINE_TRACK_COMMAND_ENABLED:
        return 1;
    case TIMELINE_TRACK_CAMERA:
        return 8;
    case TIMELINE_TRACK_DIRLIGHT:
        return 10;
    default:
        return 0;
    }
}

bool timeline_track_uses_integral_values(const TimelineTrack& track) {
    return track.kind == TIMELINE_TRACK_COMMAND_ENABLED ||
           (track.kind == TIMELINE_TRACK_USER_VAR && timeline_res_type_is_integral(track.value_type));
}

void timeline_reset() {
    memset(g_timeline_tracks, 0, sizeof(g_timeline_tracks));
    g_timeline_track_count = 0;
    s_timeline_fps = 24;
    s_timeline_length_frames = 240;
    s_timeline_current_frame = 0;
    s_timeline_sample_frame = 0.0f;
    s_timeline_setting_scene_time = false;
    s_timeline_enabled = false;
    s_timeline_loop = false;
    s_timeline_interpolate_frames = false;
}

int timeline_fps() {
    return s_timeline_fps;
}

void timeline_set_fps(int fps) {
    if (fps < 1) fps = 1;
    if (fps > 240) fps = 240;
    s_timeline_fps = fps;
    timeline_update(app_scene_time());
    app_request_scene_render();
}

int timeline_length_frames() {
    return s_timeline_length_frames;
}

static void timeline_prune_keys_to_length() {
    for (int t = 0; t < g_timeline_track_count; t++) {
        TimelineTrack& track = g_timeline_tracks[t];
        if (!track.active)
            continue;
        int write = 0;
        for (int k = 0; k < track.key_count; k++) {
            if (track.keys[k].frame < s_timeline_length_frames)
                track.keys[write++] = track.keys[k];
        }
        for (int k = write; k < track.key_count; k++)
            memset(&track.keys[k], 0, sizeof(TimelineKey));
        track.key_count = write;
    }
}

void timeline_set_length_frames(int frames) {
    if (frames < 1) frames = 1;
    if (frames > MAX_TIMELINE_FRAMES) frames = MAX_TIMELINE_FRAMES;
    s_timeline_length_frames = frames;
    timeline_prune_keys_to_length();
    int old_frame = s_timeline_current_frame;
    s_timeline_current_frame = timeline_clamp_frame(s_timeline_current_frame);
    s_timeline_sample_frame = (float)s_timeline_current_frame;
    if (s_timeline_current_frame != old_frame && !s_timeline_setting_scene_time && s_timeline_fps > 0)
        app_set_scene_time((float)s_timeline_current_frame / (float)s_timeline_fps);
    else
        app_request_scene_render();
}

int timeline_current_frame() {
    return s_timeline_current_frame;
}

void timeline_set_current_frame(int frame) {
    s_timeline_current_frame = timeline_clamp_frame(frame);
    s_timeline_sample_frame = (float)s_timeline_current_frame;
    if (!s_timeline_setting_scene_time && s_timeline_fps > 0)
        app_set_scene_time((float)s_timeline_current_frame / (float)s_timeline_fps);
}

bool timeline_enabled() {
    return s_timeline_enabled;
}

void timeline_set_enabled(bool enabled) {
    s_timeline_enabled = enabled;
    app_request_scene_render();
}

bool timeline_loop() {
    return s_timeline_loop;
}

void timeline_set_loop(bool loop) {
    s_timeline_loop = loop;
}

bool timeline_interpolate_frames() {
    return s_timeline_interpolate_frames;
}

void timeline_set_interpolate_frames(bool enabled) {
    s_timeline_interpolate_frames = enabled;
    s_timeline_sample_frame = enabled ? s_timeline_sample_frame : (float)s_timeline_current_frame;
    app_request_scene_render();
}

int timeline_play_dir() {
    return 0;
}

void timeline_set_play_dir(int dir) {
    (void)dir;
}

void timeline_update(float scene_time_seconds) {
    if (s_timeline_fps <= 0 || scene_time_seconds < 0.0f)
        return;

    float frame_f = scene_time_seconds * (float)s_timeline_fps;
    if (frame_f < 0.0f) frame_f = 0.0f;
    float max_frame_f = (float)(s_timeline_length_frames - 1);
    if (frame_f > max_frame_f) frame_f = max_frame_f;

    int frame = (int)floorf(frame_f + 0.0001f);
    s_timeline_setting_scene_time = true;
    timeline_set_current_frame(frame);
    s_timeline_setting_scene_time = false;

    // The visible/current frame remains discrete, but when enabled the sampler
    // evaluates at the true fractional timeline position. This keeps low-FPS
    // timelines intentionally steppy by default while allowing smooth in-between
    // evaluation during playback.
    s_timeline_sample_frame = s_timeline_interpolate_frames ? frame_f : (float)s_timeline_current_frame;
}

int timeline_find_track(TimelineTrackKind kind, const char* target, ResType value_type) {
    if (!target) target = "";
    for (int i = 0; i < g_timeline_track_count; i++) {
        TimelineTrack& t = g_timeline_tracks[i];
        if (!t.active || t.kind != kind)
            continue;
        if (strcmp(t.target, target) != 0)
            continue;
        if (kind == TIMELINE_TRACK_USER_VAR && t.value_type != value_type)
            continue;
        return i;
    }
    return -1;
}

int timeline_add_track(TimelineTrackKind kind, const char* target, ResType value_type) {
    if (kind == TIMELINE_TRACK_NONE)
        return -1;
    if (!target) target = "";
    int existing = timeline_find_track(kind, target, value_type);
    if (existing >= 0)
        return existing;
    if (g_timeline_track_count >= MAX_TIMELINE_TRACKS)
        return -1;

    int index = g_timeline_track_count++;
    TimelineTrack& t = g_timeline_tracks[index];
    memset(&t, 0, sizeof(t));
    t.active = true;
    t.enabled = true;
    t.kind = kind;
    t.value_type = value_type;
    strncpy(t.target, target, MAX_NAME - 1);
    t.target[MAX_NAME - 1] = '\0';
    return index;
}

void timeline_delete_track(int track_index) {
    if (track_index < 0 || track_index >= g_timeline_track_count)
        return;
    for (int i = track_index; i < g_timeline_track_count - 1; i++)
        g_timeline_tracks[i] = g_timeline_tracks[i + 1];
    memset(&g_timeline_tracks[--g_timeline_track_count], 0, sizeof(TimelineTrack));
}

void timeline_delete_tracks_for_command(const char* target) {
    if (!target || !target[0])
        return;
    for (int i = g_timeline_track_count - 1; i >= 0; i--) {
        TimelineTrack& track = g_timeline_tracks[i];
        if (!track.active)
            continue;
        bool command_track = track.kind == TIMELINE_TRACK_COMMAND_TRANSFORM ||
                             track.kind == TIMELINE_TRACK_COMMAND_ENABLED;
        if (command_track && strcmp(track.target, target) == 0)
            timeline_delete_track(i);
    }
}

void timeline_rename_tracks_for_command(const char* old_target, const char* new_target) {
    if (!old_target || !old_target[0] || !new_target || !new_target[0] ||
        strcmp(old_target, new_target) == 0)
        return;

    for (int i = 0; i < g_timeline_track_count; i++) {
        TimelineTrack& track = g_timeline_tracks[i];
        if (!track.active)
            continue;
        bool command_track = track.kind == TIMELINE_TRACK_COMMAND_TRANSFORM ||
                             track.kind == TIMELINE_TRACK_COMMAND_ENABLED;
        if (!command_track || strcmp(track.target, old_target) != 0)
            continue;
        strncpy(track.target, new_target, MAX_NAME - 1);
        track.target[MAX_NAME - 1] = '\0';
    }
}

bool timeline_track_target_exists(const TimelineTrack& track) {
    switch (track.kind) {
    case TIMELINE_TRACK_USER_VAR:
        return timeline_user_var_index(track.target) >= 0;
    case TIMELINE_TRACK_COMMAND_TRANSFORM:
    case TIMELINE_TRACK_COMMAND_ENABLED:
        return cmd_find_by_name(track.target) != INVALID_HANDLE;
    case TIMELINE_TRACK_CAMERA:
        return true;
    case TIMELINE_TRACK_DIRLIGHT:
        return res_get(g_builtin_dirlight) != nullptr;
    default:
        return false;
    }
}

const char* timeline_track_kind_token(TimelineTrackKind kind) {
    switch (kind) {
    case TIMELINE_TRACK_USER_VAR:          return "user";
    case TIMELINE_TRACK_COMMAND_TRANSFORM: return "cmd_transform";
    case TIMELINE_TRACK_COMMAND_ENABLED:   return "cmd_enabled";
    case TIMELINE_TRACK_CAMERA:            return "camera";
    case TIMELINE_TRACK_DIRLIGHT:          return "dirlight";
    default:                               return "none";
    }
}

TimelineTrackKind timeline_track_kind_from_token(const char* token) {
    if (!token) return TIMELINE_TRACK_NONE;
    if (strcmp(token, "user") == 0) return TIMELINE_TRACK_USER_VAR;
    if (strcmp(token, "cmd_transform") == 0) return TIMELINE_TRACK_COMMAND_TRANSFORM;
    if (strcmp(token, "cmd_enabled") == 0) return TIMELINE_TRACK_COMMAND_ENABLED;
    if (strcmp(token, "camera") == 0) return TIMELINE_TRACK_CAMERA;
    if (strcmp(token, "dirlight") == 0) return TIMELINE_TRACK_DIRLIGHT;
    return TIMELINE_TRACK_NONE;
}

int timeline_find_key_index(const TimelineTrack& track, int frame) {
    for (int i = 0; i < track.key_count; i++) {
        if (track.keys[i].frame == frame)
            return i;
    }
    return -1;
}

TimelineKey* timeline_set_key(int track_index, int frame) {
    if (track_index < 0 || track_index >= g_timeline_track_count)
        return nullptr;

    TimelineTrack& track = g_timeline_tracks[track_index];
    frame = timeline_clamp_frame(frame);
    int existing = timeline_find_key_index(track, frame);
    if (existing >= 0)
        return &track.keys[existing];

    if (track.key_count >= MAX_TIMELINE_KEYS)
        return nullptr;

    int insert = track.key_count;
    while (insert > 0 && track.keys[insert - 1].frame > frame) {
        track.keys[insert] = track.keys[insert - 1];
        insert--;
    }
    track.key_count++;
    memset(&track.keys[insert], 0, sizeof(TimelineKey));
    track.keys[insert].frame = frame;
    return &track.keys[insert];
}

static bool timeline_capture_user_var(TimelineTrack& track, TimelineKey& key) {
    int idx = timeline_user_var_index(track.target);
    if (idx < 0)
        return false;
    // Keep source-driven UserCB tracks fresh when keys are inserted from the
    // timeline rather than from the UserCB panel itself.
    user_cb_refresh_entry(idx);
    UserCBEntry& e = g_user_cb_entries[idx];
    track.value_type = e.type;
    int n = timeline_res_type_components(e.type);
    for (int i = 0; i < n; i++) {
        key.ival[i] = e.ival[i];
        key.fval[i] = e.fval[i];
    }
    return true;
}

static bool timeline_capture_command_transform(TimelineTrack& track, TimelineKey& key) {
    Command* c = cmd_get(cmd_find_by_name(track.target));
    if (!c)
        return false;
    for (int i = 0; i < 3; i++) key.fval[i] = c->pos[i];
    for (int i = 0; i < 3; i++) key.fval[3 + i] = c->rot[i];
    for (int i = 0; i < 3; i++) key.fval[6 + i] = c->scale[i];
    return true;
}

static bool timeline_capture_command_enabled(TimelineTrack& track, TimelineKey& key) {
    Command* c = cmd_get(cmd_find_by_name(track.target));
    if (!c)
        return false;
    key.ival[0] = c->enabled ? 1 : 0;
    return true;
}

static bool timeline_capture_camera(TimelineKey& key) {
    for (int i = 0; i < 3; i++) key.fval[i] = g_camera.position[i];
    key.fval[3] = timeline_wrap_angle(g_camera.yaw);
    key.fval[4] = g_camera.pitch;
    key.fval[5] = g_camera.fov_y;
    key.fval[6] = g_camera.near_z;
    key.fval[7] = g_camera.far_z;
    return true;
}

static bool timeline_capture_dirlight(TimelineKey& key) {
    Resource* dl = res_get(g_builtin_dirlight);
    if (!dl)
        return false;
    for (int i = 0; i < 3; i++) key.fval[i] = dl->light_pos[i];
    for (int i = 0; i < 3; i++) key.fval[3 + i] = dl->light_target[i];
    for (int i = 0; i < 3; i++) key.fval[6 + i] = dl->light_color[i];
    key.fval[9] = dl->light_intensity;
    return true;
}

bool timeline_capture_key(int track_index, int frame) {
    TimelineKey* key = timeline_set_key(track_index, frame);
    if (!key)
        return false;
    TimelineTrack& track = g_timeline_tracks[track_index];
    memset(key->ival, 0, sizeof(key->ival));
    memset(key->fval, 0, sizeof(key->fval));
    key->frame = timeline_clamp_frame(frame);

    bool ok = false;
    switch (track.kind) {
    case TIMELINE_TRACK_USER_VAR:          ok = timeline_capture_user_var(track, *key); break;
    case TIMELINE_TRACK_COMMAND_TRANSFORM: ok = timeline_capture_command_transform(track, *key); break;
    case TIMELINE_TRACK_COMMAND_ENABLED:   ok = timeline_capture_command_enabled(track, *key); break;
    case TIMELINE_TRACK_CAMERA:            ok = timeline_capture_camera(*key); break;
    case TIMELINE_TRACK_DIRLIGHT:          ok = timeline_capture_dirlight(*key); break;
    default: break;
    }
    if (!ok)
        timeline_delete_key(track_index, key->frame);
    else
        app_request_scene_render();
    return ok;
}

bool timeline_capture_if_tracked(TimelineTrackKind kind, const char* target, ResType value_type) {
    app_request_scene_render();
    int track_index = timeline_find_track(kind, target ? target : "", value_type);
    if (track_index < 0)
        return false;
    return timeline_capture_key(track_index, s_timeline_current_frame);
}

bool timeline_delete_key(int track_index, int frame) {
    if (track_index < 0 || track_index >= g_timeline_track_count)
        return false;
    TimelineTrack& track = g_timeline_tracks[track_index];
    int key_index = timeline_find_key_index(track, frame);
    if (key_index < 0)
        return false;
    for (int i = key_index; i < track.key_count - 1; i++)
        track.keys[i] = track.keys[i + 1];
    memset(&track.keys[--track.key_count], 0, sizeof(TimelineKey));
    app_request_scene_render();
    return true;
}


static UserCBSourceKind timeline_user_var_source_kind(const TimelineTrack& track) {
    int idx = timeline_user_var_index(track.target);
    if (idx < 0)
        return USER_CB_SOURCE_NONE;
    UserCBEntry& e = g_user_cb_entries[idx];
    if (e.source_kind == USER_CB_SOURCE_NONE && e.source != INVALID_HANDLE)
        return USER_CB_SOURCE_RESOURCE;
    return e.source_kind;
}

static bool timeline_sample_user_var_special_rotation(const TimelineTrack& track,
                                                      const TimelineKey& a, const TimelineKey& b,
                                                      float t, TimelineKey* out) {
    if (!out)
        return false;

    UserCBSourceKind source_kind = timeline_user_var_source_kind(track);
    if (source_kind == USER_CB_SOURCE_COMMAND_ROTATION) {
        TimelineQuat qa = timeline_quat_from_euler_xyz(a.fval);
        TimelineQuat qb = timeline_quat_from_euler_xyz(b.fval);
        TimelineQuat q = timeline_quat_slerp(qa, qb, t);
        Mat4 rot = timeline_mat4_from_quat(q);
        timeline_euler_xyz_from_mat4(rot, a.fval, out->fval);
        if (timeline_track_value_count(track) > 3)
            out->fval[3] = timeline_lerp(a.fval[3], b.fval[3], t);
        return true;
    }

    if (source_kind == USER_CB_SOURCE_CAMERA_ROTATION) {
        int n = timeline_track_value_count(track);
        if (n > 0) out->fval[0] = timeline_lerp_angle(a.fval[0], b.fval[0], t);
        for (int i = 1; i < n; i++)
            out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], t);
        return true;
    }

    return false;
}

static void timeline_sample_key(const TimelineTrack& track, TimelineKey* out) {
    if (!out || track.key_count <= 0)
        return;

    float sample_frame = s_timeline_interpolate_frames ? s_timeline_sample_frame : (float)s_timeline_current_frame;
    if (sample_frame < 0.0f) sample_frame = 0.0f;
    float max_sample_frame = (float)(s_timeline_length_frames - 1);
    if (sample_frame > max_sample_frame) sample_frame = max_sample_frame;

    int display_frame = (int)floorf(sample_frame + 0.0001f);
    int prev = -1;
    int next = -1;
    for (int i = 0; i < track.key_count; i++) {
        if ((float)track.keys[i].frame <= sample_frame)
            prev = i;
        if ((float)track.keys[i].frame >= sample_frame) {
            next = i;
            break;
        }
    }

    if (prev < 0) prev = next >= 0 ? next : 0;
    if (next < 0) next = prev;

    const TimelineKey& a = track.keys[prev];
    const TimelineKey& b = track.keys[next];
    *out = a;
    out->frame = display_frame;

    if (prev == next || timeline_track_uses_integral_values(track))
        return;

    int span = b.frame - a.frame;
    float t = span > 0 ? (sample_frame - (float)a.frame) / (float)span : 0.0f;
    t = clampf(t, 0.0f, 1.0f);

    if (track.kind == TIMELINE_TRACK_COMMAND_TRANSFORM) {
        timeline_sample_command_transform(a, b, t, out);
        return;
    }
    if (track.kind == TIMELINE_TRACK_CAMERA) {
        timeline_sample_camera(a, b, t, out);
        return;
    }
    if (track.kind == TIMELINE_TRACK_USER_VAR &&
        timeline_sample_user_var_special_rotation(track, a, b, t, out)) {
        return;
    }

    int n = timeline_track_value_count(track);
    for (int i = 0; i < n; i++)
        out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], t);
}

static void timeline_apply_user_var_to_source(UserCBEntry& e, const TimelineKey& key) {
    UserCBSourceKind source_kind = e.source_kind;
    if (source_kind == USER_CB_SOURCE_NONE && e.source != INVALID_HANDLE)
        source_kind = USER_CB_SOURCE_RESOURCE;

    if (source_kind == USER_CB_SOURCE_RESOURCE) {
        if (Resource* src = res_get(e.source)) {
            if (src->type == e.type) {
                int n = timeline_res_type_components(e.type);
                if (timeline_res_type_is_integral(e.type)) {
                    for (int i = 0; i < n; i++)
                        src->ival[i] = key.ival[i];
                } else {
                    for (int i = 0; i < n; i++)
                        src->fval[i] = key.fval[i];
                }
            }
        }
        return;
    }

    // Scene-source UserCB values are normally refreshed from their source every
    // frame. When such a variable has timeline keys, write the sampled value
    // back to the source too; otherwise the refresh would immediately overwrite
    // the keyed value and the track would feel locked.
    if (e.type != RES_FLOAT3 && e.type != RES_FLOAT4)
        return;

    if (source_kind == USER_CB_SOURCE_COMMAND_POSITION ||
        source_kind == USER_CB_SOURCE_COMMAND_ROTATION ||
        source_kind == USER_CB_SOURCE_COMMAND_SCALE) {
        Command* c = cmd_get(cmd_find_by_name(e.source_target));
        if (!c)
            return;
        float* dst = source_kind == USER_CB_SOURCE_COMMAND_POSITION ? c->pos :
                     source_kind == USER_CB_SOURCE_COMMAND_ROTATION ? c->rot : c->scale;
        for (int i = 0; i < 3; i++)
            dst[i] = key.fval[i];
        return;
    }

    if (source_kind == USER_CB_SOURCE_CAMERA_POSITION) {
        for (int i = 0; i < 3; i++)
            g_camera.position[i] = key.fval[i];
        return;
    }

    if (source_kind == USER_CB_SOURCE_CAMERA_ROTATION) {
        g_camera.yaw = timeline_wrap_angle(key.fval[0]);
        g_camera.pitch = clampf(key.fval[1], -1.50f, 1.50f);
        return;
    }

    if (source_kind == USER_CB_SOURCE_DIRLIGHT_POSITION ||
        source_kind == USER_CB_SOURCE_DIRLIGHT_TARGET) {
        Resource* dl = res_get(g_builtin_dirlight);
        if (!dl)
            return;
        float* dst = source_kind == USER_CB_SOURCE_DIRLIGHT_POSITION ? dl->light_pos : dl->light_target;
        for (int i = 0; i < 3; i++)
            dst[i] = key.fval[i];
        Vec3 pos = v3(dl->light_pos[0], dl->light_pos[1], dl->light_pos[2]);
        Vec3 target = v3(dl->light_target[0], dl->light_target[1], dl->light_target[2]);
        Vec3 dir = v3_norm(v3_sub(target, pos));
        dl->light_dir[0] = dir.x;
        dl->light_dir[1] = dir.y;
        dl->light_dir[2] = dir.z;
        return;
    }
}

static void timeline_apply_user_var(const TimelineTrack& track, const TimelineKey& key) {
    int idx = timeline_user_var_index(track.target);
    if (idx < 0)
        return;
    UserCBEntry& e = g_user_cb_entries[idx];
    int n = timeline_res_type_components(e.type);
    if (timeline_res_type_is_integral(e.type)) {
        for (int i = 0; i < n; i++)
            e.ival[i] = key.ival[i];
    } else {
        for (int i = 0; i < n; i++)
            e.fval[i] = key.fval[i];
    }
    timeline_apply_user_var_to_source(e, key);
}

static void timeline_apply_command_transform(const TimelineTrack& track, const TimelineKey& key) {
    Command* c = cmd_get(cmd_find_by_name(track.target));
    if (!c)
        return;
    for (int i = 0; i < 3; i++) c->pos[i] = key.fval[i];
    for (int i = 0; i < 3; i++) c->rot[i] = key.fval[3 + i];
    for (int i = 0; i < 3; i++) c->scale[i] = key.fval[6 + i];
}

static void timeline_apply_command_enabled(const TimelineTrack& track, const TimelineKey& key) {
    Command* c = cmd_get(cmd_find_by_name(track.target));
    if (c)
        c->enabled = key.ival[0] != 0;
}

static void timeline_apply_camera(const TimelineKey& key) {
    for (int i = 0; i < 3; i++) g_camera.position[i] = key.fval[i];
    g_camera.yaw = timeline_wrap_angle(key.fval[3]);
    g_camera.pitch = clampf(key.fval[4], -1.50f, 1.50f);
    g_camera.fov_y = clampf(key.fval[5], 0.10f, 2.80f);
    g_camera.near_z = key.fval[6] < 0.0001f ? 0.0001f : key.fval[6];
    g_camera.far_z = key.fval[7];
    if (g_camera.far_z <= g_camera.near_z + 0.001f)
        g_camera.far_z = g_camera.near_z + 0.001f;
}

static void timeline_apply_dirlight(const TimelineKey& key) {
    Resource* dl = res_get(g_builtin_dirlight);
    if (!dl)
        return;
    for (int i = 0; i < 3; i++) dl->light_pos[i] = key.fval[i];
    for (int i = 0; i < 3; i++) dl->light_target[i] = key.fval[3 + i];
    for (int i = 0; i < 3; i++) dl->light_color[i] = key.fval[6 + i];
    dl->light_intensity = key.fval[9];
}

void timeline_apply_current() {
    for (int i = 0; i < g_timeline_track_count; i++) {
        TimelineTrack& track = g_timeline_tracks[i];
        if (!track.active || !track.enabled || track.key_count <= 0)
            continue;

        TimelineKey sampled = {};
        timeline_sample_key(track, &sampled);
        switch (track.kind) {
        case TIMELINE_TRACK_USER_VAR:          timeline_apply_user_var(track, sampled); break;
        case TIMELINE_TRACK_COMMAND_TRANSFORM: timeline_apply_command_transform(track, sampled); break;
        case TIMELINE_TRACK_COMMAND_ENABLED:   timeline_apply_command_enabled(track, sampled); break;
        case TIMELINE_TRACK_CAMERA:            timeline_apply_camera(sampled); break;
        case TIMELINE_TRACK_DIRLIGHT:          timeline_apply_dirlight(sampled); break;
        default: break;
        }
    }
}

void timeline_write_project(FILE* f) {
    if (!f)
        return;

    fprintf(f, "\ntimeline\n");
    fprintf(f, "timeline_settings %d %d %d 0 %d %d %d\n",
            s_timeline_fps, s_timeline_length_frames,
            s_timeline_current_frame, s_timeline_loop ? 1 : 0,
            s_timeline_enabled ? 1 : 0,
            s_timeline_interpolate_frames ? 1 : 0);

    for (int i = 0; i < g_timeline_track_count; i++) {
        const TimelineTrack& track = g_timeline_tracks[i];
        if (!track.active)
            continue;

        fprintf(f, "timeline_track %s %s %s %d %d\n",
                timeline_track_kind_token(track.kind), track.target,
                res_type_str(track.value_type), track.key_count,
                track.enabled ? 1 : 0);

        int n = timeline_track_value_count(track);
        bool integral = timeline_track_uses_integral_values(track);
        for (int k = 0; k < track.key_count; k++) {
            const TimelineKey& key = track.keys[k];
            fprintf(f, "timeline_key %d", key.frame);
            if (integral) {
                for (int v = 0; v < n; v++)
                    fprintf(f, " %d", key.ival[v]);
            } else {
                for (int v = 0; v < n; v++)
                    fprintf(f, " %.9g", key.fval[v]);
            }
            fprintf(f, "\n");
        }
    }
    fprintf(f, "end_timeline\n");
}
