#define LT_NO_DEFAULT_SHADOWMAP
#include "../common.hlsl"
#include "fullscreen_common.hlsl"

Texture2D InputTex : register(t0);

float4 PSMain(FullscreenVSOut i) : SV_Target
{
    float2 uv = saturate(i.uv);
    float2 t = demo64k_texel_size(InputTex);

    float3 c = 0.0.xxx;
    c += InputTex.SampleLevel(LinearSampler, uv, 0).rgb * 0.25;
    c += InputTex.SampleLevel(LinearSampler, uv + t * float2( 1,  0), 0).rgb * 0.125;
    c += InputTex.SampleLevel(LinearSampler, uv + t * float2(-1,  0), 0).rgb * 0.125;
    c += InputTex.SampleLevel(LinearSampler, uv + t * float2( 0,  1), 0).rgb * 0.125;
    c += InputTex.SampleLevel(LinearSampler, uv + t * float2( 0, -1), 0).rgb * 0.125;
    c += InputTex.SampleLevel(LinearSampler, uv + t * float2( 1,  1), 0).rgb * 0.0625;
    c += InputTex.SampleLevel(LinearSampler, uv + t * float2(-1,  1), 0).rgb * 0.0625;
    c += InputTex.SampleLevel(LinearSampler, uv + t * float2( 1, -1), 0).rgb * 0.0625;
    c += InputTex.SampleLevel(LinearSampler, uv + t * float2(-1, -1), 0).rgb * 0.0625;
    return float4(max(c, 0.0), 1.0);
}
