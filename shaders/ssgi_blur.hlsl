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

Texture2D<float4> Source  : register(t0);
Texture2D<float>  GDepth  : register(t1);
Texture2D<float4> GNormal : register(t2);
RWTexture2D<float4> Dest  : register(u0);
SamplerState SmpLinear    : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 did : SV_DispatchThreadID)
{
    uint w, h;
    Dest.GetDimensions(w, h);
    if (did.x >= w || did.y >= h)
        return;

    float2 centerUv = (float2(did.xy) + 0.5) / float2(w, h);
    float centerD = GDepth.SampleLevel(SmpLinear, centerUv, 0).r;
    float3 centerN = normalize(GNormal.SampleLevel(SmpLinear, centerUv, 0).rgb * 2.0 - 1.0);

    float4 acc = 0.0;
    float wsum = 0.0;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            int2 p = clamp(int2(did.xy) + int2(x, y), int2(0, 0), int2(w - 1, h - 1));
            float2 uv = (float2(p) + 0.5) / float2(w, h);
            float d = GDepth.SampleLevel(SmpLinear, uv, 0).r;
            float3 n = normalize(GNormal.SampleLevel(SmpLinear, uv, 0).rgb * 2.0 - 1.0);
            float ndot = saturate(dot(centerN, n));
            float depthW = saturate(1.0 - abs(d - centerD) * 90.0);
            float spatialW = exp(-dot(float2(x, y), float2(x, y)) * 0.22);
            float ww = spatialW * depthW * ndot * ndot;
            acc += Source.Load(int3(p, 0)) * ww;
            wsum += ww;
        }
    }

    Dest[did.xy] = acc / max(wsum, 0.0001);
}
