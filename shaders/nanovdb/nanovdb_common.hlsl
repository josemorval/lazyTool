// Shared NanoVDB helpers for lazyTool shaders.
//
// PNanoVDB.h is the official portable NanoVDB access layer from OpenVDB. The
// helper functions below are local glue: sampler wrappers, active-bbox queries,
// normalization remaps, ray/box intersection and procedural stochastic jitter.
#ifndef LAZYTOOL_NANOVDB_COMMON_HLSL
#define LAZYTOOL_NANOVDB_COMMON_HLSL

#include "../common.hlsl"

#define PNANOVDB_HLSL
#include "PNanoVDB.h"

float lt_nvdb_read_float_coord(pnanovdb_buf_t buf, uint grid_byte_offset, int3 ijk)
{
    pnanovdb_grid_handle_t grid;
    grid.address.byte_offset = grid_byte_offset;
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc, root);
    pnanovdb_coord_t coord = ijk;
    pnanovdb_address_t value_address =
        pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_FLOAT, buf, acc, coord);
    return pnanovdb_read_float(buf, value_address);
}

float lt_nvdb_read_float_coord_acc(pnanovdb_buf_t buf, inout pnanovdb_readaccessor_t acc, int3 ijk)
{
    pnanovdb_coord_t coord = ijk;
    pnanovdb_address_t value_address =
        pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_FLOAT, buf, acc, coord);
    return pnanovdb_read_float(buf, value_address);
}

float lt_nvdb_sample_nearest_float(pnanovdb_buf_t buf, uint grid_byte_offset, float3 world_pos)
{
    pnanovdb_grid_handle_t grid;
    grid.address.byte_offset = grid_byte_offset;
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc, root);

    float3 index_pos = pnanovdb_grid_world_to_indexf(buf, grid, world_pos);
    int3 ijk = int3(floor(index_pos + 0.5));
    return lt_nvdb_read_float_coord_acc(buf, acc, ijk);
}

float lt_nvdb_sample_linear_float(pnanovdb_buf_t buf, uint grid_byte_offset, float3 world_pos)
{
    pnanovdb_grid_handle_t grid;
    grid.address.byte_offset = grid_byte_offset;
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc, root);

    float3 p = pnanovdb_grid_world_to_indexf(buf, grid, world_pos);
    float3 basef = floor(p);
    int3 i0 = int3(basef);
    float3 f = saturate(p - basef);

    float v000 = lt_nvdb_read_float_coord_acc(buf, acc, i0 + int3(0, 0, 0));
    float v100 = lt_nvdb_read_float_coord_acc(buf, acc, i0 + int3(1, 0, 0));
    float v010 = lt_nvdb_read_float_coord_acc(buf, acc, i0 + int3(0, 1, 0));
    float v110 = lt_nvdb_read_float_coord_acc(buf, acc, i0 + int3(1, 1, 0));
    float v001 = lt_nvdb_read_float_coord_acc(buf, acc, i0 + int3(0, 0, 1));
    float v101 = lt_nvdb_read_float_coord_acc(buf, acc, i0 + int3(1, 0, 1));
    float v011 = lt_nvdb_read_float_coord_acc(buf, acc, i0 + int3(0, 1, 1));
    float v111 = lt_nvdb_read_float_coord_acc(buf, acc, i0 + int3(1, 1, 1));

    float v00 = lerp(v000, v100, f.x);
    float v10 = lerp(v010, v110, f.x);
    float v01 = lerp(v001, v101, f.x);
    float v11 = lerp(v011, v111, f.x);
    return lerp(lerp(v00, v10, f.y), lerp(v01, v11, f.y), f.z);
}

void lt_nvdb_active_world_bounds(pnanovdb_buf_t buf, uint grid_byte_offset,
                                 out float3 world_min, out float3 world_max)
{
    // NanoVDB stores an active voxel bbox in index space. Convert that bbox
    // through the grid transform once per pixel, then reuse it for all samples.
    pnanovdb_grid_handle_t grid;
    grid.address.byte_offset = grid_byte_offset;
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);
    pnanovdb_coord_t bbox_min = pnanovdb_root_get_bbox_min(buf, root);
    pnanovdb_coord_t bbox_max = pnanovdb_coord_add(pnanovdb_root_get_bbox_max(buf, root),
                                                   pnanovdb_coord_uniform(1));
    float3 w0 = pnanovdb_grid_index_to_worldf(buf, grid, pnanovdb_coord_to_vec3(bbox_min));
    float3 w1 = pnanovdb_grid_index_to_worldf(buf, grid, pnanovdb_coord_to_vec3(bbox_max));
    world_min = min(w0, w1);
    world_max = max(w0, w1);
}

float3 lt_remap_box_to_box(float3 p, float3 src_min, float3 src_max,
                           float3 dst_min, float3 dst_max)
{
    float3 uv = saturate((p - src_min) / max(src_max - src_min, 1e-5.xxx));
    return lerp(dst_min, dst_max, uv);
}

float3 lt_fit_box_to_box_uniform(float3 p, float3 src_min, float3 src_max,
                                 float3 dst_min, float3 dst_max)
{
    float3 src_center = (src_min + src_max) * 0.5;
    float3 src_half = max((src_max - src_min) * 0.5, 1e-5.xxx);
    float3 dst_center = (dst_min + dst_max) * 0.5;
    float3 dst_half = (dst_max - dst_min) * 0.5;
    float dst_radius = max(max(dst_half.x, dst_half.y), max(dst_half.z, 1e-5));
    return dst_center + ((p - src_center) / src_half) * dst_radius;
}

bool lt_ray_box(float3 ro, float3 rd, float3 bmin, float3 bmax, out float t0, out float t1)
{
    float3 safe_rd = rd;
    safe_rd.x = abs(safe_rd.x) < 1e-6 ? (safe_rd.x < 0.0 ? -1e-6 : 1e-6) : safe_rd.x;
    safe_rd.y = abs(safe_rd.y) < 1e-6 ? (safe_rd.y < 0.0 ? -1e-6 : 1e-6) : safe_rd.y;
    safe_rd.z = abs(safe_rd.z) < 1e-6 ? (safe_rd.z < 0.0 ? -1e-6 : 1e-6) : safe_rd.z;
    float3 inv_rd = rcp(safe_rd);
    float3 a = (bmin - ro) * inv_rd;
    float3 b = (bmax - ro) * inv_rd;
    float3 mn = min(a, b);
    float3 mx = max(a, b);
    t0 = max(max(mn.x, mn.y), mn.z);
    t1 = min(min(mx.x, mx.y), mx.z);
    t0 = max(t0, 0.0);
    return t1 >= t0;
}

float lt_interleaved_gradient_noise(float2 pixel, uint frame)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(pixel + float2(frame & 1023u, frame >> 10u), magic.xy)));
}

float lt_hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float lt_temporal_r2_noise(float2 pixel, uint frame)
{
    // R2-style frame offset: cheap low-discrepancy temporal decorrelation.
    // This is not a true blue-noise texture, but it avoids static band patterns
    // without adding another bound resource.
    float r = frac((float)frame * 0.754877666);
    return frac(lt_interleaved_gradient_noise(pixel, frame) + r);
}

float lt_stochastic_volume_jitter(float2 pixel, uint frame, float mode)
{
    if (mode < 0.5)
        return lt_interleaved_gradient_noise(pixel, frame);
    return frac(0.72 * lt_temporal_r2_noise(pixel, frame) +
                0.28 * lt_hash12(pixel + float2(frame * 17u, frame * 29u)));
}

#endif
