#pragma once
#include "types.h"
#include <stdio.h>

// Discrete editor timelines. Each timeline stores sparse keys, but the UI
// presents them as one slot per timeline frame. Timelines are played
// sequentially; disabled timelines are skipped by playback and exporters.

#define MAX_TIMELINES        16
#define MAX_TIMELINE_TRACKS  128
#define MAX_TIMELINE_KEYS    256
#define MAX_TIMELINE_FRAMES  1024

typedef enum {
    TIMELINE_TRACK_NONE = 0,
    TIMELINE_TRACK_USER_VAR,
    TIMELINE_TRACK_COMMAND_TRANSFORM,
    TIMELINE_TRACK_COMMAND_ENABLED,
    TIMELINE_TRACK_CAMERA,
    TIMELINE_TRACK_LIGHT
} TimelineTrackKind;

typedef enum {
    TIMELINE_INTERP_STEP = 0,
    TIMELINE_INTERP_LINEAR = 1,
    TIMELINE_INTERP_QUADRATIC = 2,
    TIMELINE_INTERP_CUBIC = 3
} TimelineInterpolationMode;

struct TimelineKey {
    int   frame;
    int   interpolation_mode;
    float tangent_scale;
    int   ival[4];
    float fval[16];
};

struct TimelineTrack {
    bool              active;
    bool              enabled;
    TimelineTrackKind kind;
    char              target[MAX_NAME];
    ResType           value_type;
    int               key_count;
    TimelineKey       keys[MAX_TIMELINE_KEYS];
};

struct TimelineCameraState {
    float position[3];
    float yaw;
    float pitch;
    float roll;
    int   projection_type;
    float fov_y;
    float ortho_height;
    float near_z;
    float far_z;
};

struct TimelineLightState {
    float position[3];
    float target[3];
    float color[3];
    float intensity;
    int   light_type;
    float spot_angle;
    float spot_softness;
};

typedef enum {
    TIMELINE_CAMERA_FEEL_SUBTLE = 0,
    TIMELINE_CAMERA_FEEL_OPERATOR,
    TIMELINE_CAMERA_FEEL_HANDHELD,
    TIMELINE_CAMERA_FEEL_DOCUMENTARY,
    TIMELINE_CAMERA_FEEL_AGGRESSIVE,
    TIMELINE_CAMERA_FEEL_PRESET_COUNT
} TimelineCameraFeelPreset;

struct TimelineCameraFeelSettings {
    bool  enabled;
    int   preset;
    int   seed;
    float amount;
    float frequency;
    float roughness;
    float position_amount;
    float rotation_amount;
    float roll_amount;
    float micro_amount;
    float breathing_amount;
    int   fade_in_frames;
    int   fade_out_frames;
};

// Tracks of the currently selected timeline. Kept public so the existing UI can
// operate directly on rows/keys without copying. Use timeline_set_current_index()
// before reading/writing a different timeline.
extern TimelineTrack* g_timeline_tracks;
extern int            g_timeline_track_count;

void timeline_reset();
void timeline_update(float scene_time_seconds);
void timeline_apply_current();
void timeline_apply_current_filtered(bool apply_camera, bool apply_light);

bool timeline_recording();
void timeline_set_recording(bool recording);

int  timeline_count();
int  timeline_current_index();
int  timeline_playback_index();
bool timeline_set_current_index(int index);
bool timeline_sync_editor_to_playback();
int  timeline_add(const char* name = nullptr);
bool timeline_delete(int index);
const char* timeline_name(int index);
void timeline_set_name(int index, const char* name);
bool timeline_timeline_enabled(int index);
void timeline_set_timeline_enabled(int index, bool enabled);
int  timeline_enabled_count();
float timeline_sequence_duration_seconds();
bool timeline_current_has_keys();

void timeline_camera_feel_defaults(TimelineCameraFeelSettings* out);
void timeline_camera_feel_apply_preset(TimelineCameraFeelSettings* settings, int preset);
const char* timeline_camera_feel_preset_name(int preset);
bool timeline_get_camera_feel_settings(int index, TimelineCameraFeelSettings* out);
void timeline_set_camera_feel_settings(int index, const TimelineCameraFeelSettings* settings);

int  timeline_fps();
void timeline_set_fps(int fps);
int  timeline_length_frames();
void timeline_set_length_frames(int frames);
int  timeline_current_frame();
void timeline_set_current_frame(int frame);
bool timeline_enabled();
void timeline_set_enabled(bool enabled);
bool timeline_loop();
void timeline_set_loop(bool loop);
int  timeline_play_dir();
void timeline_set_play_dir(int dir);

int  timeline_add_track(TimelineTrackKind kind, const char* target, ResType value_type);
void timeline_delete_track(int track_index);
void timeline_delete_tracks_for_command(const char* target);
void timeline_rename_tracks_for_command(const char* old_target, const char* new_target);
void timeline_delete_tracks_for_user_var(const char* target);
void timeline_rename_tracks_for_user_var(const char* old_target, const char* new_target);
void timeline_delete_invalid_user_var_tracks();
int  timeline_find_track(TimelineTrackKind kind, const char* target, ResType value_type);
bool timeline_track_target_exists(const TimelineTrack& track);
const char* timeline_track_kind_token(TimelineTrackKind kind);
TimelineTrackKind timeline_track_kind_from_token(const char* token);

int  timeline_find_key_index(const TimelineTrack& track, int frame);
TimelineKey* timeline_set_key(int track_index, int frame);
bool timeline_capture_key(int track_index, int frame);
bool timeline_capture_if_tracked(TimelineTrackKind kind, const char* target, ResType value_type);
bool timeline_delete_key(int track_index, int frame);
int  timeline_track_value_count(const TimelineTrack& track);
bool timeline_track_uses_integral_values(const TimelineTrack& track);
bool timeline_sample_camera_state(int frame, TimelineCameraState* out);
bool timeline_sample_light_state(int frame, TimelineLightState* out);

void timeline_write_project(FILE* f);
