#include "timeline.h"
#include "commands.h"
#include "resources.h"
#include "user_cb.h"
#include "ui.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

struct TimelineState {
    bool active;
    bool enabled;
    char name[MAX_NAME];
    int fps;
    int length_frames;
    int current_frame;
    float sample_frame;
    bool interpolate_frames;
    int track_count;
    TimelineTrack tracks[MAX_TIMELINE_TRACKS];
};

static TimelineState s_timelines[MAX_TIMELINES] = {};
static int   s_timeline_count = 0;
static int   s_timeline_current_index = 0; // editor-selected timeline.
static int   s_timeline_playback_index = 0; // timeline currently sampled by sequence playback.
static bool  s_timeline_setting_scene_time = false;
static bool  s_timeline_enabled = false; // global runtime enable.
static bool  s_timeline_loop = false;    // global sequence loop.

TimelineTrack* g_timeline_tracks = nullptr;
int            g_timeline_track_count = 0;

static const float TIMELINE_PI     = 3.14159265358979323846f;
static const float TIMELINE_TWO_PI = 6.28318530717958647692f;

static float timeline_wrap_angle(float a) {
    while (a > TIMELINE_PI) a -= TIMELINE_TWO_PI;
    while (a < -TIMELINE_PI) a += TIMELINE_TWO_PI;
    return a;
}

static float timeline_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static float timeline_lerp_angle(float a, float b, float t) {
    return a + timeline_wrap_angle(b - a) * t;
}

static void timeline_init_state(TimelineState& tl, int index, const char* name) {
    memset(&tl, 0, sizeof(tl));
    tl.active = true;
    tl.enabled = true;
    tl.fps = 24;
    tl.length_frames = 240;
    tl.current_frame = 0;
    tl.sample_frame = 0.0f;
    tl.interpolate_frames = false;
    tl.track_count = 0;
    if (name && name[0]) {
        strncpy(tl.name, name, MAX_NAME - 1);
        tl.name[MAX_NAME - 1] = '\0';
    } else {
        snprintf(tl.name, sizeof(tl.name), "Timeline_%d", index + 1);
    }
    for (int i = 0; tl.name[i]; i++) {
        if (tl.name[i] == ' ' || tl.name[i] == '\t')
            tl.name[i] = '_';
    }
}

static void timeline_sync_public_tracks() {
    if (s_timeline_count <= 0 || s_timeline_current_index < 0 || s_timeline_current_index >= s_timeline_count) {
        g_timeline_tracks = nullptr;
        g_timeline_track_count = 0;
        return;
    }
    // Public track pointers are editor-facing. Playback has its own sampled
    // timeline index so the sequence can advance without stealing the UI
    // selection from the combo box.
    TimelineState& tl = s_timelines[s_timeline_current_index];
    g_timeline_tracks = tl.tracks;
    g_timeline_track_count = tl.track_count;
}

static void timeline_ensure_one() {
    if (s_timeline_count > 0) {
        timeline_sync_public_tracks();
        return;
    }
    s_timeline_count = 1;
    s_timeline_current_index = 0;
    s_timeline_playback_index = 0;
    timeline_init_state(s_timelines[0], 0, "Timeline_1");
    timeline_sync_public_tracks();
}

static TimelineState& timeline_current_state() {
    timeline_ensure_one();
    return s_timelines[s_timeline_current_index];
}

static TimelineState* timeline_state_at(int index) {
    timeline_ensure_one();
    if (index < 0 || index >= s_timeline_count)
        return nullptr;
    return &s_timelines[index];
}

static TimelineState& timeline_playback_state() {
    timeline_ensure_one();
    if (s_timeline_playback_index < 0 || s_timeline_playback_index >= s_timeline_count)
        s_timeline_playback_index = s_timeline_current_index;
    if (s_timeline_playback_index < 0 || s_timeline_playback_index >= s_timeline_count)
        s_timeline_playback_index = 0;
    return s_timelines[s_timeline_playback_index];
}

