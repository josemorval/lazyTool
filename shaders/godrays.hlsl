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

cbuffer RaysCB : register(b1)
{
    int   StepCount;
    float Density;
    float MaxDistance;
    float _pad0;
};

Texture2D<float> SceneDepth : register(t0);
Texture2D<float> ShadowMap  : register(t7);
RWTexture2D<float> Dest      : register(u0);

float3 reconstruct_world(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 clip = float4(ndc, depth, 1.0);
    float4 w = mul(InvViewProj, clip);
    return w.xyz / max(w.w, 0.0001);
}

float hash12(float2 p)
{
    p = frac(p * float2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float load_depth_nearest(float2 uv)
{
    uint w, h;
    SceneDepth.GetDimensions(w, h);
    if (w < 1 || h < 1)
        return 1.0;
    int2 p = clamp(int2(uv * float2(w, h)), int2(0, 0), int2(int(w) - 1, int(h) - 1));
    return SceneDepth.Load(int3(p, 0));
}

float shadow_at(float3 wpos)
{
    float4 sp = mul(ShadowViewProj, float4(wpos, 1.0));
    float3 p = sp.xyz / max(sp.w, 0.0001);
    float2 uv = p.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || p.z <= 0.0 || p.z >= 1.0)
        return 0.0;

    uint sw, sh;
    ShadowMap.GetDimensions(sw, sh);
    if (sw < 2 || sh < 2)
        return 0.0;

    float2 tex = uv * float2(sw, sh) - 0.5;
    int2 base = int2(floor(tex));
    int2 maxp = int2(int(sw) - 1, int(sh) - 1);
    float vis = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 q = clamp(base + int2(x, y), int2(0, 0), maxp);
        float z = ShadowMap.Load(int3(q, 0));
        vis += (p.z - 0.0012 <= z + 0.000001) ? 1.0 : 0.0;
    }
    return vis / 9.0;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 did : SV_DispatchThreadID)
{
    uint w, h;
    Dest.GetDimensions(w, h);
    if (did.x >= w || did.y >= h)
        return;

    float2 uv = (float2(did.xy) + 0.5) / float2(w, h);
    float depth = load_depth_nearest(uv);
    float3 endp = depth >= 0.9999 ? reconstruct_world(uv, 0.98) : reconstruct_world(uv, depth);

    float3 ro = CamPos.xyz;
    float3 ray = endp - ro;
    float len = min(length(ray), MaxDistance > 0.001 ? MaxDistance : 18.0);
    float3 rd = normalize(ray);
    int steps = clamp(StepCount, 8, 64);
    float jitter = hash12(float2(did.xy) + TimeVec.x);

    float accum = 0.0;
    [loop] for (int i = 0; i < 64; ++i)
    {
        if (i >= steps)
            break;
        float t = (float(i) + jitter) / float(steps) * len;
        float3 p = ro + rd * t;
        accum += shadow_at(p);
    }

    accum = accum / float(steps) * (Density > 0.001 ? Density : 0.08) * len;
    Dest[did.xy] = saturate(accum);
}
