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
    TimelineCameraFeelSettings camera_feel;
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
static bool  s_timeline_recording = false; // editor auto-key capture.

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

static int timeline_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float timeline_lerp_angle(float a, float b, float t) {
    return a + timeline_wrap_angle(b - a) * t;
}

static int timeline_clamp_interpolation_mode(int mode) {
    if (mode < TIMELINE_INTERP_STEP)
        return TIMELINE_INTERP_STEP;
    if (mode > TIMELINE_INTERP_CUBIC)
        return TIMELINE_INTERP_CUBIC;
    return mode;
}

static float timeline_clamp_tangent_scale(float scale) {
    return clampf(scale, 0.0f, 4.0f);
}

static float timeline_ease_t(int mode, float t) {
    t = clampf(t, 0.0f, 1.0f);
    if (mode == TIMELINE_INTERP_QUADRATIC) {
        if (t < 0.5f)
            return 2.0f * t * t;
        float u = 1.0f - t;
        return 1.0f - 2.0f * u * u;
    }
    return t;
}

void timeline_camera_feel_apply_preset(TimelineCameraFeelSettings* settings, int preset) {
    if (!settings)
        return;

    bool enabled = settings->enabled;
    int seed = settings->seed;
    if (seed == 0)
        seed = 1337;
    memset(settings, 0, sizeof(*settings));
    settings->enabled = enabled;
    settings->preset = timeline_clampi(preset, 0, TIMELINE_CAMERA_FEEL_PRESET_COUNT - 1);
    settings->seed = seed;
    settings->fade_in_frames = 12;
    settings->fade_out_frames = 12;

    switch (settings->preset) {
    case TIMELINE_CAMERA_FEEL_OPERATOR:
        settings->amount = 0.65f;
        settings->frequency = 1.00f;
        settings->roughness = 0.45f;
        settings->position_amount = 0.035f;
        settings->rotation_amount = 0.0070f;
        settings->roll_amount = 0.0030f;
        settings->micro_amount = 0.12f;
        settings->breathing_amount = 0.0035f;
        break;
    case TIMELINE_CAMERA_FEEL_HANDHELD:
        settings->amount = 0.85f;
        settings->frequency = 1.25f;
        settings->roughness = 0.68f;
        settings->position_amount = 0.075f;
        settings->rotation_amount = 0.0140f;
        settings->roll_amount = 0.0070f;
        settings->micro_amount = 0.30f;
        settings->breathing_amount = 0.0060f;
        break;
    case TIMELINE_CAMERA_FEEL_DOCUMENTARY:
        settings->amount = 0.75f;
        settings->frequency = 1.40f;
        settings->roughness = 0.58f;
        settings->position_amount = 0.055f;
        settings->rotation_amount = 0.0100f;
        settings->roll_amount = 0.0090f;
        settings->micro_amount = 0.25f;
        settings->breathing_amount = 0.0045f;
        break;
    case TIMELINE_CAMERA_FEEL_AGGRESSIVE:
        settings->amount = 1.20f;
        settings->frequency = 2.40f;
        settings->roughness = 0.85f;
        settings->position_amount = 0.180f;
        settings->rotation_amount = 0.0350f;
        settings->roll_amount = 0.0280f;
        settings->micro_amount = 0.65f;
        settings->breathing_amount = 0.0120f;
        settings->fade_in_frames = 8;
        settings->fade_out_frames = 8;
        break;
    case TIMELINE_CAMERA_FEEL_SUBTLE:
    default:
        settings->amount = 0.45f;
        settings->frequency = 0.85f;
        settings->roughness = 0.28f;
        settings->position_amount = 0.015f;
        settings->rotation_amount = 0.0040f;
        settings->roll_amount = 0.0015f;
        settings->micro_amount = 0.05f;
        settings->breathing_amount = 0.0015f;
        break;
    }
}

void timeline_camera_feel_defaults(TimelineCameraFeelSettings* out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->seed = 1337;
    timeline_camera_feel_apply_preset(out, TIMELINE_CAMERA_FEEL_SUBTLE);
    out->enabled = false;
}

