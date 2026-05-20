#include "gs_common.hlsl"

// Convert the culling counter into indirect draw/dispatch metadata.
//
// This pass is deliberately resource-dimension driven. It clamps the visible
// count to UserCB settings and to the actual allocated buffer capacities, then
// writes two DispatchIndirect argument ranges:
// - offset 16: one group per 128 sorted splats.
// - offset 28: one group per 1024 sort groups, with 16 Y groups for radix bins.

RWBuffer<uint> IndirectArgs : register(u0);
RWStructuredBuffer<uint> SortState : register(u1);
RWStructuredBuffer<SortPair> SortPairs : register(u2);
StructuredBuffer<uint> RadixGroupCountsForCapacity : register(t0);
StructuredBuffer<uint> RadixBlockSumsForCapacity : register(t1);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x != 0 || id.y != 0 || id.z != 0)
        return;

    uint capacity = gs_clamp_capacity(MaxVisible);
    uint sort_capacity = gs_clamp_capacity(SortCapacity);

    uint pair_capacity = 0u;
    uint pair_stride = 0u;
    SortPairs.GetDimensions(pair_capacity, pair_stride);

    uint radix_aux_capacity = 0u;
    uint radix_aux_stride = 0u;
    RadixGroupCountsForCapacity.GetDimensions(radix_aux_capacity, radix_aux_stride);
    uint radix_group_capacity = radix_aux_capacity / GS_RADIX_BINS;

    uint block_sum_capacity = 0u;
    uint block_sum_stride = 0u;
    RadixBlockSumsForCapacity.GetDimensions(block_sum_capacity, block_sum_stride);
    uint scan_block_capacity = min(block_sum_capacity / GS_RADIX_BINS, (uint)GS_RADIX_MAX_SCAN_BLOCKS);

    uint group_capacity_from_blocks = scan_block_capacity * GS_RADIX_SCAN_BLOCK_SIZE;
    uint sort_group_capacity = min(radix_group_capacity, group_capacity_from_blocks);
    uint radix_pair_capacity = sort_group_capacity * GS_THREADS;

    capacity = min(capacity, pair_capacity);
    capacity = min(capacity, radix_pair_capacity);
    sort_capacity = min(sort_capacity, capacity);

    uint raw_visible = IndirectArgs[GS_DRAW_INSTANCE_COUNT];
    uint visible = min(raw_visible, sort_capacity);
    uint sort_count = visible;
    uint sort_groups = max(gs_div_ceil(sort_count, (uint)GS_THREADS), 1u);
    sort_groups = min(sort_groups, max(sort_group_capacity, 1u));
    uint scan_blocks = max(gs_div_ceil(sort_groups, (uint)GS_RADIX_SCAN_BLOCK_SIZE), 1u);
    scan_blocks = min(scan_blocks, max(scan_block_capacity, 1u));

    // DrawInstancedIndirect reads these four DWORDs at byte offset 0.
    IndirectArgs[GS_DRAW_VERTEX_COUNT] = 6u;
    IndirectArgs[GS_DRAW_INSTANCE_COUNT] = visible;
    IndirectArgs[GS_DRAW_START_VERTEX] = 0u;
    IndirectArgs[GS_DRAW_START_INSTANCE] = 0u;

    // DispatchIndirect for histogram/scatter: sort_groups x 1 x 1.
    IndirectArgs[GS_DISPATCH_SORT_GROUPS_X] = sort_groups;
    IndirectArgs[GS_DISPATCH_SORT_GROUPS_Y] = 1u;
    IndirectArgs[GS_DISPATCH_SORT_GROUPS_Z] = 1u;

    // DispatchIndirect for hierarchical scan tile passes: scan_blocks x 16 x 1.
    IndirectArgs[GS_DISPATCH_SCAN_BLOCKS_X] = scan_blocks;
    IndirectArgs[GS_DISPATCH_SCAN_BLOCKS_Y] = GS_RADIX_BINS;
    IndirectArgs[GS_DISPATCH_SCAN_BLOCKS_Z] = 1u;

    SortState[GS_STATE_VISIBLE_COUNT] = visible;
    SortState[GS_STATE_SORT_COUNT] = sort_count;
    SortState[GS_STATE_RADIX_SHIFT] = 0u;
    SortState[GS_STATE_SORT_GROUPS] = sort_groups;
    SortState[GS_STATE_DONE] = (sort_count <= 1u) ? 1u : 0u;
    SortState[GS_STATE_RAW_VISIBLE] = raw_visible;
    SortState[GS_STATE_OVERFLOW] = raw_visible > visible ? raw_visible - visible : 0u;
    SortState[GS_STATE_SCAN_BLOCKS] = scan_blocks;
}
