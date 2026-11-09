# mk8dx: Mario Kart 8 Deluxe recompilation

Static recompilation of the user's own Mario Kart 8 Deluxe **3.0.5** (title 0100152000022000, 64-bit AArch64) to
a native Windows x64 program. **Our own implementation**: `../ref/ssbu-recomp` and Eden (`../deps/suyu`) are references
for behaviour only; don't copy their code (the user's call: a better design of our own beats a port of SSBU's).

Workspace rules, toolchain and shared tools are in `../CLAUDE.md`. Paths below are relative to `mk8dx/`, except
`runtime/...` and `tools/...`, which since 2026-10-17 (V0 of `../pokemon-violet`) are in the shared `../nxrecomp`.
Debug environment variables are `NX_*` (formerly `MK8_*`).
Game-derived data stays in `rom/`, `dump_v21/`, `extract_v21/` and `build/`.

## Start here

From the workspace root (`..\start.ps1` wraps `mk8recomp\build.bat` and `run.ps1`, which also work on their own):
`.\start.ps1 mk8` plays (builds first; runs until the game is closed since 2026-11-07, `-Timeout <s>` for unattended
runs; `-NoBuild`; log `build\run.log`), `.\start.ps1 build mk8` builds, `.\start.ps1 room mk8` hosts a room with
MK8's game id, `.\start.ps1 mk8 -Room host:port -Nick name` joins one directly (no `-Room`: offline, rooms in the menu's
Multiplayer; the old browser: `NX_ROOM_BROWSER=1`), `.\start.ps1 package mk8 <dir>` makes a folder for players. The room server is `../room`.
MK8 is not the current focus (`../CLAUDE.md`): the user checks it themselves.

## Design decisions (2026-10-15)

1. **CPU: our own AArch64 → C lifter** (`../nxrecomp/tools/lifter/a64lift.py`, new). Lift `main`, `sdk` and `subsdk0`.
   - Function boundaries from the IDA export (`tools/ida_export.py`), seeded from relocation targets and exports.
   - Guest registers are C locals inside each function, written back to `Ctx` only at calls, returns and
     indirect branches. That lets clang keep them in host registers and delete unused flag computations.
   - Indirect branches use an O(1) table indexed by (address − text start) / 4.
   - Guest memory is identity-mapped at fixed addresses (modules from 0x7100000000), so a guest pointer is a host pointer.
   - main imports no C++ exception functions; handle exceptions in sdk only if they ever happen.
   - Unicorn differential tests are generated per instruction pattern.
2. **OS: run the SDK's own code, and emulate the kernel and services under it**, not the ~900 nn:: functions.
   - main itself has no SVC instructions and doesn't touch system registers; the sdk uses 57 SVC numbers,
     TPIDRRO_EL0 (TLS), TPIDR_EL0, CNTPCT_EL0 (19.2 MHz), FPCR and FPSR.
   - Implement the SVCs: threads, memory, synchronisation and arbitration, events, IPC sessions.
   - Services are handled in-process at the IPC (CMIF) level, from the 42 listed in `main.npdm`; most are stubs.
     Online services (nifm, friends, ldn and so on) report "no network".
   - Why: SDK 4.4.0's structure layouts and threading semantics come for free and match the console exactly.
3. **Exceptions to "run the SDK":**
   - Graphics are intercepted at the NVN API: `nvnBootstrapLoader` and `nv::*` → our Vulkan back end. The sdk's
     NVN driver and nvdrv never run.
   - Movies: subsdk0's `movie::` (15 imports) → our timed stream of black frames (`movie.c`), Media Foundation later.
   - `glslc*` is unresolved and never needed (trap if called).
   - Hot libc functions may be swapped for host versions.
4. **Graphics: our own NVN on Vulkan** for the ~475 `nvn*` names main references.
   - Maxwell shader → GLSL → SPIR-V. **Changed (2026-10-16):** the translator is Python
     (`tools/shader/translate.py`), run offline over the shaders the runtime dumps (`build/shaders/<hash>.bin`);
     glslang.exe compiles the GLSL; the runtime loads `build/shaders/spv/<hash>.spv`. GLSLC code has no run-time
     compiler (glslc is never called), so every shader exists before it is drawn with. Runtime glslang (for
     variants such as integer vertex attributes) only if needed.
   - GPU: GTX 1650 Ti; Vulkan loader 1.4.341.
