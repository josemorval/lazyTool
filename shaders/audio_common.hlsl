#ifndef LAZYTOOL_AUDIO_COMMON_HLSL
#define LAZYTOOL_AUDIO_COMMON_HLSL

// Audio compute ABI used by lazyTool's GPU audio player.
//
// Output format:
//   AudioOut[i] = stereo PCM16 packed as low 16 bits = left, high 16 bits = right.
//
// Runtime bindings:
//   SceneCB = b0, UserCB = b2, AudioCB = b3, AudioOut = u0.
//
// AudioDurationSeconds is the full song duration selected in the project.
// AudioSampleCount is the number of samples this compute dispatch must write.

cbuffer AudioCB : register(b3)
{
    uint  AudioSampleStart;
    uint  AudioSampleCount;
    uint  AudioSampleRate;
    uint  AudioChannels;
    float AudioTimeSeconds;
    float AudioDurationSeconds;
    float AudioMasterVolume;
    uint  AudioLoop;
};

RWStructuredBuffer<uint> AudioOut : register(u0);

static const float LT_AUDIO_PI = 3.14159265359;
static const float LT_AUDIO_TAU = 6.28318530718;

float lt_audio_sample_time(uint local_sample)
{
    return (float)(AudioSampleStart + local_sample) / (float)max(AudioSampleRate, 1u);
}

uint lt_audio_sample_index(uint local_sample)
{
    return AudioSampleStart + local_sample;
}

float lt_audio_song_phase(float t)
{
    return saturate(t / max(AudioDurationSeconds, 0.0001));
}

float lt_audio_wrap_time(float t)
{
    return frac(t / max(AudioDurationSeconds, 0.0001)) * AudioDurationSeconds;
}

bool lt_audio_loop_enabled()
{
    return AudioLoop != 0u;
}

float lt_audio_playback_time(float t)
{
    return lt_audio_loop_enabled() ? lt_audio_wrap_time(t) : min(t, AudioDurationSeconds);
}

float lt_audio_beat(float t, float bpm)
{
    return t * bpm / 60.0;
}

float lt_audio_note_freq(float midi_note)
{
    return 440.0 * exp2((midi_note - 69.0) / 12.0);
}

float lt_audio_sine(float phase)
{
    return sin(phase * LT_AUDIO_TAU);
}

float lt_audio_saw(float phase)
{
    return frac(phase) * 2.0 - 1.0;
}

float lt_audio_square(float phase, float duty)
{
    return frac(phase) < duty ? 1.0 : -1.0;
}

float lt_audio_triangle(float phase)
{
    return abs(frac(phase) * 4.0 - 2.0) - 1.0;
}

float lt_audio_hash11(float x)
{
    return frac(sin(x * 127.1) * 43758.5453);
}

float lt_audio_noise(float sample_index)
{
    return lt_audio_hash11(sample_index + 17.0) * 2.0 - 1.0;
}

float lt_audio_exp_decay(float x, float amount)
{
    return exp2(-max(x, 0.0) * amount);
}

float lt_audio_gate(float beat, float step_index, float step_len)
{
    float x = beat - step_index * step_len;
    return step(x, step_len) * step(0.0, x);
}

float lt_audio_softclip(float x)
{
    return x / (1.0 + abs(x));
}

int lt_audio_to_i16(float v)
{
    return (int)round(clamp(v, -1.0, 1.0) * 32767.0);
}

uint lt_audio_pack_i16(float left, float right)
{
    int l = lt_audio_to_i16(left * AudioMasterVolume);
    int r = lt_audio_to_i16(right * AudioMasterVolume);
    return (uint)(l & 0xffff) | ((uint)(r & 0xffff) << 16);
}

void lt_audio_store(uint local_sample, float left, float right)
{
    if (local_sample < AudioSampleCount)
        AudioOut[local_sample] = lt_audio_pack_i16(left, right);
}

#endif