static void timeline_sanitize_camera_feel_settings(TimelineCameraFeelSettings* settings) {
    if (!settings)
        return;
    settings->preset = timeline_clampi(settings->preset, 0, TIMELINE_CAMERA_FEEL_PRESET_COUNT - 1);
    if (settings->seed == 0)
        settings->seed = 1337;
    settings->amount = clampf(settings->amount, 0.0f, 4.0f);
    settings->frequency = clampf(settings->frequency, 0.01f, 24.0f);
    settings->roughness = clampf(settings->roughness, 0.0f, 1.0f);
    settings->position_amount = clampf(settings->position_amount, 0.0f, 50.0f);
    settings->rotation_amount = clampf(settings->rotation_amount, 0.0f, 1.0f);
    settings->roll_amount = clampf(settings->roll_amount, 0.0f, 1.0f);
    settings->micro_amount = clampf(settings->micro_amount, 0.0f, 4.0f);
    settings->breathing_amount = clampf(settings->breathing_amount, 0.0f, 0.50f);
    settings->fade_in_frames = timeline_clampi(settings->fade_in_frames, 0, MAX_TIMELINE_FRAMES);
    settings->fade_out_frames = timeline_clampi(settings->fade_out_frames, 0, MAX_TIMELINE_FRAMES);
}

const char* timeline_camera_feel_preset_name(int preset) {
    switch (preset) {
    case TIMELINE_CAMERA_FEEL_OPERATOR:    return "Operator";
    case TIMELINE_CAMERA_FEEL_HANDHELD:    return "Handheld";
    case TIMELINE_CAMERA_FEEL_DOCUMENTARY: return "Documentary";
    case TIMELINE_CAMERA_FEEL_AGGRESSIVE:  return "Aggressive";
    case TIMELINE_CAMERA_FEEL_SUBTLE:
    default:                               return "Subtle";
    }
}

static float timeline_cubic_hermite(float p0, float p1, float p2, float p3,
                                    int f0, int f1, int f2, int f3,
                                    float m1_scale, float m2_scale, float t) {
    float span = (float)(f2 - f1);
    float m1 = (f2 != f0) ? (p2 - p0) * span / (float)(f2 - f0) : 0.0f;
    float m2 = (f3 != f1) ? (p3 - p1) * span / (float)(f3 - f1) : 0.0f;
    m1 *= timeline_clamp_tangent_scale(m1_scale);
    m2 *= timeline_clamp_tangent_scale(m2_scale);
    float t2 = t * t;
    float t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * p1 +
           (t3 - 2.0f * t2 + t) * m1 +
           (-2.0f * t3 + 3.0f * t2) * p2 +
           (t3 - t2) * m2;
}

static float timeline_cubic_hermite_angle(float p0, float p1, float p2, float p3,
                                          int f0, int f1, int f2, int f3,
                                          float m1_scale, float m2_scale, float t) {
    float u1 = p1;
    float u0 = u1 - timeline_wrap_angle(u1 - p0);
    float u2 = u1 + timeline_wrap_angle(p2 - p1);
    float u3 = u2 + timeline_wrap_angle(p3 - p2);
    return timeline_wrap_angle(timeline_cubic_hermite(u0, u1, u2, u3, f0, f1, f2, f3,
                                                      m1_scale, m2_scale, t));
}

static float timeline_sample_float_component(const TimelineKey& k0,
                                             const TimelineKey& k1,
                                             const TimelineKey& k2,
                                             const TimelineKey& k3,
                                             int component,
                                             float t,
                                             int mode,
                                             bool angle) {
    if (mode == TIMELINE_INTERP_CUBIC) {
        if (angle) {
            return timeline_cubic_hermite_angle(
                k0.fval[component], k1.fval[component], k2.fval[component], k3.fval[component],
                k0.frame, k1.frame, k2.frame, k3.frame,
                k1.tangent_scale, k2.tangent_scale, t);
        }
        return timeline_cubic_hermite(
            k0.fval[component], k1.fval[component], k2.fval[component], k3.fval[component],
            k0.frame, k1.frame, k2.frame, k3.frame,
            k1.tangent_scale, k2.tangent_scale, t);
    }
    float st = timeline_ease_t(mode, t);
    return angle ?
        timeline_lerp_angle(k1.fval[component], k2.fval[component], st) :
        timeline_lerp(k1.fval[component], k2.fval[component], st);
}