5. **Audio:** the audren:u service with our own renderer (PCM/ADPCM voices, mixing, the effects used) → SDL3 audio.
   **Input:** hid shared memory filled from SDL3. **Files:** fsp-srv serving `dump_v21/romfs`; save data in a host folder.

## Milestones

- M1 `a64lift.py` with Unicorn differential tests; 100% coverage of main, sdk and subsdk0.
- M2 Lift and compile all three modules.
- M3 Loader (map modules, relocations, imports, init arrays) and SVCs: SDK init runs and reaches `nnMain`.
- M4 Services until the game's main loop runs headless.
- M5 NVN on Vulkan: first frame, then menus.
- M6 Audio, input and saves: a race is playable.
- M7 Performance, shader cache, polish.

## Status (2026-10-16)

- **M1-M4 done.** `a64lift.py` decodes 100% of main, sdk and subsdk0 (4.76 M instructions) and passes the Unicorn
  tests (`a64test.py`: 1,274 instruction shapes, 1,543 flag pairs). `a64lift.py lift` → 138 C files (245 MB) →
  `lifted.lib` in 5.5 min. The loader maps relocated images, the sdk's init runs to `nnMain`, and the game's main
  loop runs with these services: sm, lm, apm, appletOE, aoc:u, pctl, fsp-srv (RomFS, saves, system data), vi, hid,
  time, acc, mii, audren:u, pl:u, set, am applets, nifm (no network: requests rejected at once), ldn:u (local wireless unavailable) and friend:u
  (no friends); prepo:u stubbed. The generic stub returns no object, so a stubbed command that opens a
  sub-interface leaves the SDK a null pointer (Time Trials crashed so in nifm, then friends): implement those.
- **M5 done (menus and races render).** NVN is recorded and replayed into Vulkan, shaders are translated offline
  (all 4,184 dumped so far compile). Rendered correctly: title, attract-mode races, Mii select, main menu, Single
  Player → Grand Prix → cc select → character grid → kart parts → cup select → a Mario Kart Stadium race (driving,
  AI, items, HUD, MKTV screen). Fixed on the way: the menus waited on the movie player's clock (`movie.c` now gives
  a timed stream), video memory ran out within seconds of a race (lazy texture-pool resolution + LRU eviction),
  perspective UI sampled outside its textures (IPA.PASS), point lists without PointSize, clears of BC images.
  The Khronos validation layer over menus + a race now reports only the two items in "How to continue" 2.
  Input works (keyboard, gamepad, scripted `NX_INPUT`).
- **M6 audio done (2026-10-16, `520f3b4`):** audren:u renders voices, mixes, effects and the device sink to SDL3
  (see "Audio notes"). Not yet checked by ear; the dumps show music, menu sounds and race engines without clicks.
- **M6 movies done:** `movie.c` decodes MovieMenu.mp4 with a Media Foundation source reader (NV12): the Grand
  Prix / cc panels show the video. Saves: `userdata.dat` is written and read back (later boots skip the first-run
  Mii setup); progress after a whole Grand Prix is not checked yet.
- **Git:** branch `a64-lifter` in `mk8recomp/` (M1-M4 commits, then M5 `ca9266b`..`c73c78c`; not merged into `main`,
  no remote). Merge into `main` when the user asks.

### How to continue (next session)

1. **Test runs: keep them short.** From `mk8recomp`, `$env:NX_NOPACE='1'` (no 60 Hz pacing) and mash buttons with
   `a-b/n:BUTTON` (3 frames every n frames) instead of waiting at fixed frames. Boot to a race, then hold A:
   `$env:NX_INPUT='300-1500/40:L+R;320-1500/40:A;2100-2900:A'; $env:NX_DUMP='2400,2700'; .\run.ps1 -NoBuild -Timeout 150`
   (main menu ~600, character grid ~900-1000, cup ~1200, race loading ~1500, driving ~2100). Screens arrive a screen
   earlier or later between runs, so trace a frame range with a filter rather than one frame (GPU notes, Debugging)
   and look at dumps as a contact sheet (PIL: `build/frames/*.ppm` → one PNG).