static int timeline_clamp_frame_for(const TimelineState& tl, int frame) {
    if (frame < 0) frame = 0;
    if (frame >= tl.length_frames)
        frame = tl.length_frames - 1;
    return frame;
}

static float timeline_duration_seconds(const TimelineState& tl) {
    if (!tl.enabled || tl.fps <= 0 || tl.length_frames <= 0)
        return 0.0f;
    // length_frames is a frame count, not the index of the last frame. A 240
    // frame timeline at 24 fps lasts 10 seconds; frame 239 is visible for the
    // final 1/24s before the next enabled timeline starts.
    return (float)tl.length_frames / (float)tl.fps;
}

static float timeline_prefix_seconds(int index) {
    float t = 0.0f;
    if (index < 0) return 0.0f;
    if (index > s_timeline_count) index = s_timeline_count;
    for (int i = 0; i < index; i++)
        t += timeline_duration_seconds(s_timelines[i]);
    return t;
}

static void timeline_sync_scene_time_from_current() {
    if (s_timeline_setting_scene_time)
        return;
    TimelineState& tl = timeline_current_state();
    if (!tl.enabled || tl.fps <= 0) {
        app_request_scene_render();
        return;
    }
    float seconds = timeline_prefix_seconds(s_timeline_current_index) +
                    (float)tl.current_frame / (float)tl.fps;
    app_set_scene_time(seconds);
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
        return 10;
    case TIMELINE_TRACK_COMMAND_ENABLED:
        return 1;
    case TIMELINE_TRACK_CAMERA:
        return 9;
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
    memset(s_timelines, 0, sizeof(s_timelines));
    s_timeline_count = 1;
    s_timeline_current_index = 0;
    s_timeline_playback_index = 0;
    timeline_init_state(s_timelines[0], 0, "Timeline_1");
    s_timeline_setting_scene_time = false;
    s_timeline_enabled = false;
    s_timeline_loop = false;
    timeline_sync_public_tracks();
}

int timeline_count() {
    timeline_ensure_one();
    return s_timeline_count;
}

int timeline_current_index() {
    timeline_ensure_one();
    return s_timeline_current_index;
}

int timeline_playback_index() {
    timeline_ensure_one();
    return s_timeline_playback_index;
}

bool timeline_sync_editor_to_playback() {
    timeline_ensure_one();
    if (s_timeline_playback_index < 0 || s_timeline_playback_index >= s_timeline_count)
        return false;
    if (s_timeline_current_index == s_timeline_playback_index) {
        timeline_sync_public_tracks();
        return true;
    }
    s_timeline_current_index = s_timeline_playback_index;
    timeline_sync_public_tracks();
    return true;
}

bool timeline_set_current_index(int index) {
    timeline_ensure_one();
    if (index < 0 || index >= s_timeline_count)
        return false;
    if (s_timeline_current_index == index) {
        timeline_sync_public_tracks();
        return true;
    }
    s_timeline_current_index = index;
    timeline_sync_public_tracks();
    timeline_sync_scene_time_from_current();
    return true;
}

int timeline_add(const char* name) {
    timeline_ensure_one();
    if (s_timeline_count >= MAX_TIMELINES)
        return -1;
    int index = s_timeline_count++;
    timeline_init_state(s_timelines[index], index, name);
    s_timeline_current_index = index;
    // New timelines are selected for editing immediately; playback will follow
    // after scene time is synced below if the global timeline runtime is active.
    timeline_sync_public_tracks();
    timeline_sync_scene_time_from_current();
    return index;
}

