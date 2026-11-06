# Windows and Linux: what a second platform needs

Written 2026-11-05 from a survey of `runtime/` (nothing ported yet). The runtime is Windows-only today; the parts that
do the heavy lifting are not: the lifted game code is plain C, graphics are Vulkan, windows and input are SDL3. The
Windows dependence is in our platform layer: 32 runtime files include `windows.h`. All of it is in nxrecomp, so a
port serves every game (Violet and MK8).

## First step: Proton / Wine (almost no work)

Try the Windows package under Proton or Wine before porting anything. Graphics need no translation (Vulkan is
passed through), and Wine implements exactly the APIs that are hard to port (below): `GetWriteWatch`,
`SuspendThread` / `GetThreadContext`, vectored exception handlers, `VirtualAlloc` with `MEM_WRITE_WATCH`. Things to
check: that Wine leaves the guest range `AS_BASE..AS_END` (hos.h: 0x10_0000_0000-0x80_0000_0000) free for
`mem.c`'s reservation; the speed of `GetWriteWatch` scans (gpu.c `refresh_cpu_textures`) and of thread suspension
(kernel.c preemption); Media Foundation (movie.c) is the weak spot of Wine and may need mfplat from Proton.
If it runs well, Linux players are covered and a native port is optional; if not, its failures say which of the
hard parts below come first.

## A native port

Approach: a small platform header (threads, locks, events, atomics, time, memory, signals / exceptions) with a
Windows and a POSIX implementation, and the rest of the runtime written against it. The build stays CMake + Ninja.
Estimate: 2-4 weeks of focused work to a Linux build that boots and plays, more to match Windows' speed and
polish; it needs a Linux machine (or a Steam Deck) to test on.

### Mechanical (most of the lines, low risk)

| Windows | POSIX / portable | Where |
|---|---|---|
| `_beginthreadex`, `WaitForSingleObject`, `CreateEvent` | pthreads, futex or condvar events | 18 + 13 + 9 sites; kernel.c, gpu.c, vk.c |
| `SRWLOCK`, `CRITICAL_SECTION`, `Interlocked*`, `INIT_ONCE` | pthread mutex / rwlock, C11 atomics, `pthread_once` | ~85 sites |
| `QueryPerformanceCounter` | `clock_gettime(CLOCK_MONOTONIC)` | 23 sites |
| `VirtualAlloc` / `VirtualProtect` / `VirtualFree` | `mmap` / `mprotect` / `munmap`; the guest range with `MAP_FIXED_NOREPLACE`, `PROT_NONE`, `MAP_NORESERVE` | mem.c (the reservation, 4 KB commits), kmem.c, dispatch.c |
| `__declspec(thread)` | `_Thread_local` | 14 sites (dispatch.c host jump buffers, ...) |
| Winsock events (`WSACreateEvent`, `WSAEventSelect`) | `poll` on the ENet socket | net/room.c (the room client's receive loop; the game's bsd service is a proxy over the room, no OS sockets) |
| mod DLLs (`LoadLibrary`) | `dlopen` (`.so` mods) | mods.c |
| `build.bat`, `vcenv.bat`, `*.ps1` | shell scripts; CMake flags: `/clang:...`, `/DEBUG`, `/MAP`, `/HIGHENTROPYVA:NO` have GCC/Clang equivalents (or none needed) | tools/, nxrecomp.cmake |
| `setjmp` / `longjmp` around lifted calls (a64rt.h `GUEST_SETJMP`) | the same functions; check that nothing relies on the MSVC CRT's unwinding `longjmp` | a64rt.h, dispatch.c |

### Library swaps (moderate)

| Windows | Replacement | Where / what |
|---|---|---|
| BCrypt (AES-XTS, -CTR, -ECB) | OpenSSL, or our own AES-NI code | nca.c: reading NSPs / NCAs directly |
| WinHTTP | libcurl | net/browser.c: the room list |
| WIC | stb_image or libpng | vk.c: the window icon from control.nacp |
| Media Foundation | FFmpeg (libavcodec / libavformat), or dav1d / openh264 for one codec | movie.c: video files decoded to NV12 (Violet's Bink movies are guest code and need none of it) |
| Win32 input box | SDL3 text input or our own overlay box | swkbd.c: the keyboard applet's text entry |
| `MessageBox`, `comctl32` | `SDL_ShowMessageBox`; the room browser's Win32 dialog becomes an SDL / overlay screen | dispatch.c, main.c (fatal errors), net/browser.c (the room browser) |
| dbghelp stack traces | libunwind / `backtrace` + `dladdr`, symbols from the ELF | dispatch.c crash reports |
| `psapi` `GetMappedFileNameA` | `/proc/self/maps` | mem.c: names what is in the way when the guest range can't be reserved |

### The hard parts (where the risk is)

1. **CPU-written texture detection** (gpu.c `refresh_cpu_textures`, mem.c `MEM_WRITE_WATCH`): `GetWriteWatch` has no
   Linux equivalent. Options: soft-dirty bits (`/proc/self/clear_refs` + `/proc/self/pagemap`: whole-process, read
   per page), userfaultfd write-protect (`UFFDIO_WRITEPROTECT`, kernel 5.7+), or `mprotect` + `SIGSEGV` per
   written page. This code is speed-sensitive (Violet notes: the scan took 5-20 ms in loads before it moved off the
   present).
2. **Preempting guest threads** (kernel.c `preempt`, dispatch.c profiler): `SuspendThread` + `GetThreadContext`
   to see whether a thread is inside lifted code and stop it. POSIX: a signal (`pthread_kill`) to the thread, its
   handler reading `ucontext` (RIP) and parking or resuming it. Timing-sensitive: it is the guest scheduler.
3. **Traps and faults** (dispatch.c `crash_handler`, the write-watch single step that sets the trap flag in
   `EFlags`): vectored exception handlers become `SIGSEGV` / `SIGBUS` / `SIGTRAP` handlers on `ucontext`
   (`REG_EFL`, `REG_RIP`), with `sigaltstack` for stack overflows.
4. **Calling convention**: the lifted code and runtime are compiled for the Windows x64 ABI by clang-cl. With Clang
   on Linux the SysV ABI applies; check the lifter's `NXCC` functions, the indirect-call table (a64rt.h
   `call_ind_fast`) and any assembly or `ms_abi` assumptions; `long` is 64-bit on Linux (32 on Windows).
5. **Guest address range**: hos.h `AS_BASE..AS_END` (64-512 GB) must be mappable at exactly those addresses. Linux
   gives user space 128 TB (47-bit), so it fits; `/HIGHENTROPYVA:NO` (kept Windows from putting stacks and heaps
   in it) has no counterpart: reserve the range first thing in `main`, before other threads start.

### Suggested order

Platform header and the mechanical swaps first (Windows keeps working through the same header), then a Linux build
that links with stubs for the hard parts (no texture refresh, no preemption: enough to boot to the title), then
the hard parts one by one, then the library swaps, then packaging (AppImage or a tarball, the same `game\` folder
layout, shaders.pak).
