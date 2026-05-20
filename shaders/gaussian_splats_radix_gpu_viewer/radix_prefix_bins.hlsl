#include "gs_common.hlsl"

// Radix pass C: convert per-bin totals into global bin starts.
//
// radix_scan_block_sums.hlsl leaves RadixBinStarts[bin] containing the bin total.
// This pass turns that into an exclusive prefix:
//
//   bin 0 starts at 0
//   bin 1 starts after all bin 0 entries
//   bin 2 starts after all bin 0+1 entries
//   ...

RWStructuredBuffer<uint> RadixBinStarts : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x != 0 || id.y != 0 || id.z != 0)
        return;

    uint running = 0u;
    [unroll]
    for (uint bin = 0u; bin < GS_RADIX_BINS; bin++)
    {
        uint count = RadixBinStarts[bin];
        RadixBinStarts[bin] = running;
        running += count;
    }
}
