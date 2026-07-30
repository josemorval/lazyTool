#define LT_NO_DEFAULT_SHADOWMAP
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
};

VSOut VSMain(VSIn v)
{
    LT_SkinInfluence influence = LT_SkinInfluences[v.vertex_id];
    uint palette_base = (PaletteInstanceOffset + v.instance_id) * JointCount;

    float4 skinned_pos = 0.0;
    [unroll]
    for (uint i = 0; i < 4; i++)
    {
        float4x4 skin = BoneStates[palette_base + influence.Joints[i]].Skin;
        skinned_pos += influence.Weights[i] * mul(skin, float4(v.pos, 1.0));
    }

    VSOut o;
    float4 world = mul(LocalToWorld, skinned_pos);
    o.pos = mul(ShadowWorldToClip, world);
    return o;
}

float4 PSMain(VSOut input) : SV_Target
{
    return 0.0;
}
