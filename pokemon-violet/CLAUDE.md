# pokemon-violet: Pokémon Violet recompilation

Static recompilation of the user's own Pokémon Violet **4.0.0** (title 01008F6008C5E000, 64-bit AArch64, with the
Teal Mask / Indigo Disk DLC) to a native Windows x64 program, built on what `../mk8dx` proved: our own AArch64 → C
lifter, the game's own SDK running over an emulated kernel and services, NVN replayed into Vulkan.

Workspace rules, toolchain and shared tools (nstool, keys) are in `../CLAUDE.md`. Paths below are relative to
`pokemon-violet/`. Game-derived data stays in `rom/`, `extract/`, `dump/` and `build/`.

## What the game is (from the 4.0.0 ExeFS, 2026-10-17)

| Module | text | ro | data+bss | Notes |
|---|---|---|---|---|
| main | 0x343fc90 (52.2 MB, ~13.7 M insns) | 0xf420a0 | 0x368868 + 0xe6e38 | game, engine ("Trinity"), all middleware statically linked; 1,013 imports |
| sdk | 0x5d5660 (5.8 MB) | 0x6add7c | | NintendoSDK 13.3.13 (MK8: 4.4.0) |
| subsdk0 | 0x33e7d0 (3.2 MB) | 0x210194 | | `movie::` (android NuPlayer/ACodec), as in MK8 |

About 16 M instructions in all, 3.4 times MK8's 4.76 M. Compared with MK8:

- **C++ exceptions are used**: main imports `__cxa_throw`, `__cxa_begin_catch`, `_Unwind_Resume`,
  `__gxx_personality_v0`, `__cxa_rethrow` (std, gRPC, Havok, the engine). The lifter needs a design for them (Plan, V1).
- **Graphics**: `nvnBootstrapLoader` + `nv::InitializeGraphics` / `SetGraphicsAllocator`, as in MK8, but through
  NintendoSDK_gfx 13.3.13 (`nn::gfx`) and NintendoWare G3d / Vfx / Ui2d / Font. `glslc*` are weak imports (never called).
- **Middleware in main** (guest code, so it needs no work of its own): Havok AI 2021.1, Oodle (archive
  decompression), Wwise (audio; banks in `romfs/audio/wwise`), Bink 2 (`romfs/demo/movie/*.bk2`), Lua, FlatBuffers,
  nlib LZ4, Pia 6.20.1 + NPLN 1.8.3 + gRPC + OpenSSL + libcurl (online: Union Circle, raids, trades).
- **libc**: musl-style (`__nnmusl_init_dso`, pthreads, stdio, sockets) from the sdk.
- **Services in main.npdm** (36): acc:u0 aoc:u appletOE audin:u audout:u audren:u banana bcat:u bsd:u bsdcfg btm:u
  caps:su caps:u csrng ectx:aw friend:u fsp-srv htc:tenv htcs hwopus ldn:u ldr:ro mii:u mm:u nfc:mf:u nfc:user
  nfp:user ngct:u nifm:u nim:eca notif:a nsd:u nvdrv pctl pl:u prepo:u sfdnsres time:u vi:u.
  New relative to MK8: audout, audin, hwopus, bsd/sfdnsres (sockets), btm, caps, csrng, ectx, mm, nfc/nfp, ngct,
  notif, nim, nsd, bcat, htc*.
- **RomFS** (30 files, 9.24 GB): almost all of it is `arc/data.trpfs` (8.0 GB, Trinity pack file system, with its
  index `arc/data.trpfd`); Wwise `.pck`/`.bnk`, two Bink movies, `event.trevt`, prefetch lists. The engine reads
  them through fsp-srv; we don't need to understand the formats.

## Plan

Reuse first: almost everything in `../mk8dx/mk8recomp` is "Switch", not "Mario Kart". Each step ends with something
that runs, as MK8's milestones did.

