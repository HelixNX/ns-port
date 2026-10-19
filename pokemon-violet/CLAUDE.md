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

Risks: exceptions (V1) and the size of the build are the unknowns MK8 didn't have; NVN coverage is the largest amount
of work; online code paths must fail as on a console with no internet, not crash.

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
    what c0[0x640..] holds on the console.
- **Second machine (2026-10-18, AMD RX 7800 XT, workspace on H:)**: set up from the NSPs alone, analysis with
  Ghidra instead of IDA. Uncommitted in nxrecomp so far (besides the earlier local edits to build paths in
  `nxrecomp.cmake`, `a64test.py`, `translate.py` and violetrecomp's `build.bat`):
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
