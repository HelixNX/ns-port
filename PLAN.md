# mk8recomp — plan

> **Update 2026-10-15:** the target is now update **3.0.5** (`dump_v21/`), which is **64-bit AArch64**, so
> ssbu-recomp's lifter and runtime are reused and the A32 lifter below is shelved. (3.0.1 was still 32-bit.) From 3.x
> the NVN driver lives in `sdk`, and `subsdk0` is the movie player. The tables below describe 1.0.0.
> Current status and next steps: `../CLAUDE.md`.

Static recompilation of Mario Kart 8 Deluxe 1.0.0 (title 0100152000022000, your own dump) to a native
Windows x64 program, following the design of [ssbu-recomp](../ref/ssbu-recomp) (GPLv3).
Nothing derived from the game leaves `dump/` and `build/`.

## What the dump is

| Module | What it is | Size (text) | Plan |
|---|---|---|---|
| `main` | the game | 10.4 MB, 2.7 M instructions | lift to C |
| `sdk` | Nintendo SDK (nn::*) | 3.6 MB | re-implement what main imports (790 symbols); lift pure library code |
| `subsdk0` | NVN graphics driver (nvn*, android/nvgr) | 1.0 MB | replace: NVN on Vulkan |
| `subsdk1` | nn::vi display + nn::sf | 70 KB | replace |
| `subsdk2` | zlib | 47 KB | lift as-is (pure code) |
| `subsdk3` | libcurl (online) | 182 KB | stub (offline) |
| `subsdk4` | movie player (android media, mkv) | 3.2 MB | replace with Media Foundation, or skip movies |
| `rtld` | dynamic loader | 13 KB | replaced by our loader |

**Key difference from SSBU: MK8DX 1.0.0 is a 32-bit (AArch32, ARM encoding, no Thumb) program.**
ssbu-recomp's lifter and runtime are AArch64 / 64-bit ABI only, so:

- the CPU lifter is new (`tools/lifter/a32lift.py`);
- the HLE runtime uses the 32-bit AAPCS ABI (args in r0-r3 then stack, 64-bit values in register pairs,
  4-byte pointers inside every SDK and NVN structure);
- what carries over mostly unchanged: the overall design, the Maxwell shader translator (`mx_translate.c`),
  texture decoding (`tex_decode.c`), SDL3 input, Vulkan back end structure, and the approach to NVN.

## Layers

1. **CPU**: `a32lift.py` — one C function per guest function, register context `Ctx` (runtime/a32rt.h), guest
   memory reserved at `GUEST_BASE` (guest address a = host pointer GUEST_BASE + a). Function boundaries and
   code/data separation from IDA (`tools/ida_export.py`), because ARM keeps literal pools between functions.
   Imports through PLT stubs become direct calls `imp_<symbol>(c)`.
2. **Loader**: map main + the modules we lift at their 32-bit addresses, apply relocations
   (RELATIVE / ABS32 / GLOB_DAT / JUMP_SLOT), fill import slots with host functions, run init arrays, call
   nnMain on the main thread.
3. **SDK (HLE)**: `nn::os` (threads, mutex, events, TLS), `nn::fs` (RomFS from the dump, save data),
   `nn::hid` (SDL3), `nn::audio` (audio renderer → WASAPI/SDL), `nn::oe`/applet, `nn::time`, `nn::settings`,
   `nn::account`, `nn::mii` (91 imports), fonts, error/report stubs, network/friends/ldn answering "offline".
4. **Graphics**: NVN API on Vulkan (`nvnBootstrapLoader` → our proc table), Maxwell shader → GLSL → SPIR-V,
   texture decode (block linear, ASTC/BCn).
5. **Audio**: nn::audio renderer (voices, mixing, effects) → host audio.

## Milestones

- [x] M0 Extract ExeFS, NSO → ELF (32-bit aware), module inventory, import list
- [x] M0 IDA analysis of main, function/data export
- [ ] M1 Lifter coverage: ≥ 99.9 % of main's instructions translated (VFP, NEON as needed)
- [ ] M1 Differential test of the lifter against Unicorn (ARM mode) on generated instruction cases
- [ ] M2 Compile the lifted main with clang into a DLL (`-ffp-contract=off`: VMLA is not fused)
- [ ] M3 Loader + minimal runtime: reach nnMain, trap on the first unimplemented import
- [ ] M4 SDK HLE until the game's main loop runs headless (no graphics)
- [ ] M5 NVN on Vulkan: first frame, then menus
- [ ] M6 Audio, input, save data; race playable
- [ ] M7 Performance, shader cache, polish

## Build notes

- clang 19 (MSYS2) is installed; ssbu-recomp used clang 9 for compile time on huge functions — measure.
- Python packages still needed: `unicorn` (differential tests), `capstone` (disassembly cross-check),
  `zstandard` (only if ssbu tools that need it are reused).
- Big outputs go to `E:\projects\ns-port\build`.