bool timeline_delete(int index) {
    timeline_ensure_one();
    if (s_timeline_count <= 1 || index < 0 || index >= s_timeline_count)
        return false;

    int old_current = s_timeline_current_index;
    int old_playback = s_timeline_playback_index;
    for (int i = index; i < s_timeline_count - 1; i++) {
        s_timelines[i] = s_timelines[i + 1];
        if (!s_timelines[i].name[0])
            snprintf(s_timelines[i].name, sizeof(s_timelines[i].name), "Timeline_%d", i + 1);
    }
    memset(&s_timelines[s_timeline_count - 1], 0, sizeof(TimelineState));
    s_timeline_count--;

    if (old_current > index)
        s_timeline_current_index = old_current - 1;
    else if (old_current == index)
        s_timeline_current_index = index < s_timeline_count ? index : s_timeline_count - 1;
    else
        s_timeline_current_index = old_current;

    if (old_playback > index)
        s_timeline_playback_index = old_playback - 1;
    else if (old_playback == index)
        s_timeline_playback_index = s_timeline_current_index;
    else
        s_timeline_playback_index = old_playback;

    if (s_timeline_current_index < 0) s_timeline_current_index = 0;
    if (s_timeline_current_index >= s_timeline_count) s_timeline_current_index = s_timeline_count - 1;
    if (s_timeline_playback_index < 0) s_timeline_playback_index = 0;
    if (s_timeline_playback_index >= s_timeline_count) s_timeline_playback_index = s_timeline_count - 1;

    timeline_sync_public_tracks();
    timeline_sync_scene_time_from_current();
    app_request_scene_render();
    return true;
}

const char* timeline_name(int index) {
    TimelineState* tl = timeline_state_at(index);
    return tl ? tl->name : "";
}

void timeline_set_name(int index, const char* name) {
    TimelineState* tl = timeline_state_at(index);
    if (!tl || !name || !name[0])
        return;
    strncpy(tl->name, name, MAX_NAME - 1);
    tl->name[MAX_NAME - 1] = '\0';
    for (int i = 0; tl->name[i]; i++) {
        if (tl->name[i] == ' ' || tl->name[i] == '\t')
            tl->name[i] = '_';
    }
}

bool timeline_timeline_enabled(int index) {
    TimelineState* tl = timeline_state_at(index);
    return tl ? tl->enabled : false;
}

int timeline_enabled_count() {
    timeline_ensure_one();
    int n = 0;
    for (int i = 0; i < s_timeline_count; i++)
        if (s_timelines[i].enabled)
            n++;
    return n;
}

void timeline_set_timeline_enabled(int index, bool enabled) {
    TimelineState* tl = timeline_state_at(index);
    if (!tl)
        return;
    if (!enabled && tl->enabled && timeline_enabled_count() <= 1)
        return; // Keep at least one playable timeline.
    tl->enabled = enabled;
    timeline_sync_scene_time_from_current();
    app_request_scene_render();
}

float timeline_sequence_duration_seconds() {
    timeline_ensure_one();
    float total = 0.0f;
    for (int i = 0; i < s_timeline_count; i++)
        total += timeline_duration_seconds(s_timelines[i]);
    return total;
}

bool timeline_current_has_keys() {
    // Runtime query: check the sampled sequence timeline, not the editor
    // selection. Otherwise selecting a different clip in the UI can prevent the
    // actually-playing clip from being applied.
    TimelineState& tl = timeline_playback_state();
    if (!tl.enabled)
        return false;
    for (int i = 0; i < tl.track_count; i++) {
        if (tl.tracks[i].active && tl.tracks[i].enabled && tl.tracks[i].key_count > 0)
            return true;
    }
    return false;
}

int timeline_fps() {
    return timeline_current_state().fps;
}

void timeline_set_fps(int fps) {
    if (fps < 1) fps = 1;
    if (fps > 240) fps = 240;
    TimelineState& tl = timeline_current_state();
    tl.fps = fps;
    timeline_update(app_scene_time());
    app_request_scene_render();
}

int timeline_length_frames() {
    return timeline_current_state().length_frames;
}

