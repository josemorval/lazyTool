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

cbuffer LitMatCB : register(b1)
{
    float3 BaseColor;
    float  Roughness;
};

Texture2D<float>       ShadowMap : register(t7);
SamplerComparisonState SmpShadow : register(s1);


struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos  : SV_POSITION;
    float3 nor  : NORMAL;
    float3 wpos : TEXCOORD0;
    float4 spos : TEXCOORD1;
};

float param_or_default(float v, float d) { return abs(v) > 0.00001 ? v : d; }
float3 param_or_default3(float3 v, float3 d) { return dot(v, v) > 0.00001 ? v : d; }

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 wpos = mul(World, float4(v.pos, 1.0));
    o.wpos = wpos.xyz;
    o.pos  = mul(ViewProj, wpos);
    o.nor  = normalize(mul(World, float4(v.nor, 0.0)).xyz);
    o.spos = mul(ShadowViewProj, wpos);
    return o;
}

float shadow_vis(float4 spos, float ndl)
{
    float3 p = spos.xyz / max(spos.w, 0.0001);
    float2 uv = p.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || p.z <= 0.0 || p.z >= 1.0) return 1.0;
    uint w, h; ShadowMap.GetDimensions(w, h);
    float2 texel = 1.0 / float2(max(w, 1), max(h, 1));
    float bias = max(0.00025, 0.0015 * (1.0 - ndl));
    float v = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        v += ShadowMap.SampleCmpLevelZero(SmpShadow, uv + float2(x, y) * texel, p.z - bias);
    return v / 9.0;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 base = param_or_default3(BaseColor, float3(0.7, 0.7, 0.7));
    float  rough = saturate(param_or_default(Roughness, 0.5));
    float3 n = normalize(i.nor);
    float3 l = normalize(-LightDir.xyz);
    float3 v = normalize(CamPos.xyz - i.wpos);
    float3 h = normalize(l + v);
    float ndl = saturate(dot(n, l));
    float ndh = saturate(dot(n, h));
    float vis = shadow_vis(i.spos, ndl);
    float shininess = lerp(128.0, 5.0, rough);
    float spec_str = lerp(0.55, 0.04, rough);
    float3 diffuse = base * LightColor.rgb * LightDir.w * ndl * vis;
    float3 specular = LightColor.rgb * LightDir.w * pow(ndh, shininess) * spec_str * vis;
    float3 ambient = base * float3(0.10, 0.11, 0.13);
    return float4(diffuse + specular + ambient, 1.0);
}