2. **Open GPU issues:** UINT vertex attributes (location 6, R8G8/R8G8B8A8_UINT, likely skinning indices) are
   declared `vec4` in the shader: works on NVIDIA, undefined by the spec; needs shader variants (`uvec4` input +
   `uintBitsToFloat`) chosen at draw time from the attribute format (offline variants via translate.py, or runtime
   glslang). A fragment output with no colour attachment is only a warning. The Grand Prix/cc screens' right panel
   is black until movies decode (expected). Gaps in gpu.c (skipped or approximated): geometry / tessellation
   shaders, DrawTexture, transform feedback, multi-draw indirect count, conditional render, compute shared memory
   (LDS/STS), textures written by compute (STG into texture memory is not seen), counters other than
   timestamps (occlusion queries report "visible"), integer render-target / texture variants, ViewportSwizzles,
   alpha-to-coverage dither, MSAA resolve details; blend factor / compare / stencil enum values are guesses.
   The game's NV12 → RGBA conversion of movie frames (main+0x6592b4, no GPU draw seen) leaves a 1-pixel green
   border on the 512x608 texture; the source frames are clean and the UI hides it so far.
3. **More content:** finish a whole Grand Prix (races 2-4, results, podium, saving progress), Time Trials, VS,
   Battle, other cups and courses. Each new course dumps new shaders: run `translate.py --spv` after such runs
   (~10% of a new course's draws are skipped until then).
4. **M6 leftovers:** listen to the audio; finish a Grand Prix and check that progress, settings and ghosts write and
   read back. Holding A drives a race with smart steering (`2100-30000:A`: 2nd place, lap 3/3 by presented frame
   12,500, ~560 s unpaced); a race needs ~14,000 frames, so a GP is ~40 min. A 13-minute run was stopped once when
   the machine ran low on memory (16 GB): close other programs before long runs.
5. **M7 performance:** the game runs at ~20-33 fps even unpaced (every draw pushes 32 descriptors and rewrites
   c[0]/c[2], all dynamic state is set per draw, global barriers around every transfer, texture pools rescanned
   per submit, imported host memory is read over PCIe). Measure before optimising.

### Netplay (2026-10-31: local play over community rooms)

- The user's call: multiplayer through the community room servers Eden/yuzu/Citra use (a bridge for LDN), MK8
  only (`nx_game(... NETPLAY)` in `mk8recomp/CMakeLists.txt`; Violet does not build it). Our own code, compatible
  on the wire with Eden so both meet in the same rooms; Eden's sources (`../deps/suyu`: `src/network`,
  `src/core/hle/service/ldn`, `src/core/internal_network/socket_proxy.cpp`) were read for the protocol only.
