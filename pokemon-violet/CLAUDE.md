# pokemon-violet: Pokémon Violet recompilation

Static recompilation of the user's own Pokémon Violet **4.0.0** (title 01008F6008C5E000, with the Teal Mask /
Indigo Disk DLC) to a native Windows x64 program, on the shared `../nxrecomp` (lifter, runtime, NVN on Vulkan).

Workspace rules, toolchain and shared tools are in `../CLAUDE.md`. Paths below are relative to `pokemon-violet/`.
Game-derived data stays in `rom/`, `extract/`, `dump/`, `build/` and `mods-ref/`. **History, findings, addresses
and every experiment so far are in `NOTES.md`** (moved there unchanged on 2026-11-07): grep it before proposing a
tool or an experiment, and add new sessions' findings there; keep this file to how-to, state and what is open.

## Start here

From the workspace root (`..\start.ps1` wraps the scripts in `violetrecomp\`, which also work on their own):

| Do | Command |
|---|---|
| Play (builds first; runs until the game is closed; log `build\run.log`) | `.\start.ps1 violet` (`-NoBuild` to skip the build, `-Log 2`, `-Timeout <s>` for unattended runs) |
| Build only (first time configures `build\cmake`, ~25 min from scratch; keep it Release) | `.\start.ps1 build violet` = `violetrecomp\build.bat` |
| Host a room for local play (UDP 24872) | `.\start.ps1 room` (`-Name`, `-Password`); join it in the game's menu, Multiplayer > Rooms |
| Join a room directly | `.\start.ps1 violet -Room 127.0.0.1:24872 -Nick Host` (the game starts offline otherwise; the old start-up browser: `NX_ROOM_BROWSER=1`) |
| A real Switch in the room | `.\start.ps1 beacon` (ESP32-S3 on USB; see `../ldn-beacon/CLAUDE.md`) |
| A folder for players | `.\start.ps1 package violet <dir>` (nxrecomp `tools\package.ps1`; `-PythonZip` / `-ShaderPack`) |
| Stand-still benchmark | `violetrecomp\bench.ps1 <name> [NAME=value ...]` (~140 s, hands off; `-Summary`) |
| New shaders (dumped as the game reaches them) | `python ..\nxrecomp\tools\shader\translate.py --spv` from `violetrecomp\`, then run again |

Never script input (`NX_INPUT`, pressing A) unless the user asks; bench.ps1 passes `NX_INPUT=` empty.

**In game:** Esc frees the mouse and shows the menu bar at the top (it fades in on mouse movement; F1 pins it; a
click on the game hides it): **Options** > Video (frame rate, render scale, FPS counter, borderless fullscreen;
Alt+Enter; the user: borderless only, never exclusive fullscreen) and Controls (keyboard, mouse, gamepad, hotkeys;
all rebindable, saved in `build\settings.ini`) and Interface (theme: the game's from game.json `theme` (violet and
gold), Dark, Light, Classic; menu size), **Mods** (each mod's windows with its hotkey; Manage turns a DLL off for the
next start), **Multiplayer** (the room, Rooms...: the public list, nickname, direct connect with password; leave;
switching or leaving during a local session asks first and ends it; `net/netmenu.c`). F3 FPS counter, F5 frame rate cycle (development), F12 captures a frame to `build\frames\`. The
menu is nxrecomp's `nxui.dll` next to the exe (ui/nxui.cpp; without it the game runs with plain hotkeys). Mods are
optional DLLs in `build\cmake\mods\` (`NX_MODS=0` loads none), one each: **violet_raids** (Tera Raids, F6: find
seeds, write dens), **violet_items** (Items, F7: give / set, a Lua console in the game's VM), **violet_mons** (Spawn
a Pokemon, F8: into the boxes; wild spawns made to order; "Around you": spawn on the player, the area's spawn list
and weights; held items). Sources `mods\<name>\`, shared `mods\common\`. Their data: `python mods\raids\raiddata.py`
(raids.dat, encounter tables; needs `build\ooz`), `mons_*.txt` and `items.txt` in `build\mods\` from svedit
(`monsdata`, `items`). Packages: `-Mods all` (or names) copies the DLLs; the data is not packaged (game-derived).

**Saves:** the game's save is `build\save\`; backups and test saves are in `build\saves\` (`save-backup-<date>`,
`save-net-a` / `-b` for two instances in one room, ...). bench.ps1 copies `save` to `build\save-idle-run`. Closed-game
edits: `violetrecomp\tools\svedit` (.NET 9 + PKHeX.Core; back up `build\save` first): ride upgrades, raid levels,
`addmon`, `dump`, ...

**Switches** (read once at boot): `NX_ROOM`, `NX_NICK`, `NX_ROOM_BROWSER=1`, `NX_MODS=0`, `NX_RES_SCALE`, `NX_FPS=0`,
`NX_VK_VALIDATION`, `NX_PROFILE`, `NX_GPU_PROF`, `NX_GPU_TIMELINE`, `NX_HITCH_MS`, `NX_WILD_LOG=1`, `NX_WATCH_MEM`,
`NX_IPC_TRACE=<interfaces>`; the rest and what they print: grep `NOTES.md`.

## State (2026-11-07)

- Playable and saves: the user plays the story (battles, catching, raids, riding); ~170 fps uncapped on the RX
  7800 XT (stand-still bench, 2026-10-22).
  Known rendering issues: letters missing in some text (run-time glyph atlas, CPU-written), rain streaks.
- Local play over rooms works between instances (Union Circle, full Tera raids), and with a **real Switch** through
  `../ldn-beacon` (three complete raids, S3 run 12).
- Packaged builds read the game from the player's own NSPs and keys (`NX_PACKAGE`, `violetrecomp_pkg`).
- Mods: F6 raids and F7 items user-checked; F8 box writer and the wild species swap user-checked. **Not yet tried
  in game:** F8 "Spawn \<species\> on me" and the row spawn fix (a kept native entry crashed the game; now found again
  at spawn time), held items, the no-battles toggle, the rest of the wild spec (shiny, IVs, moves, Tera on a caught
  one). **The new menu (2026-11-07)**: built and it loads (nxui.dll, the three mods as separate DLLs, ImGui on the
  swapchain; a 45 s unattended run). **User-checked 2026-11-07: joining a room from the menu after an offline start works**
  (it first crashed: ldn:u / bsd:u were chosen once at start-up, the offline stub when not in a room; now NETPLAY builds
  always register the room-backed ones, which answer as airplane mode until a room is joined).

## Open (the user's list, in order)

0. Try the new menu: Esc / F1, Options (Video, Controls: rebind a key and a pad button), Mods (the three windows,
   Manage), Interface (themes), Multiplayer (Rooms: join, switch between two rooms, leave, rejoin; a switch between
   Union Circle sessions). Built 2026-11-07; only start-up checked unattended.
   Later (asked 2026-11-07, not started): host a room from the menu (Multiplayer > Host: name, password, slots,
   port; the game starts room_server.exe from room\ next to the exe in a package or ../room/build in development as
   a child in a job object so it stops with the game, joins 127.0.0.1 and shows the LAN address; friends outside the
   network still need the port forwarded or a tunnel). Also later: the whole room server on the ESP32 with the beacon
   (../ldn-beacon/CLAUDE.md, Open).
1. Try the F8 changes above; confirm the wild spec on a caught mon (overworld shiny vs battle shiny).
2. Spawn-pool rules: wrap the encounter table's filter / per-entry rules (version, time, flags, weights; F8 weights
   are the first step), spawn density parameters. NOTES.md, "Planned next" item 3.
3. **Raid crystals not refreshing after F6 writes**: rebuild hook built 2026-11-07; user-tried: rebuild and
   reset both make the crystals vanish (map updates, they never come back). NOTES.md, "Raid crystals rebuilt on demand".
   Event data from a friend's save (delivery 2023-07-28) is in `build\save` (svedit `copy`); no change in game until
   a day reroll. Parked (the user: later); forcing the reroll is the next step.
   **In progress: event raids** (past, custom, and a friend's homebrewed Switch save). Start from NOTES.md, the section
   "Event raids from online" (delivery file names, save blocks, both routes, what is waiting).
4. The two F12 captures of 2026-11-06 (missing letters, rain).
5. Performance: NOTES.md, "Next (written 2026-10-22 ...)".

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
| `dump/elf/` | ELF copies of the modules; IDA databases `*.i64` |
| `dump/romfs/` | merged RomFS (base 1.0.0 + update 4.0.0), 9.24 GB |
| `dump/aoc/<data id>/` | DLC data (RomFS of the DLC NSP's PublicData NCA, via its ticket) |
| `violetrecomp/` | our source (git): `game.json`, `CMakeLists.txt`, `build.bat`, `run.ps1`, `bench.ps1`, host code (`fps.c`, `gfx.c`, `lua.c`, `wild.c`), `patches.txt`, `mods/raids/` (F6-F8), `tools/` (svedit, luabc.py, xref.py, bfbs.py, img.py) |
| `mods-ref/` | others' mods for 4.0.0 (reference only) and their download zip |
| `build/cmake/` | the build: `violetrecomp.exe`, `mods/` |
| `build/analysis/` | `<module>_seeds.json`, `<module>_ida.json` (IDA exports), logs |
| `build/lifted/`, `build/image/` | lifted C, module images |
| `build/lua/` | the game's Lua bytecode (trinity.py), read with `violetrecomp\tools\luabc.py` |
| `build/save/`, `build/saves/` | the game's save; backups and test saves |
| `build/mods/` | mod data (raids.dat, mons_*.txt, items.txt, encounter tables) |
| `build/peek/` | first 16 KB of each NCA, small NCAs, tickets; `ctl_base/`, `ctl_upd/` = control data (NACP, icons) |

## Commands

- `extract/` (the NSPs unpacked, 11.9 GB) was deleted on 2026-10-17 after `dump/` was made. Extract again (PowerShell, from `pokemon-violet/`: `$env:USERPROFILE="$PWD\..\keys"`, `$n` = `..\.tools\nstool.exe`):
  `& $n -x extract\base rom\<base>.nsp; & $n -x extract\upd rom\<update>.nsp` (40 s), then ExeFS `& $n --tik extract\upd\01008f6008c5e800…11.tik -x /0 dump\exefs
  extract\upd\d24a763a8882575f1c252dce37021db5.nca`, RomFS the same with `--basenca
  extract\base\ef6d3388d34a30dda23208f98780c610.nca -x /1 dump\romfs`.
- `llvm-nm -D [-C] dump/elf/main.elf` (LLVM's, from Visual Studio's Clang tools or MSYS2) lists imports and exports.
- Decompile: `..\nxrecomp\tools\ghidra\decomp.ps1 <abs out path> <off>...` from `violetrecomp\` (GHIDRA_HOME or
  `-Ghidra <dir>`), or REA (headless IDA; cleanup in `../CLAUDE.md`).
