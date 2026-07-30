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
StructuredBuffer<TreeBoneState> BonesPrevious : register(t1);
RWStructuredBuffer<TreeBoneState> BonesCurrent : register(u0);

cbuffer TreeSimulationParams : register(b2)
{
    uint JointCount;
    uint InstanceCount;
    float Stiffness;
    float Damping;

    float WindStrength;
    float WindFrequency;
    float MaxBend;
    float DeltaTimeScale;
};

float4x4 lt_tree_rotate_x(float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return float4x4(
        1, 0,  0, 0,
        0, c, -s, 0,
        0, s,  c, 0,
        0, 0,  0, 1);
}

float4x4 lt_tree_rotate_z(float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return float4x4(
         c, -s, 0, 0,
         s,  c, 0, 0,
         0,  0, 1, 0,
         0,  0, 0, 1);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID)
{
    uint index = dispatch_id.x;
    uint total = JointCount * InstanceCount;
    if (index >= total)
        return;

    uint instance_index = index / JointCount;
    uint joint = index % JointCount;
    uint base_index = instance_index * JointCount;

    LT_SkinRigBone rig_bone = Rig[joint];
    TreeBoneState previous = BonesPrevious[index];

    float2 angle = previous.Dynamics.xy;
    float2 velocity = previous.Dynamics.zw;
    float height_factor = JointCount > 1
        ? (float)joint / (float)(JointCount - 1)
        : 0.0;

    float phase = TimeVec.x * WindFrequency + (float)joint * 0.47 +
                  (float)instance_index * 1.37;
    float2 target = float2(sin(phase), cos(phase * 0.73 + 0.8));
    target *= WindStrength * height_factor;
    if (rig_bone.Parent == LT_SKIN_NO_PARENT)
        target = 0.0;

    float dt = min(max(TimeVec.y * DeltaTimeScale, 0.0), 1.0 / 30.0);
    float2 acceleration = (target - angle) * Stiffness - velocity * Damping;
    velocity += acceleration * dt;
    angle += velocity * dt;
    angle = clamp(angle, -MaxBend.xx, MaxBend.xx);

    float4x4 bend = mul(lt_tree_rotate_z(angle.x), lt_tree_rotate_x(angle.y));
    float4x4 local = mul(rig_bone.RestLocal, bend);
    float4x4 global = local;
    if (rig_bone.Parent != LT_SKIN_NO_PARENT)
        global = mul(BonesPrevious[base_index + rig_bone.Parent].Global, local);

    TreeBoneState current;
    current.Global = global;
    current.Skin = mul(global, rig_bone.InverseBind);
    current.Dynamics = float4(angle, velocity);
    BonesCurrent[index] = current;
}
