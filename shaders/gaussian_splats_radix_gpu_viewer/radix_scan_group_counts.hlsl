#include "gs_common.hlsl"

// Radix pass B1: scan per-group counts inside 1024-group tiles.
//
// DispatchIndirect as: scan_blocks x 16 x 1.
// group_id.x selects a tile of 1024 sort groups, group_id.y selects the radix bin.
// Outputs an exclusive prefix local to the tile plus one tile sum per bin.

StructuredBuffer<uint> RadixGroupCounts : register(t0);
StructuredBuffer<uint> SortState : register(t1);
RWStructuredBuffer<uint> RadixGroupOffsets : register(u0);
RWStructuredBuffer<uint> RadixBlockSums : register(u1);

groupshared uint Scan[GS_RADIX_SCAN_BLOCK_SIZE];

[numthreads(GS_RADIX_SCAN_BLOCK_SIZE, 1, 1)]
void CSMain(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint bin = group_id.y;
    uint sort_groups = SortState[GS_STATE_SORT_GROUPS];
    uint scan_blocks = SortState[GS_STATE_SCAN_BLOCKS];
    uint group_base = group_id.x * GS_RADIX_SCAN_BLOCK_SIZE;
    uint sort_group = group_base + lane;

    uint radix_aux_capacity = 0u;
    uint radix_aux_stride = 0u;
    RadixGroupCounts.GetDimensions(radix_aux_capacity, radix_aux_stride);
    uint aux_groups = radix_aux_capacity / GS_RADIX_BINS;

    uint block_capacity = 0u;
    uint block_stride = 0u;
    RadixBlockSums.GetDimensions(block_capacity, block_stride);
    uint block_count_capacity = block_capacity / GS_RADIX_BINS;

    uint v = 0u;
    if (bin < GS_RADIX_BINS && group_id.x < scan_blocks && sort_group < sort_groups && sort_group < aux_groups)
        v = RadixGroupCounts[sort_group * GS_RADIX_BINS + bin];

    Scan[lane] = v;
    GroupMemoryBarrierWithGroupSync();

    // Blelloch upsweep.
    [loop]
    for (uint offset = 1u; offset < GS_RADIX_SCAN_BLOCK_SIZE; offset <<= 1u)
    {
        uint step = offset << 1u;
        uint idx = (lane + 1u) * step - 1u;
        if (idx < GS_RADIX_SCAN_BLOCK_SIZE)
            Scan[idx] += Scan[idx - offset];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
    {
        uint total = Scan[GS_RADIX_SCAN_BLOCK_SIZE - 1u];
        if (bin < GS_RADIX_BINS && group_id.x < block_count_capacity)
            RadixBlockSums[group_id.x * GS_RADIX_BINS + bin] = total;
        Scan[GS_RADIX_SCAN_BLOCK_SIZE - 1u] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Blelloch downsweep. After this, Scan[lane] is the exclusive prefix.
    [loop]
    for (uint offset = GS_RADIX_SCAN_BLOCK_SIZE >> 1u; offset >= 1u; offset >>= 1u)
    {
        uint step = offset << 1u;
        uint idx = (lane + 1u) * step - 1u;
        if (idx < GS_RADIX_SCAN_BLOCK_SIZE)
        {
            uint t = Scan[idx - offset];
            Scan[idx - offset] = Scan[idx];
            Scan[idx] += t;
        }
        GroupMemoryBarrierWithGroupSync();
        if (offset == 1u)
            break;
    }

    if (bin < GS_RADIX_BINS && group_id.x < scan_blocks && sort_group < sort_groups && sort_group < aux_groups)
        RadixGroupOffsets[sort_group * GS_RADIX_BINS + bin] = Scan[lane];
}
