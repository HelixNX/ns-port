# ns-port

Static recompilations of the user's own Nintendo Switch games to native Windows x64 programs. Each game is a project
folder with its own `CLAUDE.md`; shared tools, keys, firmware, libraries and references sit at the root.

| Project | What |
|---|---|
| `mk8dx/` | Mario Kart 8 Deluxe 3.0.5 (AArch64 lifter, SDK over emulated kernel / services, NVN on Vulkan). `mk8dx/CLAUDE.md` |
| `pkmn-violet/` | Pokémon Violet 4.0.0 + DLC (playable). `pkmn-violet/CLAUDE.md` |
| `ldn-beacon/` | a real Switch's local play bridged into our (Eden-compatible) rooms, Windows or Linux, ESP32-S3 on USB as antenna (firmware `ldn-beacon/s3/`, ESP-IDF 5.5.6 via Espressif's EIM, outside the workspace); keys from the user's firmware and game; own repo `HelixNX/ldn-beacon`. `ldn-beacon/CLAUDE.md` |
| `room/` | the room server (Citra / yuzu / Eden protocol) games, Eden and the beacon meet in; one C file, ENet + zstd; own local git repo (history before 2026-11-07 in `nxrecomp/tools/room`). `room/README.md` |

**Start anything with `.\start.ps1`** (alone it lists commands): `violet` / `mk8` play (builds first), `room`,
`beacon`, `build <project>`, `package <game> <dir>`, `clone [violet|mk8]`. It wraps each project's own scripts.

