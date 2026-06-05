#include "audio_common.hlsl"

float lt_demo_kick(float beat)
{
    float local = frac(beat);
    float env = lt_audio_exp_decay(local, 8.5);
    float f = lerp(42.0, 130.0, lt_audio_exp_decay(local, 10.0));
    return lt_audio_sine(AudioTimeSeconds * f) * env * 1.35;
}

float lt_demo_hat(float sample_index, float beat)
{
    float local = frac(beat * 2.0);
    float env = lt_audio_exp_decay(local, 28.0);
    return lt_audio_noise(sample_index) * env * 0.18;
}

float lt_demo_bass(float beat)
{
    int step_i = (int)floor(beat * 2.0) & 7;
    float notes[8] = { 36.0, 36.0, 43.0, 36.0, 39.0, 39.0, 43.0, 34.0 };
    float note = notes[step_i];
    float f = lt_audio_note_freq(note);
    float local = frac(beat * 2.0);
    float env = smoothstep(0.0, 0.02, local) * lt_audio_exp_decay(local, 2.5);
    float osc = lt_audio_saw(AudioTimeSeconds * f) * 0.55 + lt_audio_sine(AudioTimeSeconds * f * 0.5) * 0.35;
    return lt_audio_softclip(osc * env * 1.4);
}

float lt_demo_lead(float beat)
{
    int step_i = (int)floor(beat * 4.0) & 15;
    float seq[16] = { 60.0, 63.0, 67.0, 70.0, 72.0, 70.0, 67.0, 63.0,
                      58.0, 62.0, 65.0, 70.0, 72.0, 75.0, 74.0, 70.0 };
    float note = seq[step_i];
    float local = frac(beat * 4.0);
    float env = smoothstep(0.0, 0.04, local) * lt_audio_exp_decay(local, 5.0);
    float f = lt_audio_note_freq(note);
    float vib = sin(AudioTimeSeconds * LT_AUDIO_TAU * 5.0) * 0.003;
    float osc = lt_audio_triangle(AudioTimeSeconds * f + vib) * 0.55 +
                lt_audio_sine(AudioTimeSeconds * f * 2.01) * 0.12;
    return osc * env * 0.35;
}

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    float t = lt_audio_sample_time(i);
    float ti = lt_audio_wrap_time(t);
    float beat = lt_audio_beat(ti, 128.0);
    float fade_in = smoothstep(0.0, 0.5, ti);
    float fade_out = 1.0 - smoothstep(AudioDurationSeconds - 1.5, AudioDurationSeconds, ti);

    float sample_index = (float)lt_audio_sample_index(i);
    float kick = lt_demo_kick(beat);
    float hat = lt_demo_hat(sample_index, beat);
    float bass = lt_demo_bass(beat);
    float lead = lt_demo_lead(beat);

    float mix = (kick + hat + bass + lead) * fade_in * fade_out;
    float pan = sin(ti * 0.37) * 0.12;
    float left = lt_audio_softclip(mix * (1.0 - pan));
    float right = lt_audio_softclip(mix * (1.0 + pan));
    lt_audio_store(i, left, right);
}
