#include "timeline.h"
#include "commands.h"
#include "resources.h"
#include "user_cb.h"
#include "ui.h"
#include <string.h>
#include <stdlib.h>

TimelineTrack g_timeline_tracks[MAX_TIMELINE_TRACKS] = {};
int           g_timeline_track_count = 0;

static int   s_timeline_fps = 24;
static int   s_timeline_length_frames = 240;
static int   s_timeline_current_frame = 0;
static bool  s_timeline_setting_scene_time = false;
static bool  s_timeline_enabled = false;
static bool  s_timeline_loop = false;

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
    s_timeline_setting_scene_time = false;
    s_timeline_enabled = false;
    s_timeline_loop = false;
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

int timeline_play_dir() {
    return 0;
}

void timeline_set_play_dir(int dir) {
    (void)dir;
}

void timeline_update(float scene_time_seconds) {
    if (s_timeline_fps <= 0 || scene_time_seconds < 0.0f)
        return;
    int frame = (int)floorf(scene_time_seconds * (float)s_timeline_fps + 0.0001f);
    s_timeline_setting_scene_time = true;
    timeline_set_current_frame(frame);
    s_timeline_setting_scene_time = false;
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
    key.fval[3] = g_camera.yaw;
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

static void timeline_sample_key(const TimelineTrack& track, TimelineKey* out) {
    if (!out || track.key_count <= 0)
        return;

    int frame = s_timeline_current_frame;
    int prev = -1;
    int next = -1;
    for (int i = 0; i < track.key_count; i++) {
        if (track.keys[i].frame <= frame)
            prev = i;
        if (track.keys[i].frame >= frame) {
            next = i;
            break;
        }
    }

    if (prev < 0) prev = next >= 0 ? next : 0;
    if (next < 0) next = prev;

    const TimelineKey& a = track.keys[prev];
    const TimelineKey& b = track.keys[next];
    *out = a;
    out->frame = frame;

    if (prev == next || timeline_track_uses_integral_values(track))
        return;

    int span = b.frame - a.frame;
    float t = span > 0 ? (float)(frame - a.frame) / (float)span : 0.0f;
    int n = timeline_track_value_count(track);
    for (int i = 0; i < n; i++)
        out->fval[i] = a.fval[i] + (b.fval[i] - a.fval[i]) * t;
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
        if (Resource* src = res_get(e.source)) {
            if (src->type == e.type) {
                for (int i = 0; i < n; i++)
                    src->ival[i] = key.ival[i];
            }
        }
    } else {
        for (int i = 0; i < n; i++)
            e.fval[i] = key.fval[i];
        if (Resource* src = res_get(e.source)) {
            if (src->type == e.type) {
                for (int i = 0; i < n; i++)
                    src->fval[i] = key.fval[i];
            }
        }
    }
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
    g_camera.yaw = key.fval[3];
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
    fprintf(f, "timeline_settings %d %d %d 0 %d %d\n",
            s_timeline_fps, s_timeline_length_frames,
            s_timeline_current_frame, s_timeline_loop ? 1 : 0,
            s_timeline_enabled ? 1 : 0);

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
