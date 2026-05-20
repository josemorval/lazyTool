#include "gs_common.hlsl"

// Radix pass E: advance the state to the next 4-bit digit.
//
// Eight passes cover all 32 key bits. The project repeats a pair of passes
// A->B and B->A four times, so after the last pass the sorted output is back
// in gs_sort_pairs_a, the buffer consumed by draw_splats.hlsl.

RWStructuredBuffer<uint> SortState : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x != 0 || id.y != 0 || id.z != 0)
        return;
    if (SortState[GS_STATE_DONE] != 0u)
        return;

    uint shift = SortState[GS_STATE_RADIX_SHIFT] + GS_RADIX_BITS;
    SortState[GS_STATE_RADIX_SHIFT] = shift;
    if (shift >= 32u)
        SortState[GS_STATE_DONE] = 1u;
}
