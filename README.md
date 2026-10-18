# ns-port

Static recompilation of Nintendo Switch games to native Windows x64 programs: an AArch64 → C lifter, the game's own
SDK running over an emulated kernel and services, and NVN replayed into Vulkan.

| Project | Game | Repo |
|---|---|---|
| `nxrecomp/` | shared runtime (kernel, SVCs, IPC, services, NVN on Vulkan, audio, input) and tools (lifter, shader translator, IDA export) | [s1099/nxrecomp](https://github.com/s1099/nxrecomp) |
| `pokemon-violet/violetrecomp/` | Pokémon Violet 4.0.0 + DLC | [s1099/violetrecomp](https://github.com/s1099/violetrecomp) |
| `mk8dx/mk8recomp/` | Mario Kart 8 Deluxe 3.0.5 | [s1099/mk8recomp](https://github.com/s1099/mk8recomp) |

This repo is the workspace: it ties the three repos together as submodules, holds the project notes (`CLAUDE.md`
files) and keeps the empty folders your own game data goes into. **No game data, keys or firmware are included, and
none may ever be committed.** You must dump them from your own console and cartridges or eShop purchases. The
`.gitignore` is a whitelist, so anything you put in those folders stays local.

## Get the code

```sh
git clone --recursive https://github.com/s1099/ns-port.git
# or, in an existing clone:
git submodule update --init
```

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
├── nxrecomp/                shared code (submodule)
├── pokemon-violet/
│   ├── rom/                 your NSPs: base, update 4.0.0 (v786432), DLC
│   ├── dump/                made by setup: exefs/, elf/, romfs/, aoc/
│   ├── build/               made by setup: analysis/, lifted/, cmake/, the .exe
│   └── violetrecomp/        game repo (submodule): game.json, CMakeLists.txt, build.bat, run.ps1
└── mk8dx/
    ├── rom/                 game.nsp (base 1.0.0), mk8u_v1376256.nsp (update 3.0.5)
    ├── extract_v21/, dump_v21/, build/   made by setup
    └── mk8recomp/           game repo (submodule)
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
- Visual Studio (MSVC `link.exe`, `vcvars64.bat`); **clang-cl 19**, CMake ≥ 3.24 and Ninja (MSYS2 `mingw64`).
- Python 3 with `capstone` and `unicorn` (`pip install capstone unicorn`; the lifter tests use unicorn).
- IDA Pro 9 with idalib, for the one-time function export (`nxrecomp/tools/ida_export.py`).
- `.tools/nstool.exe` ([jakcron/nstool](https://github.com/jakcron/nstool)).
- `deps/`: `SDL3/SDL3-<ver>` (VC development package), `Vulkan-Headers`, `glslang` (release build).

The paths in `build.bat` (`E:\Microsoft Visual Studio\…\vcvars64.bat`, `C:/msys64/mingw64/bin/clang-cl.exe`,
`ninja.exe`) are this machine's. Edit them for yours.

## Setup (Pokémon Violet)

Run in PowerShell from `pokemon-violet/`, with `$n = '..\.tools\nstool.exe'` and
`$env:USERPROFILE = '<ns-port>\keys'` (nstool's key lookup).

1. **Unpack and dump.**
   ```powershell
   & $n -x extract\base "rom\<base>.nsp"; & $n -x extract\upd "rom\<update>.nsp"
   & $n --tik extract\upd\<ticket>.tik -x /0 dump\exefs extract\upd\<update program NCA>
   & $n --tik extract\upd\<ticket>.tik --basenca extract\base\<base program NCA> -x /1 dump\romfs extract\upd\<update program NCA>
   ```
   The program NCA is the largest one. `pokemon-violet/CLAUDE.md` ("ROMs", "Commands") lists the NCA ids. For the DLC,
   extract its PublicData RomFS to `dump\aoc\01008f6008c5f001`. You can delete `extract\` afterwards.
2. **ELF copies.** From `violetrecomp/`, for each of `main`, `sdk`, `subsdk0`:
   `python ..\..\nxrecomp\tools\nso2elf.py ..\dump\exefs\<m> ..\dump\elf\<m>.elf`
3. **IDA export** (once; main takes about 40 min):
   `python ..\..\nxrecomp\tools\nxmod.py seeds ..\dump\elf\<m>.elf ..\build\analysis\<m>_seeds.json`, then
   `IDA_SEEDS=<seeds> python ..\..\nxrecomp\tools\ida_export.py ..\dump\elf\<m>.elf ..\build\analysis\<m>_ida.json`
   (with idalib's Python and `IDADIR` set).
4. **Lift** (`$L = '..\..\nxrecomp\tools\lifter'`): `python $L\a64lift.py cover` (should report 100%), then
   `python $L\a64lift.py lift ..\build\lifted` and `python $L\a64image.py ..\build\image`.
5. **Build:** `.\build.bat` (configures `build\cmake` on first use; about 25 min from scratch). Keep it Release.
6. **Run:** `.\run.ps1 -Timeout 300` (logs to `build\run.log`). New shaders are dumped as the game reaches them;
   translate them with `python ..\..\nxrecomp\tools\shader\translate.py --spv` and run again.

MK8 follows the same steps with its own folders (`extract_v21`, `dump_v21`, `build/analysis_305`). See
`mk8dx/CLAUDE.md`.

## Controls and options

Keyboard: ESDF = left stick, IJKL = right stick, arrows = D-pad, R = R; gamepads through SDL3. Environment options
(`NX_INPUT`, `NX_NOPACE`, `NX_PROFILE`, `NX_WATCH_MEM`, …) are described in the projects' `CLAUDE.md`.

## Status

See each project's `CLAUDE.md`. Violet renders the first-boot screens, the opening cutscene and the bedroom. MK8 is
further along.
