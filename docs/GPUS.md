# GPUs: what the runtime needs, and older cards

Written 2026-11-07 after a tester's Radeon RX 580 (Polaris, GCN 4) stopped at "No graphics card with Vulkan 1.3
and the extensions the game needs". Nothing below is implemented yet; it is the plan for whoever picks it up.

## What is required today

`runtime/vk.c` (`device_exts`, checked in `pick_device`): Vulkan 1.3 and these device extensions:

| Extension | Used for |
|---|---|
| `KHR_swapchain` | presenting |
| `EXT_external_memory_host` | guest memory imported as Vulkan buffers |
| `EXT_extended_dynamic_state2`, `EXT_extended_dynamic_state3` | state set per draw instead of per pipeline |
| `EXT_vertex_input_dynamic_state` | vertex formats / streams set per draw (`gpu.c` `set_vertex_input`) |
| `EXT_custom_border_color` | Maxwell samplers' border colours |
| `EXT_robustness2` | null descriptors (unset attributes read zeros) |
| `EXT_depth_clip_enable`, `EXT_depth_clip_control` | the console's depth clip / [-1, 1] depth |
| `KHR_push_descriptor` | descriptors per draw |
| `KHR_index_type_uint8` | 8-bit index buffers |
| `EXT_transform_feedback` | GPU skinning (point draws whose outputs become vertex buffers) |

Plus, not checked but assumed: **32-wide subgroups** (subgroup size control asks for 32 on every stage; a GPU that
cannot only logs "subgroups are not 32 wide").

Since 2026-11-07 the error box and log.txt name what each GPU lacks (`vk: <name> (Vulkan x.y, driver ..): missing
...`), so a tester's message or log is enough to tell what is missing. Outside the game: `vulkaninfo > vk.txt`
(ships with the driver), or the device's reports on gpuinfo.org.

## RX 580 (Polaris) on Windows: what it lacks

The tester's message (AMD's Polaris driver, Vulkan 1.3): **`EXT_vertex_input_dynamic_state` and
`KHR_index_type_uint8`**. Everything else on the list is there. Other GCN cards (RX 4xx / 5xx, Vega) are on the same
legacy driver branch and likely lack the same two.

### 1. `KHR_index_type_uint8`: small (~20 lines)

Without it Vulkan takes only 16- and 32-bit indices. Fallback, only when the extension is absent:

- Direct indexed draws with `index_type == 0` (`gpu.c`, the `OP_DRAW_ELEMENTS` path, `VK_INDEX_TYPE_UINT8_KHR`):
  read the indices on the CPU (`gpu_cpu(d->index_addr, d->count)`), widen to u16 into `staging()`, bind that as
  `VK_INDEX_TYPE_UINT16` with the same `base_vertex`. `quad_indices` already does this shape of work.
- Indirect indexed draws with 8-bit indices: the count is in a GPU buffer, so the CPU cannot convert them. Skip with
  one log line first (count them); a compute pass that widens them is the real fix if any show up.
- Primitive restart: the 8-bit restart index 0xff must become 0xffff when widened.
- Eden does it on the GPU instead: a compute pass widens u8 to u16 before the draw
  (`deps/suyu` `src/video_core/renderer_vulkan/vk_compute_pass.cpp`, `Uint8Pass`, used from `vk_buffer_cache.cpp`
  when the device lacks 8-bit indices). That also covers indices the GPU wrote; behaviour only, never copied.
- Don't enable the feature in `vkCreateDevice` (`vk.c`, `VkPhysicalDeviceIndexTypeUint8FeaturesKHR` in the chain)
  and move the extension from required to optional (like pipeline binaries).

### 2. `EXT_vertex_input_dynamic_state`: medium (a session plus testing)

Today `set_vertex_input` builds the bindings and attributes per draw and calls `vkCmdSetVertexInputEXT`; pipelines
list `VK_DYNAMIC_STATE_VERTEX_INPUT_EXT`. Without the extension the vertex input goes into the pipeline:

