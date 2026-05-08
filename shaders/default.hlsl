// Default editor shader. It demonstrates the engine cbuffers, object transform,
// directional lighting, and cascaded shadow atlas sampling used by normal draws.
// Minimal example shader used by the default scene. It now samples the shared
// shadow atlas and uses the cascade data exposed through SceneCB.
cbuffer SceneCB : register(b0)
{
    float4x4 ViewProj;
    float4 TimeVec;
    float4 LightDir;
    float4 LightColor;
    float4 CamPos;
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4x4 PrevShadowViewProj;
    float4 CamDir;
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};

cbuffer ObjectCB : register(b1)
{
    float4x4 World;
};

Texture2D ShadowMap : register(t7);
SamplerComparisonState ShadowSampler : register(s1);

struct VSIn {
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos  : SV_POSITION;
    float3 nor  : NORMAL;
    float3 wpos : TEXCOORD0;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wpos = mul(World, float4(v.pos, 1.0));
    o.pos = mul(ViewProj, wpos);
    o.wpos = wpos.xyz;
    o.nor = normalize(mul(World, float4(v.nor, 0.0)).xyz);
    return o;
}

float sample_shadow_cascade(int cascade_index, float3 wpos, float ndl) {
    uint shadow_w = 0;
    uint shadow_h = 0;
    ShadowMap.GetDimensions(shadow_w, shadow_h);
    if (shadow_w == 0 || shadow_h == 0)
        return 1.0;

    float4 shadow_clip = mul(ShadowCascadeViewProj[cascade_index], float4(wpos, 1.0));
    if (abs(shadow_clip.w) < 1e-5)
        return 1.0;

    float3 shadow_ndc = shadow_clip.xyz / shadow_clip.w;
    float2 uv_local = float2(shadow_ndc.x * 0.5 + 0.5, shadow_ndc.y * -0.5 + 0.5);
    if (uv_local.x < 0.0 || uv_local.x > 1.0 || uv_local.y < 0.0 || uv_local.y > 1.0)
        return 1.0;
    if (shadow_ndc.z < 0.0 || shadow_ndc.z > 1.0)
        return 1.0;

    float4 rect = ShadowCascadeRects[cascade_index];
    float2 atlas_uv = uv_local * rect.xy + rect.zw;
    float bias = lerp(0.0025, 0.0003, saturate(ndl));
    return ShadowMap.SampleCmpLevelZero(ShadowSampler, atlas_uv, shadow_ndc.z - bias);
}

int select_shadow_cascade(float3 wpos) {
    int cascade_count = clamp((int)ShadowParams.x, 1, 4);
    float view_depth = dot(wpos - CamPos.xyz, CamDir.xyz);
    int cascade_index = 0;
    [unroll]
    for (int i = 0; i < 3; i++) {
        if (i + 1 < cascade_count && view_depth > ShadowCascadeSplits[i])
            cascade_index = i + 1;
    }
    return cascade_index;
}

float4 PSMain(VSOut i) : SV_Target {
    float3 n = normalize(i.nor);
    float3 ld = normalize(-LightDir.xyz);
    float ndl = saturate(dot(n, ld));
    int cascade_index = select_shadow_cascade(i.wpos);
    float shadow = sample_shadow_cascade(cascade_index, i.wpos, ndl);

    float3 base = 0.30 + 0.70 * (0.5 + 0.5 * n);
    float3 sun = LightColor.xyz * LightDir.w * ndl * shadow;
    float3 ambient = float3(0.06, 0.065, 0.08);
    return float4(base * (ambient + sun), 1.0);
}
