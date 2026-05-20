#include "gs_common.hlsl"

// Radix pass B2: scan the tile totals for each bin.
//
// Dispatch as 1 x 16 x 1. One workgroup per bin scans up to
// GS_RADIX_MAX_SCAN_BLOCKS tile totals. The included project uses 62 blocks
// for 8M splats, but the shader clamps to buffer dimensions.

StructuredBuffer<uint> RadixBlockSums : register(t0);
StructuredBuffer<uint> SortState : register(t1);
RWStructuredBuffer<uint> RadixBlockOffsets : register(u0);
RWStructuredBuffer<uint> RadixBinStarts : register(u1);

groupshared uint Scan[GS_RADIX_MAX_SCAN_BLOCKS];

[numthreads(GS_RADIX_MAX_SCAN_BLOCKS, 1, 1)]
void CSMain(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint bin = group_id.y;

    uint block_capacity = 0u;
    uint block_stride = 0u;
    RadixBlockSums.GetDimensions(block_capacity, block_stride);
    uint block_count_capacity = min(block_capacity / GS_RADIX_BINS, (uint)GS_RADIX_MAX_SCAN_BLOCKS);
    uint scan_blocks = min(SortState[GS_STATE_SCAN_BLOCKS], block_count_capacity);

    uint v = 0u;
    if (bin < GS_RADIX_BINS && lane < scan_blocks)
        v = RadixBlockSums[lane * GS_RADIX_BINS + bin];

    Scan[lane] = v;
    GroupMemoryBarrierWithGroupSync();

    // Blelloch upsweep across the fixed max block count. Entries beyond the
    // active block count are zero, so the final total remains correct.
    [loop]
    for (uint offset = 1u; offset < GS_RADIX_MAX_SCAN_BLOCKS; offset <<= 1u)
    {
        uint step = offset << 1u;
        uint idx = (lane + 1u) * step - 1u;
        if (idx < GS_RADIX_MAX_SCAN_BLOCKS)
            Scan[idx] += Scan[idx - offset];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
    {
        if (bin < GS_RADIX_BINS)
            RadixBinStarts[bin] = Scan[GS_RADIX_MAX_SCAN_BLOCKS - 1u];
        Scan[GS_RADIX_MAX_SCAN_BLOCKS - 1u] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Blelloch downsweep.
    [loop]
    for (uint offset = GS_RADIX_MAX_SCAN_BLOCKS >> 1u; offset >= 1u; offset >>= 1u)
    {
        uint step = offset << 1u;
        uint idx = (lane + 1u) * step - 1u;
        if (idx < GS_RADIX_MAX_SCAN_BLOCKS)
        {
            uint t = Scan[idx - offset];
            Scan[idx - offset] = Scan[idx];
            Scan[idx] += t;
        }
        GroupMemoryBarrierWithGroupSync();
        if (offset == 1u)
            break;
    }

    if (bin < GS_RADIX_BINS && lane < scan_blocks)
        RadixBlockOffsets[lane * GS_RADIX_BINS + bin] = Scan[lane];
}