- Add the layout to the pipeline key (`gpu.c` `PipeKey`): per attribute location the format (after the
  UINT -> SFLOAT / USCALED rewrites `set_vertex_input` does), stream and offset; per stream instancing (rate,
  divisor). Hash it into the key rather than store 48 attributes inline.
- Strides can stay dynamic with core 1.3: `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` and
  `vkCmdBindVertexBuffers2` with strides, so they stay out of the key.
- Build the `VkPipelineVertexInputStateCreateInfo` from the key; drop `VK_DYNAMIC_STATE_VERTEX_INPUT_EXT` from `dyn[]`
  (as is done for the logic op without `extendedDynamicState2LogicOp`).
- The draw path picks the pipeline after the vertex layout is known; `set_vertex_input` only binds buffers.
- Cost on such cards only: more pipelines (one per program x layout), so more first-time stutter; pipeline caches
  and pipeline binaries cover repeats. GPUs with the extension keep today's path.
- Eden does the same: its pipeline key (`fixed_pipeline_state.h`, `dynamic_vertex_input` bit) carries the
  attributes and bindings only when the device has no dynamic vertex input.
- Test switch: an `NX_NO_VI_DYNAMIC=1` (read once at start) that takes the fallback on a card that has the
  extension, so it can be checked on the 7800 XT / GTX 1650 Ti before a tester's run.

### 3. 64-wide waves: the large one

GCN runs 64-wide waves only (`minSubgroupSize` 64); RDNA can run 32, NVIDIA is 32. The translated shaders assume
Maxwell's 32-lane warps (ballots, lane masks, shuffles: `tools/shader/translate.py` / `maxwell.py`). On AMD before
we forced 32, lanes 32-63 were lost and a compute loop spun until the driver reset (comment in `vk.c`, subgroup
size). So with 1 and 2 done, an RX 580 would start, then likely show broken effects or hang the GPU in some shaders.

The fix is in the translator: when the subgroup is 64 wide, emulate 32-lane warps. Eden's shader recompiler does
exactly this (`deps/suyu` `src/shader_recompiler/backend/spirv/emit_spirv_warp.cpp`, behaviour only, never copied),
switched on by `warp_size_potentially_larger_than_guest` (`vulkan_device.cpp`: no subgroup size control, or a
maximum above 32):
- lane id = subgroup invocation id & 31 (`EmitLaneId`);
- ballots and the eq / lt / le / gt / ge masks take the 32-bit word of the 128-bit ballot that holds this lane's
  half (`WarpExtract`: word (invocation id >> 5));
- vote all / any / equal are ballots of the predicate compared with the ballot of `true` (the active lanes of this
  half), not the subgroup-wide OpGroupNonUniformAll / Any;
- shuffles add the half's base (invocation id >> 5 << 5) to the source lane (`HostThreadId`); a broadcast of lane n
  reads n, n + 32, ... and picks by the half (`BroadcastLane`). Our places to change, all in `tools/shader/translate.py`: the S2R lane id (`gl_SubgroupInvocationID`) and lane
masks (`gl_Subgroup*Mask.x`), VOTE (`subgroupBallot(v_).x`, `subgroupAll / Any / AllEqual`), SHFL
(`subgroupShuffle` with a lane computed from `gl_SubgroupInvocationID`); the quad ops (`subgroupQuadSwap*`, the
FSWZADD lane & 3) are fine at any width. The translator version would need a variant
per subgroup width (shader caches and `shaders.pak` are per translator version).

## Order

1, then 2 (checked with the test switch on our cards), then a tester's run on the RX 580 to see how far it gets,
then decide on 3 from what breaks. If GCN cards are not worth it, say in the package README that the game needs an
AMD RDNA (RX 5000 or newer), NVIDIA GTX 10-series or newer, or a recent Intel GPU.
