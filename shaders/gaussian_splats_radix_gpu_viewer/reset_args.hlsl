#include "gs_common.hlsl"

// Pass 1: reset all frame-local GPU counters.
//
// This is deliberately a separate 1x1x1 dispatch. A compute dispatch has group
// barriers, but no global barrier across all groups. If the culling shader tried
// to reset counters and also compact visible splats in the same dispatch, some
// groups could allocate slots before another group resets the counter.

RWBuffer<uint> IndirectArgs : register(u0);
RWStructuredBuffer<uint> SortState : register(u1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x != 0 || id.y != 0 || id.z != 0)
        return;

    IndirectArgs[GS_DRAW_VERTEX_COUNT] = 6u; // one procedural quad = 6 vertices.
    IndirectArgs[GS_DRAW_INSTANCE_COUNT] = 0u;
    IndirectArgs[GS_DRAW_START_VERTEX] = 0u;
    IndirectArgs[GS_DRAW_START_INSTANCE] = 0u;

    // Sort-group DispatchIndirect args live at byte offset 16.
    // Scan-block DispatchIndirect args live at byte offset 28.
    // finalize_sort_args.hlsl overwrites both ranges after culling.
    IndirectArgs[GS_DISPATCH_SORT_GROUPS_X] = 1u;
    IndirectArgs[GS_DISPATCH_SORT_GROUPS_Y] = 1u;
    IndirectArgs[GS_DISPATCH_SORT_GROUPS_Z] = 1u;
    IndirectArgs[GS_DISPATCH_SCAN_BLOCKS_X] = 1u;
    IndirectArgs[GS_DISPATCH_SCAN_BLOCKS_Y] = GS_RADIX_BINS;
    IndirectArgs[GS_DISPATCH_SCAN_BLOCKS_Z] = 1u;

    [unroll]
    for (uint i = 0u; i < 16u; i++)
        SortState[i] = 0u;
}
