# ns-port

Workspace for static recompilations of the user's own Nintendo Switch games to native Windows x64 programs. Each
game is a project folder with its own `CLAUDE.md`; shared tools, keys, firmware, libraries and references sit at the
root.

| Project | What |
|---|---|
| `mk8dx/` | Mario Kart 8 Deluxe 3.0.5 (AArch64 lifter, SDK over emulated kernel/services, NVN on Vulkan). See `mk8dx/CLAUDE.md` |
| `pkmn-violet/` | Pokémon Violet 4.0.0 + DLC (V4 graphics in progress: first-boot screens and the opening cutscene render). See `pkmn-violet/CLAUDE.md` |
| `ldn-beacon/` | a real Switch's local play bridged into our (Eden-compatible) rooms; runs on Windows or Linux, with an ESP32-S3 on USB as the antenna (its firmware in `ldn-beacon/s3/`, ESP-IDF 5.5.6 from Espressif's EIM, installed outside the workspace) or, on Linux, the built-in Wi-Fi card; own git repo, local only for now, builds on its own. See `ldn-beacon/CLAUDE.md` |
| `room/` | the multiplayer room server (Citra / yuzu / Eden protocol) the games, Eden and the beacon meet in; one C file, ENet + zstd, Windows or Linux; own git repo (local; moved out of `nxrecomp/tools/room` on 2026-11-07, its history stays there). See `room/README.md` |

**Start anything with `.\start.ps1`** at the root (alone it lists the commands): `violet` / `mk8` play (builds
first), `room` hosts a room, `beacon` bridges a real Switch, `build <project>`, `package <game> <dir>`. It wraps each
project's own scripts, which still work on their own.