static void timeline_prune_keys_to_length(TimelineState& tl) {
    for (int t = 0; t < tl.track_count; t++) {
        TimelineTrack& track = tl.tracks[t];
        if (!track.active)
            continue;
        int write = 0;
        for (int k = 0; k < track.key_count; k++) {
            if (track.keys[k].frame < tl.length_frames)
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
    TimelineState& tl = timeline_current_state();
    tl.length_frames = frames;
    timeline_prune_keys_to_length(tl);
    int old_frame = tl.current_frame;
    tl.current_frame = timeline_clamp_frame_for(tl, tl.current_frame);
    tl.sample_frame = (float)tl.current_frame;
    if (tl.current_frame != old_frame)
        timeline_sync_scene_time_from_current();
    else
        app_request_scene_render();
}

int timeline_current_frame() {
    return timeline_current_state().current_frame;
}

void timeline_set_current_frame(int frame) {
    TimelineState& tl = timeline_current_state();
    tl.current_frame = timeline_clamp_frame_for(tl, frame);
    tl.sample_frame = (float)tl.current_frame;
    timeline_sync_scene_time_from_current();
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
    return timeline_current_state().interpolate_frames;
}

void timeline_set_interpolate_frames(bool enabled) {
    TimelineState& tl = timeline_current_state();
    tl.interpolate_frames = enabled;
    tl.sample_frame = enabled ? tl.sample_frame : (float)tl.current_frame;
    app_request_scene_render();
}

int timeline_play_dir() {
    return 0;
}

void timeline_set_play_dir(int dir) {
    (void)dir;
}

void timeline_update(float scene_time_seconds) {
    timeline_ensure_one();
    if (scene_time_seconds < 0.0f)
        return;

    float total = timeline_sequence_duration_seconds();
    if (total <= 0.0f) {
        for (int i = 0; i < s_timeline_count; i++) {
            if (!s_timelines[i].enabled)
                continue;
            s_timeline_playback_index = i;
            s_timelines[i].current_frame = 0;
            s_timelines[i].sample_frame = 0.0f;
            return;
        }
        return;
    }

    float sequence_time = scene_time_seconds;
    if (sequence_time < 0.0f) sequence_time = 0.0f;
    if (s_timeline_loop && total > 0.0f) {
        while (sequence_time >= total)
            sequence_time -= total;
    } else if (sequence_time > total) {
        sequence_time = total;
    }

    float accum = 0.0f;
    int selected = -1;
    float local_seconds = 0.0f;
    int last_enabled = -1;
    for (int i = 0; i < s_timeline_count; i++) {
        if (!s_timelines[i].enabled)
            continue;
        last_enabled = i;
        float dur = timeline_duration_seconds(s_timelines[i]);
        if (dur <= 0.0f) {
            if (selected < 0 && sequence_time <= accum) {
                selected = i;
                local_seconds = 0.0f;
                break;
            }
            continue;
        }
        // Half-open clips: [start, end). Exact boundaries belong to the next
        // enabled timeline. This avoids frame 0 of clip N resolving to the last
        // frame of clip N-1.
        if (selected < 0 && sequence_time < accum + dur) {
            selected = i;
            local_seconds = sequence_time - accum;
            if (local_seconds < 0.0f) local_seconds = 0.0f;
            break;
        }
        accum += dur;
    }
    if (selected < 0 && last_enabled >= 0) {
        selected = last_enabled;
        local_seconds = timeline_duration_seconds(s_timelines[selected]);
    }
    if (selected < 0)
        return;

    // Playback sampling is deliberately separate from editor selection. The UI
    // combo owns s_timeline_current_index/g_timeline_tracks; sequence playback
    // owns s_timeline_playback_index.
    s_timeline_playback_index = selected;
    TimelineState& tl = s_timelines[selected];
    if (tl.fps <= 0)
        return;

    float frame_f = local_seconds * (float)tl.fps;
    if (frame_f < 0.0f) frame_f = 0.0f;
    float max_frame_f = (float)(tl.length_frames - 1);
    if (frame_f > max_frame_f) frame_f = max_frame_f;

    int frame = (int)floorf(frame_f + 0.0001f);
    tl.current_frame = timeline_clamp_frame_for(tl, frame);

    // The visible/current frame remains discrete, but when enabled the sampler
    // evaluates at the true fractional timeline position. This keeps low-FPS
    // timelines intentionally steppy by default while allowing smooth in-between
    // evaluation during playback.
    tl.sample_frame = tl.interpolate_frames ? frame_f : (float)tl.current_frame;
}

static int timeline_find_track_in(const TimelineState& tl, TimelineTrackKind kind, const char* target, ResType value_type) {
    (void)value_type;
    if (!target) target = "";
    for (int i = 0; i < tl.track_count; i++) {
        const TimelineTrack& t = tl.tracks[i];
        if (!t.active || t.kind != kind)
            continue;
        if (strcmp(t.target, target) != 0)
            continue;
        if (kind == TIMELINE_TRACK_USER_VAR)
            return i;
        return i;
    }
    return -1;
}

int timeline_find_track(TimelineTrackKind kind, const char* target, ResType value_type) {
    return timeline_find_track_in(timeline_current_state(), kind, target, value_type);
}

int timeline_add_track(TimelineTrackKind kind, const char* target, ResType value_type) {
    if (kind == TIMELINE_TRACK_NONE)
        return -1;
    TimelineState& tl = timeline_current_state();
    if (!target) target = "";
    if (kind == TIMELINE_TRACK_USER_VAR) {
        int user_idx = timeline_user_var_index(target);
        if (user_idx >= 0)
            value_type = g_user_cb_entries[user_idx].type;
    }
    int existing = timeline_find_track_in(tl, kind, target, value_type);
    if (existing >= 0) {
        if (kind == TIMELINE_TRACK_USER_VAR)
            tl.tracks[existing].value_type = value_type;
        timeline_sync_public_tracks();
        return existing;
    }
    if (tl.track_count >= MAX_TIMELINE_TRACKS)
        return -1;

    int index = tl.track_count++;
    TimelineTrack& t = tl.tracks[index];
    memset(&t, 0, sizeof(t));
    t.active = true;
    t.enabled = true;
    t.kind = kind;
    t.value_type = value_type;
    strncpy(t.target, target, MAX_NAME - 1);
    t.target[MAX_NAME - 1] = '\0';
    timeline_sync_public_tracks();
    return index;
}

static void timeline_delete_track_in(TimelineState& tl, int track_index) {
    if (track_index < 0 || track_index >= tl.track_count)
        return;
    for (int i = track_index; i < tl.track_count - 1; i++)
        tl.tracks[i] = tl.tracks[i + 1];
    memset(&tl.tracks[--tl.track_count], 0, sizeof(TimelineTrack));
}

void timeline_delete_track(int track_index) {
    TimelineState& tl = timeline_current_state();
    timeline_delete_track_in(tl, track_index);
    timeline_sync_public_tracks();
}

void timeline_delete_tracks_for_command(const char* target) {
    if (!target || !target[0])
        return;
    timeline_ensure_one();
    for (int ti = 0; ti < s_timeline_count; ti++) {
        TimelineState& tl = s_timelines[ti];
        for (int i = tl.track_count - 1; i >= 0; i--) {
            TimelineTrack& track = tl.tracks[i];
            if (!track.active)
                continue;
            bool command_track = track.kind == TIMELINE_TRACK_COMMAND_TRANSFORM ||
                                 track.kind == TIMELINE_TRACK_COMMAND_ENABLED;
            if (command_track && strcmp(track.target, target) == 0)
                timeline_delete_track_in(tl, i);
        }
    }
    timeline_sync_public_tracks();
}

void timeline_rename_tracks_for_command(const char* old_target, const char* new_target) {
    if (!old_target || !old_target[0] || !new_target || !new_target[0] ||
        strcmp(old_target, new_target) == 0)
        return;

    timeline_ensure_one();
    for (int ti = 0; ti < s_timeline_count; ti++) {
        TimelineState& tl = s_timelines[ti];
        for (int i = 0; i < tl.track_count; i++) {
            TimelineTrack& track = tl.tracks[i];
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
}

void timeline_delete_tracks_for_user_var(const char* target) {
    if (!target || !target[0])
        return;
    timeline_ensure_one();
    for (int ti = 0; ti < s_timeline_count; ti++) {
        TimelineState& tl = s_timelines[ti];
        for (int i = tl.track_count - 1; i >= 0; i--) {
            TimelineTrack& track = tl.tracks[i];
            if (!track.active || track.kind != TIMELINE_TRACK_USER_VAR)
                continue;
            if (strcmp(track.target, target) == 0)
                timeline_delete_track_in(tl, i);
        }
    }
    timeline_sync_public_tracks();
}

void timeline_rename_tracks_for_user_var(const char* old_target, const char* new_target) {
    if (!old_target || !old_target[0] || !new_target || !new_target[0] ||
        strcmp(old_target, new_target) == 0)
        return;

    timeline_ensure_one();
    for (int ti = 0; ti < s_timeline_count; ti++) {
        TimelineState& tl = s_timelines[ti];
        for (int i = 0; i < tl.track_count; i++) {
            TimelineTrack& track = tl.tracks[i];
            if (!track.active || track.kind != TIMELINE_TRACK_USER_VAR)
                continue;
            if (strcmp(track.target, old_target) != 0)
                continue;
            strncpy(track.target, new_target, MAX_NAME - 1);
            track.target[MAX_NAME - 1] = '\0';
            int user_idx = timeline_user_var_index(new_target);
            if (user_idx >= 0)
                track.value_type = g_user_cb_entries[user_idx].type;
        }
    }
}

bool timeline_track_target_exists(const TimelineTrack& track) {
    switch (track.kind) {
    case TIMELINE_TRACK_USER_VAR:
    {
        int idx = timeline_user_var_index(track.target);
        return idx >= 0 && g_user_cb_entries[idx].type == track.value_type;
    }
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
    TimelineState& tl = timeline_current_state();
    if (track_index < 0 || track_index >= tl.track_count)
        return nullptr;

    TimelineTrack& track = tl.tracks[track_index];
    frame = timeline_clamp_frame_for(tl, frame);
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
    quat_to_array(quat_from_array(c->rotq), &key.fval[3]);
    for (int i = 0; i < 3; i++) key.fval[7 + i] = c->scale[i];
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
    camera_sync_euler_from_quat(&g_camera);
    for (int i = 0; i < 3; i++) key.fval[i] = g_camera.position[i];
    key.fval[3] = timeline_wrap_angle(g_camera.yaw);
    key.fval[4] = g_camera.pitch;
    key.fval[5] = g_camera.fov_y;
    key.fval[6] = g_camera.near_z;
    key.fval[7] = g_camera.far_z;
    key.fval[8] = timeline_wrap_angle(g_camera.roll);
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
    TimelineState& tl = timeline_current_state();
    TimelineTrack& track = tl.tracks[track_index];
    memset(key->ival, 0, sizeof(key->ival));
    memset(key->fval, 0, sizeof(key->fval));
    key->frame = timeline_clamp_frame_for(tl, frame);

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
    return timeline_capture_key(track_index, timeline_current_state().current_frame);
}

bool timeline_delete_key(int track_index, int frame) {
    TimelineState& tl = timeline_current_state();
    if (track_index < 0 || track_index >= tl.track_count)
        return false;
    TimelineTrack& track = tl.tracks[track_index];
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
        if (track.value_type == RES_FLOAT4) {
            quat_to_array(quat_slerp(quat_from_array(a.fval), quat_from_array(b.fval), t), out->fval);
            return true;
        }

        Quat q = quat_slerp(
            quat_from_euler_xyz(v3(a.fval[0], a.fval[1], a.fval[2])),
            quat_from_euler_xyz(v3(b.fval[0], b.fval[1], b.fval[2])),
            t);
        quat_to_euler_xyz(q, a.fval, out->fval);
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

static void timeline_sample_command_transform(const TimelineKey& a, const TimelineKey& b,
                                              float t, TimelineKey* out) {
    if (!out)
        return;

    for (int i = 0; i < 3; i++)
        out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], t);

    Quat q = quat_slerp(quat_from_array(&a.fval[3]), quat_from_array(&b.fval[3]), t);
    quat_to_array(q, &out->fval[3]);

    for (int i = 7; i < 10; i++)
        out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], t);
}

static void timeline_sample_camera(const TimelineKey& a, const TimelineKey& b,
                                   float t, TimelineKey* out) {
    if (!out)
        return;

    for (int i = 0; i < 3; i++)
        out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], t);
    out->fval[3] = timeline_lerp_angle(a.fval[3], b.fval[3], t); // yaw wraps.
    out->fval[4] = timeline_lerp(a.fval[4], b.fval[4], t);       // pitch is not clamped: camera can loop.
    out->fval[5] = timeline_lerp(a.fval[5], b.fval[5], t);
    out->fval[6] = timeline_lerp(a.fval[6], b.fval[6], t);
    out->fval[7] = timeline_lerp(a.fval[7], b.fval[7], t);
    out->fval[8] = timeline_lerp_angle(a.fval[8], b.fval[8], t); // roll wraps.
}

