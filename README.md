# ns-port

Static recompilation of Nintendo Switch games to native Windows x64 programs: an AArch64 → C lifter, the game's own
SDK running over an emulated kernel and services, and NVN replayed into Vulkan.

| Project | Game | Repo |
|---|---|---|
| `nxrecomp/` | shared runtime (kernel, SVCs, IPC, services, NVN on Vulkan, audio, input) and tools (lifter, shader translator, IDA export) | this repo |
| `pkmn-violet/` | Pokémon Violet 4.0.0 + DLC (source in `violetrecomp/`) | [HelixNX/pkmn-violet](https://github.com/HelixNX/pkmn-violet) |
| `mk8dx/` | Mario Kart 8 Deluxe 3.0.5 (source in `mk8recomp/`) | [HelixNX/mk8dx](https://github.com/HelixNX/mk8dx) |
| `room/` | the multiplayer room server (the protocol Citra, yuzu and Eden rooms speak); Windows or Linux | local repo |
| `ldn-beacon/` | a real Switch's local play bridged into a room, with an ESP32-S3 on USB as the radio | [HelixNX/ldn-beacon](https://github.com/HelixNX/ldn-beacon) |

## Start here

Everything starts from `start.ps1` at the root (PowerShell); `.\start.ps1` alone lists it:

```powershell
.\start.ps1 violet                 # build and play Pokemon Violet (-NoBuild to skip the build)
.\start.ps1 mk8                    # the same for Mario Kart 8 Deluxe
.\start.ps1 room                   # host a room for local play (UDP 24872); join it in the game's menu (Multiplayer)
.\start.ps1 violet -Room 127.0.0.1:24872 -Nick Host   # join a room directly
.\start.ps1 beacon                 # bring a real Switch into the room (ESP32-S3 on USB)
.\start.ps1 build violet|mk8|room|beacon
.\start.ps1 package violet <dir>   # a folder for players
```

The first time, a game needs its setup (below: unpack, dump, export, lift); after that `start.ps1` is all.

This repo is the workspace: it holds the shared source (`nxrecomp/`), the build entry, `start.ps1`, the notes
(`CLAUDE.md`) and the empty folders your keys and firmware go into. Each game is its own repo, cloned into the
workspace as its project folder (`.\start.ps1 clone`), with its source, notes and the empty `rom/` folder your
own game data goes into. **No game data, keys or firmware are included, and
none may ever be committed.** You must dump them from your own console and cartridges or eShop purchases. The
`.gitignore` is a whitelist, so anything you put in those folders stays local.

## Get the code

```sh
git clone https://github.com/HelixNX/ns-port.git
cd ns-port
.\start.ps1 clone            # both games (HelixNX/pkmn-violet, HelixNX/mk8dx); or: .\start.ps1 clone violet
```

`clone` also works on a project folder that already holds your data (`rom/`, `dump/`, `build/`): it sets up the repo
in place and leaves the rest alone.

Clone to a drive with lots of free space. One game needs about 30–40 GB (ROMs, dump, about 1 GB of lifted C and the
build).

## Where things go

```
ns-port/
├── keys/                    prod.keys, title.keys (from your console, e.g. with Lockpick_RCM)
│   └── .switch/             copies of both files (nstool reads keys only from %USERPROFILE%\.switch)
├── firmware/                your firmware dump: Firmware_<ver>.zip and its NCAs in <ver>/ (e.g. 22.5.0/)
├── deps/                    third-party libraries (see Prerequisites)
├── .tools/                  nstool.exe; ida-mcp (optional, headless IDA)
├── nxrecomp/                shared code
├── pkmn-violet/             game repo (HelixNX/pkmn-violet): CLAUDE.md, NOTES.md
│   ├── rom/                 your NSPs: base, update 4.0.0 (v786432), DLC
│   ├── dump/                made by setup: exefs/, elf/, romfs/, aoc/
│   ├── build/               made by setup: analysis/, lifted/, cmake/, the .exe
│   └── violetrecomp/        game source: game.json, game.cmake, build.bat, run.ps1
├── mk8dx/                   game repo (HelixNX/mk8dx): CLAUDE.md
│   ├── rom/                 game.nsp (base 1.0.0), mk8u_v1376256.nsp (update 3.0.5)
│   ├── extract_v21/, dump_v21/, build/   made by setup
│   └── mk8recomp/           game source
├── room/                    room server (own repo): build.bat or CMake, build/room_server.exe
├── ldn-beacon/              real-Switch bridge (own repo): build.bat or CMake, s3/ = the ESP32-S3 firmware
└── start.ps1                the front door (Start here)
```

### ROMs

| Game | Files in `<project>/rom/` | Title id |
|---|---|---|
| Pokémon Violet | base `[01008F6008C5E000][v0].nsp`, update **4.0.0** `[01008F6008C5E800][v786432].nsp`, optional DLC `[01008F6008C5F001][v0].nsp` | 01008F6008C5E000 |
| Mario Kart 8 Deluxe | base `game.nsp`, update **3.0.5** `mk8u_v1376256.nsp` | 0100152000022000 |

The exact update version matters. Module bases and lift-time overrides are tied to that build (`game.json`).

### Keys and firmware

- `keys/prod.keys` and `keys/title.keys`, with copies in `keys/.switch/`.
- `firmware/<ver>/`: the firmware's NCAs. The runtime takes system data archives from it (e.g. MiiModel); see
  `mk8dx/CLAUDE.md`, "System data archives".

## Prerequisites

- Windows x64 with a Vulkan 1.3+ GPU.
- Visual Studio 2022 with "Desktop development with C++" and "C++ Clang tools for Windows" (clang-cl, CMake and
  Ninja come with it). MSYS2's clang-cl also works. `nxrecomp/tools/vcenv.bat` finds them; set `NX_VCVARS` and
  `NX_CC` to override.
- Python 3 with `capstone` and `unicorn` (`pip install capstone unicorn`; the lifter tests use unicorn).
- For the one-time function export, either Ghidra 12 (free; `nxrecomp/tools/ghidra/ghidra_export.ps1`, with
  `GHIDRA_HOME` set) or IDA Pro 9 with idalib (`nxrecomp/tools/ida_export.py`).
- `.tools/nstool.exe` ([jakcron/nstool](https://github.com/jakcron/nstool)).
- Vulkan headers and SDL3, either in `deps/` (`Vulkan-Headers`, `SDL3/SDL3-<ver>` from SDL's VC development zip)
  or installed: the Vulkan SDK (sets `VULKAN_SDK`) and vcpkg's `sdl3` (with `VCPKG_ROOT` set).
- glslang for the shader translator: `deps/glslang` or the Vulkan SDK's `glslangValidator`.

Nothing in the build has machine-specific paths: if something isn't found, the build says what to install or set.

## Setup (Pokémon Violet)

Run in PowerShell from `pkmn-violet/`, with `$n = '..\.tools\nstool.exe'` and
`$env:USERPROFILE = '<ns-port>\keys'` (nstool's key lookup).

1. **Unpack and dump.**
   ```powershell
   & $n -x extract\base "rom\<base>.nsp"; & $n -x extract\upd "rom\<update>.nsp"
   & $n --tik extract\upd\<ticket>.tik -x /0 dump\exefs extract\upd\<update program NCA>
   & $n --tik extract\upd\<ticket>.tik --basenca extract\base\<base program NCA> -x /1 dump\romfs extract\upd\<update program NCA>
   ```
   The program NCA is the largest one. `pkmn-violet/CLAUDE.md` ("ROMs", "Commands") lists the NCA ids. For the DLC,
   extract its PublicData RomFS to `dump\aoc\01008f6008c5f001`. You can delete `extract\` afterwards.
2. **ELF copies.** From `violetrecomp/`, for each of `main`, `sdk`, `subsdk0`:
   `python ..\..\nxrecomp\tools\nso2elf.py ..\dump\exefs\<m> ..\dump\elf\<m>.elf`
3. **IDA export** (once; main takes about 40 min):
   `python ..\..\nxrecomp\tools\nxmod.py seeds ..\dump\elf\<m>.elf ..\build\analysis\<m>_seeds.json`, then
   `IDA_SEEDS=<seeds> python ..\..\nxrecomp\tools\ida_export.py ..\dump\elf\<m>.elf ..\build\analysis\<m>_ida.json`
   (with idalib's Python and `IDADIR` set).
4. **Lift** (`$L = '..\..\nxrecomp\tools\lifter'`): `python $L\a64lift.py cover` (should report 100%), then
   `python $L\a64lift.py lift ..\build\lifted` and `python $L\a64image.py ..\build\image`.
5. **Build:** `.\build.bat` here, or `.\build.bat violet` at the root (both configure `build\cmake` on first use, from
   the root `CMakeLists.txt`; about 25 min from scratch). Keep it Release.
6. **Run:** `.\run.ps1 -Timeout 300` (logs to `build\run.log`). New shaders are dumped as the game reaches them;
   translate them with `python ..\..\nxrecomp\tools\shader\translate.py --spv` and run again.

MK8 follows the same steps with its own folders (`extract_v21`, `dump_v21`, `build/analysis_305`). See
`mk8dx/CLAUDE.md`.

## Controls and options

Keyboard: ESDF = left stick, IJKL = right stick, arrows = D-pad, R = R; gamepads through SDL3. Environment options
(`NX_INPUT`, `NX_NOPACE`, `NX_PROFILE`, `NX_WATCH_MEM`, …) are described in the projects' `CLAUDE.md`.

## Status

See each project's `CLAUDE.md`. Violet is playable (story, raids, local play over rooms, and with a real Switch through the beacon), with mods
(F6 raids, F7 items, F8 mons). MK8 races, with online play over rooms.

## Going public (open)

If this goes public it will be fresh repositories under an organisation (a snapshot, not this history): build tools
(Windows and Linux build scripts side by side), the runtime, one repository per game, the room server and the
beacon. To settle first: a licence for each (none yet: all rights reserved by default; svedit links PKHeX.Core,
GPLv3); whether the NSP / NCA reader that decrypts with the player's keys (`nxrecomp/runtime/nca.c`) ships, or
public builds read an nstool dump instead; and the notes (CLAUDE.md / NOTES.md) stay private.
