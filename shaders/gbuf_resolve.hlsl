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

cbuffer ResolveCB : register(b1)
{
    float EnvIntensity;
    float Exposure;
};

Texture2D<float4> GAlbedo   : register(t0);
Texture2D<float4> GNormal   : register(t1);
Texture2D<float>  GRough    : register(t2);
Texture2D<float>  GDepth    : register(t3);
Texture2D<float>  ShadowMap : register(t4);
RWTexture2D<float4> Dest     : register(u0);

float3 reconstruct_world(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 clip = float4(ndc, depth, 1.0);
    float4 w = mul(InvViewProj, clip);
    return w.xyz / max(w.w, 0.0001);
}

float3 aces(float3 x)
{
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

float3 ggx_spec(float3 n, float3 v, float3 l, float rough)
{
    float3 h = normalize(v + l);
    float ndh = saturate(dot(n, h));
    float ndv = saturate(dot(n, v));
    float ndl = saturate(dot(n, l));
    float a = max(0.001, rough * rough);
    float a2 = a * a;
    float dden = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * dden * dden);
    float k = (rough + 1.0) * (rough + 1.0) * 0.125;
    float Gv = ndv / (ndv * (1.0 - k) + k);
    float Gl = ndl / (ndl * (1.0 - k) + k);
    return D * Gv * Gl * 0.04 / max(0.001, 4.0 * ndv * ndl);
}

float shadow_visibility(float3 wpos, float3 n, float3 l)
{
    float4 sp = mul(ShadowViewProj, float4(wpos, 1.0));
    float3 p = sp.xyz / max(sp.w, 0.0001);
    float2 uv = p.xy * float2(0.5, -0.5) + 0.5;

    if (any(uv < 0.0) || any(uv > 1.0) || p.z <= 0.0 || p.z >= 1.0)
        return 1.0;

    uint sw, sh;
    ShadowMap.GetDimensions(sw, sh);
    if (sw < 2 || sh < 2)
        return 1.0;

    float ndl = saturate(dot(n, l));
    float bias = max(0.00035, 0.0018 * (1.0 - ndl));

    float2 tex = uv * float2(sw, sh) - 0.5;
    int2 base = int2(floor(tex));
    int2 maxp = int2(int(sw) - 1, int(sh) - 1);

    float vis = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 q = clamp(base + int2(x, y), int2(0, 0), maxp);
        float z = ShadowMap.Load(int3(q, 0));
        vis += (p.z - bias <= z + 0.000001) ? 1.0 : 0.0;
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

    float depth = GDepth.Load(int3(did.xy, 0));
    if (depth >= 0.99999)
    {
        Dest[did.xy] = float4(0.045, 0.055, 0.085, 1.0);
        return;
    }

    float2 uv = (float2(did.xy) + 0.5) / float2(w, h);
    float3 albedo = GAlbedo.Load(int3(did.xy, 0)).rgb;
    float3 n = normalize(GNormal.Load(int3(did.xy, 0)).rgb * 2.0 - 1.0);
    float rough = saturate(GRough.Load(int3(did.xy, 0)));
    float3 wpos = reconstruct_world(uv, depth);

    float3 l = normalize(-LightDir.xyz);
    float3 v = normalize(CamPos.xyz - wpos);
    float ndl = saturate(dot(n, l));
    float sh = shadow_visibility(wpos, n, l);

    float envI = EnvIntensity > 0.001 ? EnvIntensity : 0.25;
    float expv = Exposure > 0.001 ? Exposure : 1.0;

    float3 direct = albedo * LightColor.rgb * LightDir.w * ndl * sh;
    float3 spec = ggx_spec(n, v, l, rough) * LightColor.rgb * LightDir.w * ndl * sh;
    float3 ambient = albedo * envI;

    float3 col = direct + spec + ambient;
    col = aces(col * expv);
    col = pow(saturate(col), 1.0 / 2.2);
    Dest[did.xy] = float4(col, 1.0);
}
