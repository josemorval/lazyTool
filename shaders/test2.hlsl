// Built-in scene constants supplied by the editor in register(b0).
// These names and order match the CPU SceneCB layout. You can delete unused
// fields from your shader; D3D reflection only keeps constants that are read.
cbuffer SceneCB : register(b0)
{
    float4x4 ViewProj;
    float4 TimeVec;              // x=time seconds, y=delta seconds, z=frame index, w=reserved.
    float4 LightDir;             // xyz=main light direction, w=intensity.
    float4 LightColor;           // rgb=color, a=reserved.
    float4 CamPos;               // xyz=camera position.
    float4x4 ShadowViewProj;
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float4x4 PrevInvViewProj;
    float4x4 PrevShadowViewProj;
    float4 CamDir;               // xyz=camera forward direction.
    float4 ShadowCascadeSplits;
    float4 ShadowParams;
    float4 ShadowCascadeRects[4];
    float4x4 ShadowCascadeViewProj[4];
};


// Compute template: fill an indirect argument buffer.
//
// Editor setup:
//   1. Create a StructuredBuffer with UAV enabled and Indirect Args enabled.
//      A stride of 4 bytes is enough because the buffer stores uint DWORDs.
//   2. Bind that buffer to UAV slot u0 on a Dispatch command.
//   3. Dispatch this shader with 1,1,1 groups.
//   4. Use the same buffer as the Indirect Buffer of an Indirect Draw or
//      Indirect Dispatch command.
//
// Mode selects how the first DWORDs are written:
//   Mode = 0: DispatchIndirect      -> x, y, z group counts.
//   Mode = 1: DrawInstancedIndirect -> vertex count, instance count,
//                                      start vertex, start instance.
//   Mode = 2: DrawIndexedInstancedIndirect -> index count, instance count,
//                                             start index, base vertex,
//                                             start instance.
//
// The editor reads the argument layout according to the command that consumes
// the buffer, so unused DWORDs are harmless. Keep Byte Offset on both commands
// aligned to 4 bytes.
//
// Instancing notes:
//   - For DrawInstancedIndirect, InstanceCount is the number of instances the
//     draw shader will receive through SV_InstanceID.
//   - InstanceCount can be hardcoded here, driven from a StructuredBuffer count,
//     or driven from a Gaussian Splat count through Shader Parameters.
//   - For procedural quad instances, set DrawVertexOrIndexCount to 6 and use
//     InstanceCount to choose how many quads to draw.
cbuffer UserCB : register(b2)
{
    int  Mode;
    int3 DispatchGroups;

    int  DrawVertexOrIndexCount;
    int  InstanceCount;
    int  StartVertexOrIndex;
    int  BaseVertex;

    int  StartInstance;
};

RWStructuredBuffer<uint> IndirectArgs : register(u0);

uint positive_or_one(int v)
{
    return (v > 0) ? (uint)v : 1u;
}

uint positive_or_zero(int v)
{
    return (v > 0) ? (uint)v : 0u;
}

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x != 0 || id.y != 0 || id.z != 0)
        return;

    if (Mode == 0)
    {
        IndirectArgs[0] = positive_or_one(DispatchGroups.x);
        IndirectArgs[1] = positive_or_one(DispatchGroups.y);
        IndirectArgs[2] = positive_or_one(DispatchGroups.z);
        return;
    }

    IndirectArgs[0] = positive_or_one(DrawVertexOrIndexCount);
    IndirectArgs[1] = positive_or_one(InstanceCount);
    IndirectArgs[2] = positive_or_zero(StartVertexOrIndex);

    if (Mode == 2)
    {
        IndirectArgs[3] = (uint)BaseVertex;
        IndirectArgs[4] = positive_or_zero(StartInstance);
    }
    else
    {
        IndirectArgs[3] = positive_or_zero(StartInstance);
    }
}
