#include "common.hlsl"
#define LT_SKIN_NO_INFLUENCE_BUFFER
#include "skinning.hlsl"

struct TreeBoneState
{
    float4x4 Global;
    float4x4 Skin;
    float4 Dynamics; // xy bend angles, zw angular velocities
};

StructuredBuffer<LT_SkinRigBone> Rig : register(t0);
RWStructuredBuffer<TreeBoneState> BonesPrevious : register(u0);
RWStructuredBuffer<TreeBoneState> BonesCurrent : register(u1);

cbuffer TreeInitParams : register(b2)
{
    uint JointCount;
    uint InstanceCount;
    float2 InitPadding;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    uint index = dispatch_id.x;
    uint total = JointCount * InstanceCount;
    if (index >= total)
        return;

    uint joint = index % JointCount;
    LT_SkinRigBone rig_bone = Rig[joint];

    TreeBoneState state;
    state.Global = rig_bone.RestGlobal;
    state.Skin = mul(state.Global, rig_bone.InverseBind);
    state.Dynamics = 0.0;

    BonesPrevious[index] = state;
    BonesCurrent[index] = state;
}
