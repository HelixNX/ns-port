# ns-port

Workspace for static recompilations of the user's own Nintendo Switch games to native Windows x64 programs. Each
game is a project folder with its own `CLAUDE.md`; shared tools, keys, firmware, libraries and references sit at the
root.

| Project | What |
|---|---|
| `mk8dx/` | Mario Kart 8 Deluxe 3.0.5 (AArch64 lifter, SDK over emulated kernel/services, NVN on Vulkan). See `mk8dx/CLAUDE.md` |
| `pokemon-violet/` | Pokémon Violet 4.0.0 + DLC (V4 graphics in progress: first-boot screens and the opening cutscene render). See `pokemon-violet/CLAUDE.md` |
| `ldn-beacon/` | a real Switch's local play bridged into our (Eden-compatible) rooms, run on a Linux laptop next to the Switch; own git repo, local only for now. See `ldn-beacon/CLAUDE.md` |

Current focus (the user's call, 2026-10-18): **Pokémon Violet only**. Changes to the shared `nxrecomp/` need no MK8
regression checks (no MK8 builds, boots or shader diffs); the user checks MK8 themselves.

Rules: nothing derived from a game (dumps, keys, lifted C, IDA databases) goes anywhere shareable; it stays in the
project's game-data folders (`rom/`, `dump*/`, `extract*/`, `build/`) or in `keys/` and `firmware/`. Big outputs go
off C: (nearly full), on whichever data drive the machine has: never hard-code a drive letter. Ask before downloading anything new. References (`ref/`, `deps/suyu`) are for
behaviour only: our own implementation, never copied code.

## Layout

| Path | What |
|---|---|
| `/` | workspace repo `s1099/ns-port` (private): README, CLAUDE.md files, placeholder folders; nxrecomp, mk8recomp and violetrecomp are submodules (each private on GitHub, branch `main`). Its `.gitignore` is a whitelist: game data, keys, firmware, deps and tools stay out |
| `<project>/` | one game: `rom/` (the user's NSPs), extracted/dumped data, `build/`, and the source repo |
| `nxrecomp/` | shared source, a git repo (split from MK8's on 2026-10-17, history kept): `runtime/` (kernel, SVCs, IPC and services, NVN on Vulkan, audio, input), `tools/` (lifter, image builder, shader translator, IDA export), `nxrecomp.cmake` (`nx_game`). A game's repo holds `game.json` (name, title, title id, modules with fixed bases, dump/analysis/build paths; see `tools/gamecfg.py`), an optional `overrides.txt` and a `CMakeLists.txt` calling `nx_game`; run the tools from inside the game's repo |
| `keys/` | `prod.keys`, `title.keys`; `.switch/` holds copies for nstool (see Tool notes) |
| `firmware/` | `Firmware_22.5.0.zip` and its contents in `22.5.0/` (NCAs; `keys/prod.keys` covers it; on the AMD machine unpacked 2026-11-04). A game's system data goes to `<project>/build/sysdata/<data id>/` (`nstool -x /0 <dir> <nca>`), e.g. NgWord2 `0100000000000823` = `00d460df….nca`, which the SDK's NG word filter mounts in local play. MiiModel is `99d7c22f….nca`; find others with `nstool <nca>` (ProgID) and extract with `nstool -x / <dir> <nca>` |
| `deps/` | `SDL3/SDL3-3.4.18` (VC), `Vulkan-Headers` (v1.4.341), `glslang` (16.6.0 release), `suyu` (sparse Eden checkout of the service, network and shader directories consulted so far; widen it with `git -C deps/suyu sparse-checkout add <dir>`. On the AMD machine, where `deps/` was otherwise empty, it is a shallow blobless clone of `src/video_core` and `src/shader_recompiler` from 2026-10-31, read for GPU translator design), `enet` (v1.3.18) and `zstd` (v1.5.6, `lib/` only) for netplay, only where vcpkg has neither (the user prefers vcpkg: `vcpkg install enet zstd`; on the AMD machine they come from vcpkg and `deps/` has no copies). In `nxrecomp/tools/vcenv.bat`'s environment `VCPKG_ROOT` points at Visual Studio's own vcpkg, so `nxrecomp.cmake` looks in the vcpkg tree SDL3 came from first. `ooz` (github.com/powzix/ooz, GPLv3+, cloned 2026-11-05 on the AMD machine): Oodle Kraken decoder for
`nxrecomp/tools/shader/extract` (built into `oozbatch.exe`, a separate process). `python-embed/python-3.12.10-embed-amd64.zip` (python.org, downloaded 2026-11-05 on the AMD machine): the shader translator's Python in a player package (`nxrecomp/tools/package.ps1 -PythonZip`) |
| `ref/` | `SavageWindRecomp` (BotW 1.8.2 recompilation on LAS Recomp, MIT, cloned 2026-10-22; notes in Spanish in `docs/SAVAGE-WIND.md`; reference only) |
| `.tools/` | `nstool.exe`, `ida-mcp` (headless IDA for REA) |
| `.claude/`, `.mcp.json` | Claude Code settings, REA MCP config |

## Toolchain (shared)

- Compiler: **clang-cl 19+** in MSVC mode, with MSVC `link.exe` (no mingw ABI). `nxrecomp/tools/vcenv.bat` finds
  them: Visual Studio through vswhere, clang-cl from its "C++ Clang tools" component (else MSYS2's); `NX_VCVARS` and
  `NX_CC` override. Never hard-code a machine's paths in anything committed: the repos must build on any machine.
- Flags: `-O1 -ffp-contract=off -fno-strict-aliasing`. If one file compiles too slowly, build it at -O0 or split it.
- Build: Python generates the C sources; CMake + Ninja build them. Libraries (`nxrecomp.cmake`): Vulkan headers and
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
