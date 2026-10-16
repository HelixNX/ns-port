# mk8recomp: MK8DX 3.0.5 recompiled for Windows x64.
# Configure from a vcvars64 shell with clang-cl and Ninja (see build.bat):
#   cmake -G Ninja -S mk8recomp -B build/cmake -DCMAKE_C_COMPILER=clang-cl -DLIFTED_DIR=build/lifted
cmake_minimum_required(VERSION 3.24)
# no CMake default flags for lifted code: Debug's /Zi /RTC1 multiply compile time and object size.
# (must come before project(), which otherwise picks Debug for clang-cl)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Release (default) or Debug")
project(mk8recomp C)
set(CMAKE_C_FLAGS_RELEASE "/MD" CACHE STRING "" FORCE)

set(LIFTED_DIR "${CMAKE_SOURCE_DIR}/../build/lifted" CACHE PATH "C sources written by tools/lifter/a64lift.py")
get_filename_component(LIFTED_DIR "${LIFTED_DIR}" ABSOLUTE)

# lifted code: no FP contraction (the guest fuses only where it says so), no strict aliasing (guest memory is
# untyped); -O1 keeps compile time reasonable for the very large generated functions
set(LIFT_FLAGS /clang:-O1 /clang:-ffp-contract=off /clang:-fno-strict-aliasing /clang:-march=haswell
               /clang:-fbracket-depth=1024 /clang:-Wno-everything
               /clang:-gline-tables-only /clang:-gcodeview)   # lines = guest offsets (a64lift #line)

file(GLOB LIFTED_SOURCES CONFIGURE_DEPENDS "${LIFTED_DIR}/*.c")
add_library(lifted STATIC ${LIFTED_SOURCES})
target_include_directories(lifted PRIVATE runtime "${LIFTED_DIR}")
target_compile_options(lifted PRIVATE ${LIFT_FLAGS})
# each compile of a ~2 MB generated file takes 1-2 GB: limit parallel jobs so a 16 GB machine does not page
set(LIFT_JOBS 8 CACHE STRING "parallel compiles of lifted code")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS lift_pool=${LIFT_JOBS})
set_property(TARGET lifted PROPERTY JOB_POOL_COMPILE lift_pool)

add_library(a64rt STATIC runtime/a64rt.c)
target_include_directories(a64rt PUBLIC runtime)
target_compile_options(a64rt PRIVATE ${LIFT_FLAGS})

# the game: the runtime (kernel, SVCs, services) around the lifted code
set(RUNTIME_SOURCES runtime/main.c runtime/loader.c runtime/dispatch.c runtime/mem.c runtime/kernel.c runtime/kmem.c
                    runtime/svc.c runtime/ipc.c runtime/services/am.c runtime/services/lm.c runtime/services/apm.c
                    runtime/services/pctl.c runtime/services/vi.c runtime/services/hid.c runtime/services/time.c runtime/services/acc.c runtime/services/mii.c runtime/services/audio.c runtime/audio_dsp.c runtime/audio_fx.c runtime/services/pl.c runtime/services/set.c
                    runtime/services/fs.c runtime/romfs.c runtime/nvn.c runtime/gpu.c runtime/tex.c runtime/vk.c runtime/astc.c runtime/input.c runtime/movie.c)
add_executable(mk8recomp ${RUNTIME_SOURCES})
target_compile_options(mk8recomp PRIVATE /clang:-O2 /clang:-march=haswell /W3 /clang:-Wno-unused-parameter /clang:-Wno-bitwise-op-parentheses
                                         /clang:-fno-strict-aliasing)
target_compile_definitions(mk8recomp PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
set(DEPS "${CMAKE_SOURCE_DIR}/../deps")
target_include_directories(mk8recomp PRIVATE "${DEPS}/Vulkan-Headers/include" "${DEPS}/SDL3/SDL3-3.4.18/include")
target_link_libraries(mk8recomp PRIVATE lifted a64rt dbghelp bcrypt psapi mfplat mfreadwrite mfuuid ole32 "${DEPS}/SDL3/SDL3-3.4.18/lib/x64/SDL3.lib")
add_custom_command(TARGET mk8recomp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                   "${DEPS}/SDL3/SDL3-3.4.18/lib/x64/SDL3.dll" "$<TARGET_FILE_DIR:mk8recomp>")
# /DEBUG: a PDB whose public symbols name the lifted functions (f_<guest address>) in crash reports
# /HIGHENTROPYVA:NO: otherwise Windows puts thread stacks and heaps anywhere up to 1 TB, inside the guest range.
# /MAP: shows where lifted code lies (kernel.c preempts threads only inside it; see dispatch_init)
target_link_options(mk8recomp PRIVATE /DEBUG /INCREMENTAL:NO /HIGHENTROPYVA:NO /MAP)

# links every lifted function with stub runtime entry points (M2 check)
add_executable(linkcheck runtime/linkcheck.c)
target_compile_options(linkcheck PRIVATE ${LIFT_FLAGS})
target_link_libraries(linkcheck PRIVATE lifted a64rt)
target_link_options(linkcheck PRIVATE /STACK:16777216)
