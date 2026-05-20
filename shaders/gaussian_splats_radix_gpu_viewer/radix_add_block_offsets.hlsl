#include "gs_common.hlsl"

// Radix pass B3: add tile prefixes to the per-group offsets.
//
// DispatchIndirect as: scan_blocks x 16 x 1. This makes RadixGroupOffsets a
// full exclusive prefix across all sort groups for each radix bin.

StructuredBuffer<uint> SortState : register(t0);
StructuredBuffer<uint> RadixBlockOffsets : register(t1);
RWStructuredBuffer<uint> RadixGroupOffsets : register(u0);

[numthreads(GS_RADIX_SCAN_BLOCK_SIZE, 1, 1)]
void CSMain(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint bin = group_id.y;
    uint scan_blocks = SortState[GS_STATE_SCAN_BLOCKS];
    uint sort_groups = SortState[GS_STATE_SORT_GROUPS];
    uint group_base = group_id.x * GS_RADIX_SCAN_BLOCK_SIZE;
    uint sort_group = group_base + lane;

    uint block_capacity = 0u;
    uint block_stride = 0u;
    RadixBlockOffsets.GetDimensions(block_capacity, block_stride);
    uint block_count_capacity = block_capacity / GS_RADIX_BINS;

    uint offset_capacity = 0u;
    uint offset_stride = 0u;
    RadixGroupOffsets.GetDimensions(offset_capacity, offset_stride);
    uint offset_group_capacity = offset_capacity / GS_RADIX_BINS;

    if (bin >= GS_RADIX_BINS || group_id.x >= scan_blocks || group_id.x >= block_count_capacity)
        return;
    if (sort_group >= sort_groups || sort_group >= offset_group_capacity)
        return;

    uint block_prefix = RadixBlockOffsets[group_id.x * GS_RADIX_BINS + bin];
    RadixGroupOffsets[sort_group * GS_RADIX_BINS + bin] += block_prefix;
}
