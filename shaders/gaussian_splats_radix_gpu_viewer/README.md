# Gaussian Splats Radix GPU Viewer

Self-contained lazyTool shader set for full-GPU Gaussian splat rendering with a
hierarchical 4-bit radix sort.

The project using this folder is:

```text
projects/gaussian_splats_radix_gpu_viewer.lt
```

Pipeline:

1. `reset_args.hlsl` clears frame counters and indirect arguments.
2. `cull_and_key.hlsl` culls splats against the camera and compact-writes visible `(depthKey, splatIndex)` pairs.
3. `finalize_sort_args.hlsl` clamps the visible count to editor settings and real buffer capacities, then writes indirect dispatch arguments.
4. `radix_histogram.hlsl` builds per-group histograms.
5. `radix_scan_group_counts.hlsl`, `radix_scan_block_sums.hlsl`, and `radix_add_block_offsets.hlsl` build deterministic global prefixes without a single giant groupshared scan.
6. `radix_prefix_bins.hlsl` converts per-bin totals into global bin starts.
7. `radix_scatter.hlsl` stable-scatters into the ping-pong pair buffer.
8. `radix_advance.hlsl` advances the 4-bit radix digit.
9. `draw_splats.hlsl` draws procedural Gaussian quads with `DrawInstancedIndirect`.
10. `debug_counter_overlay.hlsl` draws a pixel-font visible-splat counter over the final scene color.

Capacity notes:

- The included project allocates scratch buffers for up to **8,000,000 visible splats**.
- `MaxVisible` and `SortCapacity` live in UserCB and can be changed from the editor.
- The shader clamps to the actual sizes of `gs_sort_pairs_*`, `gs_radix_group_*`, and `gs_radix_block_*`, so loading a different PLY does not require editing HLSL constants.
- Lower `SortCapacity` in the editor for faster iteration on very heavy PLYs.

Transform/tuning controls live in `UserCB`:

- `SceneOffsetScale.xyz`: translation.
- `SceneOffsetScale.w`: uniform scale.
- `SceneAxisScale.xyz`: per-axis scale; use a negative component to flip an upside-down PLY.
- `SplatTuning.x/y`: visible splat footprint.
- `SplatTuning.z`: alpha discard threshold.
- `SplatTuning.w`: opacity multiplier.
- `CullingTuning.x`: conservative frustum padding.
- `CullingTuning.z`: optional max one-sigma world radius clamp.
- `CullingTuning.w`: anisotropy amount.
- `CloseTuning`: near-camera tail softening.

This radix path is still a full per-frame sort. 8M splats is a high ceiling, not
a promise that every GPU will sort and draw 8M splats interactively. For very
large scenes, the next optimization step would be tiled/approximate ordering,
LOD, or an OIT path rather than exact global sorting every frame.
