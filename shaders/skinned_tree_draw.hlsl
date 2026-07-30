#include "common.hlsl"
#include "skinning.hlsl"

struct TreeBoneState
{
    float4x4 Global;
    float4x4 Skin;
    float4 Dynamics;
};

StructuredBuffer<TreeBoneState> BoneStates : register(t6);

cbuffer TreeDrawParams : register(b2)
{
    uint JointCount;
    uint PaletteInstanceOffset;
    float2 DrawPadding;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float2 uv : TEXCOORD0;
    uint vertex_id : SV_VertexID;
    uint instance_id : SV_InstanceID;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 nor : NORMAL;
    float3 wpos : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

VSOut VSMain(VSIn v)
{
    LT_SkinInfluence influence = LT_SkinInfluences[v.vertex_id];
    uint palette_base = (PaletteInstanceOffset + v.instance_id) * JointCount;

    float4 skinned_pos = 0.0;
    float3 skinned_nor = 0.0;
    [unroll]
    for (uint i = 0; i < 4; i++)
    {
        float weight = influence.Weights[i];
        float4x4 skin = BoneStates[palette_base + influence.Joints[i]].Skin;
        skinned_pos += weight * mul(skin, float4(v.pos, 1.0));
        skinned_nor += weight * mul((float3x3)skin, v.nor);
    }

    VSOut o;
    float4 world = mul(LocalToWorld, skinned_pos);
    o.pos = lt_world_to_clip(world.xyz);
    o.wpos = world.xyz;
    o.nor = normalize(mul(LocalToWorld, float4(normalize(skinned_nor), 0.0)).xyz);
    o.uv = v.uv;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 n = normalize(i.nor);
    float3 light_dir = LightParams.x >= 0.5
        ? normalize(LightPos.xyz - i.wpos)
        : normalize(-LightDir.xyz);
    float ndl = saturate(dot(n, light_dir));
    float shadow = lt_sample_shadow_pcf3x3(i.wpos, n, light_dir);

    float3 bark_dark = float3(0.055, 0.018, 0.006);
    float3 bark_light = float3(0.24, 0.075, 0.018);
    float rings = 0.82 + 0.18 * sin(i.uv.y * 75.0);
    float3 base = lerp(bark_dark, bark_light, saturate(i.uv.y * 0.35 + 0.35)) * rings;
    float3 ambient = float3(0.055, 0.065, 0.045);
    float3 direct = LightColor.xyz * LightDir.w * ndl * shadow;
    return float4(base * (ambient + direct), 1.0);
}