Notes: each project's `CLAUDE.md` is short (start here, state, open); its `NOTES.md` holds history, findings and
runs (pkmn-violet, ldn-beacon; mk8dx's reference notes stay in its CLAUDE.md). Grep both before proposing a tool or
experiment; add findings to NOTES.md and update state / open lists.

Current focus (the user's call, 2026-10-18): **Pokémon Violet only**. Changes to shared `nxrecomp/` need no MK8
regression checks (no MK8 builds, boots or shader diffs); the user checks MK8 themselves.

Rules: nothing derived from a game (dumps, keys, lifted C, IDA databases) goes anywhere shareable; it stays in the
project's game-data folders (`rom/`, `dump*/`, `extract*/`, `build/`) or `keys/` / `firmware/`. Big outputs go off C:
(nearly full) onto the machine's data drive: never hard-code a drive letter. Ask before downloading anything new.
References (`ref/`, `deps/suyu`) are for behaviour only: our own implementation, never copied code.

## Layout

| Path | What |
|---|---|
| `/` | workspace repo `HelixNX/ns-port` (private): README, `start.ps1`, build entry (`CMakeLists.txt`, `CMakePresets.json`, `build.bat`), shared source `nxrecomp/`. `.gitignore` is a whitelist (game data, keys, firmware, deps, tools stay out). Games are their own private repos, cloned in as project folders: `HelixNX/pkmn-violet` (`violetrecomp/` source), `HelixNX/mk8dx` (`mk8recomp/`). A change across nxrecomp and a game is two commits. History: old submodules merged in 2026-11-10 (old `.git`s, incl. a stash in nxrecomp's, in `.git/old-submodules/`), games split out the same day. A public release would be fresh snapshot repos under an organisation (README, "Going public") |
| `nxrecomp/` | shared source: `runtime/` (kernel, SVCs, IPC + services, NVN on Vulkan, audio, input), `tools/` (lifter, image builder, shader translator, IDA export, `package.ps1`, which also builds `../room`), `nxrecomp.cmake` (`nx_game`). A game's repo holds `game.json` (modules, bases, paths; `tools/gamecfg.py`), optional `overrides.txt`, `game.cmake`; run the tools from inside the game's repo |
| `keys/` | `prod.keys`, `title.keys`; `.switch/` copies for nstool |
| `firmware/` | `Firmware_22.5.0.zip`, unpacked in `22.5.0/` (covered by `keys/prod.keys`); `Firmware.23.0.1.zip` (not unpacked, key coverage unchecked). System data goes to `<project>/build/sysdata/<data id>/` (`nstool -x /0 <dir> <nca>`): NgWord2 `0100000000000823` = `00d460df….nca` (the NG word filter needs it in local play), MiiModel `99d7c22f….nca`; find others with `nstool <nca>` (ProgID) |
| `deps/` | `SDL3/SDL3-3.4.18`, `Vulkan-Headers` (v1.4.341), `glslang` (16.6.0), `suyu` (sparse Eden checkout; widen with `git -C deps/suyu sparse-checkout add <dir>`), `enet` (1.3.18) / `zstd` (1.5.6) only where vcpkg lacks them (the user prefers vcpkg), `vcpkg/` (manifest: opus, imgui[vulkan-binding], installed to `deps/vcpkg/installed/x64-windows` with `vcpkg install --triplet x64-windows --x-install-root=installed`: Visual Studio's vcpkg has no classic mode), `ooz` (Oodle Kraken decoder, GPLv3+, built into the separate `oozbatch.exe`), `python-embed/python-3.12.10-embed-amd64.zip` (for `package.ps1 -PythonZip`). Contents differ per machine. `vcenv.bat` points `VCPKG_ROOT` at Visual Studio's vcpkg; `nxrecomp.cmake` searches SDL3's vcpkg tree, then `deps/vcpkg` |
| `ref/` | reference only, each MIT unless noted: `SavageWindRecomp` (BotW on LAS Recomp; notes `docs/SAVAGE-WIND.md`), `NintendoClients.wiki` (kinnay's wiki: LDN, Pia, Station / Mesh protocols, game keys; grep, don't fetch), `LDN` (kinnay's Python LDN, GPLv3), `PokeZ-ARecomp-Online` (Pokémon Legends Z-A, Violet's engine family; its `CLAUDE.md` maps it), `totk-pc-recomp` (TotK; `docs/architecture.md`, `docs/pgo.md`) |
| `.tools/` | `nstool.exe`, `ida-mcp` (headless IDA for REA), `rdts/` (Radeon tools) |
| `.claude/`, `.mcp.json` | Claude Code settings, REA MCP config |

## Toolchain (shared)

- **clang-cl 19+** in MSVC mode with MSVC `link.exe`. `nxrecomp/tools/vcenv.bat` finds Visual Studio (vswhere) and
  clang-cl (VS "C++ Clang tools", else MSYS2); `NX_VCVARS` / `NX_CC` override. Never hard-code a machine's paths in
  anything committed.
- Flags: `-O1 -ffp-contract=off -fno-strict-aliasing`; a file that compiles too slowly goes to -O0 or is split.
- Build: Python generates C; CMake + Ninja build from the root `CMakeLists.txt`, one game per tree (`NX_GAME`,
  `<project>/build/cmake`). `build.bat violet|mk8 [ninja args]`; in a vcenv shell `cmake --preset violet` / `cmake
  --build --preset violet`. A new game: `game.json`, `game.cmake`, a line in `CMakeLists.txt`, a preset. Libraries:
  Vulkan headers and SDL3 from `deps/`, else `VULKAN_SDK` / `VCPKG_ROOT`.
- Tested on NVIDIA GTX 1650 Ti (16 GB RAM, 12 cores) and AMD RX 7800 XT. Don't rely on one vendor's handling of
  undefined behaviour: keep NX_VK_VALIDATION runs free of errors.

## Tool notes

- nstool reads `title.keys` only from `%USERPROFILE%\.switch`: from PowerShell at the root,
  `$env:USERPROFILE="$PWD\keys"; .\.tools\nstool.exe …` (Git Bash rewrites `/0`). To peek at an update's ExeFS (at the
  end of its NCA) without copying it all, write a sparse file with the header (first 0x4000) and that range.
- Python: unicorn 2.1.4 and capstone 5.0.7 are in user site-packages (`py -3.12`): don't run them with `python -I`.
- NoDefaultCurrentDirectoryInExePath is set: run `.\x.exe`. A shell inside a folder keeps Windows from moving or
  deleting it.

## REA / IDA cleanup

REA uses headless IDA (`.claude/ida-mcp.json` → `.tools/ida-mcp`). `close_binary` always returns
`cleanup_incomplete`, leaving workers and a temp workspace. After REA analysis (after `close_binary`, even if it
errors), never while a binary is open:

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'ida-mcp' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -Confirm:$false -ErrorAction SilentlyContinue }
Get-ChildItem "$env:LOCALAPPDATA\Temp" -Directory -Filter 'rea-ida-*' |
  Remove-Item -Recurse -Force -Confirm:$false
```

Match on the command line (other Python processes are unrelated), then verify nothing remains.