**V0 Shared framework (restructure, do first).** Split `mk8recomp` into a shared `../nxrecomp/` (lifter, `a64rt`,
loader, kernel, IPC + services, NVN-on-Vulkan, audio renderer, input, shader translator, build scripts) and thin
per-game parts (module bases and lists, `overrides.txt`, game-specific host code such as MK8's `movie.c`, paths).
(At the time MK8 was checked to build and boot; MK8 regression checks are no longer done, see `../CLAUDE.md`.) Per-game config instead of constants:
module list, `a64lift.BASES`, title id, RomFS / save paths, services allowed by the npdm.

**V1 Lifter at scale.**
- IDA export for the three modules (main is 74 MB as ELF: the first analysis takes long; run `ida_export.py` with
  idalib in the background), then `a64lift.py cover` to 100%. New instruction shapes from an SDK-13 / newer-clang
  build get Unicorn tests (`a64test.py`).
- Build time and memory: ~3.4x MK8's 245 MB of C. Split output by module and by size so files compile in under
  30 s each; one static lib per module; check link time and the PDB size; keep within 16 GB (`LIFT_JOBS`).
  Re-lifting one module must not rebuild the others.
- **C++ exceptions.** First measure: trap in `__cxa_throw` and see whether anything throws offline (much of it may
  be gRPC/online code that never runs). If it does throw: lift landing pads (from `.eh_frame` / LSDA call-site
  tables) as secondary entries, run the guest's own personality routine and search phase over the guest stack
  (frame records are in guest memory), and resume by restoring guest registers and host-longjmp'ing to the lifted
  frame that owns the landing pad, the way `GUEST_SETJMP` / `h_longjmp` already work.

**V2 Boot to `nnMain`.** Loader with the new module sizes; SDK 13 SVCs (new numbers or semantics compared with 4.4:
check every SVC the sdk uses, as for MK8); sm over TIPC if SDK 13 uses it (check the sdk's sm client); the musl
init (`__nnmusl_init_dso`) and TLS layout of the newer SDK.

**V3 Headless main loop.** Services, in the order the game asks for them. Offline services answer "no network"
(nifm, bsd/sfdnsres sockets fail cleanly, nsd, bcat, friends, ldn/Pia local play off, nim, notif, ectx).
Must work: fsp-srv (RomFS, 9 GB with random access to data.trpfs; saves; the DLC via aoc:u listing
01008F6008C5F001 as owned), acc, time, set, hid, applet, pctl, mii, pl (fonts), caps (screenshots: accept and drop),
audren and/or audout (find which Wwise uses), hwopus (Opus decoding, if anything uses it), csrng (host CSPRNG),
ngct (profanity filter: pass everything), nfc/nfp (no amiibo), btm (no Bluetooth devices). Implement sub-interface
openers for real, never the null-object stub (the lesson from MK8's Time Trials crashes).

**V4 Graphics.** Our NVN on Vulkan, extended to what nn::gfx 13 + G3d use: newer API entry points, more compute
(Bink 2 may decode on the GPU, Vfx particles), tessellation / geometry shaders if used, texture views, sparse or
compressed formats, queries. Shaders: MK8's translator (Maxwell → GLSL → SPIR-V) with the dump-then-translate
loop; SV has far more shaders, so measure how many draws are skipped on first sight and decide between an offline
pre-pass (walking the shader archives in data.trpfs) and translating at run time in C. Target: title screen, then
the opening cutscene, then the overworld.

**V5 Audio, movies, input, saves.** Wwise's output through our audren/audout; Bink movies (guest code, check
speed); hid from SDL3 (as MK8); save data write/read; DLC area reachable.

**V6 Playable.** Overworld, battles, Tera raids (offline), DLC areas; long sessions (memory leaks in our caches,
16 GB RAM).

**V7 Performance and PC features.** Measure first. Unlocked frame rate and resolution only where the game's own
timing allows (SV ties logic to frame time in places: check before unlocking).
Quality options (the user's idea, 2026-10-19), ideally in the game's own Options menu:
- What could be offered. Things we control: render scale (the targets the game creates at 1920x1080 and
  its half-/quarter-res chains, scaled together; texture coordinates are normalised, but viewport, scissor
  and texel-size constants are not), anisotropic filtering (sampler overrides), shadow map size (1024x1024
  cascades today). Things in the game's own data: LOD and draw distance, grass density, the 30 fps cap.
  Find the variables in main first (the engine likely has quality tiers for docked and handheld:
  look for where the docked/handheld check, `nn::oe::GetOperationMode`, feeds render settings).
- Where the options live. (a) The game's Options menu: Trinity UI layouts and message text come from
  `data.trpfs`, the menu logic is guest code, maybe Lua. Adding entries means patching the menu's item list
  and text at load time, plus overriding the functions that apply them (`overrides.txt`). It looks native but
  breaks with game updates, and the new text must exist in every language the game ships. (b) Our own
  overlay (like the F3 FPS counter): a host-drawn menu on a hotkey, settings saved next to the exe. Much
  less work and independent of the game's version. Start with (b), then try (a) once the settings work.
- Order of work. Measure the cost of each option first (V7: measure before changing). Render scale first,
  since it is all ours (gpu.c image creation and viewport mapping).

**V8 Packaging for friends who own the game.** One launcher exe dropped next to their own NSPs, keys and firmware:
on first launch it checks the files (update version, keys), unpacks the NSPs (our own NSP/NCA reader, no nstool),
lifts and compiles the game code, extracts and translates every shader, then caches the result; later launches
start the game. Ships only our code: a prebuilt runtime, a bundled LLVM (clang + lld) and generated Windows import
libraries, embedded Python or the tools ported, the function list as data (or our own analysis) instead of
IDA/Ghidra. **Never ship anything built from the game** (lifted C, exe, shaders), not even to owners: each person
builds from their own copy. Needs before it: the up-front shader extraction (shaders from `data.trpfs` instead
of collected while playing) and a faster lift/compile (first-run time, now 20-40 min).

Risks: exceptions (V1) and the size of the build are the unknowns MK8 didn't have; NVN coverage is the largest amount
of work; online code paths must fail as on a console with no internet, not crash.

## Next (written 2026-10-22, updated 2026-10-31)

Where things stand (2026-10-22, RX 7800 XT + Ryzen 7 7800X3D, stand-still benchmark below): ~170 fps uncapped,
5.8-6.0 ms a frame, 1% lows ~9 ms. The goal is to get much closer to what the hardware allows. Rough scaling from a
docked Switch (Violet uses its whole 33 ms on both sides): the CPU is ~12-15x per thread (5.0 vs 1.02 GHz, ~2.5-3x
IPC over the A57), the GPU ~50x (37 TFLOPS with dual issue, ~19 without, vs 0.39), memory bandwidth ~24x. So game
logic should fit in ~2.2-2.8 ms (350-450 fps) and the GPU's work in ~0.7 ms. Measured: CPU ~4.1-4.3 ms (55-65% of
that, normal for a static recompilation, room left), GPU ~5.5 ms (**~8x off: the outlier, and what holds the frame
rate now**).

- **Stand-still benchmark** (use this for frame-rate work; the cutscene save is for hitches): copy `build/save` to
  `build/save-idle-run`, then `violetrecomp.exe --log 1 --save build\save-idle-run` with NX_NOPACE=1,
  NX_TIME=1792494000, NX_INPUT='300-900/60:A' and no input after that (keep hands off the window). The title and
  load run ~16,700 frames at ~880 fps, then two windows of load hitches; average the `nvn: frame time` windows after
  those (24+ windows in 105 s). Run to run spread ~0.2 ms a frame. `violetrecomp\bench.ps1 <name> [NX_X=v ...]`
  does all of it (log build/bench_<name>.log, frame time and NX_GPU_TIMELINE busy over the windows after the first
  one over 3 ms and the next: later single hitches are kept, they are part of the scene); `-Summary` re-reads a
  log; `-Rgp <frame>` takes an RGP capture of that frame into build/<name>.rgp (written 2026-10-31 from the
  steps that worked by hand, not run as a script yet). Run A / B pairs in both orders.
- What a frame is (2026-10-22): with rendering, the render thread (5c) translates ~3.4 ms (~2,000 draws, ~1.7 us a
  draw; Savage Wind's is 2.4) and waits ~1.6-2.8 ms for the GPU; main runs only ~40% (waits on 5c). With every draw
  skipped (NX_GPU_SKIP=1): 235-244 fps (4.1-4.3 ms), main 73% running, blocked ~27% (~21% on one condition
  variable, main+b47d18, ~11 waits a frame: its jobs on 5a/5b, each ~57% busy), translation 0.8 ms. Main's samples
  then: lifted code ~60% (flat: the hottest function 2.3%), our runtime ~8% (`call_ind` / `jump_ind` ~5%).
- Lifted calls (2026-10-30, nxrecomp `cf5f543`): NX_PROFILE now splits lifted samples by guest instruction kind.
  Main, CPU-only: calls ~23% of lifted samples (bl 9%, blr 4%, function entry 4%, ret 3%, br 3%), loads/stores
  27%, integer 29%, branches 12%, exclusives 3.5%; a `bl` was ~60-80 x86 instructions (~24 stores to Ctx, the call,
  ~24 reloads, spills). Now calls follow the AAPCS64 (a64lift SAVEC / LOADC: no flags, x19-x30 / sp / v8-v15 not
  reloaded; everything still stored first): CPU-only **4.41 -> 3.99 ms (228 -> 252 fps)**, ~40 windows each.
  Indirect calls look up one flat table inline (a64rt.h `call_ind_fast` / `jump_ind_fast`): main's runtime share
  9.6 -> 3.1%, **no frame-time change**. a64test passes (18,163 + 17,355 cases; run it with `py -3.12`, the
  Python that has capstone and unicorn).
- Why CPU-only stays ~250 fps (2026-10-30): not one thread's speed. Main runs ~70%, blocked ~20% at the job join
  (main+b47d18, ~11 waits a frame, ~6 us to wake); 5a is idle ~35% waiting for jobs at the same condvar (~16 waits
  a frame), 5a/5b ~50% busy. The frame is a chain of hand-offs between main and the workers. With rendering
  (~160 fps, 6.2-6.4 ms this session): 5c runs ~70% (translating 3.8 ms, GPU wait 1.7 ms), main ~45%: game code
  is not the limit there.
- Ruled out (2026-10-22): lifted code at -O2 (NX_LIFT_OPT in nxrecomp.cmake; 5 min full build): CPU-only 235 -> 244
  fps (~3%, inside the noise), with rendering 172 vs 165 (noise). The default stays -O1. Decompiling the game
  (IDA / Ghidra) to recompile it natively: not worth it (output does not compile, silently wrong functions, years
  of work), and the CPU side is not what holds the frame rate. Hand-written overrides of single hot functions are
  the way, once a profile has hot spots (it has none now).
- Ruled out (2026-10-22): render targets sampled by the draw that renders into them (what Savage Wind, the BotW
  recompilation in `ref/SavageWindRecomp`, needed feedback loops for). NX_FEEDBACK_LOG=1 logs each case once
  (gpu.c `feedback_check`; slots read come from the SPIR-V's access chains into cb2). In the benchmark save and the
  stand-still scene only two are undefined: fs `3614c5d014241256` reads and writes the same 1x1 f46 target (slot 1,
  every frame; likely exposure / luminance adaptation: **a lead for the too-bright glows**, untested) and fs
  `8f2c86b8511ac03b` samples the 1920x1080 depth buffer with depth writes on. Every other case is depth read with
  depth writes off (defined). Fixing those two (copy the target first) is cheap.

- **Where a frame goes now (2026-10-31, nxrecomp after `c427890`): ~6.1 ms (164 fps), GPU busy ~5.5 ms of it.**
  NX_GPU_TIMELINE=1 (gpu.c `tl_*`) timestamps each submission on the GPU, lines the clocks up by the smallest
  start - submit, and every 600 frames reports GPU busy and idle per frame, idle split into nothing submitted yet
  (at a guest submission's start / between its flushes) and submitted but not started. Before the early flushes:
  6.4 ms = busy 5.2 + idle 1.14, of which 0.93 ms in one gap a frame at the guest submission's start: GPU done ->
  5c woken 0.07, -> its emulated core back 0.21 (main holds core 0; kernel.c's new per-thread counters in the
  `cpu:` lines show 5c never takes the 1 ms retry path nor preempts: it waits for main's release, as on the
  console), -> gpu_submit 0.00, -> **first flush 0.63** (flushes only came at render target changes after 250
  draws, and the frame's first pass is long). Gaps between flushes ~0.02, submitted-not-started ~0.2 (presents
  share AMD's one queue).
  - Fix: **early flushes** (gpu.c `head_flush`, NX_FLUSH_FIRST, default 32, 0 off): each guest submission flushes
    after 32 draws, then 64, 128, ... (mid-pass: the pass splits) until NX_FLUSH_DRAWS takes over. **6.60 -> 6.09 ms
    (151 -> 164 fps)**, two runs each way; idle 1.14 -> 0.65 (start 0.33: core 0.13 + first flush 0.13; between
    flushes 0.16; submitted-not-started 0.16), busy 5.2 -> 5.5 (the extra splits). A single early flush gave 6.22.
    Frames checked (the player's pose differs: the idle animation, user-checked). **Not run on the GTX 1650 Ti**:
    with pool mirrors on, each flush waits for the mirror scan (~0.5 ms), and an earlier first flush may meet it
    sooner; compare NX_FLUSH_FIRST=0 there.
  - Ruled out, each against clean baselines (6.37-6.6 ms): skipping skinning (`vs 44f2ec77`, top item under
    NX_GPU_PROF with 1.6 ms) 6.61-6.72; NX_FLUSH_DRAWS=60 6.37 (same as 250; =1 8.07, translation doubles); no
    barrier at pass ends (temporary switch, removed) 6.38: ~0.2, the noise. NX_GPU_PROF's per-pass times inflate
    around barriers (its timestamps drain the GPU): its 1.6 ms of skinning and 1.48 ms for the 30 passes on the
    1080p RGBA8 target are mostly drains. Use NX_GPU_TIMELINE for totals and experiments (skip / switch) for causes.
  - Benchmark scripts: an env switch set for one run must be cleared before the next in the same shell (two runs
    were void until that was caught).
- **What we may be missing (2026-10-31): the GPU's 5.5 ms is ~8x what the hardware should need, and none of the
  per-pass suspects moved it. The likely cause is how we use Vulkan for every pass, not one pass:**
  1. **Color compression off for every image.** gpu.c `image_get` creates every image with
     VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT (+ EXTENDED_USAGE) and no VkImageFormatListCreateInfo; on AMD that
     generally disables DCC, so the G-buffer (1080p, 4 colour + depth, ~1.8 ms) and every 1080p pass move
     uncompressed. Format lists were "ruled out" 2026-10-19, but in a CPU-bound frame (translation 4 ms, ~130
     fps) where no GPU saving could show. Test first: drop MUTABLE_FORMAT for render-target formats that are only
     ever viewed as themselves (log which images get views in another format), or add format lists of the formats
     actually viewed; compare NX_GPU_TIMELINE busy.
     **Tried (2026-10-31, nxrecomp `abd7a93`): no change.** NX_VIEW_LOG=1 logs views in another format than their
     image's: in the title, load and stand-still scene only one (a 960x540 sRGB target sampled as UNORM). Images are
     now mutable only when a view can need another format (gpu.c `format_list`: sampled, integer, storage and
     sRGB / UNORM twins), with those formats as a format list; NX_MUTABLE_ALL=1 is the old way. Two A/B rounds
     (order swapped, 31-33 windows each): frame 6.14 / 6.02 ms old vs 6.05 / 6.16 new, GPU busy 5.41 / 5.37 vs
     5.37 / 5.49: noise. Kept as the more precise setup. So AMD's driver already compresses mutable images, or
     something else stops it (GENERAL layouts, in-pass clears), or these passes are not bandwidth-bound. RGP (works
     since Adrenalin 26.10.2, below) shows no DCC decompress at any barrier; whether targets are compressed at all
     is still unknown. Frames not yet checked with NX_DUMP.
  2. **No fast clears, no load / store hints.** Every pass is LOAD_OP_LOAD / STORE_OP_STORE in GENERAL layout;
     clears are vkCmdClearAttachments inside the pass (do_clear_color). A clear at a pass's start could be the
     pass's loadOp CLEAR (AMD fast clear, and HTILE for depth), and targets the game discards or fully overwrites
     could be DONT_CARE. GENERAL may also keep the driver from compressing (check amdvlk's behaviour for GENERAL
     colour / depth with DCC / HTILE).
  3. **72 pass splits a frame from NVN barrier commands** (gpu.c `OP_BARRIER` -> end_rendering, plus ~21 at
     render target changes and ~25 from transform feedback). Eden (`deps/suyu`, cloned 2026-10-31: src/video_core
     and src/shader_recompiler only) ends a render pass only when the framebuffer changes, with a barrier on that
     pass's attachments from the attachment-output stages; ours is ALL_COMMANDS -> ALL_COMMANDS every time.
     Dropping the barrier alone saved ~0.2 ms, so the split itself (store and reload of each 1080p target) is the
     suspect: keep rendering across an NVN barrier when the targets stay the same and no later draw samples them.
  4. **Shader quality** (translate.py's SPIR-V): compare one hot fragment shader (the G-buffer's, NX_GPU_PROF=2)
     against Eden's shader_recompiler for how it handles constant buffers, texture handles and control flow. Only
     after 1-3, which are cheaper to test.
     Compared 2026-11-01 (`deps/suyu` widened to all of src/shader_recompiler). Eden lifts to SSA IR, runs passes
     (constant propagation, texture tracking, global memory -> SSBO, structured control flow) and emits SPIR-V;
     we emit GLSL with no passes. Worth taking, as ideas (counts over the 4,437 translated shaders).
     **a and b done (2026-11-01, nxrecomp `3f6dbc1`)**: translate.py `ix()` / the c[1] literals in `cbuf()`; vk.c enables
     shaderSampled/StorageImageArrayDynamicIndexing. All 4,433 shaders compile. GTX 1650 Ti: GPU busy 8.587 ->
     8.592 ms (no change, as expected on NVIDIA); the presented frame 3000 is byte-identical, 116 of 117 render
     targets too (the other: a 1024x1024 foliage depth map, differing at leaf edges, likely wind; not chased).
     RX 7800 XT (2026-10-31, one exe, translate.py swapped between `471f5c7` and `3f6dbc1`, old / new / new /
     old): frame 6.581 / 6.509 / 6.253 / 6.207 ms, GPU busy 5.406 / 5.369 / 5.601 / 5.541 ms; averages old 6.394
     / 5.474, new 6.381 / 5.485: **no change on AMD either** (NonUniform waterfalls were not the GPU's cost).
     Drift between the pairs (~0.35 ms, either version) was bigger than the difference: a translator change
     drops pipelines.bins, so the first runs built their pipelines again. Logs build/bench_nonuni_*.log.
     a. **No `nonuniformEXT` on handles read from c[2] at a fixed offset** (68,161 uses; TEX.B is unsupported, so
        that is every texture access). The value is uniform across the draw; Eden marks NonUniform only when the
        index isn't a constant. On AMD, NonUniform can turn each sample into a waterfall loop. Keep it for
        register handles (surface() without bit 51, TMML.B).
     b. **Inline c[1] as literals** (86,891 UBO loads; fd87a85cf5d0fd96.frag has 93). c[1] is the shader's own
        embedded constants (gpu.c binds `sc->const_addr`), so `sh.consts` + flit() give the values at translate
        time. Eden does the same for bank 1 (FoldDriverConstBuffer). Keep the binding only for dynamic reads (cbr1, BRX).
     c. **XMAD triples -> one multiply** (~4,000 XMADs). GLSLC builds 32-bit `a * b + c` from XMAD; XMAD.MRG;
        XMAD.PSL.CBCC. Eden matches it in the IR (FoldXmadMultiplyAdd / FoldXmadMultiply); we can match the three
        instructions in a block (same Ra/Rb, not written in between) and emit `a * b + c` at the third.
        Skinning and index math use it.
     d. **FSWZADD 0x99 / 0xA5 on a quad swap -> dFdxFine / dFdyFine** (722 FSWZADDs; Eden's FoldFSwizzleAdd,
        matching the SHFL.BFLY before it). Native derivatives instead of subgroup lane math.
     e. Correctness, not speed: Eden marks every FADD / FMUL / FFMA NoContraction (`precise`). We let the driver
        fuse FMUL + FADD into an FMA, which the console doesn't do (depth pre-pass / G-buffer positions can
        differ between shaders). Try it if z-fighting shows up; it costs some speed.
     Not worth it for us: Eden's texture pass (reads handles at pipeline build and specializes; conflicts with
     offline translation), global memory -> SSBO (our buffer_reference addresses are fine), and its structured
     control flow (only 8 shaders loop; the `reached` flag DAG is something to look at only if RGP shows branch costs).
  Order: 1 (one switch, one run), then 2, then 3. Measure each with NX_GPU_TIMELINE busy, and check frames
  (NX_DUMP) as above.
- **RGP on the stand-still scene (2026-10-31, Adrenalin 26.10.2; build/standstill*.rgp).** NX_GPU_LABELS=1
  (vk.c enables VK_EXT_debug_utils; gpu.c labels each pass `pass WxH c<i>:<vk format> ds:<format> @<addr>`, each
  draw `draw vs <hash> fs <hash>`, dispatches `compute <hash>`, barriers `barrier L<gpu.c line>`, `end pass
  L<line>` for who ended a pass, `nvn barrier` for the game's). Line numbers are of the gpu.c that was captured.
  - No DCC / depth decompress, fast-clear eliminate or FMask work at any barrier. Barriers 2.1-2.5% of the frame
    directly, but ~230 a frame, every one a FULL stall that invalidated K / L0 / L1 / L2 / CB / DB and flushed
    L2 / CB / DB. The driver also adds "Before / After CS color clear" barriers: some clears run as compute.
  - The frame opens with the skinning chain: passes `1920x1080 c0:37 @12fad3a000` of transform-feedback draws
    (no fragment shader; vs 44f2ec77be3a3fd3 and others), split every 1-5 draws by the game's NVN barriers (each
    ends our pass, then a full barrier). Draws of 48-1,804 vertices take 5-20 us each and never overlap, also
    inside one pass (AMD keeps transform feedback in order; the shader may be slow too). Its total length is not
    measured yet (RGP's own pass grouping put ~2.1 ms at the frame start).
  - Most expensive draw: a full-screen triangle on the lit scene buffer (`1920x1080 c0:122 ds:130 @12f5ee2000`,
    vs 5250e040fae92a2c fs fd87a85cf5d0fd96), 236 us of work. The ~20 DrawIndexed(132) beside it (fs
    8516272888b0f0c0, likely light volumes) do 1.5-10 us each; their ~217 us "duration" is waiting beside it.
    Next: the G-buffer pass (`@12f80a2000`, 4 colour + depth), 8-21 us of work per draw.
  - **Tried: narrower pass-end barriers (nxrecomp `471f5c7`): no change.** A pass with no colour or depth
    writes (no rasterizing draw, no clear: gpu.c `pass_att_writes`) ends with a barrier naming only shader-store
    and transform-feedback writes (`ACCESS_NO_ATTACHMENTS`); the xfb_hazard path no longer adds a second barrier
    after end_rendering's. RGP: CB / DB flushes 234 -> 180 a frame, the skinning barriers flush only L2; still
    FULL stalls. Two A/B rounds: frame 6.38 / 6.33 ms full (NX_FULL_BARRIERS=1) vs 6.28 / 6.40 narrow, GPU busy
    5.73 / 5.68 vs 5.64 / 5.73: noise (both ~0.3 ms above the morning's, the RGP window was open). Kept (correct,
    fewer flushes). So the cost is the stall (each barrier drains the GPU), not the flushes.
  - Next: in a pass with no colour or depth writes, do not end the pass at the game's NVN barrier (the
    transform-feedback data is already guarded by xfb_hazard), behind a switch; measure and check skinning in
    NX_DUMP frames. Then the skinning chain's total time, `fs fd87a85cf5d0fd96`, and shaders against Eden's
    shader_recompiler (the user's plan).

In order:

1. **GPU efficiency** (the list above): DCC / format lists, fast clears and load / store ops, keeping passes
   across NVN barriers. NX_GPU_PROF=2 for which shaders, NX_GPU_TIMELINE for whether a change moved GPU busy time.
2. **Render thread translation** (3.2-3.4 ms a frame): cheaper translation, then take it off the render thread.
   It does not hold the frame now (below); it will once the GPU's frame is under ~3 ms.
   - What "translation" is: NVN calls never reach a driver. Recording (nvn.c, on whichever thread records) writes
     each command in our own encoding (nvn.h) into the game's command memory: cheap. At nvnQueueSubmitCommands
     (nvn.c `queue_submit` -> gpu.c `gpu_submit`, on 5c, emulated core given up) gpu.c replays that stream into
     Vulkan: state tracking, pipeline lookup / build, uniform staging and push descriptors (stage_ubos,
     descset_use), vertex / index binds, render-target changes and barriers, the image cache (deswizzle on a
     first read), then the vkCmd* calls. ~2,000 draws at ~1.6 us. On the console this is the GPU front end's work
     (the command buffer holds GPU methods) and costs the CPU nothing at submit. Shader translation (Maxwell ->
     SPIR-V, tools/shader/translate.py) is separate: once per shader, cached, a hitch cost, not per frame.
   - **Where it sits (2026-10-30): just under the GPU's pace.** A 1 us spin per draw (+2.0 ms translating) cost
     +1.44 ms a frame (GPU wait -0.47), but cutting translation 3.42 -> 3.13 ms (nxrecomp `c427890`: vk_format
     memoised, word-wise pipe_bucket, the refresh scan's sort on its thread, stage_ubos compared in place,
     set_vertex_input skipped when attributes / streams / vertex buffers are unchanged; frames checked) left the
     frame at 6.37 ms: the GPU wait grew 2.02 -> 2.44. NX_PROFILE now also lists runtime call sites into outside
     code and the hottest runtime lines (function:line, through inlining). 5c then: AMD driver 18% (push
     descriptors 3.8%, vkCmdDrawIndexedIndirect 3.3%, vkCmdDrawIndexed 3.1%), run 6.5% (its command header loads),
     draw 5%, stage_ubos 4%, memmove 3.5%.
   - From Eden (design only, never its code): its Scheduler records each vkCmd* as a small closure into 32 KB
     chunks and a worker thread makes the driver calls, so the driver's share runs beside translation. In C that
     is a tagged command stream (a struct per command, a switch on the worker), like nvn.h one level down. Its
     dynamic state is emitted from dirty flags set when the game writes those registers (state_tracker Touch*),
     where our set_dynamic_state rebuilds and memcmps the whole state every draw: our NVN state commands
     (OP_BLEND, OP_DEPTH_STENCIL, ...) could set dirty bits.
   - Off the render thread (Savage Wind: `ref/SavageWindRecomp/runtime/nvn/objects.c`, "the submission queue",
     SAVAGEWIND_ASYNC_EXEC / SAVAGEWIND_QUEUE_AHEAD): submits, queue fences, presents and texture / sync
     finalizes go in order to one host thread that replays them, and the guest returns at once. The guest waits
     only where it would wait for the GPU on the console (nvnSyncWait, nvnQueueFinish): first for the queue to get
     past that fence, then for the GPU. Replays read guest memory when they run, as the GPU does; the game fences
     its CPU writes to memory in use. Cap how many presents may be queued (Savage Wind defaults to 1).
   - For us: Violet's 5c waits for frame N-1's fence (`main+cd0eb4`, nvnSyncWait) before submitting frame N, so a
     replay thread only helps once translation is longer than the GPU's frame. To check first: everything in
     nvn.c / gpu.c that reads GPU-side results or gpu.c state synchronously (queries and reports, nvnSyncWait /
     GetStatus, nvnWindowAcquireTexture, texture / pool / sync finalize, CPU texture writes and the CPU-written
     texture check, the pool mirror's copy-in), and gpu_cs, the one lock over gpu.c.
3. **CPU-only hand-offs** (only matters without rendering): a short spin after giving up the emulated core,
   before sleeping, at the job condvar; measure with NX_PROFILE's wake columns. Lifted calls are done (Status
   above); passing arguments in host registers is the remaining step there (a big lifter rewrite, ~5-10% at most).
4. **Measure the dip when spawning / loading** (~80 fps for a second). Polling waits make it worse: a draw
   waiting for a pipeline a worker builds, and JIT waits, poll with Sleep(1) (gpu.c pipeline_get, the jit waits):
   each rounds up to >= 1 ms; make them events or condition variables. NX_HITCH_MS=20 logs each slow frame split
   into shader loads, JIT waits, pipeline builds, images, uploads, slot waits and the rest (Status, "Performance",
   "Lows"). Run it on the benchmark save (`build/save-bench`). In that scene the lows follow new pipelines and
   images (1% lows ~7-10 ms without, 10-49 ms with). The pipeline cache is now kept between sessions
   (`build/pipelines.bin`, or `pipelines.bins` with VK_KHR_pipeline_binary), and `pipelines.list` is prebuilt on
   worker threads at start; their effect on hitches is not measured yet.
5. **Extract every shader up front** (the "unpacker", also a V8 prerequisite): walk the shader archives in
   `dump/romfs/arc/data.trpfs` (8 GB) instead of collecting shaders as the game creates them (`build/shaders`,
   5,488 so far), and translate them all ahead of time. First learn the trpfs layout and its compression; if it
   is a proprietary compressor, prefer running the game's own lifted loading code in a tool mode over writing a
   decoder. Gains: no translation stall the first time an area is reached, a complete shader set, and no Python
   needed during play. It will not raise the frame rate in areas already visited.
6. **Keep the shader JIT as a fallback** after item 5 (gpu.c `jit_request`, NX_SHADER_JIT): anything not found
   in the archives (DLC, generated shaders) still gets translated. Removing Python entirely means porting
   `translate.py` to C, which belongs with V8 (packaging), not here.

## Status (2026-10-18)

- The three NSPs are identified; every NCA decrypts with the shared `../keys` (key generations 14, 15, 17).
  `title.keys` has no Violet entries: the update and DLC NSPs carry their own tickets (`--tik <file>.tik`); the
  base NSP has none (standard crypto).
- Extracted: the update's ExeFS (`dump/exefs`), ELF copies (`dump/elf`, via `../mk8dx/mk8recomp/tools/nso2elf.py`),
  RomFS merged over the base (`dump/romfs`, 3 min).
- **V0 done.** The shared code is `../nxrecomp` (runtime, tools, `nxrecomp.cmake`); MK8 is now `game.json` + scripts
  (re-lift byte-identical, builds, boots). Our repo is `violetrecomp/`: `game.json` with bases main 0x7100000000,
  subsdk0 0x7104800000, sdk 0x7105000000 (module sizes 0x47d3000, 0x69a000, 0xda8000, 2 MB-aligned), CMake on
  `nx_game`, `build.bat`, `run.ps1`. Run nxrecomp's tools from inside `violetrecomp/`.
- **V1 done.** IDA export (`build/analysis/`; main 37 min, 165,000 functions; sdk 100 s, subsdk0 48 s). The lifter
  translates all 15.86 M instructions (6 header words in main stay traps). Added for SV: SQDMULH/SQRDMULH, SQXTUN,
  SRSHR/SRSRA, SHSUB, S/UABDL, S/UABAL, SQSHL/SQRSHL, SQABS, scalar saturating ops and narrows; LD3/LD4/ST3/ST4
  single-lane decoding fixed; USRA precedence bug fixed (latent in MK8). Unicorn: 43,812 + 35,632 pair cases pass.
  Jump tables IDA missed are recovered by pattern (`Mod.recover_switches`: 34 in sdk, 1 subsdk0, 14 main).
  Weak imports nothing defines get generated trap stubs (gRPC TLS wrappers, `getentropy`).
  Lift: 4.8 min, 830 files, 1.0 GB of C. Build: 25 min from scratch, exe 407 MB, PDB 304 MB, lifted.lib 748 MB.
- **V2 and V3 done (boots, main loop runs headless).** `nnMain` runs; 60 s runs present 1,200 frames, stable, with
  the DLC owned (`game.json` "dlc": [1]; its PublicData RomFS, one `keyfile.dat`, extracted to `dump/aoc/01008f6008c5f001`).
  Done in nxrecomp: SDK 13 start-up (`nn::init::Start` with DT_FINI), TIPC (sm:), MapMemory from .bss / Normal memory,
  virtual address memory (GetInfo 16 = NPDM system resource size; MapPhysicalMemory: the game's 12 GB allocator),
  GetInfo 20 page, time shared memory, CreateManagedDisplaySeparableLayer, nvnProgramSetShadersExt, fsp 53 and other
  titles' saves = not found (SV probes 9 older Pokémon games), bcat (empty), csrng, btm, audout:u (Wwise's output,
  48 kHz stereo), hwopus (silent but sample-exact), bsd:u (ENETDOWN), ngct:u; unimplemented commands answer with a
  stub object. Lifter: jump tables recovered by pattern (bounded by AND/UBFX/CMP), BL-target function recovery,
  fall-through into IDA-split functions; analysis cached in `build/modcache`.
- **V4 in progress (2026-10-18): first-boot screens and the opening cutscene render.** Language select, "Choose a
  look for yourself" (3D portraits), the name keyboard (answered with Enter and "Player", or `NX_PLAYER_NAME`),
  "Enrollment complete!", then the opening cutscene with subtitles and the Uva Academy classroom in 3D.
  - Translator: all 4,309 dumped shaders translate and compile. Added: BRX jump tables (from c[1]), PBK/BRK,
    loops (blocks rerun in a `while`), FCMP, IMNMX, BFI, TLDS, SHFL / FSWZADD (quad subgroup ops), HADD2, HSETP2,
    ICMP, R2P, POPC, FLO, ISCADD.CC, ISET.CC, LDG.128; compute code starts at control word 0x708.
    2026-10-18: storage images (SUST.P, SULD.P, SURED as image atomics; bound or bindless handles, which NVN images
    keep in c[2] at 0x120 + 8k), VMNMX, VOTE, global ATOM (U32/S32), LEA (LO/HI). Runtime: descriptor set 3 = the
    texture pool's entries as storage images (indexed by image id, like set 1), views of one level with no swizzle
    and sRGB as UNORM; images bound as storage count as GPU-written. A 5-minute run skips 1 draw (no render target).
  - Fixed on the way: `OP_PROGRAM` now binds per NVN stage bits (compute is bound apart from graphics; before, most
    draws had "no vertex shader"); a lost wake-up in `condvar_wait` (the key's waiter flag must be set before the
    mutex is released: the SDK's Signal skips the SVC while the key is 0; this froze the job system); audout's
    GetReleasedAudioOutBuffers must zero the rest of the output array (the SDK returns entry 0 regardless of the
    count; a stale tag kept Wwise's sound thread draining forever under its global lock); audout releases buffers
    on a playback clock; library applet ids (0x11 is the keyboard; web applets are 0x13, 0x14, 0x17-0x1a).
  - Workflow: each run reaches new content and dumps new shaders (their draws are skipped, often a black screen);
    run `translate.py --spv` and run again. Runs: `$env:NX_NOPACE='1'; $env:NX_INPUT='300-30000/60:A'`, about
    4,000 frames in 5 minutes. Skipped draws are counted per reason every 600 frames.
    `run.ps1` kills the game when `-Timeout` runs out (default 60 s): for play sessions give it hours
    (`-Timeout 86400`), or a session is cut off without saving (2026-10-19: a 30-minute timeout did that).
    After pulling a change to translate.py: SPIR-V lives in `build/shaders/spv/<translator version>` (an FNV-1a
    hash of translate.py), so every shader is translated again. Run `translate.py --spv` once (~1 min for 5,000
    shaders) or the first run waits on the JIT for each one.
  - Debugging added: `run.ps1 -DumpAfter N` also prints mutex owners, full host stacks of running threads (taken
    without the kernel lock) and audout state; `NX_DUMP_MEM=addr:len,*ptr:len` (hex dumps, `*` dereferences);
    `NX_TRACE_CALLS=addr,...` (log calls through pointers: args, caller, result, the word at x1).
    Decompiling: idalib on the saved `dump/elf/main.elf.i64` (IDADIR, the ida-mcp venv's Python) takes 15 s a batch,
    closed without saving. Guest offsets in host stacks are truncated past 16 MB (`#line` numbers are 24-bit in
    CodeView): main+416f28 means main+3416f28.
  - Crash at ~4,300 frames (a Bink movie with audio starts): solved 2026-10-18. Bink's sound backend (main+20c50)
    puts its renderer work buffer and its AudioRendererConfig in one malloc, taken from a 192 KB (0x30-page)
    sub-heap that the game makes current for the main thread (malloc goes through a per-thread allocator at
    TPIDR_EL0+0x30). Our audren GetWorkBufferSize said 0x29000, so Bink overran the sub-heap into pages the heap
    later gave a shader file; the config's memory pool manager read 0 pools and RequestAttachMemoryPool faulted.
    GetWorkBufferSize now follows the console service's sum (estimated record sizes; 0x1d000 for Bink's
    32 voices). How it was found: the overlap is deterministic (same addresses every run, also with
    `NX_ONE_CORE`), and `NX_WATCH_MEM` on the nlib heap's span tags (`TlsHeapCentral`, base 0x1040000000:
    page count, then the span table at +0xa48, 8 bytes per page, tags at span ends) showed which allocation owned
    which pages.
  - Then a Bink decode thread jumped into a computed `adr; ldrb; add lsl #2; br` table the lifter had not
    recovered: its slice treated `ldr w19, [x7], #4` as writing "Rt2" (bits 10-14 of the immediate) and lost the
    table register. `_writes` in a64lift.py is now exact for loads and stores (base only with writeback, Rt2
    only for pairs). Re-lifted 2026-10-18.
  - Debugging added: `NX_WATCH_MEM=addr:len@ms` (every write to a range: writer, old value, host stack of
    `NX_WATCH_DEPTH` frames; pages read-only from when they are mapped, single-stepping writes; GPU writes are not
    seen), `NX_ONE_CORE` (all threads on core 0: tells races from logic bugs), `NX_DUMP_MEM` also on a crash,
    MapPhysicalMemory callers at log level 2, audren logs its work buffer size and renderer stop/close.
  - Also open: hwopus decoding (V5), the audren log's final-mix volume reads garbage.
  - Black screen from ~frame 4,600: solved 2026-10-18. Not the movie (Bink's audio voice plays and is consumed in
    real time). The scene rendered, but the final tonemap samples a 32x32x32 color-grading LUT (3D texture, f61)
    that the game draws one slice per instance (gl_Layer); we rendered with layerCount 1, so 31 slices stayed
    black. Render targets now render all the layers their view covers, and a 3D target all its depth slices from
    the view's first (`view_layers` in gpu.c). (Making view_resolve give 3D textures depth-many layers instead lost
    the device.) Open detail: the LUT shader writes layer = instance + c0[0x644] + 1 and reads gl_Layer - 1; c[0]
    past 0x600 (NVN driver data, perhaps draw parameters) is unbound, so slices end up one off.
  - Then a guest trap on the main thread, "longjmp to a jmp_buf no lifted setjmp call set" (Lua's luaD_throw):
    the per-thread table of host jump buffers kept 16, evicted round-robin, losing an outer pcall's buffer. Now 64,
    and slots whose buffer lies on the guest stack below SP (returned frames) are reused first.
  - Now (2026-10-18): runs (which resume from the save the earlier runs made) reach the player's bedroom in 3D
    with the player character, then the "Adventure Guide" tutorial panel, which closes with B (NX_INPUT presses
    only A). 4,359 shaders translate.
  - Frame rate (2026-10-18): ~10 fps in the bedroom, the render thread (thread 5c, which also replays NVN into
    Vulkan) at 89% of a core. `NX_PROFILE='thread 5c'` showed 86% of it in GetWriteWatch: refresh_cpu_textures
    asked about every page of every GPU pool each frame. It now asks only about the ranges of textures uploaded
    from guest memory (sorted, merged): 58 fps unpaced, the render thread at 38%, no thread saturated.
    Every 600 frames the log now has `cpu: <fps>; threads <name> <% of a core>...`.
  - Input for unattended runs from the save: `NX_INPUT='300-30000/60:A;330-30000/60:B'` (A and B alternate; B
    closes the tutorial panels). The runs resume from the save in build/save, not from a new game.
  - Keyboard (2026-10-18): ESDF = left stick, IJKL = right stick, arrows = D-pad only (arrows used to press the
    D-pad and the left stick together; Violet's D-pad opens emotes / camera mode), R = R (was E).
  - **Open: the player character does not move.** In the bedroom the left stick moves the camera through the room
    (it ends up in the window corner) while the character's mesh stays by the bed; the right stick rotates the
    camera. Since the camera follows the player at a fixed offset, the game's player probably moves and only its
    mesh is drawn stale. Suspects: (1) its transform or skinning data does not reach the GPU updated: matrices in
    a texture we never upload again because it counts as GPU-written (refresh_cpu_textures skips gpu_written
    images, before and after the 2026-10-18 change), or a skinning compute pass whose output we do not pick up;
    (2) a uniform buffer we leave unbound (stage_ubos skips ones not 64-byte aligned or past 64 KB); (3) less
    likely, physics really holds the player and the camera moves anyway. Next: NX_TRACE two frames (standing,
    after walking) and compare the character's draws (UBOs, textures, vertex buffers); `NX_REFRESH_POOLS=1`
    brings back the whole-pool refresh to rule out the frame-rate change. Unattended runs from the save need A at
    the title screen first: `NX_INPUT='300-900/60:A;1000-1600/90:B;1800-2400:LSDOWN'`.
  - Next: fix the character movement above, then the first steps in the overworld (leave the bedroom). Also find
    what c0[0x640..] holds on the console (0x640 / 0x644: base vertex / instance, solved 2026-10-19).
- **Portability pass (2026-10-18, AMD RX 7800 XT; committed: nxrecomp `e705520`..`234d70d`, violetrecomp
  `e999f18`, workspace `7ab7c28`).** The overworld is playable on AMD. Every bug below relied on what NVIDIA does
  with undefined behaviour; the rule now is spec-correct SPIR-V and Vulkan use, checked with NX_VK_VALIDATION.
  - Device losses (driver timeouts ~30 s after loading into the overworld), flickering grass, missing trees and
    particles: compute shader `fba20ffd…` (GPU culling) uses ballots/shuffles written for 32-lane warps; AMD ran
    64-wide waves and its "all lanes done" count never matched, so a loop spun. Compute pipelines now require
    32-wide subgroups (VK_EXT_subgroup_size_control; AMD allows it for compute only, `vk.warp32`). No vertex or
    fragment shader uses warp ops yet; if one does, translate them per 32-lane half.
    Found with the crash-diagnostic layer (VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_crash_diagnostic; dumps go to
    %USERPROFILE%\cdl) and NX_PIPE_LOG (pipeline handle -> shader hash).
  - Sky in four quadrants: vertex outputs a shader leaves unwritten must read (0,0,0,1) (the sky divides by a w
    only the default sets); translate.py initialises every output.
  - Square cells on the terrain: TLDS 2D ARRAY.LZ takes x, y from Rb, Rb+1 (layer in Ra), not Ra+1, Rb.
  - Dotted mountains / terrain edges: single-sampled alpha to coverage became an alpha >= 0.5 test (AMD dithers
    A2C), only when color target 0 exists (G-buffer passes use targets 1-4).
  - Integer vertex formats: 32-bit UINT/SINT fetched as same-size float formats; 8/16-bit ones get vertex shader
    variants with uvec4/ivec4 inputs (`translate.py --only=<hash>:<mask>`, 2 bits per location; runtime
    `shader_variant`, JIT-translated on first use). Used by the transform-feedback skinning pass (bone indices).
  - Render thread 64% -> 28% of a core at 30 fps (a getenv per storage buffer per draw). The game holds itself at
    30 (it creates its window with interval 1); its logic advances per frame (unpaced runs play fast).
  - Build: no machine paths. tools/vcenv.bat (vswhere, clang-cl), nxrecomp.cmake (deps/ or Vulkan SDK + vcpkg),
    GHIDRA_HOME for ghidra_export.ps1. FPS overlay (F3; NX_FPS=0). NX_DUMP_SSBO=<hash>, NX_DUMP_DRAWS=<target>
    with F12 (render target after every draw), NX_PROFILE='thread 5c'.
  - Objects near the camera were invisible (trees, the Gimmighoul rock; collision present): solved 2026-10-19.
    GPU-culled objects are indirect draws with large base instances (257,199...), and vertex shaders add the base
    instance from NVN's driver data, c0[0x644] (base vertex at c0[0x640]): Maxwell's instance / vertex ids leave
    them out. We left c0 past 0x600 unbound (0), so every such object took instance 0's transform. translate.py
    now reads c0[0x640] / c0[0x644] in vertex shaders as gl_BaseVertex / gl_BaseInstance, and the vertex id as
    gl_VertexIndex - gl_BaseVertex. Found with F12's per-draw occlusion queries: depth pre-pass draws with 1-2
    instances and 0 samples. (Not the culling compute `b4b95eb6…`: it never culls, it writes a fade value; its
    stencil read now goes through integer views, HANDLE_INT.) Debugging added: NX_NO_DISCARD (discards off in
    depth-writing draws), per-draw sample counts and indirect arguments in traced frames.
  - **Open: the Let's Go glow is far too bright** (green haze, a hexagon flare). Bloom is fine (this engine's
    "bright pass" is a clamped downsample by design); the composite `acbcacd1…` adds it from the half-res effects
    layer (960x540 f41 `12f4f6a000`). Suspect NVN blend factor / equation mappings (MK8 notes call them guesses).
  - **Open: huge coloured glows from the `?` / `!` markers over wild Pokémon** (2026-10-19, frame 10268: red,
    green, blue, white and pink blobs up to a quarter of the screen, near the camera). The user thinks it is
    lighting / reflections rather than bloom. Perhaps the same cause as the Let's Go glow above. A `!` marker
    was also seen with a blue outline. Next: F12 with
    `NX_DUMP_DRAWS` on the lit scene buffer (12f5ee2000) and on the effects layer (12f4f6a000) to find the draw
    that adds them.
  - **Open (next, 2026-10-19): the player's portrait is black** on the map marker (frame 50234, the circle
    under the eyes) and on the enrollment form (probably the same cause). What is known: a 360x360 f56 target
    `12ef662600` (portrait-sized) is cleared to (0,0,0,0) in every traced frame, and no draw in those frames
    renders into it or samples it. Either the portrait is rendered once before that and lost, or the marker
    reads another texture: every UI draw lists the same 26 textures, so the draw log cannot tell. Next:
    `NX_DUMP_DRAWS=12fb5aa000` (the final 1920x1080 f37 target) with the map open and F12: the first image
    showing the black circle names the marker's draw and its texture. Then trace back who should have written
    that texture (a render, a copy or a CPU upload; `NX_TRACE` from a new game's portrait screen).
  - Open: the "…" thought bubble shows only its corners, grey (draws `de9e66ba…` / `f3fbc253…`, BC3 sRGB atlas);
    the `?` icon is fixed.
- **Frame rate (2026-10-19): any rate plays at normal speed.** `NX_FRAMERATE=60` (or 120, 0 uncapped; unset is
  the game's own 30); F5 cycles game / 60 / 120 / uncapped during play; F3's overlay shows the cap, TICK (game
  time per second in 30 Hz ticks: 30 = normal speed) and SPEED.
  - How: the game steps each frame by a frame duration from a table (`main+3c2fbb0` = table[present interval -
    1], 33.3 ms) and derives its speed from the same entry (`main+d9643c`). `violetrecomp/fps.c` writes the entry
    every frame (rodata: through VirtualProtect; WriteProcessMemory failed silently and the game ran fast) and
    keeps the speed's divisor at 30 fps. The instructions are patched through `violetrecomp/patches.txt` (lifter
    feature: "if" alternates switched at run time, "call"/"after" host hooks); re-lift main after changing it.
  - Found with Fl4sh's Violet 4.0.0 "60 FPS" and "Dynamic FPS (ASM)" pchtxt patches (gamebanana) as a map, in
    `pokemon-violet/mods-ref/` (text only, not in git; the zips are at the workspace root). pchtxt addresses equal
    our main.elf addresses (no 0x100 shift); their build id is ours (709BFD66...). Not ported yet from the
    dynamic patch: its scaling of a movement value at `cce37c` (stutter when walking). Its NOP at `1526d64` is
    housekeeping for its code cave: that `stp` stores the address of the game's function at `1760ec8` (a callback),
    where the patch writes its own code, so we do not need it. The purpose of the 60.0 at `170c408` and the 100000.0 at `20a8d3c` is not known.
- **Performance (2026-10-19, RX 7800 XT, Mesagoza at 1080p, uncapped):** ~85 -> 103-120 fps. Uncapped runs:
  `$env:NX_NOPACE='1'` (= `NX_FRAMERATE=0`; presents with IMMEDIATE above 60 when there is no MAILBOX: with FIFO
  a visible window is held to 60).
  - Found: the render thread kept its emulated core while we translated and presented (~6 ms a frame), and the
    main thread, sharing that core, waited for it 59% of the time. Submit and present now give the core up.
  - Also: work goes to the GPU every 250 draws (`NX_FLUSH_DRAWS`), the CPU-written texture check runs on its own
    thread (was 2 ms a frame), fewer pass breaks after transform feedback captures.
  - Now (8-10 ms a frame): the render thread translates for 4-5 ms, GPU work ~7.3 ms. Next: cheaper translation
    (sample `NX_PROFILE='thread 5c'`), the shaders of the main scene pass (`NX_GPU_PROF=2`), and the 1% lows
    (~12-19 ms).
  - Reading the log: every 600 frames, `nvn: frame time` (average, 1% low, worst), `gpu: per frame` (render thread
    translating / presenting / waiting for the GPU), `cpu:` per thread (running, waiting for a core, asleep).
    `NX_GPU_PROF=1` adds GPU time per pass and what ended each pass, but its timestamps slow the GPU and make
    frame times uneven: leave it off for play.
  - Second pass (2026-10-19): render thread 6.2 -> 3-5 ms a frame translating. shaders_intern / shader_intern
    looked up through hash maps (were linear scans, 6%); stage_ubos reuses the last draw's c[0]/c[2] staging when
    the stage's storage buffers, texture and image handles are unchanged (whole-stage caching hit only 15%:
    uniform buffers or the shader change on most draws). The runtime now has line tables, so NX_PROFILE names
    static functions (before, `run`, `draw`, `stage_ubos` all showed as `gpu_init`).
  - Measured, not worth doing: deferring NVN barriers (almost all are 0x12 right before new render targets: ~7
    passes a frame saved of ~165) and compute skinning (skipping the skinning shader `44f2ec77…` entirely changed
    no frame time; NX_GPU_PROF's 1.9 ms for it is the drain after the barrier before each capture).
  - Uncapped runs crashed in h_fps_speed_base (the frame duration's page set back to read-only raced another
    thread's write); the page now stays writable. Sticks are clamped to the unit circle (WASD diagonals were 1.41x).
  - Animations ran with the frame rate: the timer at `main+b92784` (fps.c h_fps_timer_step) was fixed at 33.3 ms
    in scene kind 2 only; the Dynamic FPS patch fixes it in every kind except 2, and with that animations play at
    normal speed at 120 fps (user-checked 2026-10-19). Tried and not needed: its `2b7a874` patch (`add w1, w20,
    #1` -> 1: whole ticks per animation update; `2b7a7f0` turns the frame duration into a level 0-3, `2b7c540`
    is the distance LOD). One room looked jittery with that patch; not checked with the timer fix alone.
  - CPU side (2026-10-19): FRSQRTE's estimate searched up to ~500 steps per instruction (a64rt.c, now memoised:
    was 2.5-6.5% of main and 5a). NX_PROFILE now also reports the profiled thread's blocked time by wait kind and
    guest call chain: main and 5a spend 44-47% waiting at `main+b47d18` (a condvar in the job system's thread
    loop, ~3,000 waits a second), 5a another 20% at `main+d64f14`. Next: our kernel's wake-up latency (signal ->
    woken thread running) on those waits. Measured (NX_PROFILE's table: after a wake, µs to klk and to a core):
    5.4 µs and 0.2 µs at `main+b47d18` (~1,700/s at 120 fps), ~1% of the thread: the blocked time is idle workers,
    not wake latency; spinning or host primitives are not worth it (a wait still has to give its emulated core up).
    Wakes are now sent after klk is released (kernel.c wake_later / ksleep).
  - GPU-bound (2026-10-19, ~130 fps uncapped): the game keeps one frame in flight. Its render thread (5c) waits
    (`main+cd0eb4`, nvnSyncWait) for frame N-1's fence (`cd1c54`, fenced right after submitting at `cd1a74`)
    before submitting frame N, so a frame is ~the GPU's work (~5.7 ms, NX_GPU_PROF) plus the start of translation
    before the first flush: NX_FLUSH_DRAWS=1000 -> 93 fps (GPU starts late), 60 = 250. NX_GPU_PROF now names the
    guest callers of GPU waits. nvnWindowAcquireTexture's sync now waits for that texture's last present only (was
    all work so far; no measurable change). Biggest GPU items: the G-buffer pass (1080p, 4 colour + depth, 2.2 ms,
    split in 2), 23 passes a frame on one 1080p target (0.96 ms), 9 on another (0.69 ms), ~165 passes a frame.
    Ruled out (each measured in Mesagoza, uncapped, no change from ~130 fps / ~5.7 ms GPU, then removed): image
    format lists (DCC), skipping the barrier at every pass end (NX_PASS_BARRIER experiment: also corrupts lighting),
    CPU_NO_ACCESS pools in video memory (582 MB moved), staging in Resizable-BAR video memory. RGP captures lose
    the device the moment the trace starts (any trace size, early or late; RDTS 2026-10-02 and the 2025-07-01
    suite matched to the user's driver, Adrenalin 25.9.2, which they keep for stability: both in `.tools/`). Not
    ours: on 2026-10-31 a Frame capture of the SDK's vkcube hangs all of Windows too (the RX 7800 XT's screen
    goes black, the iGPU's monitor freezes, no TDR recovery, manual reset). RGP is out on this driver: don't
    try it again. The Panel must be running and connected before the program starts, or it is not listed.
    **Planned (2026-10-31): try Adrenalin 26.10.2.** The user's earlier update trouble was Vulkan broken for an AI
    workload (compute path), likely on an early 26.x driver; game rendering may be fine. Keep the 25.9.2 installer
    and roll back cleanly (AMD Cleanup Utility or DDU in safe mode). Check in order, stop at the first failure:
    (1) vkcube runs, (2) the stand-still benchmark (vs 6.1 ms frame / 5.4 ms GPU busy, NX_GPU_TIMELINE) and a clean
    NX_VK_VALIDATION run, (3) the AI workload, (4) an RGP Frame capture of vkcube with an RDTS release of the same
    period, (5) only then a capture of the game. Installed 2026-10-31 (RX 7800 XT 32.0.32015.7029; the iGPU got
    32.0.21045.16010): (1) passes; (2) passes: 6.05 ms frame / 5.40 GPU busy (unchanged), validation only the
    known interface mismatches (RuntimeSpirv-Location-06272, -OpEntryPoint-08743); (3) skipped (not needed);
    (4) passes: vkcube captured, no hang. Capture from the command line, CLI first, then the program:
    `.tools\rdts\RadeonDeveloperToolSuite-2026-10-02-1872\RadeonDeveloperPanelCLI.exe -m profiling -p <name>
    --rgp-auto-capture=frame:N -o <file>.rgp` (N >= 5). Neither it nor the program exits after the capture:
    stop both. (5) passes: the stand-still scene captured at frame 22,000 (build/standstill.rgp, 20.8 MB; the
    scene starts near frame 13,000-17,000, so 20,000 can still be in the load). With the CLI attached but not
    capturing, 6.36 ms a frame. **RGP works on 26.10.2: stay on it.** Captures since: NX_GPU_LABELS=1 (names
    in RGP's user events, see Next), capture at frame 19,000 with `--rgp-sqtt-buffer-size high`, and wait for
    the .rgp file to appear before stopping the game (a fixed 120 s cut two captures off mid-trace: "trace failed
    with result: -2"). Findings are under Next;
    Windows' GPU engine counters show 0% for us (AMD submits past them). Stable clocks (1295 MHz GPU / 750 MHz memory, ~-40%) cost only ~13% (117 fps). With every draw
    skipped (NX_GPU_SKIP=1) the game runs ~290 fps, translation 0.6 ms (4.0 normally): main 75%, 5a/5b 62-70%,
    5c waits for a core 11%. Pools (Mesagoza): 3,314 CPU_NO_ACCESS (367 MB), one 350 MB CPU_UNCACHED pool, a
    268 MB compressible one (render targets), 98 MB CPU/GPU uncached.
  - CPU side (2026-10-20): the render thread spent ~23% in the AMD driver, from ~40 vkCmdSet / bind calls a draw.
    draw() now records only state that differs from the last draw in the same command buffer (gpu.c `rec`,
    DynState): 128-141 -> 150-152 fps uncapped, translation 3.7-4.1 -> 3.3-3.4 ms, 1% lows 8-9.5 ms; validation
    shows nothing new. Then stage_ubos pushes only the uniform slots a shader declares (ShaderCode.slots_used,
    from the SPIR-V's cbN bindings; was all 16 per stage) and pool_find tries the last pool hit first: 150-152 ->
    167-195 fps, translation 2.5-2.7 ms, and the GPU wait shrank too (1.6-2.2 ms): the 32 push descriptors a draw
    cost GPU time as well (the even per-draw GPU cost NX_GPU_PROF=2 showed). Then descset_use skips draws whose
    texture / image handles, entries (DescSet.gen) and images are as the last call's in the same frame, and
    vertex buffers are bound for the used streams and 31 only: no change in 2 x 600-frame windows here, but the
    user saw ~160 -> ~170 average and better frame rates after loads (2026-10-20). DMB/DSB: load- or store-only barriers lift as compiler barriers, full ones as
    `lock or (%rsp)` instead of MFENCE (a64insn.py, a64rt.h FENCE / FENCE_ORDER): CPU-bound (NX_GPU_SKIP=1)
    287-296 -> 297-304 fps. The game pins main and the render thread to core 0, workers 5a/5b to cores 1/2
    (as on the console), so more emulated cores would not help without breaking per-core data. Main's profile is
    flat (hottest: nn::os::SignalLightEvent, 2.2%). gpu.c's NX_* switches are read once in env_init().
  - Open, parked (user, 2026-10-19: minor next to frame rate): cutscenes (and maybe the top-of-screen
    notifications) run fast above 30 fps. Scene kind (`main+2936584`, setter `29364c8`, a vtable method) stays 0 on
    the title, the new-game screens, the opening and Mesagoza, so it does not mark cutscenes; `b926f0` posts one
    global step (timer 2, from the main loop at `main+27b04`) that we force to 33.3 ms in kind 0. Likely a cutscene
    clock uses that step without the speed factor. Not ported from the Dynamic patch yet: `cce37c`. User
    (2026-10-19): cutscenes look right at 80-90 fps; at 120+ characters sometimes teleport (so it grows with frame
    rate). Also seen: small cyan-white streaks hanging in the air in Mesagoza (frames/frame_14699.ppm; particles?).
    A new game for tests: `violetrecomp.exe --save <empty dir>` with `NX_INPUT='300-1000000/60:A'`.
  - GTX 1650 Ti laptop (2026-10-20, Mesagoza garden, 1080p, uncapped): 23-29 -> 75-78 fps. It was GPU-bound
    (96% busy) but starved: one 367 MB CPU_UNCACHED|GPU_CACHED pool holds every mesh, index and uniform buffer,
    and the GPU read it from host memory over the laptop's PCIe 3.0 x8 (`nvidia-smi dmon -s ut`: rx 2-3.5 GB/s,
    VRAM controller 4%). The G-buffer pass took 19-24 ms (AMD: 2.2). Now gpu.c mirrors pools >= 8 MB that are
    GPU_CACHED and CPU-accessible in video memory (Mirror; NX_POOL_MIRROR=0 off): draws read vertex, index and
    uniform buffers from the mirror; pages the CPU wrote (write watch, scanned on a thread started at each guest
    submit, ~0.5 ms per 367 MB whatever is dirty) are copied in by a `pre` command buffer before each submission
    (~1.6 MB/frame); GPU copies, clears and small writes go to both; ranges written by storage buffers (shaders with
    SHC_GLOBAL) or transform feedback are read from host memory from then on (Mirror.bad). G-buffer 19 -> 1.0 ms,
    GPU work 32-38 -> 8.7 ms per frame, PCIe rx ~0.6 GB/s. Not measured on AMD yet (its earlier CPU_NO_ACCESS
    experiment saw no change; the 0x022 pool was not tried there).
    Then the frame rate sat at 59: the panel is 60 Hz on the Intel GPU (Optimus), and NVIDIA's vkQueuePresentKHR
    blocked the render thread ~4-5 ms a frame in every present mode. Presents now run on a thread of their own
    (vk.c, sw_lock): on the family's second queue where there is one (NVIDIA), else sharing vk.queue under
    queue_lock (AMD's graphics family has one queue; every submission goes through vk_submit). Uncapped, a frame
    that arrives while the last is still being presented is not shown (mailbox behaviour; the 600-frame log says
    how many).
    AMD (RX 7800 XT, display on the dGPU, PCIe 4.0 x16), run 2026-10-20 from the benchmark save (below), 2-minute
    runs, NX_TIME=1792494000: **the mirror loses there.** Uncapped, after loading: NX_POOL_MIRROR=0 155-205 fps
    (translating 2.3-2.9 ms, GPU wait 1.6-2.6 ms), =1 137-167 fps (translating 3.1-4.2 ms, GPU wait 0.8-1.7 ms).
    The mirror's own log claims 0.07 ms/frame of render-thread time, so the extra ~1 ms of translating is
    unexplained (the wait at the first flush for the scan thread is the suspect). With =1 the title screen also
    had 18-66 ms spikes that =0 did not. NX_GPU_PROF=1: G-buffer (1920x1080 c1:43 c2:37 c3:37 c4:122 ds:130)
    1.0-1.5 ms with the mirror vs 1.7-2.3 ms without at the scene's start, ~1.3-1.45 both later; copies outside
    passes 0.36 vs 0.23 ms (the copy-in); whole GPU frame 4.6-6.3 vs 3.7-6.8 ms (overlapping). So on a desktop
    card with a fast bus the host-memory reads were never the problem (G-buffer was 2.2 ms before). Open: turn
    the mirror on only where it pays (the user's call, not changed yet); AMD runs use NX_POOL_MIRROR=0 for now.
    Present thread on AMD: presenting 0.2-0.4 ms/frame, 0 frames not shown; no switch to compare the old path.
    - Benchmark save (2026-10-20): `build/save-bench` (a copy of build/save from 2026-10-19; the user sends it
      over, it is never committed: it is game data). Loading it starts a scripted scene that takes input away
      for ~5 minutes, so every run renders the same thing, with content streaming in as it goes. Runs copy it to a
      throwaway folder first so the game never saves over it: copy `build/save-bench` to `build/save-bench-run`,
      then `violetrecomp.exe --log 1 --save <that folder>` with NX_NOPACE=1, NX_TIME=1792494000,
      NX_INPUT='300-900/60:A;1000-1600/90:B'. The scene runs on game time, so at different frame rates a
      600-frame window covers a different part of it: compare whole runs, not window by window. These numbers are
      not comparable with Mesagoza's.
    - Lows (2026-10-20, AMD, that scene): 600-frame windows with no new pipelines or images had 1% lows of
      6.7-10.3 ms (mirror off); windows with them 9.9-48.9 ms, worst frames up to 146 ms. So the spikes come with
      new content. NX_HITCH_MS=<ms> (gpu.c, `hf`) now logs each frame longer than that, present to present: the
      render thread's translating / presenting / GPU wait / the rest (game code, waits for a core), and in them
      the time and count of shader loads, JIT waits (base shaders and integer-input variants; variants are not
      made by `translate.py --spv` and their wait was not logged before), pipeline builds (graphics and compute),
      image creation, uploads, waits for a free command slot and the mirror copy-in. Built, not run yet: run the
      benchmark save with NX_HITCH_MS=20 and NX_POOL_MIRROR=0 to pick between Next items 2 and 3.
    Also: the alpha test is a
    specialization constant (ALPHA_FUNC, PipeKey.alpha_func) so draws without one have no discard (early depth);
    no measurable change here. NX_TIME=<posix seconds> pins the guest clock: the day/night cycle follows the real
    clock and moved the GPU cost run to run (19 -> 24 ms G-buffer within 30 minutes).
    Open: GPU 8.7-9.9 ms (pass 1920x1080 c0:122 with 22 passes/frame 2.3 ms, G-buffer 1.9, ~172 barriers 1.2),
    render thread translating ~7 ms, 1% lows ~38 ms. Validation shows 3x VUID-RuntimeSpirv-Location-06272 and 1x
    OpEntryPoint-08743 on NVIDIA (shader interfaces, not from these changes).
  - Lift and build: a64lift lifts in worker processes (NX_LIFT_PROCS; default by free memory, ~3 GB each: 129 ->
    59 s here, output identical); LIFT_JOBS=0 (default) = min(logical cores, RAM / 2 GB) compiles: a full build
    ~6 -> 4.4 min on this machine (16 threads, 31 GB). Changing a64lift.py invalidates build/modcache (it is keyed
    on the file's mtime): the next lift re-runs the analysis.
- **Second machine set-up (2026-10-18, AMD RX 7800 XT)**: set up from the NSPs alone, analysis with Ghidra
  instead of IDA:
  - `tools/ghidra/` (`ghidra_export.ps1`, `GhidraExport.java`, `GhidraOptions.java`): writes the same
    `<module>_ida.json`. Run `ghidra_export.ps1 -Seeds` inside violetrecomp (Ghidra 12.1.3; `GHIDRA_HOME` or
    `-Ghidra` names its folder). "Non-Returning Functions - Discovered" must stay off: it took PLT stubs as noreturn
    and cut 9,706 sdk functions short (51% -> 96% coverage). main: 19 min analysis + 5 min seeding, 156,946
    functions, 4,289 switch tables. Lift: 15,860,007 instructions, 6 untranslated (as with IDA).
  - a64lift: function and jump-table recovery repeat until neither finds anything (case code of a table Ghidra
    missed was the only caller of main+14260, a guest trap at startup). The test suites pass (43,810 + 35,652).
  - AMD (amdvlk aborts with fail-fast 7 on a lost device; catch it with a Win32 debugger, not lldb): no D24
    formats (`usable_format` falls back to D32S8 / D32), no dynamic logic op (in the pipeline key), set 1 has one
    texture array per view type (TB_*, translate.py TEX_BINDING; a 3D read of a 2D-array view lost the device;
    32-bit integer textures get same-bits float views), and c[16] (st.ubo[13]) was at push binding 32, past the
    layout: translate.py now puts it in a slot the shader leaves free and the runtime reads the slots from the
    SPIR-V (`cb_of_slot`). Found with GPU-assisted validation (VK_LAYER_SETTINGS_PATH to a settings file with
    gpuav_enable, gpuav_safe_mode, validate_best_practices_amd).
  - Debugging added: NX_PIPE_LOG, NX_SKIP_SHADERS=hash,..., NX_GPU_SKIP=8 (non-2D textures), NX_INPUT_LOG,
    unsupported-format warnings.
  - Input: WASD = left stick, mouse = right stick (click captures, Esc releases, NX_MOUSE_SENS, dead zone).
  - Menu cursor, player and camera wander (mostly left; pressing the opposite way only offsets it) with
    NX_INPUT_LOG showing zero input, also on one core. Not the gyro (the six-axis lifos are at NPAD +0x1758, 0x708
    each; the first try at +0x17e0 was wrong). Suspect: main reads the Handheld npad as well as No1, and hid
    wrote no lifo but No1's full key and system ext. hid now writes all 7 lifos of all 10 npads (idle states);
    fixed (the drift stopped).
  - Black grass outdoors: the translator read TLDS encoding 8 as 3D.LZ; it is 2D ARRAY.LZ (layer in Ra, as TEXS;
    3D.LZ is 7). 1,099 of 4,425 shaders fetched through the 3D array, which holds a dummy for 2D images, so the
    terrain's material map read as zeros (no albedo in the G-buffer). Fixed: grass and ground render outdoors,
    and the starter Pokémon now load (they lost the device before).
  - Shaders are translated while the game runs (gpu.c, "Shaders are translated while the game runs"): nvn.c
    queues a shader with no SPIR-V when the game creates it, a host thread runs `translate.py --spv
    --only=@build/shaders/jit_list.txt` (a file: the game creates ~2,300 shaders at boot, past the command-line
    limit; _popen also died on that with fail-fast 5), and a draw waits for its shader instead of being skipped.
    NX_SHADER_JIT=0 turns it off, NX_PYTHON picks the interpreter. NX_ROOT is now in game_config.h.
  - Input recording: NX_INPUT_LOG logs every input change; NX_INPUT_REPLAY=<log> plays a log back by frame.
    build/input_walkout.log is a session that walks out of the house (save near the front door now);
    input_door.log is its walk shifted to start at frame 2400.
  - Validation (2026-10-18): plain NX_VK_VALIDATION is usable. GPU-assisted validation (VK_LAYER_SETTINGS_PATH
    with gpuav_enable) made Windows reset the display drivers (TDR) on this machine: don't use it again.
    Fixed from its report: blits the formats cannot do are skipped and logged (AMD: no depth/stencil format is a
    blit destination); descriptorBindingStorageImageUpdateAfterBind and
    shaderStorageImageArrayNonUniformIndexing are enabled. Still reported (build/validation_1.log): the shadow
    pass's R8_UINT target sampled and stored as float, alpha-to-coverage on with no alpha output, vertex
    attribute types (R8G8B8A8_UINT / R32_SINT into float inputs), a point-list topology mismatch, a
    vertex-to-fragment interface mismatch.
  - Open: device lost at random points outdoors (frame ~1,800 and ~7,400; build/crash_1800.log), also before the
    driver resets. Next: one run with VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_crash_diagnostic (Vulkan SDK) to see
    the draw in flight.
  - Open: the sky is split into four quadrants at the screen centre (x 960, y 540; ~8% step), already in the lit
    scene buffer (12f5ee2000). Not the lighting pass's half-res reads (their coordinates check out). Next:
    `NX_DUMP_DRAWS=12f5ee2000` and F12 at the sky: frames/frame_<n>_draw_<d>_fs_<hash>.ppm after every draw into
    it (added, untested); the first one showing the split names the shader.
  - robustBufferAccess2 / robustImageAccess2 are on where supported (vk.c), against shaders reading past what they
    were given. Raw-address global memory is not covered; clamping it in the translator is the next step if
    device losses go on.
  - Fail-fast (0xC0000409) codes seen: 5 is the CRT's invalid parameter (ours), 7 is amdvlk's lost device. A
    run under a Win32 debugger got VK_ERROR_UNKNOWN on an image bind, so don't trust debugger runs for GPU work.
  - Open: the black portrait on the enrollment form, lighting (LUT slices one off? 8/16-bit integer textures
    read as float), hwopus silent, storage images (set 3) still share one array across dimensions.
    Commits: nxrecomp `e122065` (translator), `abb28ce` (runtime fixes), `dda831d` (storage images), `8024860`
    (lifter jump tables), `576381c` (audren work buffer), `f408ac0` (debugging tools).

## ROMs

| File in `rom/` | What |
|---|---|
| `Pokemon Violet [01008F6008C5E000][v0].nsp` (6.4 GB) | base 1.0.0, SDK 13.3.5. Program NCA `ef6d3388…` (6.7 GB), control `e9f7d8c6…`, manuals `ec46bfa3…`, `8700e090…` |
| `Pokemon Violet [01008F6008C5E800][v786432].nsp` (5.0 GB) | update **4.0.0** (v786432), SDK 13.3.13, key gen 17. Program NCA `d24a763a…` (5.2 GB), control `48b4e53e…`, manuals `29e25fe6…`, `3d7999fe…`; ticket `01008f6008c5e800…11.tik` |
| `Pokemon Violet [DLC …] [01008F6008C5F001][v0].nsp` (118 KB) | DLC 01008F6008C5F001 (The Teal Mask, The Indigo Disk, uniform set): one 112 KB PublicData NCA `16a4c25a…` (an unlock; the DLC content ships in the update); ticket `01008f6008c5f001…0f.tik` |

## Files

| Path | What |
|---|---|
| `rom/` | the user's NSPs (above) |
| `dump/exefs/` | 4.0.0 code: main, sdk, subsdk0, rtld, main.npdm |
| `dump/elf/` | ELF copies of the modules |
| `dump/romfs/` | merged RomFS (base 1.0.0 + update 4.0.0), 9.24 GB |
| `violetrecomp/` | our source (git): `game.json`, `CMakeLists.txt`, scripts; shared code in `../nxrecomp` |
| `dump/aoc/<data id>/` | DLC data (RomFS of the DLC NSP's PublicData NCA, via its ticket) |
| `build/analysis/` | `<module>_seeds.json`, `<module>_ida.json` (IDA exports), logs; IDA databases are `dump/elf/*.i64` |
| `build/peek/` | first 16 KB of each NCA, small NCAs, tickets; `ctl_base/`, `ctl_upd/` = control data (NACP, icons) |

## Commands

- `extract/` (the NSPs unpacked, 11.9 GB) was deleted on 2026-10-17 after `dump/` was made. Extract again (PowerShell, from `pokemon-violet/`: `$env:USERPROFILE="$PWD\..\keys"`, `$n` = `..\.tools\nstool.exe`):
  `& $n -x extract\base rom\<base>.nsp; & $n -x extract\upd rom\<update>.nsp` (40 s), then ExeFS `& $n --tik extract\upd\01008f6008c5e800…11.tik -x /0 dump\exefs
  extract\upd\d24a763a8882575f1c252dce37021db5.nca`, RomFS the same with `--basenca
  extract\base\ef6d3388d34a30dda23208f98780c610.nca -x /1 dump\romfs`.
- `llvm-nm -D [-C] dump/elf/main.elf` (LLVM's, from Visual Studio's Clang tools or MSYS2) lists imports and exports.
