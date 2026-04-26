cbuffer SceneCB : register(b0)
{
    float4x4 ViewProj;
    float4   TimeVec;
    float4   LightDir;
    float4   LightColor;
    float4   CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
};

cbuffer SSGICB : register(b1)
{
    float Radius;
    float Thickness;
    int   SampleCount;
    float _pad0;
};

Texture2D<float4> GAlbedo    : register(t0);
Texture2D<float4> GNormal    : register(t1);
Texture2D<float4> SceneColor : register(t2);
Texture2D<float>  GDepth     : register(t3);
RWTexture2D<float4> Dest     : register(u0);
SamplerState SmpLinear       : register(s0);

float3 reconstruct_world(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 clip = float4(ndc, depth, 1.0);
    float4 w = mul(InvViewProj, clip);
    return w.xyz / max(w.w, 0.0001);
}

float hash12(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 did : SV_DispatchThreadID)
{
    uint w, h;
    Dest.GetDimensions(w, h);
    if (did.x >= w || did.y >= h)
        return;

    float2 uv = (float2(did.xy) + 0.5) / float2(w, h);
    float depth = GDepth.SampleLevel(SmpLinear, uv, 0).r;
    if (depth >= 0.99999)
    {
        Dest[did.xy] = float4(0, 0, 0, 0);
        return;
    }

    float3 wpos = reconstruct_world(uv, depth);
    float3 n = normalize(GNormal.SampleLevel(SmpLinear, uv, 0).rgb * 2.0 - 1.0);
    float3 albedo = GAlbedo.SampleLevel(SmpLinear, uv, 0).rgb;

    float radius = Radius > 0.001 ? Radius : 1.15;
    float thickness = Thickness > 0.001 ? Thickness : 0.35;
    int count = clamp(SampleCount, 4, 32);

    float viewDist = max(length(CamPos.xyz - wpos), 0.25);
    float uvRadius = saturate(radius / viewDist) * 0.24;
    float jitter = hash12(float2(did.xy) + TimeVec.x * 17.0);

    float3 indirect = 0.0;
    float wsum = 0.0;

    [loop]
    for (int s = 0; s < 32; ++s)
    {
        if (s >= count)
            break;

        float fi = (float(s) + 0.5) / float(count);
        float a = fi * 15.199963 + jitter * 6.2831853;
        float2 dir2 = float2(cos(a), sin(a));
        float2 suv = uv + dir2 * uvRadius * sqrt(fi);
        if (any(suv < 0.0) || any(suv > 1.0))
            continue;

        float sd = GDepth.SampleLevel(SmpLinear, suv, 0).r;
        if (sd >= 0.99999)
            continue;

        float3 spos = reconstruct_world(suv, sd);
        float3 v = spos - wpos;
        float dist = length(v);
        if (dist <= 0.001 || dist > radius + thickness)
            continue;

        float3 ldir = v / dist;
        float3 sn = normalize(GNormal.SampleLevel(SmpLinear, suv, 0).rgb * 2.0 - 1.0);
        float facingA = saturate(dot(n, ldir));
        float facingB = saturate(dot(sn, -ldir));
        float rangeW = saturate(1.0 - dist / max(radius, 0.001));
        float thickW = saturate(1.0 - abs(dist - radius * fi) / max(thickness, 0.001));
        float weight = facingA * (0.35 + 0.65 * facingB) * max(rangeW, thickW * 0.35);

        float3 sampleColor = SceneColor.SampleLevel(SmpLinear, suv, 0).rgb;
        indirect += sampleColor * weight;
        wsum += weight;
    }

    float confidence = saturate(wsum / max(float(count) * 0.18, 0.001));
    indirect = (wsum > 0.0001) ? (indirect / wsum) : 0.0;
    indirect *= albedo * confidence;
    Dest[did.xy] = float4(indirect, confidence);
}
