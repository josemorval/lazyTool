#ifndef LAZYTOOL_SKINNING_HLSL
#define LAZYTOOL_SKINNING_HLSL

// GPU ABI emitted by RES_SKINNED_MESH. The resource itself exposes
// StructuredBuffer<LT_SkinRigBone> through its generic SRV. Draw commands bind
// LT_SkinInfluences automatically to VS t5.
struct LT_SkinRigBone
{
    float4x4 RestLocal;
    float4x4 RestGlobal;
    float4x4 InverseBind;
    uint Parent;
    uint JointIndex;
    uint2 Padding;
};

struct LT_SkinInfluence
{
    uint4 Joints;
    float4 Weights;
};

#ifndef LT_SKIN_NO_INFLUENCE_BUFFER
StructuredBuffer<LT_SkinInfluence> LT_SkinInfluences : register(t5);
#endif

static const uint LT_SKIN_NO_PARENT = 0xffffffffu;

#endif