static void timeline_sample_key(const TimelineState& tl, const TimelineTrack& track, TimelineKey* out) {
    if (!out || track.key_count <= 0)
        return;

    float sample_frame = tl.interpolate_frames ? tl.sample_frame : (float)tl.current_frame;
    if (sample_frame < 0.0f) sample_frame = 0.0f;
    float max_sample_frame = (float)(tl.length_frames - 1);
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
        if (source_kind == USER_CB_SOURCE_COMMAND_ROTATION) {
            if (e.type == RES_FLOAT4)
                quat_to_array(quat_from_array(key.fval), c->rotq);
            else
                quat_to_array(quat_from_euler_xyz(v3(key.fval[0], key.fval[1], key.fval[2])), c->rotq);
            return;
        }
        float* dst = source_kind == USER_CB_SOURCE_COMMAND_POSITION ? c->pos :
                     c->scale;
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
        camera_set_euler(&g_camera,
                         timeline_wrap_angle(key.fval[0]),
                         key.fval[1],
                         timeline_wrap_angle(key.fval[2]));
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
    quat_to_array(quat_from_array(&key.fval[3]), c->rotq);
    for (int i = 0; i < 3; i++) c->scale[i] = key.fval[7 + i];
}

static void timeline_apply_command_enabled(const TimelineTrack& track, const TimelineKey& key) {
    Command* c = cmd_get(cmd_find_by_name(track.target));
    if (c)
        c->enabled = key.ival[0] != 0;
}

