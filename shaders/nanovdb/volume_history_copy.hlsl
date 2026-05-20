// Copies the resolved volume history into the previous-history buffer.
// The project uses this explicit copy pass instead of reading and writing the
// same texture in one draw, which would be an invalid D3D11 resource hazard.
#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"

Texture2D SourceTex : register(t0);
SamplerState LinearSampler : register(s0);

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

float4 PSMain(VSOut i) : SV_Target
{
    return SourceTex.SampleLevel(LinearSampler, saturate(i.uv), 0);
}
