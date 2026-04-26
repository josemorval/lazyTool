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

cbuffer ObjectCB : register(b2)
{
    float4x4 World;
};

Texture2D TexBaseColor   : register(t0);
Texture2D TexMetalRough  : register(t1);
Texture2D TexNormal      : register(t2);
SamplerState SmpLinear   : register(s0);

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos  : SV_POSITION;
    float3 nor  : NORMAL;
    float2 uv   : TEXCOORD0;
    float3 wpos : TEXCOORD1;
};

struct PSOut {
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 rough  : SV_Target2;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 wpos = mul(World, float4(v.pos, 1.0));
    o.pos  = mul(ViewProj, wpos);
    o.wpos = wpos.xyz;
    o.nor  = normalize(mul(World, float4(v.nor, 0.0)).xyz);
    o.uv   = v.uv;
    return o;
}

float3 normal_from_map(float3 n, float3 wpos, float2 uv)
{
    float3 s = TexNormal.Sample(SmpLinear, uv).xyz;
    if (dot(s, s) < 0.0001)
        return normalize(n);

    float3 tn = normalize(s * 2.0 - 1.0);
    float3 dp1 = ddx(wpos);
    float3 dp2 = ddy(wpos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 t = normalize(dp1 * duv2.y - dp2 * duv1.y);
    float3 b = normalize(-dp1 * duv2.x + dp2 * duv1.x);
    float3x3 tbn = float3x3(t, b, normalize(n));
    return normalize(mul(tn, tbn));
}

PSOut PSMain(VSOut i)
{
    PSOut o;

    float4 baseTex = TexBaseColor.Sample(SmpLinear, i.uv);
    bool hasBase = dot(baseTex.rgb, baseTex.rgb) > 0.00001 || baseTex.a > 0.00001;
    if (hasBase && baseTex.a < 0.35)
        discard;

    float3 base = hasBase ? pow(max(baseTex.rgb, float3(0.001, 0.001, 0.001)), 2.2)
                          : float3(0.55, 0.55, 0.58);

    float4 orm = TexMetalRough.Sample(SmpLinear, i.uv);
    float rough = dot(orm.rgb, orm.rgb) > 0.0001 ? orm.g : 0.72;
    rough = saturate(rough);

    float3 n = normal_from_map(i.nor, i.wpos, i.uv);

    o.albedo = float4(base, 1.0);
    o.normal = float4(n * 0.5 + 0.5, 1.0);
    o.rough  = float4(rough, 0.0, 0.0, 1.0);
    return o;
}
