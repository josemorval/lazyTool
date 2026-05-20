#include "gs_common.hlsl"

// Radix pass D: stable scatter into the other ping-pong SortPair buffer.
//
// Stability matters because this is an LSD radix sort: lower digits are sorted
// first, and later higher-digit passes must preserve that previous order.
//
// We keep the implementation deliberately transparent:
// - RadixBinStarts gives the global base offset for each bin.
// - RadixGroupOffsets gives the prefix for prior thread groups in the same bin.
// - The local rank is counted in lane order inside this 128-thread group.
//
// That local rank loop is simple and predictable. It avoids hidden counters and
// works on ordinary StructuredBuffers.

StructuredBuffer<SortPair> InputPairs : register(t0);
StructuredBuffer<uint> RadixGroupOffsets : register(t1);
StructuredBuffer<uint> RadixBinStarts : register(t2);
StructuredBuffer<uint> SortState : register(t3);
RWStructuredBuffer<SortPair> OutputPairs : register(u0);

groupshared uint LocalBins[GS_THREADS];
groupshared uint LocalValid[GS_THREADS];

[numthreads(GS_THREADS, 1, 1)]
void CSMain(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex, uint3 dispatch_id : SV_DispatchThreadID)
{
    uint sort_count = SortState[GS_STATE_SORT_COUNT];
    uint sort_groups = SortState[GS_STATE_SORT_GROUPS];
    uint shift = SortState[GS_STATE_RADIX_SHIFT];
    bool valid = SortState[GS_STATE_DONE] == 0u && group_id.x < sort_groups && dispatch_id.x < sort_count;

    SortPair p = gs_invalid_pair();
    uint bin = 0u;
    if (valid)
    {
        p = InputPairs[dispatch_id.x];
        bin = gs_radix_bin(p.key, shift);
    }

    LocalBins[lane] = bin;
    LocalValid[lane] = valid ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();

    if (!valid)
        return;

    uint local_rank = 0u;
    [loop]
    for (uint i = 0u; i < lane; i++)
    {
        if (LocalValid[i] != 0u && LocalBins[i] == bin)
            local_rank++;
    }

    uint group_prefix = RadixGroupOffsets[group_id.x * GS_RADIX_BINS + bin];
    uint bin_start = RadixBinStarts[bin];
    OutputPairs[bin_start + group_prefix + local_rank] = p;
}
