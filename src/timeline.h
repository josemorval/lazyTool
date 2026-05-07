#pragma once
#include "types.h"
#include <stdio.h>

// Discrete editor timeline. It stores sparse keys, but the UI presents them as
// one slot per timeline frame.

#define MAX_TIMELINE_TRACKS 128
#define MAX_TIMELINE_KEYS   256
#define MAX_TIMELINE_FRAMES 1024

typedef enum {
    TIMELINE_TRACK_NONE = 0,
    TIMELINE_TRACK_USER_VAR,
    TIMELINE_TRACK_COMMAND_TRANSFORM,
    TIMELINE_TRACK_COMMAND_ENABLED,
    TIMELINE_TRACK_CAMERA,
    TIMELINE_TRACK_DIRLIGHT
} TimelineTrackKind;

struct TimelineKey {
    int   frame;
    int   ival[4];
    float fval[16];
};

struct TimelineTrack {
    bool              active;
    TimelineTrackKind kind;
    char              target[MAX_NAME];
    ResType           value_type;
    int               key_count;
    TimelineKey       keys[MAX_TIMELINE_KEYS];
};

extern TimelineTrack g_timeline_tracks[MAX_TIMELINE_TRACKS];
extern int           g_timeline_track_count;

void timeline_reset();
void timeline_update(float scene_time_seconds);
void timeline_apply_current();

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

void timeline_write_project(FILE* f);