- Libraries (the user approved both downloads): `../deps/enet` (lsalzman/enet v1.3.18, the rooms' transport) and
  `../deps/zstd` (v1.5.6, `lib/` only: Eden zstd-compresses every proxy payload), built from source by
  `nxrecomp.cmake` (`nx_netplay_libs`).
- Start-up (`runtime/net/browser.c`; since 2026-11-07 only with `NX_ROOM_BROWSER=1`, rooms are joined in the menu): a Win32 "Public Room Browser" (nickname, search, "<game> only" by title id:
  the room's preferredGameId or a member's gameId, hide empty/full, refresh, member list, description, password
  prompt, direct connect host[:port], Play Offline; closing quits). Settings in `build/save/netplay.ini`. Lobby:
  `GET https://api.ynet-fun.xyz/lobby` (`net/lobby.c`; ids above 2^53, parsed as integers). `NX_NETPLAY=0` skips
  the browser (test runs!), `NX_ROOM=host[:port[:password]]` + `NX_NICK` joins without it.
- In a room: `services/ldn_net.c` (ldn:u: Scan/CreateNetwork/Connect/SyncNetwork as LDN packets via the room) and
  `services/bsd.c` (bsd:u: UDP sockets as proxy packets from our fake 192.168.x.y address; TCP refused, as in
  Eden). Offline both stay as before (ldn unavailable, bsd stubbed). `net/room.c` owns ENet on its own thread.
- Verified: the browser lists the rooms (25 MK8 of 133); joining a real public room works (fake address assigned,
  game info shown to the room).
- 2026-11-01, local play with an Eden player (a friend, "meow"), public rooms and a room of our own:
  - Protocol numbering: proxy packets carry a `Network::Protocol`, and Eden builds number it two ways: releases (and
    yuzu, Citra) have Unspecified 0, ICMP 1, TCP 2, UDP 3; `deps/suyu`'s newer sources insert IP = 1 (UDP 4). Both
    sides drop packets of another protocol without a word, so each side's host saw the station join (LDN fine) and
    then nothing: "A communication error has occurred" after 5 s. `bsd.c` now sends 3, switches to 4 if a member
    sends 4, and datagram sockets take either. Fixed: we joined an Eden host and raced (2 min, ~1,500 packets each
    way). Not yet seen working: an Eden player joining us as host.
  - Stalls drop us out: a freeze of ~1 s or more mid-session makes the game give up ("communication error").
    Causes found: (1) draws waiting for shader translation: `NX_ASYNC_SHADERS` (on in netplay builds) skips them
    instead; (2) translate.py's glslang on every core starved the game's threads (main waiting for a core 73%): it
    now runs in a job at below-normal priority on half the cores; (3) 1-4 s frames on the pre-race loading screen:
    the main thread (prio 44, core 0) waited for a core half the time while the loader thread 52 (prio 45, core 0)
    sat in host code, which cannot be preempted: `NX_PROFILE="thread 52"` put it in `vk_import_host` (the driver
    pinning a pool's memory page by page, NtQueryVirtualMemory) under `gpu_pool_created`. nvnMemoryPoolInitialize /
    Finalize now give up the core (as queue submits do): main's core wait gone (<1%), the load window's worst
    frame 1,049 -> 236 ms (one 726 ms frame left, main blocked on the loader itself, which spends ~45% of its time
    importing pools: next, if it matters, make imports cheaper, e.g. keep imports of the same memory across a pool's
    destroy and re-create). Repro offline: `NX_NETPLAY=0 NX_INPUT="600-7000/45:A"` mashes A into a 50cc race.
  - Still open: sessions ending after 1-2 min with our game leaving on its own, no stall nearby and packets flowing
    both ways (needs the Eden player's log of that moment); with async shaders, a green-tinted, washed-out screen
    on one track and bright flashes (suspect: a skipped draw into a texture drawn once per track, e.g. colour
    grading or exposure; test: the same track again with its shaders cached).
  - `../room` (its own folder since 2026-11-07, was `nxrecomp/tools/room`; `build.bat`, built to `room/build/room_server.exe`): a room server of our
    own (same protocol, 192.168.1.x for everyone as Eden's rooms; `--log 2` prints every relayed packet with its
    decompressed first bytes: Pia's packets start `32 ab 98 64`). To host: run it on UDP 24872 and expose that
    port (playit.gg UDP tunnel worked; ngrok has no UDP); join with `NX_ROOM=127.0.0.1:24872`.
- 2026-10-31: MK8 was re-lifted with the current lifter (the Violet-era lifter writes unresolved-import stubs into
  `table.c`); `dump_v21` was rebuilt from the NSPs (`elf/` by `../nxrecomp/tools/nso2elf.py`).

### Runtime notes (`../nxrecomp/runtime`)

- Run: `mk8recomp\run.ps1 [-Log 0-2] [-Timeout s] [-NoBuild]` builds, runs `build/cmake/mk8recomp.exe` and prints
  the end of `build/run.log`. Defaults: `--game dump_v21`, `--image build/image`, `--save build/save`.
  After a re-lift also run `python a64image.py ../../../build/image` if relocations/symbols changed (rarely).
- Address space (`hos.h`): the whole guest range 0x1000000000-0x8000000000 is one Windows reservation, committed at
  4 KB granularity; a block list answers QueryMemory. Alias, heap (6 GB max), stack (MapMemory copies instead of
  aliasing), TLS and the main stack have fixed places. The exe needs `/HIGHENTROPYVA:NO`, or Windows puts thread
  stacks inside that range.
- Host calls: guest addresses from 0x7104000000 that `call_ind` maps to host functions (`hostcall_register`);
  `c->pc` holds the called address. `dispatch_override(addr, fn)` redirects calls through pointers to an sdk
  function; `overrides.txt` does the same for direct calls at lift time.
- Kernel (`kernel.c`): one host thread per guest thread, one kernel lock, a condition variable per thread;
  mutex/condvar/arbiter waiters are found by scanning threads. IPC (`ipc.c`): HIPC + CMIF with domains, services
  in `runtime/services/*.c` registered by name; an unknown service or command logs and succeeds with zeroed output.
- rtld facts: its module list is in insertion-at-front order, so DT_INIT runs sdk → subsdk0 → main (after
  `nninitStartup`); symbol lookup is in load order. The sdk's nn::ro globals point at empty lists (`loader.c`).
- Crash reports: the guest instruction (lifted C carries `#line <guest offset> "<module>"` and is compiled with
  line tables; `LIFT_LINES=0` turns it off), the lifted function, guest registers as of the last call, and a
  frame-pointer backtrace symbolised with the modules' exports. `run.ps1 -DumpAfter N` prints every thread's wait
  state, backtrace and (for running threads) five samples of where it is, then exits.
- Guest scalar loads and stores are `volatile` (a64rt.h): plain C loads were hoisted out of guest spin loops.
- Cores (`kernel.c`): a guest thread runs only while it owns a core from its affinity mask; it gives the core up
  when it waits, sleeps or makes an IPC call. A higher-priority thread takes a lower-priority owner's core: the
  owner is suspended on the spot if its RIP is in lifted code (`lifted_lo/hi`: lifted.lib's code is one block
  after the runtime's objects; the link map `build/cmake/mk8recomp.map` shows it), else it yields at its next SVC.
  The game keeps per-core scratch tables (index = GetCurrentProcessorNumber), so this is needed for correctness.
- Mii (`services/mii.c`): Miis must pass the SDK's `CharInfoRaw::Verify` (create id with the RFC 4122 variant,
  nickname, per-field ranges). Get/GetCount with SourceFlag Default (2) return six default Miis; the game's player
  Mii comes from them. MiiModel format (from main's parser): ShapeMid/High.dat = "NFSR" header 0x6c (+0xc and
  +0x3c: 12 u32 each) then 12 tables of 0x58-byte entries (counts 4, 12, 12, 132 x6, 1, 18, 18 at main+f71ff0;
  offsets main+8f6cc4); NXTexture*.dat = "NFTR" with 11 tables of 0x1c-byte entries from 0x64 (counts at
  main+f72070), entry = 0x10 common attribute + {u32, u16 w, u16 h, u8 format, u8 mips, u8 type}.
- IPC: copied handles stay the client's (never close them). A domain is shared by its session's clones (the fs
  session pool relies on this). Output buffers may be B or C (auto-select): use `buf_out` / `buf_in`.
- System data archives: `build/sysdata/<data id>/`, extracted from the user's firmware dump (`../firmware/22.5.0/`).
  MiiModel (0100000000000802) is there; missing ones get placeholder files (empty MiiModel headers are not enough:
  the game builds Mii models from them); fonts fall back to Windows' arial.ttf.

### GPU notes (`runtime/nvn.c`, `gpu.c`, `vk.c`, `tex.c`, `astc.c`, `tools/shader`)

- Run the shader tools from `mk8recomp`: `python ../../nxrecomp/tools/shader/translate.py --spv` (1 min; new dumps appear as the
  game reaches new content), `python stats.py` (opcode histogram).
- NVN command buffers are recorded in our encoding into the game's command memory (nvn.h) and replayed into Vulkan
  at `nvnQueueSubmitCommands`; state carries over between command buffers as on NVN. Memory pools are imported as
  Vulkan buffers over guest memory (VK_EXT_external_memory_host); virtual pools get GPU addresses from 0x8000000000.
- NVN constant buffers as GLSLC uses them: c[0] driver data (graphics: storage buffer table at 0x110, 16 bytes per
  slot {addr lo, hi, size}; compute: uniform buffers at 0x220 + 16 b, storage at 0x310 + 16 k, read with LDG),
  c[1] the shader's embedded constants (control section 0x6fc size, 0x700 offset), c[2] texture handles (bound
  slot k at 0x20 + 8 k), c[3 + b] uniform buffer b. Our c[0] holds Vulkan device addresses, so LDG/STG work as
  buffer references. Handles are id | sampler << 20; textures are bindless (set 1 = texture pool, set 2 = sampler
  pool, every entry pre-filled with a dummy).
- GLSLC control section: 0x6f8 code size, 0x704 end, 0x70c GPRs, 0x748 compute workgroup size. Graphics code starts
  with a 0x30 header (0x12345678) + 0x50 SPH; compute code has neither. Branch targets on a scheduling word mean
  the next instruction; the only backward branch in MK8's shaders is the trailing `BRA self`.
- The game sets the upper-left window origin: the viewport is flipped (winding too) so render targets hold rows
  as on the console; present then needs no flip. Depth mode stays NEAR_IS_MINUS_W (depth clip control -1..1).
- Images are keyed by storage and shape (not mip count); textures the CPU rewrites are found with write watch
  (`MEM_WRITE_WATCH` on the guest reservation) and uploaded again unless the GPU wrote them.
- Texture pool entries are resolved lazily: a draw/dispatch resolves (creates, uploads) the entries its bound
  handles name (shaders read handles only from c[2]) and marks those images used. Over 55% of device-local memory,
  or when an allocation fails, images unused for 60 frames are evicted (`images_evict`). The game leaves old
  courses' descriptors in its pools and changes render-target sizes every few frames (dynamic resolution), so
  "named by a pool entry" says nothing about use.
- IPA: GLSL inputs are perspective-correct, so IPA.MULTIPLY's multiplier is dropped, and IPA.PASS of a smooth
  attribute becomes `attribute * gl_FragCoord.w` (the raw interpolant; perspective-projected UI multiplies by w
  itself). The vertex stage writes `gl_PointSize` from NVN's point size (push constant at offset 8).
- Debugging: `NX_DUMP=n,m` (window texture and every render target of those frames → `build/frames/*.ppm`;
  `NX_DUMP_TEX=<addr>` adds every image at that address), `NX_TRACE=n` or `n-m` (every draw of those frames:
  shaders, targets, state, attributes, buffers, bound textures; `NX_TRACE_FILTER=<text>` keeps only draws whose
  trace contains it, e.g. `128x128x1 f56`; `NX_TRACE_UBO=0,4,33` adds those 16-byte rows of the vertex stage's
  c[3]), `NX_NOPACE=1` (no present pacing),
  F12 while playing (trace and dump the next frame, as NX_TRACE + NX_DUMP), `NX_WATCH=<addr>` (GPU writes to one texture's storage), `NX_GPU_SKIP=1|2|4` (skip all / global-memory /
  textured draws: bisects device loss), `NX_DUMP_ASTC=1`. `mk8recomp\gputest.ps1 -Skip n -Timeout s` runs and
  greps the GPU log. `NX_VK_VALIDATION=1` enables the Khronos validation layer; its messages go to the log as
  `vk error:` / `vk warning:` (each message id its first 3 times). `NX_FS_TRACE=<part of a path>` logs every
  read of matching RomFS files.

### Audio notes (`runtime/services/audio.c`, `audio_dsp.c`, `audio_fx.c`)

- The game opens one renderer: 48 kHz, 240 samples, REV4, 96 voices, 30 mix buffers, final mix + 1 sub mix
  (24 buffers), 14 effects, 1 device sink. Voices are mono 32 kHz DSP-ADPCM (music streams: stereo pairs of
  0x2000-byte buffers with every buffer flagged end-of-stream) and PCM16 (engines, 25.6-32 kHz, pitch varies).
- Effects in use: an Aux on sub mix buffers 6-11 (the game's own CPU reverb: atk reads the send ring and writes the
  return ring) and a 4-channel Delay on 12-15. Reverb/I3DL2 are implemented but not seen used yet.
- REV4 means: wave buffer v1 (a looping buffer loops until flushed/stopped), Q15 volumes, no flush count, effect
  parameters v1, six-channel effects in the old FL FR BL BR C LFE order, no elapsed-frame render info.
- The renderer thread (highest priority) renders whenever the SDL stream holds fewer than 4 frames (one per 5 ms
  tick without a device) and signals the renderer event per frame. Mix buffers are float in 16-bit units; the Aux exchanges s32.
- Debugging: `NX_AUDIO_DUMP=<file.wav>` (everything sent to the device), `NX_AUDIO_TRACE=1` (voices and wave
  buffers as the game queues them), `NX_NOAUDIO=1`; log level 1 prints a renderer status line every 1000 frames.
  Check a dump with numpy: per-second RMS, a spectrogram, second-difference spikes for clicks.

### Movies, applets, input

- `runtime/movie.c`: main's calls of `movie::Extractor` / `movie::Decoder` constructors are lift-time overrides
  (`tools/lifter/overrides.txt`); our objects (16 bytes: vtable in subsdk0's layout, D2 D0 then methods; our state)
  give a timed stream of frames: the extractor reads the MP4's video track (mdhd duration, stts frame time,
  tkhd size: MovieMenu.mp4 is 220 s, 60 fps, 512x608) and walks/seeks its samples; the decoder (4 input, 4 output
  slots) turns each sent sample into an output with that sample's time and holds outputs until released. Each
  output gets the frame at its time from a Media Foundation source reader on the same file (`video_frame`: reads
  forwards, seeks when the time goes back or jumps over 2 s); the extractor's samples stay one byte (unused).
  `movie::Status`: 0 ok, 0x2000 end of stream, 0x5003 try again, 0x4004 format changed.
- main's movie player (main+0x379f28..0x37ca88): client events [0] input available, [1] output available,
  [2] format changed, [3] error (never waited on). An event thread collects input indices (each posts "feed" to
  the player thread, which does GetTrackIndexForAvailableData / Read / GetSampleTime / SendInputBufferForDecode /
  Advance and loops with SeekTo) and output indices (into the renderer's queue: 32 entries, no overflow check).
  Seek (wrapper main+41bdb8) sets player+0x420; the player thread then SeekTo()s and the renderer drops frames
  until one has exactly the new GetSampleTime. The renderer (main+37c578) shows one output per movie frame; its
  time (renderer+8) is the player's current time (wrapper main+41bf8c), which the title (93-143 s) and menus poll.
  `movie::Buffer`: +0 offset, +4 size, +0x18 capacity, +0x28 data. `tools/a64dis.py <module> <offset>` disassembles
  a function with PLT names (capstone).
- Library applets (am.c) finish at once as cancelled, with an output storage whose first word is 1 (the Mii editor
  is applet 0x12, started from "Create a Mii").
- Input: arrows = stick + D-pad, Z/X/C/V = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter/Backspace = +/-, IJKL = right
  stick; gamepads by label. `NX_INPUT="first-last:A+B;..."` holds buttons over ranges of presented frames,
  `first-last/n:A` presses A for 3 frames every n (see "How to continue" 1).

### Lifter implementation notes

- Files: `tools/nxmod.py` (64-bit module reader, IDA seeds), `tools/lifter/a64lift.py` (driver, functions, imports),
  `a64insn.py` (integer, branch, load/store, system), `a64simd.py` (FP, SIMD, crypto), `a64test.py`;
  `runtime/a64rt.h` (inline helpers, `Ctx`), `runtime/a64rt.c` (crypto, estimates, half floats, narrowing shifts).
- IDA export: `nxmod.py seeds` → `IDA_SEEDS=… ida_export.py` → `build/analysis_305/<module>_ida.json` (functions,
  data in .text, switch tables). A re-export from the saved `.i64` takes seconds.
- Module bases: main 0x7100000000, subsdk0 0x7101400000, sdk 0x7101c00000 (`a64lift.BASES`; the loader must agree).
- Calls through PLT stubs are resolved at lift time in load order main → subsdk0 → sdk (main's malloc/new override
  the sdk's, as with rtld). `tools/lifter/overrides.txt` (one symbol per line) turns calls into `h_<symbol>(c)`.
  The only unresolved import that is called is `nnosGetDefaultUserExceptionStackForApplication` (sdk, `u_…`).
- A function with secondary entries (direct branch targets inside it, found in a pre-pass) is emitted once as
  `b_<addr>`; each entry `f_<addr>` sets `c->entry` and tail-calls it. BR without a switch table and B to another
  function are `musttail` calls (`jump_ind(c)` takes the target in `c->pc`).
- Accepted differences from the console: x86's default NaN has the sign bit set (ARM's doesn't); FPCR rounding
  modes other than nearest are not modelled; RET always returns to the caller; STXR compares values (ABA-tolerant),
  as other emulators do.
- setjmp/longjmp (the sdk's libjpeg errors, e.g. the Time Trials ghost thumbnail): calls of the guest setjmp are lifted
  as `GUEST_SETJMP` (a64rt.h: also a CRT setjmp in the lifted caller's frame, keyed by the guest jmp_buf); longjmp is
  an override (`h_longjmp`, dispatch.c) that restores x19-x30, sp, d8-d15 and longjmps the host. `__builtin_setjmp`
  crashes on Windows x64 with clang-cl; the CRT's unwinding version works with musttail.

## Files

| Path | What |
|---|---|
| `rom/game.nsp` | base 1.0.0 (program NCA `e1ace524…`); its RomFS is the base of every update's RomFS. Its unpacked copy `extract/` was deleted (2026-10-17): unpack it again with `nstool -x / extract rom\game.nsp` when the base NCA is needed |
| `rom/mk8u_v1376256.nsp` → `extract_v21/` | update 3.0.5 (program NCA `51ace2d9…`, hash-verified) |
| `dump_v21/exefs`, `elf`, `control`, `romfs` | 3.0.5 code (main, sdk, subsdk0, rtld), ELF copies, NACP, merged data (6,596 files, 11.3 GB) |
| `build/analysis_305/main_imports.txt` | main's 902 imports (875 sdk, 15 subsdk0, 12 unresolved) |
| `build/analysis_305/<module>_ida.json`, `dump_v21/elf/*.i64` | IDA exports and databases for 3.0.5 |
| `build/lifted/`, `build/cmake/`, `build/a64test/` | lifted C, the CMake/Ninja build, differential-test work files |
| `build/image/`, `build/save/`, `build/sysdata/`, `build/run.log` | relocated module images, save data, system data archives (from `../firmware/22.5.0/`), the last run's log |
| `build/shaders/`, `build/frames/` | dumped Maxwell shaders (+ `glsl/`, `spv/` translations); frame and render target dumps |
| `mk8recomp/` | MK8's part of the source: `game.json` (modules and bases, title, paths), `CMakeLists.txt` (calls `nx_game` from `../../nxrecomp/nxrecomp.cmake`), `build.bat`, `run.ps1`, `gputest.ps1`. Everything else (runtime, lifter, shader tools) is in the shared `../nxrecomp` (see `../CLAUDE.md`); its tools find `game.json` from the current directory, so run them from `mk8recomp` |

3.0.5 main: text 0xb510c8 (11.3 MB), ro 0x654c68, data 0x16abe0, bss 0x50cd0. 1.0.0 and 3.0.1 are 32-bit (A32); the shelved 1.0.0 work (dump, A32 lifter, plan) was deleted on 2026-10-17
(still in git history before that date).

## Tool notes

- Update RomFS with nstool (see `../CLAUDE.md`): `--basenca extract\e1ace5248218fa6393ec1637ce8c2c79.nca -x /1 <dir>
  <update NCA>` (unpack `extract/` from `rom\game.nsp` first).
- Lifter (from `mk8recomp`, `$L` = `../../nxrecomp/tools/lifter`): `python $L/a64lift.py cover` (coverage, 40 s),
  `python $L/a64lift.py lift ../build/lifted`, `python $L/a64image.py ../build/image`, `python $L/a64test.py --per 8 --states 4 [--pairs] [--filter REGEX]` (5 min; failures go to
  `build/a64test/failures.txt` with their states). Rerun the tests after any change to a64insn/a64simd/a64rt.
- Build: `mk8recomp\build.bat` (vcvars + CMake + Ninja + clang-cl; configures `build/cmake` on first use). Keep it
  Release: CMake's default Debug flags for clang-cl (`/Zi /RTC1`) made one file take 6 min and the library 3.5x
  larger. A file compiles in 15-30 s; `LIFT_JOBS` (8) limits parallel compiles to fit 16 GB.

## REA / IDA (see `../CLAUDE.md` for cleanup)

REA can't open the saved `.i64` ("unsupported binary format"): open `dump_v21/elf/main.elf` (image base 0, so
addresses are main offsets); the first analysis takes ~4 min (the first call times out into a background task),
then decompiles are quick. For quick looks without IDA: `python ../../nxrecomp/tools/a64dis.py main <offset>` from `mk8recomp` (capstone,
PLT names). `../nxrecomp/tools/ida_export.py` (idalib directly) leaves no stray processes.
