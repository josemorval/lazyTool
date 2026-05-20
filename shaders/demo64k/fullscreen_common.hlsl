#ifndef DEMO64K_FULLSCREEN_COMMON_HLSL
#define DEMO64K_FULLSCREEN_COMMON_HLSL

SamplerState LinearSampler : register(s0);

struct FullscreenVSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct FullscreenVSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

FullscreenVSOut VSMain(FullscreenVSIn v)
{
    FullscreenVSOut o;
    o.pos = float4(v.pos.xy, 0.0, 1.0);
    o.uv = v.uv;
    return o;
}

float2 demo64k_texel_size(Texture2D tex)
{
    uint w = 1;
    uint h = 1;
    tex.GetDimensions(w, h);
    return 1.0 / float2(max(w, 1), max(h, 1));
}

#endif