static void timeline_init_state(TimelineState& tl, int index, const char* name) {
    memset(&tl, 0, sizeof(tl));
    tl.active = true;
    tl.enabled = true;
    tl.fps = 24;
    tl.length_frames = 240;
    tl.current_frame = 0;
    tl.sample_frame = 0.0f;
    timeline_camera_feel_defaults(&tl.camera_feel);
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
        return 11;
    case TIMELINE_TRACK_LIGHT:
        return 13;
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
    s_timeline_recording = false;
    timeline_sync_public_tracks();
}

bool timeline_recording() {
    return s_timeline_recording;
}

void timeline_set_recording(bool recording) {
    s_timeline_recording = recording;
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
    for (int i = 0; i < s_timeline_count; i++) {
        if (!s_timelines[i].enabled)
            continue;
        total += timeline_duration_seconds(s_timelines[i]);
    }
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

bool timeline_get_camera_feel_settings(int index, TimelineCameraFeelSettings* out) {
    if (!out)
        return false;
    TimelineState* tl = timeline_state_at(index);
    if (!tl)
        return false;
    *out = tl->camera_feel;
    timeline_sanitize_camera_feel_settings(out);
    return true;
}

void timeline_set_camera_feel_settings(int index, const TimelineCameraFeelSettings* settings) {
    TimelineState* tl = timeline_state_at(index);
    if (!tl || !settings)
        return;
    tl->camera_feel = *settings;
    timeline_sanitize_camera_feel_settings(&tl->camera_feel);
    app_request_scene_render();
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

    // The visible/current frame remains discrete, but the sampler evaluates at
    // the true fractional timeline position. Each key decides how its outgoing
    // segment handles that fraction.
    tl.sample_frame = frame_f;
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

void timeline_delete_invalid_user_var_tracks() {
    timeline_ensure_one();
    for (int ti = 0; ti < s_timeline_count; ti++) {
        TimelineState& tl = s_timelines[ti];
        for (int i = tl.track_count - 1; i >= 0; i--) {
            TimelineTrack& track = tl.tracks[i];
            if (!track.active || track.kind != TIMELINE_TRACK_USER_VAR)
                continue;
            int user_idx = timeline_user_var_index(track.target);
            if (user_idx < 0 || g_user_cb_entries[user_idx].type != track.value_type)
                timeline_delete_track_in(tl, i);
        }
    }
    timeline_sync_public_tracks();
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
    case TIMELINE_TRACK_LIGHT:
        return res_get(g_builtin_light) != nullptr;
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
    case TIMELINE_TRACK_LIGHT:          return "light";
    default:                               return "none";
    }
}

TimelineTrackKind timeline_track_kind_from_token(const char* token) {
    if (!token) return TIMELINE_TRACK_NONE;
    if (strcmp(token, "user") == 0) return TIMELINE_TRACK_USER_VAR;
    if (strcmp(token, "cmd_transform") == 0) return TIMELINE_TRACK_COMMAND_TRANSFORM;
    if (strcmp(token, "cmd_enabled") == 0) return TIMELINE_TRACK_COMMAND_ENABLED;
    if (strcmp(token, "camera") == 0) return TIMELINE_TRACK_CAMERA;
    if (strcmp(token, "light") == 0) return TIMELINE_TRACK_LIGHT;
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
    track.keys[insert].interpolation_mode = TIMELINE_INTERP_CUBIC;
    track.keys[insert].tangent_scale = 1.0f;
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
    key.fval[9] = (float)(g_camera.projection_type == CAMERA_PROJECTION_ORTHOGRAPHIC ? 1 : 0);
    key.fval[10] = g_camera.ortho_height;
    return true;
}

static bool timeline_capture_light(TimelineKey& key) {
    Resource* dl = res_get(g_builtin_light);
    if (!dl)
        return false;
    for (int i = 0; i < 3; i++) key.fval[i] = dl->light_pos[i];
    for (int i = 0; i < 3; i++) key.fval[3 + i] = dl->light_target[i];
    for (int i = 0; i < 3; i++) key.fval[6 + i] = dl->light_color[i];
    key.fval[9] = dl->light_intensity;
    key.fval[10] = (float)(dl->light_type == LIGHT_TYPE_SPOT ? 1 : 0);
    key.fval[11] = dl->spot_angle;
    key.fval[12] = dl->spot_softness;
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
    case TIMELINE_TRACK_LIGHT:          ok = timeline_capture_light(*key); break;
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
    if (!s_timeline_recording)
        return false;
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
                                                      float t, int mode, TimelineKey* out) {
    if (!out)
        return false;
    float st = timeline_ease_t(mode, t);

    UserCBSourceKind source_kind = timeline_user_var_source_kind(track);
    if (source_kind == USER_CB_SOURCE_COMMAND_ROTATION) {
        if (track.value_type == RES_FLOAT4) {
            quat_to_array(quat_slerp(quat_from_array(a.fval), quat_from_array(b.fval), st), out->fval);
            return true;
        }

        Quat q = quat_slerp(
            quat_from_euler_xyz(v3(a.fval[0], a.fval[1], a.fval[2])),
            quat_from_euler_xyz(v3(b.fval[0], b.fval[1], b.fval[2])),
            st);
        quat_to_euler_xyz(q, a.fval, out->fval);
        return true;
    }

    if (source_kind == USER_CB_SOURCE_CAMERA_ROTATION) {
        int n = timeline_track_value_count(track);
        if (n > 0) out->fval[0] = timeline_lerp_angle(a.fval[0], b.fval[0], st);
        for (int i = 1; i < n; i++)
            out->fval[i] = timeline_lerp(a.fval[i], b.fval[i], st);
        return true;
    }

    return false;
}

static void timeline_sample_command_transform(const TimelineKey& k0, const TimelineKey& a,
                                              const TimelineKey& b, const TimelineKey& k3,
                                              float t, int mode, TimelineKey* out) {
    if (!out)
        return;

    for (int i = 0; i < 3; i++)
        out->fval[i] = timeline_sample_float_component(k0, a, b, k3, i, t, mode, false);

    Quat q = quat_slerp(quat_from_array(&a.fval[3]), quat_from_array(&b.fval[3]),
                        timeline_ease_t(mode, t));
    quat_to_array(q, &out->fval[3]);

    for (int i = 7; i < 10; i++)
        out->fval[i] = timeline_sample_float_component(k0, a, b, k3, i, t, mode, false);
}

static void timeline_sample_camera_key(const TimelineKey& k0, const TimelineKey& a,
                                       const TimelineKey& b, const TimelineKey& k3,
                                       float t, int mode, TimelineKey* out) {
    if (!out)
        return;

    for (int i = 0; i < 3; i++)
        out->fval[i] = timeline_sample_float_component(k0, a, b, k3, i, t, mode, false);
    out->fval[3] = timeline_sample_float_component(k0, a, b, k3, 3, t, mode, true);
    out->fval[4] = timeline_sample_float_component(k0, a, b, k3, 4, t, mode, false);
    out->fval[5] = timeline_sample_float_component(k0, a, b, k3, 5, t, mode, false);
    out->fval[6] = timeline_sample_float_component(k0, a, b, k3, 6, t, mode, false);
    out->fval[7] = timeline_sample_float_component(k0, a, b, k3, 7, t, mode, false);
    out->fval[8] = timeline_sample_float_component(k0, a, b, k3, 8, t, mode, true);
    out->fval[9] = t < 0.5f ? a.fval[9] : b.fval[9];
    out->fval[10] = timeline_sample_float_component(k0, a, b, k3, 10, t, mode, false);
}

static void timeline_sample_light_key(const TimelineKey& k0, const TimelineKey& a,
                                      const TimelineKey& b, const TimelineKey& k3,
                                      float t, int mode, TimelineKey* out) {
    if (!out)
        return;

    TimelineTrack light_track = {};
    light_track.kind = TIMELINE_TRACK_LIGHT;
    int n = timeline_track_value_count(light_track);
    for (int i = 0; i < n; i++)
        out->fval[i] = timeline_sample_float_component(k0, a, b, k3, i, t, mode, false);
    out->fval[10] = t < 0.5f ? a.fval[10] : b.fval[10];
}

static void timeline_sample_key_at(const TimelineState& tl, const TimelineTrack& track,
                                   float sample_frame, TimelineKey* out) {
    if (!out || track.key_count <= 0)
        return;

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

    int before = prev > 0 ? prev - 1 : prev;
    int after = next + 1 < track.key_count ? next + 1 : next;
    const TimelineKey& k0 = track.keys[before];
    const TimelineKey& a = track.keys[prev];
    const TimelineKey& b = track.keys[next];
    const TimelineKey& k3 = track.keys[after];
    *out = a;
    out->frame = display_frame;

    if (prev == next || timeline_track_uses_integral_values(track))
        return;

    int mode = timeline_clamp_interpolation_mode(a.interpolation_mode);
    if (mode == TIMELINE_INTERP_STEP)
        return;

    int span = b.frame - a.frame;
    float t = span > 0 ? (sample_frame - (float)a.frame) / (float)span : 0.0f;
    t = clampf(t, 0.0f, 1.0f);

    if (track.kind == TIMELINE_TRACK_COMMAND_TRANSFORM) {
        timeline_sample_command_transform(k0, a, b, k3, t, mode, out);
        return;
    }
    if (track.kind == TIMELINE_TRACK_CAMERA) {
        timeline_sample_camera_key(k0, a, b, k3, t, mode, out);
        return;
    }
    if (track.kind == TIMELINE_TRACK_LIGHT) {
        timeline_sample_light_key(k0, a, b, k3, t, mode, out);
        return;
    }
    if (track.kind == TIMELINE_TRACK_USER_VAR &&
        timeline_sample_user_var_special_rotation(track, a, b, t, mode, out)) {
        return;
    }

    int n = timeline_track_value_count(track);
    for (int i = 0; i < n; i++)
        out->fval[i] = timeline_sample_float_component(k0, a, b, k3, i, t, mode, false);
}

static void timeline_sample_key(const TimelineState& tl, const TimelineTrack& track, TimelineKey* out) {
    timeline_sample_key_at(tl, track, tl.sample_frame, out);
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

    if (source_kind == USER_CB_SOURCE_LIGHT_POSITION ||
        source_kind == USER_CB_SOURCE_LIGHT_TARGET) {
        Resource* dl = res_get(g_builtin_light);
        if (!dl)
            return;
        float* dst = source_kind == USER_CB_SOURCE_LIGHT_POSITION ? dl->light_pos : dl->light_target;
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

static uint32_t timeline_camera_feel_hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float timeline_camera_feel_hash_float(int x, int seed) {
    uint32_t h = timeline_camera_feel_hash((uint32_t)x ^ timeline_camera_feel_hash((uint32_t)seed));
    return ((float)(h & 0x00ffffffu) / 8388607.5f) - 1.0f;
}

static float timeline_camera_feel_smooth(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float timeline_camera_feel_value_noise(float x, int seed) {
    int ix = (int)floorf(x);
    float t = x - (float)ix;
    float a = timeline_camera_feel_hash_float(ix, seed);
    float b = timeline_camera_feel_hash_float(ix + 1, seed);
    return timeline_lerp(a, b, timeline_camera_feel_smooth(t));
}

static float timeline_camera_feel_fbm(float x, int seed, float roughness) {
    roughness = clampf(roughness, 0.0f, 1.0f);
    float sum = timeline_camera_feel_value_noise(x, seed);
    float norm = 1.0f;
    float amp = 0.5f * roughness;
    float freq = 2.13f;
    for (int i = 0; i < 3; i++) {
        sum += timeline_camera_feel_value_noise(x * freq + 19.37f * (float)(i + 1), seed + 41 + i * 17) * amp;
        norm += amp;
        amp *= 0.5f * roughness;
        freq *= 2.07f;
    }
    return norm > 0.0f ? sum / norm : sum;
}

static float timeline_camera_feel_fade(const TimelineState& tl) {
    const TimelineCameraFeelSettings& s = tl.camera_feel;
    float fade = 1.0f;
    if (s.fade_in_frames > 0)
        fade = fminf(fade, clampf(tl.sample_frame / (float)s.fade_in_frames, 0.0f, 1.0f));
    if (s.fade_out_frames > 0) {
        float last = (float)(tl.length_frames - 1);
        fade = fminf(fade, clampf((last - tl.sample_frame) / (float)s.fade_out_frames, 0.0f, 1.0f));
    }
    return timeline_camera_feel_smooth(fade);
}

static void timeline_apply_camera_feel(const TimelineState& tl, TimelineKey* key) {
    if (!key || !tl.camera_feel.enabled || tl.fps <= 0)
        return;

    TimelineCameraFeelSettings s = tl.camera_feel;
    timeline_sanitize_camera_feel_settings(&s);
    float fade = timeline_camera_feel_fade(tl);
    float strength = s.amount * fade;
    if (strength <= 0.000001f)
        return;

    float t = tl.sample_frame / (float)tl.fps;
    float f = s.frequency;
    int seed = s.seed;

    float drift_yaw = timeline_camera_feel_fbm(t * f * 0.29f + 3.1f, seed + 11, s.roughness);
    float drift_pitch = timeline_camera_feel_fbm(t * f * 0.37f + 7.3f, seed + 17, s.roughness);
    float drift_roll = timeline_camera_feel_fbm(t * f * 0.31f + 2.4f, seed + 23, s.roughness);
    float operator_yaw = timeline_camera_feel_fbm(t * f * 1.10f + 5.8f, seed + 29, s.roughness);
    float operator_pitch = timeline_camera_feel_fbm(t * f * 1.28f + 9.4f, seed + 31, s.roughness);
    float operator_roll = timeline_camera_feel_fbm(t * f * 0.92f + 4.2f, seed + 37, s.roughness);
    float micro_yaw = timeline_camera_feel_value_noise(t * f * 5.70f + 1.7f, seed + 43);
    float micro_pitch = timeline_camera_feel_value_noise(t * f * 6.20f + 8.9f, seed + 47);
    float micro_roll = timeline_camera_feel_value_noise(t * f * 5.10f + 6.2f, seed + 53);

    key->fval[3] = timeline_wrap_angle(key->fval[3] +
        (drift_yaw * 0.72f + operator_yaw * 0.28f + micro_yaw * s.micro_amount * 0.10f) *
        s.rotation_amount * strength);
    key->fval[4] +=
        (drift_pitch * 0.62f + operator_pitch * 0.38f + micro_pitch * s.micro_amount * 0.12f) *
        s.rotation_amount * 0.78f * strength;
    key->fval[8] = timeline_wrap_angle(key->fval[8] +
        (drift_roll * 0.52f + operator_roll * 0.48f + micro_roll * s.micro_amount * 0.20f) *
        s.roll_amount * strength);

    Camera basis = {};
    for (int i = 0; i < 3; i++)
        basis.position[i] = key->fval[i];
    camera_set_euler(&basis, key->fval[3], key->fval[4], key->fval[8]);

    Vec3 right = camera_right(basis);
    Vec3 up = camera_up(basis);
    float pos_right = timeline_camera_feel_fbm(t * f * 0.55f + 11.4f, seed + 59, s.roughness);
    float pos_up = timeline_camera_feel_fbm(t * f * 0.73f + 14.1f, seed + 61, s.roughness);
    float pos_operator_right = timeline_camera_feel_fbm(t * f * 1.34f + 20.3f, seed + 67, s.roughness);
    float pos_operator_up = timeline_camera_feel_fbm(t * f * 1.18f + 17.7f, seed + 71, s.roughness);
    float pos_scale = s.position_amount * strength;
    Vec3 offset = v3_add(v3_scale(right, (pos_right * 0.78f + pos_operator_right * 0.22f) * pos_scale),
                         v3_scale(up, (pos_up * 0.65f + pos_operator_up * 0.35f) * pos_scale));
    key->fval[0] += offset.x;
    key->fval[1] += offset.y;
    key->fval[2] += offset.z;

    float breath = timeline_camera_feel_fbm(t * f * 0.18f + 25.6f, seed + 79, s.roughness);
    key->fval[5] = clampf(key->fval[5] + breath * s.breathing_amount * strength, 0.10f, 2.80f);
}

static void timeline_apply_camera(const TimelineState& tl, TimelineKey key) {
    timeline_apply_camera_feel(tl, &key);
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
    g_camera.projection_type = key.fval[9] >= 0.5f ? CAMERA_PROJECTION_ORTHOGRAPHIC : CAMERA_PROJECTION_PERSPECTIVE;
    if (key.fval[10] > 0.001f)
        g_camera.ortho_height = key.fval[10];
}

static void timeline_apply_light(const TimelineKey& key) {
    Resource* dl = res_get(g_builtin_light);
    if (!dl)
        return;
    for (int i = 0; i < 3; i++) dl->light_pos[i] = key.fval[i];
    for (int i = 0; i < 3; i++) dl->light_target[i] = key.fval[3 + i];
    for (int i = 0; i < 3; i++) dl->light_color[i] = key.fval[6 + i];
    dl->light_intensity = key.fval[9];
    dl->light_type = key.fval[10] >= 0.5f ? LIGHT_TYPE_SPOT : LIGHT_TYPE_DIRECTIONAL;
    if (key.fval[11] > 0.001f)
        dl->spot_angle = key.fval[11];
    dl->spot_softness = clampf(key.fval[12], 0.0f, 0.95f);
}

static void timeline_camera_state_from_key(const TimelineKey& key, TimelineCameraState* out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 3; i++)
        out->position[i] = key.fval[i];
    out->yaw = timeline_wrap_angle(key.fval[3]);
    out->pitch = key.fval[4];
    out->roll = timeline_wrap_angle(key.fval[8]);
    out->projection_type = key.fval[9] >= 0.5f ? CAMERA_PROJECTION_ORTHOGRAPHIC : CAMERA_PROJECTION_PERSPECTIVE;
    out->fov_y = clampf(key.fval[5], 0.10f, 2.80f);
    out->near_z = key.fval[6] < 0.0001f ? 0.0001f : key.fval[6];
    out->far_z = key.fval[7] > out->near_z + 0.001f ? key.fval[7] : out->near_z + 0.001f;
    out->ortho_height = key.fval[10] > 0.001f ? key.fval[10] : 0.001f;
}

static void timeline_light_state_from_key(const TimelineKey& key, TimelineLightState* out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 3; i++)
        out->position[i] = key.fval[i];
    for (int i = 0; i < 3; i++)
        out->target[i] = key.fval[3 + i];
    for (int i = 0; i < 3; i++)
        out->color[i] = key.fval[6 + i];
    out->intensity = key.fval[9];
    out->light_type = key.fval[10] >= 0.5f ? LIGHT_TYPE_SPOT : LIGHT_TYPE_DIRECTIONAL;
    out->spot_angle = clampf(key.fval[11] > 0.001f ? key.fval[11] : 0.78539816339f, 0.05f, 3.0f);
    out->spot_softness = clampf(key.fval[12], 0.0f, 0.95f);
}

bool timeline_sample_camera_state(int frame, TimelineCameraState* out) {
    if (!out)
        return false;
    TimelineState& tl = timeline_current_state();
    int track_index = timeline_find_track_in(tl, TIMELINE_TRACK_CAMERA, "camera", RES_NONE);
    if (track_index < 0)
        return false;
    TimelineTrack& track = tl.tracks[track_index];
    if (!track.active || !track.enabled || track.key_count <= 0)
        return false;
    TimelineKey sampled = {};
    timeline_sample_key_at(tl, track, (float)timeline_clamp_frame_for(tl, frame), &sampled);
    timeline_camera_state_from_key(sampled, out);
    return true;
}

bool timeline_sample_light_state(int frame, TimelineLightState* out) {
    if (!out)
        return false;
    TimelineState& tl = timeline_current_state();
    int track_index = timeline_find_track_in(tl, TIMELINE_TRACK_LIGHT, "light", RES_NONE);
    if (track_index < 0)
        return false;
    TimelineTrack& track = tl.tracks[track_index];
    if (!track.active || !track.enabled || track.key_count <= 0)
        return false;
    TimelineKey sampled = {};
    timeline_sample_key_at(tl, track, (float)timeline_clamp_frame_for(tl, frame), &sampled);
    timeline_light_state_from_key(sampled, out);
    return true;
}

static bool timeline_track_drives_camera(const TimelineTrack& track) {
    if (track.kind == TIMELINE_TRACK_CAMERA)
        return true;
    if (track.kind != TIMELINE_TRACK_USER_VAR)
        return false;
    UserCBSourceKind source_kind = timeline_user_var_source_kind(track);
    return source_kind == USER_CB_SOURCE_CAMERA_POSITION ||
           source_kind == USER_CB_SOURCE_CAMERA_ROTATION;
}

static bool timeline_track_drives_light(const TimelineTrack& track) {
    if (track.kind == TIMELINE_TRACK_LIGHT)
        return true;
    if (track.kind != TIMELINE_TRACK_USER_VAR)
        return false;
    UserCBSourceKind source_kind = timeline_user_var_source_kind(track);
    return source_kind == USER_CB_SOURCE_LIGHT_POSITION ||
           source_kind == USER_CB_SOURCE_LIGHT_TARGET;
}

void timeline_apply_current_filtered(bool apply_camera, bool apply_light) {
    TimelineState& tl = timeline_playback_state();
    if (!tl.enabled)
        return;
    for (int i = 0; i < tl.track_count; i++) {
        TimelineTrack& track = tl.tracks[i];
        if (!track.active || !track.enabled || track.key_count <= 0)
            continue;
        if (!apply_camera && timeline_track_drives_camera(track))
            continue;
        if (!apply_light && timeline_track_drives_light(track))
            continue;

        TimelineKey sampled = {};
        timeline_sample_key(tl, track, &sampled);
        switch (track.kind) {
        case TIMELINE_TRACK_USER_VAR:          timeline_apply_user_var(track, sampled); break;
        case TIMELINE_TRACK_COMMAND_TRANSFORM: timeline_apply_command_transform(track, sampled); break;
        case TIMELINE_TRACK_COMMAND_ENABLED:   timeline_apply_command_enabled(track, sampled); break;
        case TIMELINE_TRACK_CAMERA:            timeline_apply_camera(tl, sampled); break;
        case TIMELINE_TRACK_LIGHT:          timeline_apply_light(sampled); break;
        default: break;
        }
    }
}

void timeline_apply_current() {
    timeline_apply_current_filtered(true, true);
}

static void timeline_write_tracks(FILE* f, const TimelineState& tl) {
    for (int i = 0; i < tl.track_count; i++) {
        const TimelineTrack& track = tl.tracks[i];
        if (!track.active)
            continue;
        if (!timeline_track_target_exists(track))
            continue;

        fprintf(f, "timeline_track %s %s %s %d %d\n",
                timeline_track_kind_token(track.kind), track.target,
                res_type_str(track.value_type), track.key_count,
                track.enabled ? 1 : 0);

        int n = timeline_track_value_count(track);
        bool integral = timeline_track_uses_integral_values(track);
        for (int k = 0; k < track.key_count; k++) {
            const TimelineKey& key = track.keys[k];
            fprintf(f, "timeline_key %d %d %.9g",
                    key.frame,
                    timeline_clamp_interpolation_mode(key.interpolation_mode),
                    timeline_clamp_tangent_scale(key.tangent_scale));
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
    timeline_delete_invalid_user_var_tracks();

    fprintf(f, "\ntimeline\n");
    fprintf(f, "timeline_global %d %d %d\n",
            s_timeline_current_index,
            s_timeline_loop ? 1 : 0,
            s_timeline_enabled ? 1 : 0);

    for (int i = 0; i < s_timeline_count; i++) {
        const TimelineState& tl = s_timelines[i];
        fprintf(f, "timeline_clip %s %d %d %d %d\n",
                tl.name[0] ? tl.name : "Timeline",
                tl.fps, tl.length_frames, tl.current_frame,
                tl.enabled ? 1 : 0);
        TimelineCameraFeelSettings feel = tl.camera_feel;
        timeline_sanitize_camera_feel_settings(&feel);
        fprintf(f, "timeline_camera_feel %d %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d\n",
                feel.enabled ? 1 : 0,
                feel.preset,
                feel.seed,
                feel.amount,
                feel.frequency,
                feel.roughness,
                feel.position_amount,
                feel.rotation_amount,
                feel.roll_amount,
                feel.micro_amount,
                feel.breathing_amount,
                feel.fade_in_frames,
                feel.fade_out_frames);
        timeline_write_tracks(f, tl);
        fprintf(f, "end_timeline_clip\n");
    }
    fprintf(f, "end_timeline\n");
}
