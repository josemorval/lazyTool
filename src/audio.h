#pragma once
#include "types.h"

// GPU-generated audio playback. A selected audio shader writes packed stereo
// int16 samples into AudioOut (u0) using AudioCB (b3).

extern AudioSettings g_audio_settings;

void audio_init();
void audio_shutdown();
void audio_reset_settings();
const AudioSettings& audio_default_settings();

void audio_update(float scene_time_seconds, bool scene_running);
void audio_request_reset(float scene_time_seconds);
void audio_stop();

bool audio_running();
const char* audio_status();
int audio_queued_buffer_count();
float audio_latency_seconds();
unsigned long long audio_sample_cursor();
