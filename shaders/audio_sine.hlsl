#include "audio_common.hlsl"

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    float t = lt_audio_sample_time(i);

    float tone = sin(t * 440.0 * LT_AUDIO_TAU);
    float sub = sin(t * 110.0 * LT_AUDIO_TAU) * 0.35;
    float env = 0.65 + 0.35 * sin(t * 0.5 * LT_AUDIO_TAU);
    float sample = (tone * 0.7 + sub) * env;

    lt_audio_store(i, sample, sample);
}
