#include "gs_common.hlsl"

// Radix pass A: build per-sort-group histograms.
//
// Each 128-thread group handles up to 128 SortPair entries and writes 16 counts:
//   RadixGroupCounts[group * 16 + bin]

StructuredBuffer<SortPair> InputPairs : register(t0);
StructuredBuffer<uint> SortState : register(t1);
RWStructuredBuffer<uint> RadixGroupCounts : register(u0);

groupshared uint SharedBins[GS_RADIX_BINS];

[numthreads(GS_THREADS, 1, 1)]
void CSMain(uint3 group_id : SV_GroupID, uint lane : SV_GroupIndex, uint3 dispatch_id : SV_DispatchThreadID)
{
    if (lane < GS_RADIX_BINS)
        SharedBins[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint sort_count = SortState[GS_STATE_SORT_COUNT];
    uint sort_groups = SortState[GS_STATE_SORT_GROUPS];
    uint shift = SortState[GS_STATE_RADIX_SHIFT];

    if (SortState[GS_STATE_DONE] == 0u && group_id.x < sort_groups && dispatch_id.x < sort_count)
    {
        uint bin = gs_radix_bin(InputPairs[dispatch_id.x].key, shift);
        InterlockedAdd(SharedBins[bin], 1u);
    }

    GroupMemoryBarrierWithGroupSync();

    uint radix_aux_capacity = 0u;
    uint radix_aux_stride = 0u;
    RadixGroupCounts.GetDimensions(radix_aux_capacity, radix_aux_stride);
    uint aux_groups = radix_aux_capacity / GS_RADIX_BINS;

    if (lane < GS_RADIX_BINS && group_id.x < aux_groups)
        RadixGroupCounts[group_id.x * GS_RADIX_BINS + lane] = SharedBins[lane];
}