static void timeline_apply_camera(const TimelineKey& key) {
    for (int i = 0; i < 3; i++) g_camera.position[i] = key.fval[i];
    camera_set_euler(&g_camera,
                     timeline_wrap_angle(key.fval[3]),
                     key.fval[4],
                     timeline_wrap_angle(key.fval[8]));
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
    TimelineState& tl = timeline_playback_state();
    if (!tl.enabled)
        return;
    for (int i = 0; i < tl.track_count; i++) {
        TimelineTrack& track = tl.tracks[i];
        if (!track.active || !track.enabled || track.key_count <= 0)
            continue;

        TimelineKey sampled = {};
        timeline_sample_key(tl, track, &sampled);
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

static void timeline_write_tracks(FILE* f, const TimelineState& tl) {
    for (int i = 0; i < tl.track_count; i++) {
        const TimelineTrack& track = tl.tracks[i];
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
}

void timeline_write_project(FILE* f) {
    if (!f)
        return;
    timeline_ensure_one();

    fprintf(f, "\ntimeline\n");
    fprintf(f, "timeline_global %d %d %d\n",
            s_timeline_current_index,
            s_timeline_loop ? 1 : 0,
            s_timeline_enabled ? 1 : 0);

    for (int i = 0; i < s_timeline_count; i++) {
        const TimelineState& tl = s_timelines[i];
        fprintf(f, "timeline_clip %s %d %d %d %d %d\n",
                tl.name[0] ? tl.name : "Timeline",
                tl.fps, tl.length_frames, tl.current_frame,
                tl.enabled ? 1 : 0,
                tl.interpolate_frames ? 1 : 0);
        timeline_write_tracks(f, tl);
        fprintf(f, "end_timeline_clip\n");
    }
    fprintf(f, "end_timeline\n");
}