Notes: each project's `CLAUDE.md` is kept short (start here, state, what is open); its `NOTES.md` holds the history,
findings and runs (pkmn-violet, ldn-beacon; mk8dx's notes are reference and stay in its CLAUDE.md). Grep both
before proposing a tool or an experiment; add new findings to NOTES.md and update the state / open lists.

Current focus (the user's call, 2026-10-18): **Pokémon Violet only**. Changes to the shared `nxrecomp/` need no MK8
regression checks (no MK8 builds, boots or shader diffs); the user checks MK8 themselves.

Rules: nothing derived from a game (dumps, keys, lifted C, IDA databases) goes anywhere shareable; it stays in the
project's game-data folders (`rom/`, `dump*/`, `extract*/`, `build/`) or in `keys/` and `firmware/`. Big outputs go
off C: (nearly full), on whichever data drive the machine has: never hard-code a drive letter. Ask before downloading anything new. References (`ref/`, `deps/suyu`) are for
behaviour only: our own implementation, never copied code.

## Layout

| Path | What |
|---|---|
| `/` | workspace repo `HelixNX/ns-port` (private): README, `start.ps1`, the build entry (`CMakeLists.txt`, `CMakePresets.json`, `build.bat`), this CLAUDE.md, placeholder folders, and the shared source `nxrecomp/`. Each game is its own repo in the HelixNX organisation (private), cloned into the workspace as its project folder (`.\start.ps1 clone [violet\|mk8]`, which also sets up a folder that already holds game data): `HelixNX/pkmn-violet` (`pkmn-violet/`: CLAUDE.md, NOTES.md, `rom/`, `violetrecomp/`) and `HelixNX/mk8dx` (`mk8dx/`: CLAUDE.md, `rom/`, `mk8recomp/`). History: the old repos `s1099/nxrecomp`, `mk8recomp`, `violetrecomp` were submodules until 2026-11-10, then merged into this repo with their full history (old `.git` folders, with a stash in nxrecomp's, kept in `.git/old-submodules/`); the same day each game folder was split out into its own repo with its history (the old commits are still in this repo's history up to the split). A change across both (nxrecomp and a game) is two commits, one per repo. `ldn-beacon/` and `room/` are repos of their own, not tracked here. Its `.gitignore` is a whitelist: game data, keys, firmware, deps and tools stay out. A public release would be fresh repos (a snapshot, no history), under an organisation: open questions in README.md, "Going public" |
| `<project>/` | one game: `rom/` (the user's NSPs), extracted/dumped data, `build/`, and the source repo |
| `nxrecomp/` | shared source (split from MK8's on 2026-10-17, history kept): `runtime/` (kernel, SVCs, IPC and services, NVN on Vulkan, audio, input), `tools/` (lifter, image builder, shader translator, IDA export, `package.ps1`, which builds `../room` into packages), `nxrecomp.cmake` (`nx_game`). A game's repo holds `game.json` (name, title, title id, modules with fixed bases, dump/analysis/build paths; see `tools/gamecfg.py`), an optional `overrides.txt` and a `game.cmake` calling `nx_game`; run the tools from inside the game's repo |
| `keys/` | `prod.keys`, `title.keys`; `.switch/` holds copies for nstool (see Tool notes) |
| `firmware/` | `Firmware_22.5.0.zip` and its contents in `22.5.0/` (NCAs; `keys/prod.keys` covers it; on the AMD machine unpacked 2026-11-04). `Firmware.23.0.1.zip` added 2026-11-06, not unpacked; whether `prod.keys` covers it is unchecked. A game's system data goes to `<project>/build/sysdata/<data id>/` (`nstool -x /0 <dir> <nca>`), e.g. NgWord2 `0100000000000823` = `00d460df….nca`, which the SDK's NG word filter mounts in local play. MiiModel is `99d7c22f….nca`; find others with `nstool <nca>` (ProgID) and extract with `nstool -x / <dir> <nca>` |
| `deps/` | `SDL3/SDL3-3.4.18` (VC), `Vulkan-Headers` (v1.4.341), `glslang` (16.6.0 release), `suyu` (sparse Eden checkout of the service, network and shader directories consulted so far; widen it with `git -C deps/suyu sparse-checkout add <dir>`. On the AMD machine, where `deps/` was otherwise empty, it is a shallow blobless clone of `src/video_core` and `src/shader_recompiler` from 2026-10-31, read for GPU translator design), `enet` (v1.3.18) and `zstd` (v1.5.6, `lib/` only) for netplay, only where vcpkg has neither (the user prefers vcpkg: `vcpkg install enet zstd`; on the AMD machine they come from vcpkg and `deps/` has no copies). In `nxrecomp/tools/vcenv.bat`'s environment `VCPKG_ROOT` points at Visual Studio's own vcpkg, so `nxrecomp.cmake` looks in the vcpkg tree SDL3 came from first. Visual Studio's vcpkg has no classic mode, so
`deps/vcpkg/` holds a manifest (`vcpkg.json`: opus, imgui[vulkan-binding]; made 2026-11-10 on the machine with
Visual Studio on E:) installed into `deps/vcpkg/installed/x64-windows` (`vcpkg install --triplet x64-windows
--x-install-root=installed` from there), which `nxrecomp.cmake` also searches. `ooz` (github.com/powzix/ooz, GPLv3+, cloned 2026-11-05 on the AMD machine): Oodle Kraken decoder for
`nxrecomp/tools/shader/extract` (built into `oozbatch.exe`, a separate process). `python-embed/python-3.12.10-embed-amd64.zip` (python.org, downloaded 2026-11-05 on the AMD machine): the shader translator's Python in a player package (`nxrecomp/tools/package.ps1 -PythonZip`) |
| `ref/` | `SavageWindRecomp` (BotW 1.8.2 recompilation on LAS Recomp, MIT, cloned 2026-10-22; notes in Spanish in `docs/SAVAGE-WIND.md`; reference only). `NintendoClients.wiki` (kinnay's wiki as markdown, shallow clone 2026-11-07 at b49001c; sparse checkout without the three pages whose names have a colon, which Windows cannot create): LDN, Pia (Pia-Protocol, Pia-Types, Station-Protocol, Mesh-Protocol, Pia-Game-Keys), behaviour only; grep it rather than fetching pages. `LDN` (kinnay's Python LDN library, GPLv3, shallow clone 2026-11-07 at 13edfaf): hosting / joining on Linux with monitor interfaces; behaviour only, never copied. `PokeZ-ARecomp-Online` (ZA Recomp: Pokémon Legends Z-A on a LAS Recomp fork, MIT, shallow clone 2026-11-10 at 98be853; the same Game Freak engine family as Violet: trpfd RomFS, Wwise, Lua): its own `CLAUDE.md` maps it (`tools/` lifter passes flagfuse / regpromote, `runtime/nvn/vulkan/` exec / record / textures, `runtime/services/fs.c` romfs mods); reference only |
| `.tools/` | `nstool.exe`, `ida-mcp` (headless IDA for REA) |
| `.claude/`, `.mcp.json` | Claude Code settings, REA MCP config |

## Toolchain (shared)

- Compiler: **clang-cl 19+** in MSVC mode, with MSVC `link.exe` (no mingw ABI). `nxrecomp/tools/vcenv.bat` finds
  them: Visual Studio through vswhere, clang-cl from its "C++ Clang tools" component (else MSYS2's); `NX_VCVARS` and
  `NX_CC` override. Never hard-code a machine's paths in anything committed: the repos must build on any machine.
- Flags: `-O1 -ffp-contract=off -fno-strict-aliasing`. If one file compiles too slowly, build it at -O0 or split it.
- Build: Python generates the C sources; CMake + Ninja build them, from the one root `CMakeLists.txt`: a build tree
  holds one game (`NX_GAME`), its tree is the game's `<project>/build/cmake`. `build.bat violet|mk8 [ninja args]` at
  the root (vcenv, then the game's preset from `CMakePresets.json`); each game's `build.bat` calls it. In a vcenv
  shell: `cmake --preset violet` and `cmake --build --preset violet`. A new game: its folder's `game.json` and
  `game.cmake`, a line in the root `CMakeLists.txt`, a preset. Libraries (`nxrecomp.cmake`): Vulkan headers and
  SDL3 from `deps/` if there, else the Vulkan SDK (`VULKAN_SDK`) and vcpkg (`VCPKG_ROOT`).
- Verified: a clang-cl test linked SDL3 and glslang statically and ran. A 2.6 M-instruction lift compiled with
  clang 19 in 5.4 min on 12 cores.
- Tested on: NVIDIA GTX 1650 Ti (16 GB RAM, 12 cores) and AMD RX 7800 XT. Graphics must not rely on one vendor's
  handling of undefined behaviour: keep NX_VK_VALIDATION runs free of errors.

## Tool notes

- nstool reads `title.keys` only from `%USERPROFILE%\.switch`; private copies are in `keys/.switch`. Run it from
  PowerShell (Git Bash rewrites `/0`), from the workspace root: `$env:USERPROFILE="$PWD\keys"; .\.tools\nstool.exe …`
  - An update's ExeFS sits at the end of its NCA. To peek at a new update without copying it all, write a sparse
    file with only the header (first 0x4000) and that range.
- Python: unicorn 2.1.4 and capstone 5.0.7 are in user site-packages, so don't run them with `python -I`.
- The shell here sets NoDefaultCurrentDirectoryInExePath, so run a program in the current folder as `.\x.exe`.
- A shell whose current directory is inside a folder keeps Windows from moving or deleting that folder.

## REA / IDA cleanup

REA uses headless IDA (`.claude/ida-mcp.json` → `.tools/ida-mcp`). `close_binary` always returns
`cleanup_incomplete`, leaving worker processes and a temp workspace. After REA analysis (after `close_binary`, even if
it errors), and never while a binary is still open:

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'ida-mcp' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -Confirm:$false -ErrorAction SilentlyContinue }
Get-ChildItem "$env:LOCALAPPDATA\Temp" -Directory -Filter 'rea-ida-*' |
  Remove-Item -Recurse -Force -Confirm:$false
```

Match on the command line (other Python processes are unrelated), then verify nothing remains.
