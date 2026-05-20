// Temporal resolve for the NanoVDB volume pass.
//
// This pass takes the current low-resolution volume color, a low-resolution
// moments buffer containing the mean world position of the visible volume, and
// the previous full-resolution history. It reprojects the current world
// position through PrevViewProj, samples history at that previous-frame UV, and
// blends it with the current frame.
//
// References / context:
// - Brian Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014.
// - Activision/Iryoku, "Next Generation Post Processing in Call of Duty:
//   Advanced Warfare", SIGGRAPH 2014, for practical temporal filtering context.
// This implementation is local and deliberately small: no neighborhood
// clipping, no velocity buffer, no copied code from those references.
#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"

Texture2D CurrentVolumeTex : register(t0);
Texture2D CurrentMomentsTex : register(t1);
Texture2D HistoryTex : register(t2);
SamplerState LinearSampler : register(s0);

cbuffer UserCB : register(b2)
{
    float4 VolumeTemporalTuning; // x: history weight, y: min alpha, z: enable, w: edge alpha clamp.
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    o.pos = float4(v.pos.xy, 0.0, 1.0);
    o.uv = v.uv;
    return o;
}

float2 previous_uv_from_world(float3 world_pos)
{
    float4 prev_clip = mul(PrevViewProj, float4(world_pos, 1.0));
    float3 prev_ndc = prev_clip.xyz / max(abs(prev_clip.w), 1e-5);
    return float2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float4 cur = CurrentVolumeTex.SampleLevel(LinearSampler, uv, 0);
    float4 moments = CurrentMomentsTex.SampleLevel(LinearSampler, uv, 0);

    float min_alpha = max(VolumeTemporalTuning.y, 0.0);
    if (VolumeTemporalTuning.z < 0.5 || cur.a <= min_alpha || moments.a <= min_alpha)
        return cur;

    float2 prev_uv = previous_uv_from_world(moments.xyz);
    bool valid = all(prev_uv >= 0.0.xx) && all(prev_uv <= 1.0.xx);
    if (!valid)
        return cur;

    float4 hist = HistoryTex.SampleLevel(LinearSampler, prev_uv, 0);
    float alpha_delta = abs(hist.a - cur.a);
    float edge_reject = max(VolumeTemporalTuning.w, 0.02);
    float history_weight = saturate(VolumeTemporalTuning.x);
    history_weight *= saturate(1.0 - alpha_delta / edge_reject);

    float4 outv = lerp(cur, hist, history_weight);
    outv.rgb = max(outv.rgb, 0.0);
    outv.a = saturate(outv.a);
    return outv;
}
