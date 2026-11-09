# GPUs: what the runtime needs, and older cards

Written 2026-11-07 after a tester's Radeon RX 580 (Polaris, GCN 4) stopped at "No graphics card with Vulkan 1.3
and the extensions the game needs". Nothing below is implemented yet; it is the plan for whoever picks it up.

Rough sizes (2026-11-07): parts 1 and 2 together about half an hour of edits plus builds and a test run; part 3
a few hours, most of it re-translating and checking shaders. Parts 1 and 2 get an RX 580 past the start; part 3 is
what makes it draw right.

## Before starting

- Build and run: `pkmn-violet/CLAUDE.md`, "Start here" (`.\start.ps1 violet` builds and plays;
  `.\start.ps1 package violet <dir>` makes a player folder). Toolchain: the root `CLAUDE.md`.
- New `NX_*` switches are read once at start (an init function or a static on first use), never per draw or frame.
- Run with `NX_VK_VALIDATION=1` (needs the Vulkan SDK's layers) and keep it free of errors: graphics must not rely
  on one vendor's handling of undefined behaviour, which is exactly what an older card will expose.
- `deps/suyu` (Eden) is for behaviour only: read it, write our own; never copy code.

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

### 1. `KHR_index_type_uint8`: small (~15-20 lines)

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
  and move the extension from required to optional (like pipeline binaries): out of `device_exts`, added to the
  device's extension list only when `has_ext` finds it, with a `vk.u8_indices` flag for gpu.c. Same pattern for
  part 2's extension.
- Check: a test switch (`NX_NO_U8_INDICES=1`) that takes the fallback on a card that has the extension; the log
  should count the widened draws, and the scenes should look the same as without it.

### 2. `EXT_vertex_input_dynamic_state`: medium (~15-20 minutes of edits plus testing)

Today `set_vertex_input` builds the bindings and attributes per draw and calls `vkCmdSetVertexInputEXT`; pipelines
list `VK_DYNAMIC_STATE_VERTEX_INPUT_EXT`. Without the extension the vertex input goes into the pipeline:

- Add the layout to the pipeline key (`gpu.c` `PipeKey`): per attribute location the format (after the
  UINT -> SFLOAT / USCALED rewrites `set_vertex_input` does), stream and offset; per stream instancing (rate,
  divisor). Hash it into the key rather than store 48 attributes inline.
- Strides can stay dynamic with core 1.3: `VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE` and
  `vkCmdBindVertexBuffers2` with strides, so they stay out of the key.
- Build the `VkPipelineVertexInputStateCreateInfo` from the key; drop `VK_DYNAMIC_STATE_VERTEX_INPUT_EXT` from `dyn[]`
  (as is done for the logic op without `extendedDynamicState2LogicOp`).
- Order in the draw path: today the pipeline is found and bound first (`gpu.c`, `vkCmdBindPipeline` around line
  4650, through `pipe_find` / `pipe_claim`) and `set_vertex_input` runs after (around line 4697). On the fallback,
  compute the layout (the attribute loop at the top of `set_vertex_input`) before the pipeline lookup, put its hash
  in the key, then let `set_vertex_input` only bind the vertex buffers.
- The key is saved: `pipelines.list` (header `PIPE_LIST_MAGIC`, `PIPE_LIST_VERSION`, `sizeof(PipeKey)`) lists every
  key to rebuild at start, and `pipelines.bin` / `pipelines.bins` hold the driver's cache and pipeline binaries. A
  bigger `PipeKey` already invalidates the list (the size is in the header); bump `PIPE_LIST_VERSION` anyway. Keep
  `PipeKey` free of padding (memcmp and hashing: see its `pad` field).
- Keep the hash's inputs as the key, not only the hash, or two layouts with the same hash share a pipeline: either
  store the layout in a side table keyed by its hash and compare it on lookup, or store it in `PipeKey`.
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
  reads n, n + 32, ... and picks by the half (`BroadcastLane`).

Our places to change, all in `tools/shader/translate.py`:
- S2R: the lane id (`gl_SubgroupInvocationID`, special register 0) and the lane masks (`gl_Subgroup*Mask.x`,
  around line 1160): `& 31u` and the half's word of the mask;
- VOTE (around line 1736): `subgroupBallot(v_).x` becomes the half's word; `subgroupAll / Any / AllEqual` become
  compares against the ballot of `true`, as above;
- SHFL (around line 575): the lane `l_` is the 32-lane one, and `subgroupShuffle`'s source adds the half's base;
- the quad ops (`subgroupQuadSwap*`, FSWZADD's `gl_SubgroupInvocationID & 3u`) are fine at any width.

Emit the 64-wide forms behind a translator option (e.g. `--warp64`), not always: they cost instructions on cards
that run 32 wide. Translations are stored per translator version (`translator_version()`, an FNV-1a of
translate.py; `gpu.c` `spv_path` computes the same), and `shaders.pak` is per version too, so the 64-wide output needs
its own directory and pack: e.g. the version plus a width suffix, chosen in `spv_path` / `pak_open` from `vk.warp32`
(`vk.c`, around line 358: the stages that can be forced to 32). A package would then carry two packs, or translate
the 64-wide set on the player's PC (`-PythonZip`).

Testing without a GCN card: RDNA (the 7800 XT) can run 64-wide waves too (`maxSubgroupSize` 64). A switch
(`NX_WARP64=1`) that requests `requiredSubgroupSize = 64` instead of `vk_warp32` (`vk.c` line 30, used in `gpu.c`
pipeline creation around line 3308) and selects the 64-wide translations reproduces the RX 580's situation: without
the translator change it should show the old breakage (lost lanes, a compute loop hanging), with it the scenes should
match the 32-wide run. NVIDIA is 32 only and cannot test this.

## Order

1, then 2 (checked with the test switches on our cards), then a tester's run on the RX 580 to see how far it gets
(their log.txt, and F12 captures of anything drawn wrong), then 3 (checked with `NX_WARP64=1` on the 7800 XT before
the tester's next run). Also expect, on an RX 580: AMD's Polaris driver is a frozen legacy branch (bugs there stay),
and the card is far slower than our test GPUs, so a lower render scale and 30 fps are the starting point. If GCN cards are not worth it, say in the package README that the game needs an
AMD RDNA (RX 5000 or newer), NVIDIA GTX 10-series or newer, or a recent Intel GPU.
